// Tests of `EnetHostedMeshTransport` and `TickMultiplayerPeer` over real sockets on the loopback interface: joining
// through the host, direct and relayed pairs, DTLS, the admission of players, the host migration (handed over, or after
// a host that stops answering), the engines on top of the mesh, and what a host and a player don't let each other do
// (ADR-077).
//
// `HostedMesh` is a host and its players; `TestCertificate` is a self-signed certificate for the DTLS cases. Some cases
// play one side with a bare ENet socket, to say what this module never would.

#pragma once

#include "../source/common/tick_engine_compat.h"
#include "../source/sync/tick_sync_core.h"
#include "../source/transport/enet_hosted_mesh_transport.h"
#include "../source/transport/tick_multiplayer_peer.h"
#include "test_tick_fuzz.h"
#include "test_tick_mesh_dolls.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "tests/test_macros.h"

namespace TestEnetHostedMesh {

typedef EnetHostedMeshTransport Mesh;
using TestTickSyncCore::TestMover;

// A host (1) and players joining one at a time on localhost, so their ids follow the order (2, 3, ...).
struct HostedMesh {
	int port = 0;
	int count = 0;
	Ref<Mesh> nodes[6];

	// Hosts a mesh on a random port and joins `p_players` players one at a time, so their ids follow the order.
	// `p_relay_only_player` never tries direct links. With TLS options, the mesh uses DTLS (the host also opens `port +
	// 1` for the rendezvous).
	HostedMesh(int p_players, int p_relay_only_player = 0, const Ref<TLSOptions> &p_host_tls = Ref<TLSOptions>(), const Ref<TLSOptions> &p_player_tls = Ref<TLSOptions>()) {
		// A random port pair, tried again if another program uses it (the caller's error printing is kept).
		const bool printing = CoreGlobals::print_error_enabled;
		CoreGlobals::print_error_enabled = false;
		for (int attempt = 0; attempt < 10 && nodes[1].is_null(); attempt++) {
			port = 43000 + 2 * int((OS::get_singleton()->get_ticks_usec() + uint64_t(attempt) * 7919) % 5000);
			nodes[1] = Mesh::create_host(port, 8, "*", Mesh::COMPRESSION_RANGE_CODER, p_host_tls);
		}
		CoreGlobals::print_error_enabled = printing;
		REQUIRE(nodes[1].is_valid());
		count = 1;
		for (int id = 2; id <= p_players + 1; id++) {
			nodes[id] = Mesh::create_player("127.0.0.1", port, Mesh::COMPRESSION_RANGE_CODER, p_player_tls, "localhost");
			CHECK(nodes[id].is_valid());
			nodes[id]->set_direct_connections(id != p_relay_only_player);
			count = id;
			for (int t = 0; t < 3000 && nodes[id]->get_status() != Mesh::STATUS_CONNECTED; t++) {
				poll(1);
			}
		}
	}


	// Closes the players, then the host: each one after the others saw the previous one leave.
	~HostedMesh() {
		// Players first, each after the others saw the previous one leave: a DTLS link to a closed port reports errors.
		for (int i = count; i >= 1; i--) {
			if (nodes[i].is_valid()) {
				nodes[i]->close();
				poll(50);
			}
		}
	}


	// Polls every node `p_rounds` times, a millisecond apart.
	void poll(int p_rounds) {
		for (int round = 0; round < p_rounds; round++) {
			for (int i = 1; i <= count; i++) {
				if (nodes[i].is_valid()) {
					nodes[i]->poll();
				}
			}
			OS::get_singleton()->delay_usec(1000);
		}
	}


	// Whether every node sees all the others as connected.
	bool everyone_connected() const {
		for (int i = 1; i <= count; i++) {
			LocalVector<int> connected;
			nodes[i]->get_connected_peers(connected);
			if (int(connected.size()) != count - 1) {
				return false;
			}
		}
		return true;
	}


	// Polls until every node sees all the others, for eight seconds at most.
	void wait_everyone_connected() {
		for (int t = 0; t < 8000 && !everyone_connected(); t++) {
			poll(1);
		}
	}


	// Waits for a packet from `p_from` on `p_channel` at node `p_node`, dropping the others.
	bool receive(int p_node, int p_from, int p_channel) {
		for (int t = 0; t < 2000; t++) {
			poll(1);
			TickTransport::Packet packet;
			while (nodes[p_node]->pop_packet(packet)) {
				if (packet.from_peer == p_from && packet.channel == p_channel) {
					return true;
				}
			}
		}
		return false;
	}
};

// The host gives the ids, the players reach it through the host link and each other through a punched direct link, and
// every packet carries the sender of its connection.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] Players join through the host and punch direct links") {
	HostedMesh mesh(2);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());

	// The host gives the ids.
	CHECK(mesh.nodes[1]->get_local_peer_id() == 1);
	CHECK(mesh.nodes[2]->get_local_peer_id() == 2);
	CHECK(mesh.nodes[3]->get_local_peer_id() == 3);
	CHECK(mesh.nodes[1]->get_peer_path(2) == Mesh::PATH_HOST);
	CHECK(mesh.nodes[2]->get_peer_path(1) == Mesh::PATH_HOST);
	// On localhost, the punched links always succeed.
	CHECK(mesh.nodes[2]->get_peer_path(3) == Mesh::PATH_DIRECT);
	CHECK(mesh.nodes[3]->get_peer_path(2) == Mesh::PATH_DIRECT);

	// The sender comes from the connection, on every path.
	const uint8_t payload[3] = { 7, 1, 2 };
	CHECK(mesh.nodes[3]->send(2, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, payload, 3) == OK);
	CHECK(mesh.receive(2, 3, TICK_CHANNEL_INPUTS));
	CHECK(mesh.nodes[2]->send(1, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, payload, 3) == OK);
	CHECK(mesh.receive(1, 2, TICK_CHANNEL_CONTROL));
	CHECK(mesh.nodes[1]->send(TickTransport::PEER_BROADCAST, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_RELIABLE, payload, 3) == OK);
	CHECK(mesh.receive(3, 1, TICK_CHANNEL_STATE));

	const Dictionary stats = mesh.nodes[1]->get_stats();
	CHECK(int(stats["relayed_pairs"]) == 0);
	CHECK(int(mesh.nodes[2]->get_stats()["direct_pairs"]) == 1);
}


// A player that never tries direct links has its pairs relayed by the host: relayed packets keep their origin, channel,
// mode and order, and the host counts them.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A pair without a direct link is relayed by the host") {
	// Player 3 never tries direct links: the pairs 2-3 and 3-4 are relayed, 2-4 is direct.
	HostedMesh mesh(3, 3);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	CHECK(mesh.nodes[2]->get_peer_path(3) == Mesh::PATH_RELAYED);
	CHECK(mesh.nodes[3]->get_peer_path(2) == Mesh::PATH_RELAYED);
	CHECK(mesh.nodes[4]->get_peer_path(3) == Mesh::PATH_RELAYED);
	CHECK(mesh.nodes[2]->get_peer_path(4) == Mesh::PATH_DIRECT);

	// Relayed packets keep their origin, channel and order.
	uint8_t payload[2] = { 0, 0 };
	for (uint8_t i = 0; i < 20; i++) {
		payload[0] = i;
		CHECK(mesh.nodes[2]->send(3, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, payload, 2) == OK);
	}
	int next = 0;
	for (int t = 0; t < 2000 && next < 20; t++) {
		mesh.poll(1);
		TickTransport::Packet packet;
		while (mesh.nodes[3]->pop_packet(packet)) {
			CHECK(packet.from_peer == 2);
			CHECK(packet.channel == TICK_CHANNEL_CONTROL);
			CHECK(packet.mode == TickTransport::TRANSFER_MODE_RELIABLE);
			CHECK(int(packet.data[0]) == next);
			next++;
		}
	}
	CHECK(next == 20);
	CHECK(mesh.nodes[3]->send(TickTransport::PEER_BROADCAST, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_UNRELIABLE, payload, 2) == OK);
	CHECK(mesh.receive(4, 3, TICK_CHANNEL_STATE));
	CHECK(int(mesh.nodes[1]->get_stats()["relayed_packets"]) >= 21);
	CHECK(int(mesh.nodes[1]->get_stats()["relayed_pairs"]) == 2);
}


// A connection with an unknown token is refused; a player that leaves is reported once to the host and to the other
// players, which stay connected.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] Strangers are refused and leaving players are reported") {
	HostedMesh mesh(2);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());

	// A connection with an unknown registration token (`ENetConnection` through the class database: the tests don't
	// include ENet's headers).
	Ref<RefCounted> stranger = Object::cast_to<RefCounted>(ClassDB::instantiate("ENetConnection"));
	REQUIRE(stranger.is_valid());
	REQUIRE(int(stranger->call("create_host", 1, 27, 0, 0)) == OK);
	stranger->call("compress", 1);
	stranger->call("connect_to_host", "127.0.0.1", mesh.port, 27, 12345);
	for (int t = 0; t < 500 && int(mesh.nodes[1]->get_stats()["rejected_connections"]) == 0; t++) {
		stranger->call("service", 0);
		mesh.poll(1);
	}
	CHECK(int(mesh.nodes[1]->get_stats()["rejected_connections"]) == 1);
	CHECK(mesh.nodes[1]->get_peers().size() == 2);
	stranger->call("destroy");

	// Player 3 leaves: the host and player 2 see it.
	TickTransport::Event event;
	while (mesh.nodes[2]->pop_event(event)) {
	}
	mesh.nodes[3]->close();
	bool left = false;
	for (int t = 0; t < 3000 && !left; t++) {
		mesh.poll(1);
		while (mesh.nodes[2]->pop_event(event)) {
			left = left || (event.type == TickTransport::EVENT_PEER_DISCONNECTED && event.peer == 3);
		}
	}
	CHECK(left);
	CHECK_FALSE(mesh.nodes[1]->is_peer_connected(3));
	CHECK(mesh.nodes[2]->get_peer_path(3) == Mesh::PATH_NONE);
	CHECK(mesh.nodes[2]->is_peer_connected(1));
	CHECK(mesh.nodes[3]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_CLOSED);
	CHECK(mesh.nodes[2]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_NONE);
}


// Packets that no engine consumes are kept up to the limit of the queue; the ones beyond it are dropped and counted.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] Packets nothing consumes are dropped past a limit") {
	HostedMesh mesh(1);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());

	// No engine runs on the host: nothing takes its packets.
	const uint8_t payload[3] = { 1, 2, 3 };
	const int extra = 300;
	for (int i = 0; i < TickTransport::MAX_QUEUED_PACKETS + extra; i++) {
		mesh.nodes[2]->send(1, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_RELIABLE, payload, 3);
	}
	for (int t = 0; t < 10000 && int(mesh.nodes[1]->get_stats()["dropped_packets"]) < extra; t++) {
		mesh.poll(1);
	}
	CHECK(int(mesh.nodes[1]->get_stats()["dropped_packets"]) == extra);
	int queued = 0;
	TickTransport::Packet packet;
	while (mesh.nodes[1]->pop_packet(packet)) {
		queued++;
	}
	CHECK(queued == TickTransport::MAX_QUEUED_PACKETS);
}


// The multiplayer peer of each node sends to a player, to everyone or to everyone but one, over direct and relayed
// pairs; its packets and the engines' packets don't mix.
TEST_CASE("[Modules][TickSynchronizer][TickMultiplayerPeer] The multiplayer peer reaches direct and relayed players") {
	HostedMesh mesh(3, 4);
	Ref<TickMultiplayerPeer> peers[5];
	for (int i = 1; i <= 4; i++) {
		peers[i] = mesh.nodes[i]->get_multiplayer_peer();
		// The same peer every time.
		CHECK(peers[i] == mesh.nodes[i]->get_multiplayer_peer());
	}
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	CHECK(peers[1]->is_server());
	CHECK_FALSE(peers[2]->is_server());
	CHECK(peers[3]->get_unique_id() == 3);
	CHECK(peers[3]->get_connection_status() == MultiplayerPeer::CONNECTION_CONNECTED);

	// Player 2 to everyone but player 3, on channel 2, unreliable: the host (direct) and player 4 (relayed).
	const uint8_t payload[4] = { 9, 8, 7, 6 };
	peers[2]->set_target_peer(-3);
	peers[2]->set_transfer_channel(2);
	peers[2]->set_transfer_mode(MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
	CHECK(peers[2]->put_packet(payload, 4) == OK);
	bool got[5] = {};
	for (int t = 0; t < 2000 && !(got[1] && got[4]); t++) {
		for (int i = 1; i <= 4; i++) {
			peers[i]->poll();
			while (peers[i]->get_available_packet_count() > 0) {
				CHECK(peers[i]->get_packet_peer() == 2);
				CHECK(peers[i]->get_packet_channel() == 2);
				CHECK(peers[i]->get_packet_mode() == MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
				const uint8_t *buffer = nullptr;
				int size = 0;
				CHECK(peers[i]->get_packet(&buffer, size) == OK);
				CHECK(size == 4);
				got[i] = true;
			}
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(got[1]);
	CHECK(got[4]);
	CHECK_FALSE(got[3]);

	// The engines' packets don't reach the multiplayer peer, and the other way around.
	TickTransport::Packet packet;
	CHECK_FALSE(mesh.nodes[4]->pop_packet(packet));
	CHECK(mesh.nodes[4]->send(2, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_RELIABLE, payload, 4) == OK);
	CHECK(mesh.receive(2, 4, TICK_CHANNEL_STATE));
	CHECK(peers[2]->get_available_packet_count() == 0);
	CHECK(peers[3]->put_packet(payload, 4) == OK);
}


// Three engines run over a mesh with a relayed pair: the inputs of a player reach the other through the host, and its
// body is a doll there.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] Dolls work across a relayed pair") {
	HostedMesh mesh(2, 3);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	REQUIRE(mesh.nodes[2]->get_peer_path(3) == Mesh::PATH_RELAYED);

	TickSyncCore cores[4];
	TestTickMeshDolls::DollMover *movers[4][4] = {};
	for (int peer = 1; peer <= 3; peer++) {
		for (int controller = 1; controller <= 3; controller++) {
			movers[peer][controller] = memnew(TestTickMeshDolls::DollMover(vformat("mover_%d", controller), controller));
			movers[peer][controller]->script_length = 100000;
			cores[peer].register_object(movers[peer][controller]);
		}
		TickTransport::Event event;
		while (mesh.nodes[peer]->pop_event(event)) {
		}
		REQUIRE(cores[peer].start(mesh.nodes[peer], OS::get_singleton()->get_ticks_usec()) == OK);
	}
	uint64_t last = OS::get_singleton()->get_ticks_usec();
	const uint64_t end = last + 3000000;
	while (OS::get_singleton()->get_ticks_usec() < end) {
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		const double delta = double(now - last) / 1000000.0;
		last = now;
		for (int peer = 1; peer <= 3; peer++) {
			cores[peer].process(delta, now);
		}
		OS::get_singleton()->delay_usec(2000);
	}
	// Player 3's inputs reach player 2 through the host: its mover is a doll there.
	CHECK(cores[2].is_predicting());
	CHECK(cores[2].get_doll_delay(3) >= 0);
	CHECK(cores[3].get_doll_delay(2) >= 0);
	CHECK(cores[2].get_stats().malformed_packets == 0);
	CHECK(int(mesh.nodes[1]->get_stats()["relayed_packets"]) > 60);
	for (int peer = 1; peer <= 3; peer++) {
		cores[peer].stop();
		for (int controller = 1; controller <= 3; controller++) {
			memdelete(movers[peer][controller]);
		}
	}
}

// A key and a self-signed certificate for localhost.
struct TestCertificate {
	Ref<CryptoKey> key;
	Ref<X509Certificate> certificate;

	// Generates the key and the certificate.
	TestCertificate() {
		Ref<Crypto> crypto = Ref<Crypto>(Crypto::create());
		key = crypto->generate_rsa(2048);
		certificate = crypto->generate_self_signed_certificate(key, "CN=localhost,O=TickSynchronizer,C=BR", "20250101000000", "20350101000000");
	}
};

// With DTLS, the host links, a punched direct link (each side pinning the other's certificate) and the relay all carry
// packets, and the payload shrinks by the DTLS overhead.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] DTLS on the host links, the direct links and the relay") {
	TestCertificate host;
	REQUIRE(host.certificate.is_valid());
	// Player 4 is relayed: the pairs with it go through the host's encrypted links.
	HostedMesh mesh(3, 4, TLSOptions::server(host.key, host.certificate), TLSOptions::client(host.certificate));
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	for (int i = 1; i <= 4; i++) {
		CHECK(mesh.nodes[i]->is_encrypted());
	}
	// The DTLS overhead is subtracted from the payload size.
	CHECK(mesh.nodes[2]->get_max_payload_size() < 1350);
	// A punched DTLS link, each side pinning the other's certificate through the host.
	CHECK(mesh.nodes[2]->get_peer_path(3) == Mesh::PATH_DIRECT);
	CHECK(mesh.nodes[3]->get_peer_path(2) == Mesh::PATH_DIRECT);
	CHECK(mesh.nodes[2]->get_peer_path(4) == Mesh::PATH_RELAYED);

	const uint8_t payload[3] = { 5, 6, 7 };
	CHECK(mesh.nodes[3]->send(2, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, payload, 3) == OK);
	CHECK(mesh.receive(2, 3, TICK_CHANNEL_INPUTS));
	CHECK(mesh.nodes[2]->send(4, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, payload, 3) == OK);
	CHECK(mesh.receive(4, 2, TICK_CHANNEL_CONTROL));
	CHECK(mesh.nodes[4]->send(1, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_RELIABLE, payload, 3) == OK);
	CHECK(mesh.receive(1, 4, TICK_CHANNEL_STATE));
	CHECK(int(mesh.nodes[1]->get_stats()["rejected_connections"]) == 0);
}


// A player that doesn't trust the host's certificate never joins.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player that doesn't trust the host's certificate can't join") {
	TestCertificate host;
	TestCertificate other;
	REQUIRE(other.certificate.is_valid());
	ERR_PRINT_OFF;
	HostedMesh mesh(1, 0, TLSOptions::server(host.key, host.certificate), TLSOptions::client(other.certificate));
	mesh.poll(500);
	ERR_PRINT_ON;
	CHECK(mesh.nodes[2]->get_status() != Mesh::STATUS_CONNECTED);
	CHECK(mesh.nodes[1]->get_peers().is_empty());
}


// Waits until every node but the removed ones sees `p_expected` peers.
static bool wait_peer_counts(HostedMesh &r_mesh, const int *p_nodes, int p_node_count, int p_expected, int p_rounds, int p_skip_node) {
	for (int t = 0; t < p_rounds; t++) {
		for (int i = 1; i <= r_mesh.count; i++) {
			if (i != p_skip_node && r_mesh.nodes[i].is_valid()) {
				r_mesh.nodes[i]->poll();
			}
		}
		OS::get_singleton()->delay_usec(1000);
		bool done = true;
		for (int i = 0; i < p_node_count; i++) {
			done = done && int(r_mesh.nodes[p_nodes[i]]->get_peers().size()) == p_expected;
		}
		if (done) {
			return true;
		}
	}
	return false;
}


// When the host hands the mesh over, the first player of the succession hosts, the others follow it keeping their
// direct links, and the engines hear of the new host before the old one's disconnection.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player takes over when the host leaves") {
	HostedMesh mesh(3);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	// Everyone got the same succession: all direct, so by id.
	mesh.poll(20);
	CHECK(mesh.nodes[3]->get_succession() == PackedInt32Array({ 2, 3, 4 }));
	for (int i = 2; i <= 4; i++) {
		TickTransport::Event event;
		while (mesh.nodes[i]->pop_event(event)) {
		}
	}

	CHECK(mesh.nodes[1]->hand_over() == OK);
	const int remaining[3] = { 2, 3, 4 };
	CHECK(wait_peer_counts(mesh, remaining, 3, 2, 3000, 1));
	CHECK(mesh.nodes[2]->is_hosting());
	CHECK(mesh.nodes[3]->get_host_peer() == 2);
	CHECK(mesh.nodes[4]->get_host_peer() == 2);
	CHECK(mesh.nodes[3]->get_peer_path(2) == Mesh::PATH_HOST);
	CHECK(mesh.nodes[3]->get_peer_path(4) == Mesh::PATH_DIRECT);
	CHECK(mesh.nodes[2]->get_peer_path(3) == Mesh::PATH_HOST);
	// The engines are told the new host first, then that the old one left.
	TickTransport::Event event;
	REQUIRE(mesh.nodes[3]->pop_event(event));
	CHECK(event.type == TickTransport::EVENT_HOST_MIGRATED);
	CHECK(event.peer == 2);
	REQUIRE(mesh.nodes[3]->pop_event(event));
	CHECK(event.type == TickTransport::EVENT_PEER_DISCONNECTED);
	CHECK(event.peer == 1);

	const uint8_t payload[2] = { 1, 2 };
	CHECK(mesh.nodes[4]->send(2, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, payload, 2) == OK);
	CHECK(mesh.receive(2, 4, TICK_CHANNEL_CONTROL));
	CHECK(mesh.nodes[2]->send(TickTransport::PEER_BROADCAST, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_RELIABLE, payload, 2) == OK);
	CHECK(mesh.receive(3, 2, TICK_CHANNEL_STATE));
	CHECK(mesh.nodes[3]->send(4, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, payload, 2) == OK);
	CHECK(mesh.receive(4, 3, TICK_CHANNEL_INPUTS));
}

// What the multiplayer peers reported, in order: a migration (`old_host` to `new_host`), or a peer that left (`left`).
struct MultiplayerReport {
	int peer = 0;
	int old_host = 0;
	int new_host = 0;
	int left = 0;
};
static MultiplayerReport multiplayer_reports[16];
static int multiplayer_report_count = 0;

// Records a `host_migrated` signal of a multiplayer peer.
static void report_migration(int p_old_host, int p_new_host, int p_peer) {
	if (multiplayer_report_count < 16) {
		MultiplayerReport &report = multiplayer_reports[multiplayer_report_count++];
		report = MultiplayerReport();
		report.peer = p_peer;
		report.old_host = p_old_host;
		report.new_host = p_new_host;
	}
}


// Records a `peer_disconnected` signal of a multiplayer peer.
static void report_peer_left(int p_left, int p_peer) {
	if (multiplayer_report_count < 16) {
		MultiplayerReport &report = multiplayer_reports[multiplayer_report_count++];
		report = MultiplayerReport();
		report.peer = p_peer;
		report.left = p_left;
	}
}


// Every player's multiplayer peer reports the migration, then that the old host left.
TEST_CASE("[Modules][TickSynchronizer][TickMultiplayerPeer] The multiplayer peer reports the host migration") {
	HostedMesh mesh(3);
	Ref<TickMultiplayerPeer> peers[5];
	for (int i = 2; i <= 4; i++) {
		peers[i] = mesh.nodes[i]->get_multiplayer_peer();
		peers[i]->connect(SNAME("host_migrated"), callable_mp_static(&report_migration).bind(i));
		peers[i]->connect(SNAME("peer_disconnected"), callable_mp_static(&report_peer_left).bind(i));
	}
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	mesh.poll(20);
	for (int i = 2; i <= 4; i++) {
		peers[i]->poll();
	}
	multiplayer_report_count = 0;

	CHECK(mesh.nodes[1]->hand_over() == OK);
	for (int t = 0; t < 3000 && multiplayer_report_count < 6; t++) {
		for (int i = 2; i <= 4; i++) {
			peers[i]->poll();
		}
		OS::get_singleton()->delay_usec(1000);
	}
	// Every player is told the new host, then that the old one left.
	REQUIRE(multiplayer_report_count == 6);
	for (int i = 2; i <= 4; i++) {
		int migrated_at = -1;
		int left_at = -1;
		for (int r = 0; r < multiplayer_report_count; r++) {
			const MultiplayerReport &report = multiplayer_reports[r];
			if (report.peer == i && report.old_host == 1 && report.new_host == 2) {
				migrated_at = r;
			} else if (report.peer == i && report.left == 1) {
				left_at = r;
			}
		}
		CHECK(migrated_at >= 0);
		CHECK(left_at > migrated_at);
	}
	// `SceneMultiplayer` still takes peer 1 for the server; the peer itself knows who hosts.
	CHECK(peers[2]->is_server());
	CHECK_FALSE(peers[3]->is_server());
	CHECK(peers[3]->get_unique_id() == 3);
	multiplayer_report_count = 0;
}


// Hands the mesh over from the host (1) and waits until player 2 hosts and the players `p_first`..`p_last` follow it.
static void hand_over_to_player_2(HostedMesh &r_mesh, int p_first, int p_last) {
	CHECK(r_mesh.nodes[1]->hand_over() == OK);
	bool done = false;
	for (int t = 0; t < 3000 && !done; t++) {
		r_mesh.poll(1);
		done = r_mesh.nodes[2]->is_hosting();
		for (int i = p_first; i <= p_last; i++) {
			done = done && r_mesh.nodes[i]->get_host_peer() == 2;
		}
	}
	REQUIRE(done);
	// The players' rejoin requests, and the ports the new host tells them.
	r_mesh.poll(50);
}


// A player with a takeover port takes new players there once it hosts: a new player gets a fresh id, learns who hosts,
// and punches a direct link with a player that followed.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] New players join the host that took over") {
	HostedMesh mesh(3);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	// Player 4 leaves: its id isn't given to anyone else.
	mesh.nodes[4]->close();
	for (int t = 0; t < 2000 && mesh.nodes[2]->is_peer_connected(4); t++) {
		mesh.poll(1);
	}
	REQUIRE_FALSE(mesh.nodes[2]->is_peer_connected(4));
	mesh.poll(20);

	// The successor takes new players on a port of its own (the host's rendezvous port, free without DTLS).
	const int takeover = mesh.port + 1;
	mesh.nodes[2]->set_takeover_port(takeover);
	hand_over_to_player_2(mesh, 3, 3);

	// A new player joins the new host, where the game told it.
	mesh.nodes[5] = Mesh::create_player("127.0.0.1", takeover);
	REQUIRE(mesh.nodes[5].is_valid());
	mesh.count = 5;
	for (int t = 0; t < 8000 && !(mesh.nodes[5]->get_peer_path(3) == Mesh::PATH_DIRECT && mesh.nodes[3]->get_peer_path(5) == Mesh::PATH_DIRECT); t++) {
		mesh.poll(1);
	}
	CHECK(mesh.nodes[5]->get_local_peer_id() == 5);
	// The welcome says who hosts; for the engines, it's as if the host had migrated before this player joined.
	CHECK(mesh.nodes[5]->get_host_peer() == 2);
	CHECK(mesh.nodes[5]->get_peers() == PackedInt32Array({ 2, 3 }));
	bool migrated = false;
	TickTransport::Event event;
	while (mesh.nodes[5]->pop_event(event)) {
		migrated = migrated || (event.type == TickTransport::EVENT_HOST_MIGRATED && event.peer == 2);
	}
	CHECK(migrated);
	CHECK(mesh.nodes[2]->get_peers() == PackedInt32Array({ 3, 5 }));
	// The pair with the player that followed was introduced and punched through the new host's port.
	CHECK(mesh.nodes[3]->get_peer_path(5) == Mesh::PATH_DIRECT);
	CHECK(mesh.nodes[5]->get_peer_path(3) == Mesh::PATH_DIRECT);
}


// The same with DTLS: the successor takes new players with its own certificate, and the pairs register on the port
// after its takeover port.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] New players join the host that took over, with DTLS") {
	TestCertificate host;
	TestCertificate successor;
	REQUIRE(successor.certificate.is_valid());
	HostedMesh mesh(2, 0, TLSOptions::server(host.key, host.certificate), TLSOptions::client(host.certificate));
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	mesh.poll(20);

	// The successor's port for new players and its certificate; the pairs register on the next port.
	const int takeover = mesh.port + 2;
	mesh.nodes[2]->set_takeover_port(takeover);
	mesh.nodes[2]->set_takeover_tls_options(TLSOptions::server(successor.key, successor.certificate));
	hand_over_to_player_2(mesh, 3, 3);

	mesh.nodes[4] = Mesh::create_player("127.0.0.1", takeover, Mesh::COMPRESSION_RANGE_CODER, TLSOptions::client(successor.certificate), "localhost");
	REQUIRE(mesh.nodes[4].is_valid());
	mesh.count = 4;
	for (int t = 0; t < 10000 && !(mesh.nodes[4]->get_peer_path(3) == Mesh::PATH_DIRECT && mesh.nodes[3]->get_peer_path(4) == Mesh::PATH_DIRECT); t++) {
		mesh.poll(1);
	}
	CHECK(mesh.nodes[4]->is_encrypted());
	CHECK(mesh.nodes[4]->get_local_peer_id() == 4);
	CHECK(mesh.nodes[2]->get_peers() == PackedInt32Array({ 3, 4 }));
	// Player 3 sent its certificate again to the new host, which handed it to the new player.
	CHECK(mesh.nodes[3]->get_peer_path(4) == Mesh::PATH_DIRECT);
	CHECK(mesh.nodes[4]->get_peer_path(3) == Mesh::PATH_DIRECT);
}


// When the host stops answering, the players time out and migrate to the successor; a player that was only relayed
// can't reach it and leaves the mesh.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A host that stops answering is replaced; relayed players are lost") {
	// Player 4 is only relayed: it can't reach the successor.
	HostedMesh mesh(3, 4);
	for (int i = 2; i <= 4; i++) {
		mesh.nodes[i]->set_host_timeout(1.0);
	}
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	mesh.poll(20);
	// The relayed player comes last.
	CHECK(mesh.nodes[2]->get_succession() == PackedInt32Array({ 2, 3, 4 }));

	// The host stops answering (a crash): the players time out and migrate.
	const int remaining[2] = { 2, 3 };
	CHECK(wait_peer_counts(mesh, remaining, 2, 1, 6000, 1));
	CHECK(mesh.nodes[2]->is_hosting());
	CHECK(mesh.nodes[3]->get_host_peer() == 2);
	for (int t = 0; t < 1000 && mesh.nodes[4]->get_status() != Mesh::STATUS_DISCONNECTED; t++) {
		mesh.nodes[4]->poll();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(mesh.nodes[4]->get_status() == Mesh::STATUS_DISCONNECTED);
	CHECK(mesh.nodes[4]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_LOST);
	CHECK_FALSE(mesh.nodes[2]->is_peer_connected(4));
}


// The authority of `TickSyncCore` migrates with the host: the new host simulates the old one's objects, owns its
// spawns, and the other players join it again and predict.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] The authority migrates with the host") {
	HostedMesh mesh(3);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());

	// Every node has the host's NPC (controller 1) and the three players' movers.
	TickSyncCore cores[5];
	TestTickSyncCore::TestListener listeners[5];
	TestMover *movers[5][5] = {};
	for (int peer = 1; peer <= 4; peer++) {
		for (int controller = 1; controller <= 4; controller++) {
			TestMover *mover = memnew(TestMover(controller == 1 ? String("npc") : vformat("mover_%d", controller), controller, TickCodec::PRECISION_SINGLE));
			mover->constant_direction = true;
			mover->direction_override = 1;
			movers[peer][controller] = mover;
			cores[peer].register_object(mover);
		}
		TickTransport::Event event;
		while (mesh.nodes[peer]->pop_event(event)) {
		}
		cores[peer].set_listener(&listeners[peer]);
		REQUIRE(cores[peer].start(mesh.nodes[peer], OS::get_singleton()->get_ticks_usec()) == OK);
	}
	uint64_t last = OS::get_singleton()->get_ticks_usec();
	int stopped = 0;
	uint32_t crate = 0;
	const uint64_t durations[4] = { 2500000, 500000, 2500000, 500000 };
	for (int phase = 0; phase < 4; phase++) {
		const uint64_t end = last + durations[phase];
		while (OS::get_singleton()->get_ticks_usec() < end) {
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			const double delta = double(now - last) / 1000000.0;
			last = now;
			for (int peer = 1; peer <= 4; peer++) {
				if (peer != stopped) {
					cores[peer].process(delta, now);
				}
			}
			OS::get_singleton()->delay_usec(2000);
		}
		if (phase == 0) {
			REQUIRE(cores[3].is_predicting());
			crate = cores[1].spawn("Spawner", 0, "Crate", 1, Variant());
			REQUIRE(crate != 0);
			CHECK(cores[1].owns_spawn(crate));
		} else if (phase == 1) {
			CHECK(listeners[3].spawns == 1);
			CHECK_FALSE(cores[3].owns_spawn(crate));
			// The host leaves, handing the mesh over.
			cores[1].stop();
			mesh.nodes[1]->hand_over();
			stopped = 1;
		} else if (phase == 2) {
			// The new authority took over the old one's spawns: removing one reaches the others.
			CHECK(cores[2].owns_spawn(crate));
			CHECK_FALSE(cores[3].owns_spawn(crate));
			cores[2].despawn(crate);
			CHECK_FALSE(cores[2].owns_spawn(crate));
		}
	}
	for (int peer = 3; peer <= 4; peer++) {
		CHECK(listeners[peer].despawns == 1);
		CHECK(listeners[peer].last_despawn == crate);
	}

	// Player 2 is the authority now; the others joined it again and predict.
	CHECK(cores[2].is_server());
	CHECK(listeners[3].ready_peers >= 2);
	for (int peer = 3; peer <= 4; peer++) {
		CHECK(cores[peer].get_settings().authority_peer == 2);
		CHECK(cores[peer].is_welcomed());
		CHECK(cores[peer].is_predicting());
		CHECK(cores[peer].get_stats().malformed_packets == 0);
	}
	// The old host's NPC is simulated by the new authority and interpolated by the others; the players' inputs reach
	// the new authority.
	const real_t npc = movers[2][1]->position.x;
	const real_t mover_3 = movers[2][3]->position.x;
	CHECK(npc > 20.0);
	CHECK(Math::abs(movers[3][1]->position.x - npc) < 1.5);
	CHECK(mover_3 > 20.0);
	CHECK(Math::abs(movers[3][3]->position.x - mover_3) < 1.5);

	for (int peer = 1; peer <= 4; peer++) {
		cores[peer].stop();
		for (int controller = 1; controller <= 4; controller++) {
			memdelete(movers[peer][controller]);
		}
	}
}


// A player that joins the host that took over is welcomed by the new authority, predicts, and has its inputs simulated
// there.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player that joins after a migration plays with the new host") {
	HostedMesh mesh(2);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	mesh.nodes[2]->set_takeover_port(mesh.port + 1);

	// Every node has the host's NPC (controller 1) and the movers of players 2, 3 and 4 (who joins later).
	TickSyncCore cores[5];
	TestTickSyncCore::TestListener listeners[5];
	TestMover *movers[5][5] = {};
	for (int peer = 1; peer <= 4; peer++) {
		for (int controller = 1; controller <= 4; controller++) {
			TestMover *mover = memnew(TestMover(controller == 1 ? String("npc") : vformat("mover_%d", controller), controller, TickCodec::PRECISION_SINGLE));
			mover->constant_direction = true;
			mover->direction_override = 1;
			movers[peer][controller] = mover;
			cores[peer].register_object(mover);
		}
		cores[peer].set_listener(&listeners[peer]);
	}
	for (int peer = 1; peer <= 3; peer++) {
		TickTransport::Event event;
		while (mesh.nodes[peer]->pop_event(event)) {
		}
		REQUIRE(cores[peer].start(mesh.nodes[peer], OS::get_singleton()->get_ticks_usec()) == OK);
	}
	uint64_t last = OS::get_singleton()->get_ticks_usec();
	bool running[5] = { false, true, true, true, false };
	const uint64_t durations[3] = { 2500000, 2500000, 4000000 };
	for (int phase = 0; phase < 3; phase++) {
		const uint64_t end = last + durations[phase];
		while (OS::get_singleton()->get_ticks_usec() < end) {
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			const double delta = double(now - last) / 1000000.0;
			last = now;
			for (int peer = 1; peer <= 4; peer++) {
				if (running[peer]) {
					cores[peer].process(delta, now);
				} else if (mesh.nodes[peer].is_valid()) {
					mesh.nodes[peer]->poll();
				}
			}
			OS::get_singleton()->delay_usec(2000);
		}
		if (phase == 0) {
			// The host leaves, handing the mesh over to player 2.
			cores[1].stop();
			running[1] = false;
			mesh.nodes[1]->hand_over();
		} else if (phase == 1) {
			REQUIRE(cores[2].is_server());
			// A new player joins player 2, which took over; its network starts before it's even connected.
			mesh.nodes[4] = Mesh::create_player("127.0.0.1", mesh.port + 1);
			REQUIRE(mesh.nodes[4].is_valid());
			mesh.count = 4;
			REQUIRE(cores[4].start(mesh.nodes[4], OS::get_singleton()->get_ticks_usec()) == OK);
			running[4] = true;
		}
	}

	// The new player's authority is the new host, which welcomed it, and its inputs are simulated there.
	CHECK(mesh.nodes[4]->get_host_peer() == 2);
	CHECK(cores[4].get_settings().authority_peer == 2);
	CHECK(cores[4].is_welcomed());
	CHECK(cores[4].is_predicting());
	CHECK(cores[4].get_stats().malformed_packets == 0);
	CHECK(movers[2][4]->position.x > 10.0);
	CHECK(Math::abs(movers[4][1]->position.x - movers[2][1]->position.x) < 1.5);

	for (int peer = 1; peer <= 4; peer++) {
		cores[peer].stop();
		for (int controller = 1; controller <= 4; controller++) {
			memdelete(movers[peer][controller]);
		}
	}
}


// A player the host removes leaves with `DISCONNECT_REASON_REFUSED`, without taking over; the others stay with the
// host.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player the host removes leaves without migrating") {
	HostedMesh mesh(3);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	mesh.poll(20);

	mesh.nodes[1]->disconnect_peer(3);
	const int remaining[3] = { 1, 2, 4 };
	CHECK(wait_peer_counts(mesh, remaining, 3, 2, 3000, 0));
	for (int t = 0; t < 1000 && mesh.nodes[3]->get_status() != Mesh::STATUS_DISCONNECTED; t++) {
		mesh.poll(1);
	}
	CHECK(mesh.nodes[3]->get_status() == Mesh::STATUS_DISCONNECTED);
	CHECK(mesh.nodes[3]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_REFUSED);
	CHECK_FALSE(mesh.nodes[3]->is_hosting());
	CHECK(mesh.nodes[2]->get_host_peer() == 1);
	CHECK(mesh.nodes[4]->get_host_peer() == 1);
}


// A player the authority refuses (another protocol version) is removed by the host and doesn't take over.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player refused by the authority doesn't take over") {
	HostedMesh mesh(2);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	TickSyncCore host;
	TickTransport::Event event;
	while (mesh.nodes[1]->pop_event(event)) {
	}
	REQUIRE(host.start(mesh.nodes[1], OS::get_singleton()->get_ticks_usec()) == OK);

	// Player 3 speaks another protocol version: the authority refuses it and removes it a second later.
	TickDataBuffer hello;
	hello.begin_write();
	hello.add_uint_bits(TICK_MESSAGE_HELLO, 8);
	hello.add_uint_bits(999, 16);
	hello.add_bool(sizeof(real_t) == sizeof(double));
	hello.dry();
	REQUIRE(mesh.nodes[3]->send(1, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, hello.get_buffer().get_bytes().ptr(), int(hello.get_buffer().get_bytes().size())) == OK);
	uint64_t last = OS::get_singleton()->get_ticks_usec();
	for (int t = 0; t < 3000 && mesh.nodes[3]->get_status() != Mesh::STATUS_DISCONNECTED; t++) {
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		host.process(double(now - last) / 1000000.0, now);
		last = now;
		mesh.nodes[2]->poll();
		mesh.nodes[3]->poll();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(mesh.nodes[3]->get_status() == Mesh::STATUS_DISCONNECTED);
	CHECK_FALSE(mesh.nodes[3]->is_hosting());
	CHECK(mesh.nodes[2]->get_host_peer() == 1);
	host.stop();
}


// Drops the events waiting at `p_node`; returns whether one of them was a host migration.
static bool drain_events(const Ref<Mesh> &p_node) {
	bool migrated = false;
	TickTransport::Event event;
	while (p_node->pop_event(event)) {
		migrated = migrated || event.type == TickTransport::EVENT_HOST_MIGRATED;
	}
	return migrated;
}


// When the host closes, the mesh ends for every player: no migration, and `DISCONNECT_REASON_ENDED`.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A host that closes ends the mesh for everyone") {
	HostedMesh mesh(3);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	mesh.poll(20);
	for (int i = 2; i <= 4; i++) {
		drain_events(mesh.nodes[i]);
	}

	mesh.nodes[1]->close();
	bool all_left = false;
	for (int t = 0; t < 3000 && !all_left; t++) {
		mesh.poll(1);
		all_left = true;
		for (int i = 2; i <= 4; i++) {
			all_left = all_left && mesh.nodes[i]->get_status() == Mesh::STATUS_DISCONNECTED;
		}
	}
	CHECK(all_left);
	CHECK(mesh.nodes[1]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_CLOSED);
	for (int i = 2; i <= 4; i++) {
		CHECK_FALSE(mesh.nodes[i]->is_hosting());
		CHECK_FALSE(drain_events(mesh.nodes[i]));
		CHECK(mesh.nodes[i]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_ENDED);
	}
}

// The player `check_join()` left waiting for the game's decision.
static int waiting_player = 0;
// The join validator of the tests: admits the players that send "secret" from localhost, leaves the ones that send
// "later" waiting, and refuses the others.
static Variant check_join(int p_peer, const PackedByteArray &p_data, const String &p_address) {
	const String password = String::utf8((const char *)p_data.ptr(), p_data.size());
	if (password == "later") {
		waiting_player = p_peer;
		return Variant();
	}
	return password == "secret" && p_address == "127.0.0.1";
}


// Joins the mesh as the next node, with `p_join_data`, and waits until it's connected or refused.
static void join(HostedMesh &r_mesh, const String &p_join_data) {
	r_mesh.count++;
	r_mesh.nodes[r_mesh.count] = Mesh::create_player("127.0.0.1", r_mesh.port, Mesh::COMPRESSION_RANGE_CODER, Ref<TLSOptions>(), String(), p_join_data.to_utf8_buffer());
	REQUIRE(r_mesh.nodes[r_mesh.count].is_valid());
	for (int t = 0; t < 3000 && r_mesh.nodes[r_mesh.count]->get_status() == Mesh::STATUS_CONNECTING && p_join_data != "later"; t++) {
		r_mesh.poll(1);
	}
}


// The host admits the players the validator accepts; a refused one never gets an id and nobody hears of it; one left
// waiting joins when the game admits it.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] The host admits only the players the game accepts") {
	HostedMesh mesh(0);
	mesh.nodes[1]->set_join_validator(callable_mp_static(&check_join));

	join(mesh, "secret");
	CHECK(mesh.nodes[2]->get_status() == Mesh::STATUS_CONNECTED);
	CHECK(mesh.nodes[2]->get_local_peer_id() == 2);

	// Refused: it never gets an id, and nobody learns about it.
	join(mesh, "wrong");
	CHECK(mesh.nodes[3]->get_status() == Mesh::STATUS_DISCONNECTED);
	CHECK(mesh.nodes[3]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_REFUSED);
	CHECK(mesh.nodes[3]->get_local_peer_id() == 0);
	CHECK(mesh.nodes[1]->get_peers() == PackedInt32Array({ 2 }));
	CHECK(mesh.nodes[2]->get_peers() == PackedInt32Array({ 1 }));
	CHECK(int(mesh.nodes[1]->get_stats()["rejected_connections"]) == 1);

	// The game decides later.
	join(mesh, "later");
	mesh.poll(200);
	CHECK(mesh.nodes[4]->get_status() == Mesh::STATUS_CONNECTING);
	CHECK(mesh.nodes[1]->get_peers().size() == 1);
	REQUIRE(waiting_player != 0);
	CHECK(mesh.nodes[1]->admit_player(waiting_player) == OK);
	for (int t = 0; t < 3000 && !mesh.nodes[2]->is_peer_connected(waiting_player); t++) {
		mesh.poll(1);
	}
	CHECK(mesh.nodes[4]->get_local_peer_id() == waiting_player);
	CHECK(mesh.nodes[2]->is_peer_connected(waiting_player));
	CHECK(mesh.nodes[4]->is_peer_connected(2));
	waiting_player = 0;
}


// Five joins at once from one address are admitted; the sixth is refused right away, and learns that the address is
// busy.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] Joins from one address are limited") {
	HostedMesh mesh(0);
	// Five joins at once from the same address are fine; the sixth is refused right away.
	Ref<Mesh> players[6];
	for (int i = 0; i < 6; i++) {
		players[i] = Mesh::create_player("127.0.0.1", mesh.port);
		REQUIRE(players[i].is_valid());
	}
	for (int t = 0; t < 3000 && int(mesh.nodes[1]->get_peers().size()) + int(mesh.nodes[1]->get_stats()["rejected_connections"]) < 6; t++) {
		mesh.poll(1);
		for (int i = 0; i < 6; i++) {
			players[i]->poll();
		}
	}
	CHECK(mesh.nodes[1]->get_peers().size() == 5);
	CHECK(int(mesh.nodes[1]->get_stats()["rejected_connections"]) == 1);
	// The refused player learns why.
	int busy = 0;
	for (int t = 0; t < 2000 && busy == 0; t++) {
		mesh.poll(1);
		for (int i = 0; i < 6; i++) {
			players[i]->poll();
			busy += players[i]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_BUSY ? 1 : 0;
		}
	}
	CHECK(busy == 1);
	for (int i = 0; i < 6; i++) {
		players[i]->close();
	}
	mesh.poll(20);
}


// A bare ENet socket on localhost, to play a host the protocol tests control (`ENetConnection` through the class
// database: the tests don't include ENet's headers). Uses the players' compression (range coder).
static Ref<RefCounted> bare_host(int &r_port) {
	Ref<RefCounted> host = Object::cast_to<RefCounted>(ClassDB::instantiate("ENetConnection"));
	r_port = 0;
	for (int attempt = 0; host.is_valid() && attempt < 10 && r_port == 0; attempt++) {
		const int candidate = 38000 + int((OS::get_singleton()->get_ticks_usec() + uint64_t(attempt) * 7919) % 4000);
		if (int(host->call("create_host_bound", "127.0.0.1", candidate, 4, 27, 0, 0)) == OK) {
			r_port = candidate;
		}
	}
	if (r_port != 0) {
		host->call("compress", 1);
	}
	return host;
}


// A player refused because the mesh is full, or because its protocol version isn't the host's, learns why, in both
// directions (an older player, a newer host).
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] Refused players learn why: a full mesh, another version of the protocol") {
	// A mesh of two: the host and one player (on a random port, tried again if another program uses it).
	int port = 0;
	Ref<Mesh> host;
	const bool printing = CoreGlobals::print_error_enabled;
	CoreGlobals::print_error_enabled = false;
	for (int attempt = 0; attempt < 10 && host.is_null(); attempt++) {
		port = 36000 + 2 * int((OS::get_singleton()->get_ticks_usec() + uint64_t(attempt) * 7919) % 1000);
		host = Mesh::create_host(port, 2);
	}
	CoreGlobals::print_error_enabled = printing;
	REQUIRE(host.is_valid());
	Ref<Mesh> first = Mesh::create_player("127.0.0.1", port);
	REQUIRE(first.is_valid());
	for (int t = 0; t < 3000 && first->get_status() == Mesh::STATUS_CONNECTING; t++) {
		host->poll();
		first->poll();
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(first->get_status() == Mesh::STATUS_CONNECTED);
	Ref<Mesh> second = Mesh::create_player("127.0.0.1", port);
	REQUIRE(second.is_valid());
	for (int t = 0; t < 3000 && second->get_status() == Mesh::STATUS_CONNECTING; t++) {
		host->poll();
		first->poll();
		second->poll();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(second->get_status() == Mesh::STATUS_DISCONNECTED);
	CHECK(second->get_disconnect_reason() == Mesh::DISCONNECT_REASON_FULL);
	CHECK(host->get_peers() == PackedInt32Array({ 2 }));

	// A player of version 1 ("TKM1", on a bare socket) learns the host's version ("TKV2"), full mesh or not.
	Ref<RefCounted> old_player = Object::cast_to<RefCounted>(ClassDB::instantiate("ENetConnection"));
	REQUIRE(old_player.is_valid());
	REQUIRE(int(old_player->call("create_host", 1, 27, 0, 0)) == OK);
	old_player->call("compress", 1);
	old_player->call("connect_to_host", "127.0.0.1", port, 27, 0x544B4D31);
	int refusal = 0;
	for (int t = 0; t < 2000 && refusal == 0; t++) {
		host->poll();
		const Array event = old_player->call("service", 0);
		if (int(event[0]) == 2) {
			refusal = int(event[2]);
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(refusal == 0x544B5632);
	old_player->call("destroy");

	// A host of version 3 refuses this player the same way: the player tells why.
	int future_port = 0;
	Ref<RefCounted> future_host = bare_host(future_port);
	REQUIRE(future_port != 0);
	Ref<Mesh> player = Mesh::create_player("127.0.0.1", future_port);
	REQUIRE(player.is_valid());
	ERR_PRINT_OFF;
	for (int t = 0; t < 2000 && player->get_status() == Mesh::STATUS_CONNECTING; t++) {
		player->poll();
		const Array event = future_host->call("service", 0);
		if (int(event[0]) == 1) {
			Object *link = event[1];
			// Not `peer_disconnect_now()`: this socket goes on being serviced (see `enet_close_link()`).
			link->call("peer_disconnect", 0x544B5633);
		}
		OS::get_singleton()->delay_usec(1000);
	}
	ERR_PRINT_ON;
	CHECK(player->get_status() == Mesh::STATUS_DISCONNECTED);
	CHECK(player->get_disconnect_reason() == Mesh::DISCONNECT_REASON_VERSION);
	future_host->call("destroy");
	first->close();
	host->poll();
	host->close();
}


// A player that hears nobody for a while finds the host gone for it alone: the other player still has it, so it leaves
// instead of taking over.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player cut off from the host leaves instead of taking over") {
	HostedMesh mesh(2);
	for (int i = 1; i <= 3; i++) {
		mesh.nodes[i]->set_host_timeout(1.0);
	}
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	mesh.poll(20);
	REQUIRE(mesh.nodes[3]->get_succession() == PackedInt32Array({ 2, 3 }));
	drain_events(mesh.nodes[2]);

	// Player 2, the first of the succession, hears nobody for a while (a phone in a tunnel), long enough for the host
	// to drop it.
	const uint64_t end = OS::get_singleton()->get_ticks_usec() + 3000000;
	while (OS::get_singleton()->get_ticks_usec() < end) {
		mesh.nodes[1]->poll();
		mesh.nodes[3]->poll();
		OS::get_singleton()->delay_usec(1000);
	}
	// Back, it finds the host gone for it alone: player 3 still has it.
	for (int t = 0; t < 4000 && mesh.nodes[2]->get_status() != Mesh::STATUS_DISCONNECTED; t++) {
		mesh.poll(1);
	}
	CHECK(mesh.nodes[2]->get_status() == Mesh::STATUS_DISCONNECTED);
	CHECK(mesh.nodes[2]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_LOST);
	CHECK_FALSE(mesh.nodes[2]->is_hosting());
	CHECK_FALSE(drain_events(mesh.nodes[2]));
	CHECK(mesh.nodes[3]->get_host_peer() == 1);
	CHECK(mesh.nodes[3]->is_peer_connected(1));
	CHECK(mesh.nodes[1]->is_peer_connected(3));
}


// A player that stops answering is reported as disconnected once to each of the others, and never as connected again
// through a relay.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player that vanishes leaves once for the others") {
	HostedMesh mesh(3);
	// The players' direct links time out before the host's link, as seen over the internet.
	mesh.nodes[1]->set_host_timeout(1.5);
	for (int i = 2; i <= 4; i++) {
		mesh.nodes[i]->set_host_timeout(1.0);
	}
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	mesh.poll(20);
	REQUIRE(mesh.nodes[2]->get_peer_path(4) == Mesh::PATH_DIRECT);
	REQUIRE(mesh.nodes[3]->get_peer_path(4) == Mesh::PATH_DIRECT);
	for (int i = 1; i <= 3; i++) {
		drain_events(mesh.nodes[i]);
	}

	// Player 4 stops answering (it isn't polled anymore): the others don't ask for a relay before the host says it left.
	int connected[4] = {};
	int disconnected[4] = {};
	const uint64_t end = OS::get_singleton()->get_ticks_usec() + 4000000;
	while (OS::get_singleton()->get_ticks_usec() < end) {
		for (int i = 1; i <= 3; i++) {
			mesh.nodes[i]->poll();
			TickTransport::Event event;
			while (mesh.nodes[i]->pop_event(event)) {
				if (event.peer == 4) {
					connected[i] += event.type == TickTransport::EVENT_PEER_CONNECTED ? 1 : 0;
					disconnected[i] += event.type == TickTransport::EVENT_PEER_DISCONNECTED ? 1 : 0;
				}
			}
		}
		OS::get_singleton()->delay_usec(1000);
	}
	for (int i = 1; i <= 3; i++) {
		CHECK(disconnected[i] == 1);
		CHECK(connected[i] == 0);
		CHECK_FALSE(mesh.nodes[i]->is_peer_connected(4));
	}
	// A direct link that dropped isn't a failed punch.
	CHECK(int(mesh.nodes[2]->get_stats()["failed_punches"]) == 0);
	CHECK(int(mesh.nodes[3]->get_stats()["failed_punches"]) == 0);
}


// Players that notice the frozen host first follow the successor before it notices; once it hosts, it takes them in,
// and their relayed pair comes back through it.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] Players that follow the successor before it notices the host is gone") {
	// Players 2 and 3 link directly; then player 3 refuses direct links, so 3-4 is relayed while 2-4 is direct.
	HostedMesh mesh(2);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	REQUIRE(mesh.nodes[2]->get_peer_path(3) == Mesh::PATH_DIRECT);
	mesh.nodes[3]->set_direct_connections(false);
	join(mesh, String());
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	REQUIRE(mesh.nodes[3]->get_peer_path(4) == Mesh::PATH_RELAYED);
	REQUIRE(mesh.nodes[2]->get_peer_path(4) == Mesh::PATH_DIRECT);
	mesh.poll(20);
	REQUIRE(mesh.nodes[3]->get_succession() == PackedInt32Array({ 2, 3, 4 }));

	// The host freezes; players 3 and 4 notice first and follow player 2 before it notices.
	mesh.nodes[2]->set_host_timeout(1.6);
	mesh.nodes[3]->set_host_timeout(1.0);
	mesh.nodes[4]->set_host_timeout(1.0);
	bool done = false;
	for (int t = 0; t < 8000 && !done; t++) {
		for (int i = 2; i <= 4; i++) {
			mesh.nodes[i]->poll();
		}
		OS::get_singleton()->delay_usec(1000);
		done = mesh.nodes[2]->is_hosting() && mesh.nodes[3]->get_host_peer() == 2 && mesh.nodes[4]->get_host_peer() == 2 && mesh.nodes[3]->is_peer_connected(4) && mesh.nodes[4]->is_peer_connected(3);
	}
	CHECK(mesh.nodes[2]->is_hosting());
	CHECK(mesh.nodes[3]->get_host_peer() == 2);
	CHECK(mesh.nodes[4]->get_host_peer() == 2);
	// Their pair comes back, relayed by the new host.
	CHECK(mesh.nodes[3]->get_peer_path(4) == Mesh::PATH_RELAYED);
	CHECK(mesh.nodes[4]->get_peer_path(3) == Mesh::PATH_RELAYED);
	const uint8_t payload[2] = { 4, 2 };
	CHECK(mesh.nodes[3]->send(4, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, payload, 2) == OK);
	bool received = false;
	for (int t = 0; t < 2000 && !received; t++) {
		for (int i = 2; i <= 4; i++) {
			mesh.nodes[i]->poll();
		}
		OS::get_singleton()->delay_usec(1000);
		TickTransport::Packet packet;
		while (mesh.nodes[4]->pop_packet(packet)) {
			received = received || (packet.from_peer == 3 && packet.channel == TICK_CHANNEL_CONTROL);
		}
	}
	CHECK(received);
}


// Sends a control message of the hosted mesh protocol: a type byte, then little-endian fields.
static void send_control(const Variant &p_link, int p_type, const LocalVector<uint32_t> &p_fields) {
	PackedByteArray message;
	message.push_back(uint8_t(p_type));
	for (const uint32_t field : p_fields) {
		for (int i = 0; i < 4; i++) {
			message.push_back(uint8_t(field >> (8 * i)));
		}
	}
	Object *link = p_link;
	// Null after a failed `REQUIRE`, which doesn't stop the test case (the engine builds without exceptions).
	if (link) {
		link->call("send", 0, message, 1);
	}
}


// A player introduced by a fake host to more players than the mesh can have keeps only as many pairs as the mesh has
// other players.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player keeps no more pairs than the mesh can have") {
	// A fake host welcomes a player into a mesh of 4 players, then introduces it to 50 others.
	int port = 0;
	Ref<RefCounted> host = bare_host(port);
	REQUIRE(port != 0);
	Ref<Mesh> player = Mesh::create_player("127.0.0.1", port);
	REQUIRE(player.is_valid());
	Variant link;
	bool joined = false;
	for (int t = 0; t < 2000 && !joined; t++) {
		player->poll();
		const Array event = host->call("service", 0);
		if (int(event[0]) == 1) {
			link = event[1];
		} else if (int(event[0]) == 3) {
			// The join data: welcomed as player 2 of a mesh of 4.
			joined = true;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(joined);
	LocalVector<uint32_t> welcome;
	welcome.push_back(2);
	welcome.push_back(0);
	welcome.push_back(4);
	send_control(link, 1, welcome);
	for (uint32_t i = 0; i < 50; i++) {
		LocalVector<uint32_t> open;
		open.push_back(10 + i);
		open.push_back(1000 + i);
		send_control(link, 2, open);
		LocalVector<uint32_t> relay;
		relay.push_back(100 + i);
		send_control(link, 4, relay);
	}
	for (int t = 0; t < 300; t++) {
		player->poll();
		host->call("service", 0);
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(player->get_status() == Mesh::STATUS_CONNECTED);
	int pairs = 0;
	for (int id = 10; id < 150; id++) {
		pairs += player->get_peer_path(id) != Mesh::PATH_NONE ? 1 : 0;
	}
	CHECK(pairs == 2);
	player->close();
	host->call("destroy");
}


// A connection holds a place of the mesh only once its join data arrived: until then it's one of two its address may
// have, for two seconds. Before, three connections that said nothing filled a mesh of four for as long as they kept
// coming back.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] Connections that never ask to join don't fill the mesh") {
	// A mesh of four: the host and three places.
	Ref<Mesh> host;
	int port = 0;
	const bool printing = CoreGlobals::print_error_enabled;
	CoreGlobals::print_error_enabled = false;
	for (int attempt = 0; attempt < 10 && host.is_null(); attempt++) {
		port = 43000 + 2 * int((OS::get_singleton()->get_ticks_usec() + uint64_t(attempt) * 7919) % 5000);
		host = Mesh::create_host(port, 4);
	}
	CoreGlobals::print_error_enabled = printing;
	REQUIRE(host.is_valid());

	// From another address of this machine, three bare connections that never send their join data.
	TestTickFuzz::RogueSocket idle[3];
	bool bound = true;
	for (int i = 0; i < 3; i++) {
		bound = bound && idle[i].open("127.0.0.2", "127.0.0.1", port, 0x544B4D32, 27);
	}
	if (!bound) {
		MESSAGE("Can't bind a socket to 127.0.0.2 on this system: not tested.");
		host->close();
		return;
	}
	for (int t = 0; t < 500; t++) {
		host->poll();
		for (int i = 0; i < 3; i++) {
			idle[i].poll();
		}
		OS::get_singleton()->delay_usec(1000);
	}
	// Two of them wait; the third was told its address is busy ("TKBZ").
	int waiting = 0;
	int busy = 0;
	for (int i = 0; i < 3; i++) {
		waiting += idle[i].connected && !idle[i].disconnected ? 1 : 0;
		busy += idle[i].disconnected && idle[i].disconnect_data == 0x544B425A ? 1 : 0;
	}
	CHECK(waiting == 2);
	CHECK(busy == 1);

	// The three places are still there for players.
	Ref<Mesh> players[3];
	for (int i = 0; i < 3; i++) {
		players[i] = Mesh::create_player("127.0.0.1", port);
		REQUIRE(players[i].is_valid());
	}
	int joined = 0;
	for (int t = 0; t < 3000 && joined < 3; t++) {
		host->poll();
		joined = 0;
		for (int i = 0; i < 3; i++) {
			players[i]->poll();
			idle[i].poll();
			joined += players[i]->get_status() == Mesh::STATUS_CONNECTED ? 1 : 0;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(joined == 3);
	CHECK(host->get_peers().size() == 3);

	// The connections that said nothing are dropped two seconds after they came ("TKRX").
	int dropped = 0;
	for (int t = 0; t < 4000 && dropped < 2; t++) {
		host->poll();
		dropped = 0;
		for (int i = 0; i < 3; i++) {
			players[i]->poll();
			idle[i].poll();
			dropped += idle[i].disconnected && idle[i].disconnect_data == 0x544B5258 ? 1 : 0;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(dropped == 2);
	for (int i = 0; i < 3; i++) {
		idle[i].close();
		players[i]->close();
	}
	for (int t = 0; t < 50; t++) {
		host->poll();
		OS::get_singleton()->delay_usec(1000);
	}
	host->close();
}


// The relay spends the host's bandwidth on what a player sends to another. Beyond the budget of the sender,
// unreliable packets are dropped; a reliable one can't just go missing, so the sender is removed.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] The relay takes so many bytes per second from a player") {
	// Player 3 never tries direct links: the pair 2-3 is relayed.
	HostedMesh mesh(2, 3);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	REQUIRE(mesh.nodes[2]->get_peer_path(3) == Mesh::PATH_RELAYED);
	// 10 000 bytes per second, 20 000 at once.
	mesh.nodes[1]->set_relay_rate_limit(10000);
	CHECK(mesh.nodes[1]->get_relay_rate_limit() == 10000);

	uint8_t payload[1000] = {};
	for (int i = 0; i < 100; i++) {
		CHECK(mesh.nodes[2]->send(3, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, payload, 1000) == OK);
	}
	int received = 0;
	for (int t = 0; t < 500; t++) {
		mesh.poll(1);
		TickTransport::Packet packet;
		while (mesh.nodes[3]->pop_packet(packet)) {
			received += packet.from_peer == 2 ? 1 : 0;
		}
	}
	const int dropped = int(mesh.nodes[1]->get_stats()["relay_dropped_packets"]);
	CHECK(received >= 15);
	CHECK(received <= 40);
	CHECK(dropped >= 60);
	CHECK(received + dropped == 100);
	CHECK(mesh.nodes[2]->get_status() == Mesh::STATUS_CONNECTED);

	// Reliable packets beyond the budget: the player that sends them is removed, and the other one stays.
	for (int i = 0; i < 40; i++) {
		mesh.nodes[2]->send(3, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, payload, 1000);
	}
	for (int t = 0; t < 4000 && mesh.nodes[2]->get_status() != Mesh::STATUS_DISCONNECTED; t++) {
		mesh.poll(1);
	}
	CHECK(mesh.nodes[2]->get_status() == Mesh::STATUS_DISCONNECTED);
	CHECK(mesh.nodes[2]->get_disconnect_reason() == Mesh::DISCONNECT_REASON_REFUSED);
	mesh.poll(100);
	CHECK(mesh.nodes[3]->get_status() == Mesh::STATUS_CONNECTED);
	CHECK(mesh.nodes[1]->is_peer_connected(3));
	CHECK_FALSE(mesh.nodes[1]->is_peer_connected(2));
}


// The host holds what it relays to a player until the player takes it. A player that takes it slower than it comes
// would make the host hold more and more: beyond the limit, a reliable packet for it means the player is removed.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] The relay holds so many bytes for a player") {
	HostedMesh mesh(2, 3);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	REQUIRE(mesh.nodes[2]->get_peer_path(3) == Mesh::PATH_RELAYED);
	mesh.nodes[1]->set_relay_rate_limit(0);
	mesh.nodes[1]->set_relay_queue_limit(50000);
	CHECK(mesh.nodes[1]->get_relay_queue_limit() == 50000);

	// Player 3 stops taking anything (its process hangs); player 2 goes on sending to it through the host.
	uint8_t payload[1000] = {};
	bool removed = false;
	for (int t = 0; t < 6000 && !removed; t++) {
		if (t < 300) {
			mesh.nodes[2]->send(3, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, payload, 1000);
		}
		mesh.nodes[1]->poll();
		mesh.nodes[2]->poll();
		OS::get_singleton()->delay_usec(1000);
		removed = !mesh.nodes[1]->is_peer_connected(3);
	}
	CHECK(removed);
	CHECK(int(mesh.nodes[1]->get_stats()["relay_dropped_packets"]) >= 1);
	// The player that sent stays in the mesh.
	CHECK(mesh.nodes[1]->is_peer_connected(2));
	CHECK(mesh.nodes[2]->get_status() == Mesh::STATUS_CONNECTED);
}


// A player that loses the host asks the players it reaches directly, and migrates when more of them lost the host
// too than still hear it. Before, a single player that still heard the host (or said so) kept the others from
// migrating: they left the mesh instead.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] Most answers decide whether the host is gone") {
	HostedMesh mesh(4);
	// Players 2 to 4 give the host a second; player 5 is slow to give it up, and still counts it alive for 3 seconds.
	for (int i = 2; i <= 4; i++) {
		mesh.nodes[i]->set_host_timeout(1.0);
	}
	mesh.nodes[5]->set_host_timeout(6.0);
	mesh.wait_everyone_connected();
	REQUIRE(mesh.everyone_connected());
	mesh.poll(20);
	REQUIRE(mesh.nodes[2]->get_succession() == PackedInt32Array({ 2, 3, 4, 5 }));

	// The host stops answering. Players 3 and 4 tell player 2 they lost it too; player 5 says it's alive.
	bool migrated = false;
	for (int t = 0; t < 4000 && !migrated; t++) {
		for (int i = 2; i <= 5; i++) {
			mesh.nodes[i]->poll();
		}
		OS::get_singleton()->delay_usec(1000);
		migrated = mesh.nodes[2]->is_hosting() && mesh.nodes[3]->get_host_peer() == 2 && mesh.nodes[4]->get_host_peer() == 2;
	}
	CHECK(mesh.nodes[2]->is_hosting());
	CHECK(mesh.nodes[2]->get_status() == Mesh::STATUS_CONNECTED);
	CHECK(mesh.nodes[3]->get_host_peer() == 2);
	CHECK(mesh.nodes[4]->get_host_peer() == 2);
	CHECK(mesh.nodes[3]->get_status() == Mesh::STATUS_CONNECTED);
	CHECK(mesh.nodes[4]->get_status() == Mesh::STATUS_CONNECTED);
}


// What a host says isn't taken blindly: a player doesn't stay in a mesh bigger than it accepts (it would open a socket
// per player), takes no port that isn't one, and sends no direct link to an address that isn't a host's.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player checks what the host tells it") {
	int port = 0;
	Ref<RefCounted> host = bare_host(port);
	REQUIRE(port != 0);

	// A mesh of 100 players: more than the 64 links a player keeps by default.
	for (int round = 0; round < 3; round++) {
		Ref<Mesh> player = Mesh::create_player("127.0.0.1", port);
		REQUIRE(player.is_valid());
		if (round == 1) {
			player->set_pair_limit(200);
		}
		CHECK(player->get_pair_limit() == (round == 1 ? 200 : 64));
		Variant link;
		bool joined = false;
		for (int t = 0; t < 2000 && !joined; t++) {
			player->poll();
			const Array event = host->call("service", 0);
			if (int(event[0]) == 1) {
				link = event[1];
			} else if (int(event[0]) == 3) {
				joined = true;
			}
			OS::get_singleton()->delay_usec(1000);
		}
		REQUIRE(joined);
		LocalVector<uint32_t> welcome;
		welcome.push_back(2);
		// The rendezvous port: none, or (third round) a number that isn't a port.
		welcome.push_back(round == 2 ? 70000 : 0);
		welcome.push_back(round == 2 ? 4 : 100);
		send_control(link, 1, welcome);
		ERR_PRINT_OFF;
		for (int t = 0; t < 300; t++) {
			player->poll();
			host->call("service", 0);
			OS::get_singleton()->delay_usec(1000);
		}
		ERR_PRINT_ON;
		if (round == 0) {
			CHECK(player->get_status() == Mesh::STATUS_DISCONNECTED);
			CHECK(player->get_disconnect_reason() == Mesh::DISCONNECT_REASON_TOO_LARGE);
		} else if (round == 1) {
			CHECK(player->get_status() == Mesh::STATUS_CONNECTED);
		} else {
			// The welcome is ignored: the player isn't in the mesh.
			CHECK(player->get_status() == Mesh::STATUS_CONNECTING);
		}
		player->close();
		for (int t = 0; t < 50; t++) {
			host->call("service", 0);
			OS::get_singleton()->delay_usec(1000);
		}
	}

	// Introduced to a player "at" a multicast address: no direct link is tried, and the host is asked for the relay.
	Ref<Mesh> player = Mesh::create_player("127.0.0.1", port);
	REQUIRE(player.is_valid());
	Variant link;
	bool joined = false;
	for (int t = 0; t < 2000 && !joined; t++) {
		player->poll();
		const Array event = host->call("service", 0);
		if (int(event[0]) == 1) {
			link = event[1];
		} else if (int(event[0]) == 3) {
			joined = true;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(joined);
	LocalVector<uint32_t> welcome;
	welcome.push_back(2);
	welcome.push_back(0);
	welcome.push_back(4);
	send_control(link, 1, welcome);
	LocalVector<uint32_t> open;
	open.push_back(3);
	open.push_back(1234567);
	send_control(link, 2, open);
	// PAIR_PUNCH: the peer, the token, the port, then the address and the certificate as strings.
	PackedByteArray punch;
	punch.push_back(3);
	const uint32_t punch_fields[3] = { 3, 7654321, 40000 };
	for (const uint32_t field : punch_fields) {
		for (int i = 0; i < 4; i++) {
			punch.push_back(uint8_t(field >> (8 * i)));
		}
	}
	const CharString address = String("224.0.0.1").utf8();
	const uint32_t string_sizes[2] = { uint32_t(address.length()), 0 };
	for (int i = 0; i < 4; i++) {
		punch.push_back(uint8_t(string_sizes[0] >> (8 * i)));
	}
	for (int i = 0; i < address.length(); i++) {
		punch.push_back(uint8_t(address[i]));
	}
	for (int i = 0; i < 4; i++) {
		punch.push_back(uint8_t(string_sizes[1] >> (8 * i)));
	}
	Object *host_link = link;
	if (host_link) {
		host_link->call("send", 0, punch, 1);
	}
	bool asked_relay = false;
	for (int t = 0; t < 1000 && !asked_relay; t++) {
		player->poll();
		const Array event = host->call("service", 0);
		if (int(event[0]) == 3 && int(event[3]) == 0) {
			// Every packet the link received until now (the join data came first): one of them is PAIR_FAILED for player 3.
			Object *from = event[1];
			while (from && int(from->call("get_available_packet_count")) > 0) {
				const PackedByteArray packet = from->call("get_packet");
				asked_relay = asked_relay || (packet.size() == 5 && packet[0] == 5 && packet[1] == 3);
			}
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(asked_relay);
	CHECK(player->get_status() == Mesh::STATUS_CONNECTED);
	player->close();
	host->call("destroy");
}


// ENet drops a link that is still connecting without ever reporting it: a player that gives up on the host then is
// out of the mesh at once, instead of waiting forever for a disconnection that never comes.
TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player that gives up while connecting is out at once") {
	// Nobody listens there.
	Ref<Mesh> player = Mesh::create_player("127.0.0.1", 39000 + int(OS::get_singleton()->get_ticks_usec() % 900));
	REQUIRE(player.is_valid());
	player->poll();
	CHECK(player->get_status() == Mesh::STATUS_CONNECTING);
	player->disconnect_peer(TickTransport::PEER_SERVER);
	CHECK(player->get_status() == Mesh::STATUS_DISCONNECTED);
	CHECK(player->get_disconnect_reason() == Mesh::DISCONNECT_REASON_CLOSED);
	player->poll();
}

} // namespace TestEnetHostedMesh

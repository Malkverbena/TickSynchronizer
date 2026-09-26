#pragma once

#include "../source/common/tick_engine_compat.h"
#include "../source/sync/tick_sync_core.h"
#include "../source/transport/enet_hosted_mesh_transport.h"
#include "../source/transport/tick_multiplayer_peer.h"
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

	// With TLS options, the mesh uses DTLS (the host also opens `port + 1` for the rendezvous).
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

	~HostedMesh() {
		// Players first, each after the others saw the previous one leave: a DTLS link to a closed port reports errors.
		for (int i = count; i >= 1; i--) {
			if (nodes[i].is_valid()) {
				nodes[i]->close();
				poll(50);
			}
		}
	}

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
}

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

struct TestCertificate {
	Ref<CryptoKey> key;
	Ref<X509Certificate> certificate;

	TestCertificate() {
		Ref<Crypto> crypto = Ref<Crypto>(Crypto::create());
		key = crypto->generate_rsa(2048);
		certificate = crypto->generate_self_signed_certificate(key, "CN=localhost,O=TickSynchronizer,C=BR", "20250101000000", "20350101000000");
	}
};

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
	CHECK_FALSE(mesh.nodes[2]->is_peer_connected(4));
}

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
	for (int phase = 0; phase < 2; phase++) {
		const uint64_t end = last + 2500000;
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
			// The host leaves, handing the mesh over.
			cores[1].stop();
			mesh.nodes[1]->hand_over();
			stopped = 1;
		}
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
	CHECK_FALSE(mesh.nodes[3]->is_hosting());
	CHECK(mesh.nodes[2]->get_host_peer() == 1);
	CHECK(mesh.nodes[4]->get_host_peer() == 1);
}

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
	for (int i = 2; i <= 4; i++) {
		CHECK_FALSE(mesh.nodes[i]->is_hosting());
		CHECK_FALSE(drain_events(mesh.nodes[i]));
	}
}

// The game's decision on the players joining: the ones that send "secret" are admitted, "later" waits for the game.
static int waiting_player = 0;
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

TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] The host admits only the players the game accepts") {
	HostedMesh mesh(0);
	mesh.nodes[1]->set_join_validator(callable_mp_static(&check_join));

	join(mesh, "secret");
	CHECK(mesh.nodes[2]->get_status() == Mesh::STATUS_CONNECTED);
	CHECK(mesh.nodes[2]->get_local_peer_id() == 2);

	// Refused: it never gets an id, and nobody learns about it.
	join(mesh, "wrong");
	CHECK(mesh.nodes[3]->get_status() == Mesh::STATUS_DISCONNECTED);
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
	for (int i = 0; i < 6; i++) {
		players[i]->close();
	}
	mesh.poll(20);
}

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
	CHECK_FALSE(mesh.nodes[2]->is_hosting());
	CHECK_FALSE(drain_events(mesh.nodes[2]));
	CHECK(mesh.nodes[3]->get_host_peer() == 1);
	CHECK(mesh.nodes[3]->is_peer_connected(1));
	CHECK(mesh.nodes[1]->is_peer_connected(3));
}

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

TEST_CASE("[Modules][TickSynchronizer][EnetHostedMeshTransport] A player keeps no more pairs than the mesh can have") {
	// A fake host welcomes a player into a mesh of 4 players, then introduces it to 50 others.
	Ref<RefCounted> host = Object::cast_to<RefCounted>(ClassDB::instantiate("ENetConnection"));
	REQUIRE(host.is_valid());
	int port = 0;
	for (int attempt = 0; attempt < 10 && port == 0; attempt++) {
		const int candidate = 38000 + int((OS::get_singleton()->get_ticks_usec() + uint64_t(attempt) * 7919) % 4000);
		if (int(host->call("create_host_bound", "127.0.0.1", candidate, 4, 27, 0, 0)) == OK) {
			port = candidate;
		}
	}
	REQUIRE(port != 0);
	// The players' compression (range coder).
	host->call("compress", 1);
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

} // namespace TestEnetHostedMesh

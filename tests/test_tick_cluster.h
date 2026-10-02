// Tests of a cluster of servers: a mesh with one authority whose game servers relay the objects to their own clients
// through a star that follows the cluster's timeline (ADR-038, ADR-039); and of `EnetMeshTransport`, the real mesh
// between servers, with who it accepts as a node (ADR-076).
//
// `BridgeObject` is the edge's view of a cluster proxy; `ClusterWorld` has three servers in a mesh, one of them with a
// star and a client.

#pragma once

#include "../source/transport/enet_mesh_transport.h"
#include "test_tick_fuzz.h"
#include "test_tick_sync_core.h"

#include "core/os/os.h"
#include "tests/test_macros.h"

namespace TestTickCluster {

using TestTickSyncCore::TestMover;

// The edge's view of a cluster proxy: it reads and writes the proxy's state and simulates nothing (the cluster
// network moves it).
class BridgeObject : public TickSyncObject {
public:
	TestMover *proxy = nullptr;

	// The view of `p_proxy`, the object the cluster's network moves.
	explicit BridgeObject(TestMover *p_proxy) :
			proxy(p_proxy) {}


	// `TickSyncObject`: the proxy's path.
	virtual String get_sync_path() const override { return proxy->get_sync_path(); }


	// `TickSyncObject`: the server of the edge.
	virtual int get_controller_peer() const override { return 1; }


	// `TickSyncObject`: the proxy's variables.
	virtual const TickSchema &get_sync_schema() const override { return proxy->get_sync_schema(); }


	// `TickSyncObject`: reads the proxy's variable.
	virtual Variant get_sync_var(int p_index) const override { return proxy->get_sync_var(p_index); }


	// `TickSyncObject`: sets the proxy's variable.
	virtual void set_sync_var(int p_index, const Variant &p_value) override { proxy->set_sync_var(p_index, p_value); }


	// `TickSyncObject`: no input: the edge simulates nothing.
	virtual void collect_input(TickDataBuffer &r_input) override {}


	// `TickSyncObject`: nothing to simulate: the cluster's network moves the proxy.
	virtual void process_tick(double p_delta, TickDataBuffer &p_input) override {}
};

// W (authority, 1), A (2) and B (3) in a mesh; A also runs a star (edge) with one client.
struct ClusterWorld {
	TickLocalNetwork cluster_network;
	TickLocalNetwork edge_network;
	Ref<TickLocalTransport> cluster_transports[4];
	Ref<TickLocalTransport> edge_server_transport;
	Ref<TickLocalTransport> edge_client_transport;
	TickSyncCore w;
	TickSyncCore a;
	TickSyncCore b;
	TickSyncCore edge_a;
	TickSyncCore client;

	TestMover w_npc = TestMover("npc", 1, TickCodec::PRECISION_SINGLE);
	TestMover a_npc = TestMover("npc", 1, TickCodec::PRECISION_SINGLE);
	TestMover b_npc = TestMover("npc", 1, TickCodec::PRECISION_SINGLE);
	BridgeObject a_bridge = BridgeObject(&a_npc);
	TestMover client_npc = TestMover("npc", 1, TickCodec::PRECISION_SINGLE);

	// Builds the cluster (1 ms between servers) and the edge (30 ms to its client), registers the NPC on every engine,
	// and starts them all.
	ClusterWorld() {
		cluster_network.set_latency_usec(1000);
		edge_network.set_latency_usec(30000);
		for (int i = 1; i <= 3; i++) {
			cluster_transports[i] = cluster_network.add_peer();
		}
		edge_server_transport = edge_network.add_peer();
		edge_client_transport = edge_network.add_peer();

		// W's NPC always moves right.
		w_npc.constant_direction = true;
		w_npc.direction_override = 1;

		TickSyncCore::Settings cluster_settings;
		cluster_settings.trusted = true;
		w.set_settings(cluster_settings);
		// The game servers relay the proxies without interpolating them.
		cluster_settings.interpolate_remote = false;
		a.set_settings(cluster_settings);
		b.set_settings(cluster_settings);
		// A's star follows the cluster's timeline.
		edge_a.set_clock_source(&a);

		w.register_object(&w_npc);
		a.register_object(&a_npc);
		b.register_object(&b_npc);
		edge_a.register_object(&a_bridge);
		client.register_object(&client_npc);

		REQUIRE(w.start(cluster_transports[1], 0) == OK);
		REQUIRE(a.start(cluster_transports[2], 0) == OK);
		REQUIRE(b.start(cluster_transports[3], 0) == OK);
		REQUIRE(edge_a.start(edge_server_transport, 0) == OK);
		REQUIRE(client.start(edge_client_transport, 0) == OK);
		cluster_network.connect_all();
		edge_network.connect_all();
	}

	// The local clocks of A, B and the client started this long after W's (other processes, started later).
	uint64_t late_start_usec = 0;

	// Runs every network for `p_seconds`, in the process order of a game server: the cluster before the edge. With
	// `p_only_w`, the other processes don't exist yet.
	void run(double p_seconds, bool p_only_w = false) {
		for (int i = 0; i < int(p_seconds * 60.0); i++) {
			const double delta = (i % 2 == 0) ? 0.016 : 0.0173333;
			cluster_network.process(delta);
			edge_network.process_usec(uint64_t(delta * 1000000.0 + 0.5));
			const uint64_t now = cluster_network.get_time_usec();
			w.process(delta, now);
			if (p_only_w) {
				continue;
			}
			const uint64_t local_now = now - late_start_usec;
			a.process(delta, local_now);
			b.process(delta, local_now);
			edge_a.process(delta, local_now);
			client.process(delta, local_now);
		}
	}
};

// The game servers' proxies follow the authority within a few frames, the edge runs on the authority's frames, and the
// final client predicts ahead and interpolates the NPC behind by the interpolation delay plus the latency.
TEST_CASE("[Modules][TickSynchronizer][Cluster] A mesh with one authority and a bridged star share the timeline") {
	ClusterWorld world;
	world.run(0.1);
	// The edge doesn't tick until the cluster's clock is known.
	CHECK_FALSE(world.a.get_clock().is_synchronized());
	CHECK(world.edge_a.get_frame() == 0);

	world.run(5.0);
	REQUIRE(world.a.is_welcomed());
	REQUIRE(world.b.is_welcomed());
	REQUIRE(world.client.is_predicting());

	// The proxies follow W closely: no interpolation delay on the servers.
	const double speed = 5.0;
	CHECK(world.w_npc.position.x > 20.0);
	CHECK(Math::abs(world.a_npc.position.x - world.w_npc.position.x) <= speed * 3.0 / 60.0);
	CHECK(Math::abs(world.b_npc.position.x - world.w_npc.position.x) <= speed * 3.0 / 60.0);

	// The edge runs on W's frames, and its client runs ahead of them.
	CHECK(Math::abs(int32_t(world.edge_a.get_frame() - world.w.get_frame())) <= 2);
	CHECK(tick_frame_after(world.client.get_frame(), world.edge_a.get_frame()));

	// The final client interpolates: behind W by the interpolation delay and the latency.
	const double lag_frames = (world.w_npc.position.x - world.client_npc.position.x) / speed * 60.0;
	CHECK(lag_frames > 4.0);
	CHECK(lag_frames < 10.0);
	CHECK(world.client.get_stats().malformed_packets == 0);
	CHECK(world.a.get_stats().malformed_packets == 0);
}


// A game server that starts 10 seconds after the authority (so the timeline is older than its clock) still keeps its
// client on time.
TEST_CASE("[Modules][TickSynchronizer][Cluster] A game server started after the authority keeps the clients on time") {
	ClusterWorld world;
	// W runs alone for 10 s: when A starts, the cluster's frames are older than A's clock (negative epoch).
	world.run(10.0, true);
	world.late_start_usec = world.cluster_network.get_time_usec();
	world.run(5.0);
	REQUIRE(world.client.is_predicting());
	CHECK(Math::abs(int32_t(world.edge_a.get_frame() - world.w.get_frame())) <= 2);
	// Interpolation delay (6 frames) plus the latency: the client isn't left behind by A's late start.
	const double lag_frames = (world.w_npc.position.x - world.client_npc.position.x) / 5.0 * 60.0;
	CHECK(lag_frames > 4.0);
	CHECK(lag_frames < 10.0);
}


// Polls `p_count` transports `p_times` times, a millisecond apart.
inline void poll_mesh(const Ref<EnetMeshTransport> *p_transports, int p_count, int p_times) {
	for (int t = 0; t < p_times; t++) {
		for (int i = 0; i < p_count; i++) {
			p_transports[i]->poll();
		}
		OS::get_singleton()->delay_usec(1000);
	}
}


// Three ENet nodes connect into a full mesh; a node that isn't in the list, or that claims an id already connected, is
// refused; a packet carries the sender of its connection; and a cluster with one authority synchronizes over the real
// sockets.
TEST_CASE("[Modules][TickSynchronizer][EnetMeshTransport] Three ENet nodes form a mesh and sync a cluster") {
	const int base_port = 42000 + int(OS::get_singleton()->get_ticks_usec() % 10000);
	Ref<EnetMeshTransport> nodes[3];
	for (int i = 0; i < 3; i++) {
		nodes[i] = EnetMeshTransport::create(i + 1, base_port + i, "*", EnetMeshTransport::COMPRESSION_RANGE_CODER);
		REQUIRE(nodes[i].is_valid());
		nodes[i]->set_retry_interval(0.05);
	}
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			if (i != j) {
				CHECK(nodes[i]->add_node(j + 1, "127.0.0.1", base_port + j) == OK);
			}
		}
	}
	for (int t = 0; t < 2000; t++) {
		poll_mesh(nodes, 3, 1);
		if (nodes[0]->is_peer_connected(2) && nodes[0]->is_peer_connected(3) && nodes[1]->is_peer_connected(3)) {
			break;
		}
	}
	REQUIRE(nodes[0]->is_peer_connected(2));
	REQUIRE(nodes[0]->is_peer_connected(3));
	REQUIRE(nodes[1]->is_peer_connected(3));
	CHECK(nodes[2]->is_peer_connected(1));

	// Nodes that aren't in the list, or that claim an id already connected, are refused.
	Ref<EnetMeshTransport> intruder = EnetMeshTransport::create(9, base_port + 9);
	REQUIRE(intruder.is_valid());
	intruder->add_node(1, "127.0.0.1", base_port);
	Ref<EnetMeshTransport> impostor = EnetMeshTransport::create(3, base_port + 8);
	REQUIRE(impostor.is_valid());
	impostor->add_node(1, "127.0.0.1", base_port);
	Ref<EnetMeshTransport> all[5] = { nodes[0], nodes[1], nodes[2], intruder, impostor };
	poll_mesh(all, 5, 300);
	CHECK_FALSE(intruder->is_peer_connected(1));
	CHECK_FALSE(impostor->is_peer_connected(1));
	LocalVector<int> connected;
	nodes[0]->get_connected_peers(connected);
	CHECK(connected.size() == 2);
	intruder.unref();
	impostor.unref();

	// The sender comes from the connection.
	const uint8_t payload[2] = { 4, 2 };
	CHECK(nodes[2]->send(2, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_UNRELIABLE, payload, 2) == OK);
	TickTransport::Packet packet;
	bool received = false;
	for (int t = 0; t < 500 && !received; t++) {
		poll_mesh(nodes, 3, 1);
		while (nodes[1]->pop_packet(packet)) {
			received = true;
			CHECK(packet.from_peer == 3);
			CHECK(packet.channel == TICK_CHANNEL_STATE);
		}
	}
	CHECK(received);

	// A cluster over the real ENet mesh: node 1 is the authority.
	TickSyncCore cores[3];
	TestMover npcs[3] = { TestMover("npc", 1, TickCodec::PRECISION_SINGLE), TestMover("npc", 1, TickCodec::PRECISION_SINGLE), TestMover("npc", 1, TickCodec::PRECISION_SINGLE) };
	npcs[0].constant_direction = true;
	npcs[0].direction_override = 1;
	TickSyncCore::Settings settings;
	settings.trusted = true;
	settings.interpolate_remote = false;
	for (int i = 0; i < 3; i++) {
		cores[i].set_settings(settings);
		cores[i].register_object(&npcs[i]);
		// Discards the events already consumed above, then starts.
		TickTransport::Event event;
		while (nodes[i]->pop_event(event)) {
		}
		REQUIRE(cores[i].start(nodes[i], OS::get_singleton()->get_ticks_usec()) == OK);
	}
	uint64_t last = OS::get_singleton()->get_ticks_usec();
	const uint64_t end = last + 3000000;
	while (OS::get_singleton()->get_ticks_usec() < end) {
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		const double delta = double(now - last) / 1000000.0;
		last = now;
		for (int i = 0; i < 3; i++) {
			cores[i].process(delta, now);
		}
		OS::get_singleton()->delay_usec(2000);
	}
	CHECK(cores[1].is_welcomed());
	CHECK(cores[2].is_welcomed());
	CHECK(npcs[0].position.x > 5.0);
	CHECK(Math::abs(npcs[1].position.x - npcs[0].position.x) < 0.5);
	CHECK(Math::abs(npcs[2].position.x - npcs[0].position.x) < 0.5);
	for (int i = 0; i < 3; i++) {
		cores[i].unregister_object(&npcs[i]);
		cores[i].stop();
	}
}


// A node is added with the address it listens on, and its connections must come from there: whoever reaches the port
// can't take the place of a node that isn't connected just by declaring its id. With `check_addresses` off the
// connection is taken, as it was before the audit of 2026-10-01.
TEST_CASE("[Modules][TickSynchronizer][EnetMeshTransport] A connection must come from the address of its node") {
	const int port = 43000 + int(OS::get_singleton()->get_ticks_usec() % 10000);
	Ref<EnetMeshTransport> node = EnetMeshTransport::create(1, port);
	REQUIRE(node.is_valid());
	CHECK(node->is_checking_addresses());
	// Node 2 is somewhere else, and it isn't up.
	CHECK(node->add_node(2, "10.11.12.13", 9002) == OK);

	// A bare ENet socket at another address of this machine declares itself node 2.
	TestTickFuzz::RogueSocket rogue;
	if (!rogue.open("127.0.0.2", "127.0.0.1", port, 2, TICK_CHANNEL_COUNT)) {
		MESSAGE("Can't bind a socket to 127.0.0.2 on this system: not tested.");
		return;
	}
	for (int t = 0; t < 2000 && !rogue.disconnected; t++) {
		node->poll();
		rogue.poll();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(rogue.disconnected);
	CHECK_FALSE(node->is_peer_connected(2));
	TickTransport::Event event;
	CHECK_FALSE(node->pop_event(event));
	rogue.close();

	node->set_check_addresses(false);
	REQUIRE(rogue.open("127.0.0.2", "127.0.0.1", port, 2, TICK_CHANNEL_COUNT));
	for (int t = 0; t < 2000 && !node->is_peer_connected(2); t++) {
		node->poll();
		rogue.poll();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(node->is_peer_connected(2));
	rogue.close();
}


// With a secret, the two sides of a link prove to each other that they know it before the link is reported. A node
// with another secret, or with none, never becomes a node of the mesh, and nothing it sends reaches the engines.
TEST_CASE("[Modules][TickSynchronizer][EnetMeshTransport] With a secret, only the nodes that prove it join the mesh") {
	const int base_port = 44000 + int(OS::get_singleton()->get_ticks_usec() % 10000);
	PackedByteArray secret;
	PackedByteArray other_secret;
	for (int i = 0; i < 32; i++) {
		secret.push_back(uint8_t(i * 7 + 3));
		other_secret.push_back(uint8_t(i * 5 + 1));
	}
	Ref<EnetMeshTransport> nodes[4];
	for (int i = 0; i < 4; i++) {
		nodes[i] = EnetMeshTransport::create(i + 1, base_port + i);
		REQUIRE(nodes[i].is_valid());
		nodes[i]->set_retry_interval(0.05);
	}
	// Nodes 1 and 2 have the secret; node 3 has another one, node 4 none.
	nodes[0]->set_secret(secret);
	nodes[1]->set_secret(secret);
	nodes[2]->set_secret(other_secret);
	CHECK(nodes[0]->has_secret());
	CHECK_FALSE(nodes[3]->has_secret());
	for (int i = 1; i < 4; i++) {
		CHECK(nodes[0]->add_node(i + 1, "127.0.0.1", base_port + i) == OK);
		CHECK(nodes[i]->add_node(1, "127.0.0.1", base_port) == OK);
	}

	ERR_PRINT_OFF;
	for (int t = 0; t < 2000 && !(nodes[0]->is_peer_connected(2) && nodes[1]->is_peer_connected(1)); t++) {
		poll_mesh(nodes, 4, 1);
	}
	REQUIRE(nodes[0]->is_peer_connected(2));
	REQUIRE(nodes[1]->is_peer_connected(1));
	// Long enough for the other two to try several times.
	poll_mesh(nodes, 4, 400);
	CHECK_FALSE(nodes[0]->is_peer_connected(3));
	CHECK_FALSE(nodes[0]->is_peer_connected(4));
	CHECK_FALSE(nodes[2]->is_peer_connected(1));

	// Node 4 asks for no proof, so it takes its link for a node's and sends on it: node 1 delivers nothing of it.
	const uint8_t payload[3] = { 9, 9, 9 };
	bool sent = false;
	for (int t = 0; t < 1000 && !sent; t++) {
		poll_mesh(nodes, 4, 1);
		sent = nodes[3]->is_peer_connected(1) && nodes[3]->send(1, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, payload, 3) == OK;
	}
	CHECK(sent);
	CHECK(nodes[1]->send(1, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, payload, 3) == OK);
	int from_node_2 = 0;
	int from_others = 0;
	for (int t = 0; t < 300; t++) {
		poll_mesh(nodes, 4, 1);
		TickTransport::Packet packet;
		while (nodes[0]->pop_packet(packet)) {
			from_node_2 += packet.from_peer == 2 ? 1 : 0;
			from_others += packet.from_peer != 2 ? 1 : 0;
		}
	}
	ERR_PRINT_ON;
	CHECK(from_node_2 == 1);
	CHECK(from_others == 0);
	LocalVector<int> connected;
	nodes[0]->get_connected_peers(connected);
	CHECK(connected.size() == 1);
}

} // namespace TestTickCluster

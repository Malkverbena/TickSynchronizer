#pragma once

#include "../source/transport/enet_mesh_transport.h"
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

	explicit BridgeObject(TestMover *p_proxy) :
			proxy(p_proxy) {}

	virtual String get_sync_path() const override { return proxy->get_sync_path(); }
	virtual int get_controller_peer() const override { return 1; }
	virtual const TickSchema &get_sync_schema() const override { return proxy->get_sync_schema(); }
	virtual Variant get_sync_var(int p_index) const override { return proxy->get_sync_var(p_index); }
	virtual void set_sync_var(int p_index, const Variant &p_value) override { proxy->set_sync_var(p_index, p_value); }
	virtual void collect_input(TickDataBuffer &r_input) override {}
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

	// Each network in the process order of a game server: the cluster before the edge. With `p_only_w`, the
	// others don't exist yet.
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

inline void poll_mesh(const Ref<EnetMeshTransport> *p_transports, int p_count, int p_times) {
	for (int t = 0; t < p_times; t++) {
		for (int i = 0; i < p_count; i++) {
			p_transports[i]->poll();
		}
		OS::get_singleton()->delay_usec(1000);
	}
}

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

} // namespace TestTickCluster

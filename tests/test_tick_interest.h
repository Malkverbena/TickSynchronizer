#pragma once

#include "test_tick_sync_core.h"

#include "tests/test_macros.h"

namespace TestTickInterest {

using TestTickSyncCore::TestListener;
using TestTickSyncCore::TestMover;
using TestTickSyncCore::TestWorld;

// Records the relevance changes a client is told about, and answers the server's filter.
class InterestListener : public TestListener {
public:
	int shown = 0;
	int hidden = 0;
	// Server: objects whose path is in this list are hidden from peer 3.
	LocalVector<String> hidden_from_3;

	virtual void on_relevance_changed(TickSyncObject *p_object, bool p_relevant) override {
		(p_relevant ? shown : hidden)++;
	}
	virtual int filter_relevance(int p_peer, TickSyncObject *p_object) override {
		if (p_peer != 3) {
			return -1;
		}
		return hidden_from_3.has(p_object->get_sync_path()) ? 0 : 1;
	}
};

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Irrelevant objects aren't sent, and come back from a full state") {
	TestWorld world(30000, 0, 0.0, 100000);
	InterestListener server_listener;
	InterestListener b_listener;
	world.server.set_listener(&server_listener);
	world.client_b.set_listener(&b_listener);
	world.run(2.0);
	REQUIRE(world.client_b.is_predicting());

	// The NPC stops being relevant to client B (peer 3): B stops getting it; A still does.
	server_listener.hidden_from_3.push_back("npc");
	world.run(1.0);
	CHECK(b_listener.hidden == 1);
	CHECK_FALSE(world.server.is_relevant(&world.server_npc, 3));
	CHECK(world.server.is_relevant(&world.server_npc, 2));
	CHECK_FALSE(world.client_b.is_relevant(&world.b_npc, 3));
	const Vector2 frozen = world.b_npc.position;
	world.run(1.0);
	CHECK(world.b_npc.position == frozen);
	CHECK(world.a_npc.position != frozen);

	// An object is always relevant to its controller.
	CHECK(world.server.set_relevant(&world.server_mover_b, 3, false) == OK);
	CHECK(world.server.is_relevant(&world.server_mover_b, 3));

	// Relevant again: B gets it, from a full state (B has no base for it), and interpolates it with A.
	server_listener.hidden_from_3.clear();
	world.run(1.0);
	CHECK(b_listener.shown == 1);
	CHECK(world.client_b.is_relevant(&world.b_npc, 3));
	CHECK(world.b_npc.position.distance_to(world.a_npc.position) < 0.5);
	CHECK(world.client_b.get_stats().malformed_packets == 0);
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Hidden by default, shown on demand") {
	// The settings must be set before the engines start: a world built by hand.
	TickLocalNetwork network;
	network.set_latency_usec(20000);
	Ref<TickLocalTransport> server_transport = network.add_peer();
	Ref<TickLocalTransport> client_transport = network.add_peer();
	TickSyncCore server;
	TickSyncCore client;
	InterestListener client_listener;
	client.set_listener(&client_listener);
	TickSyncCore::Settings settings;
	settings.default_relevant = false;
	server.set_settings(settings);
	TestMover server_npc("npc", 1);
	TestMover server_player("player", 2);
	TestMover client_npc("npc", 1);
	TestMover client_player("player", 2);
	server_npc.constant_direction = true;
	server_npc.direction_override = 1;
	server.register_object(&server_npc);
	server.register_object(&server_player);
	client.register_object(&client_npc);
	client.register_object(&client_player);
	REQUIRE(server.start(server_transport, 0) == OK);
	REQUIRE(client.start(client_transport, 0) == OK);
	network.connect_peers(1, 2);
	for (int i = 0; i < 120; i++) {
		network.process(1.0 / 60.0);
		server.process(1.0 / 60.0, network.get_time_usec());
		client.process(1.0 / 60.0, network.get_time_usec());
	}
	// Only the client's own player is relevant.
	CHECK(client.is_relevant(&client_player, 2));
	CHECK_FALSE(client.is_relevant(&client_npc, 2));
	CHECK(client_listener.hidden == 1);
	CHECK(client_npc.position == Vector2());

	CHECK(server.set_relevant(&server_npc, TickTransport::PEER_BROADCAST, true) == OK);
	for (int i = 0; i < 60; i++) {
		network.process(1.0 / 60.0);
		server.process(1.0 / 60.0, network.get_time_usec());
		client.process(1.0 / 60.0, network.get_time_usec());
	}
	CHECK(client_listener.shown == 1);
	CHECK(client_npc.position.x > 1.0);
	ERR_PRINT_OFF;
	CHECK(client.set_relevant(&client_npc, 2, false) == ERR_UNAVAILABLE);
	ERR_PRINT_ON;
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Deltas bigger than a datagram are split and reassembled") {
	TickLocalNetwork network;
	network.set_latency_usec(20000);
	network.set_jitter_usec(10000);
	network.set_packet_loss(0.05);
	network.set_seed(5);
	Ref<TickLocalTransport> server_transport = network.add_peer();
	Ref<TickLocalTransport> client_transport = network.add_peer();
	TickSyncCore server;
	TickSyncCore client;
	LocalVector<TestMover *> server_npcs;
	LocalVector<TestMover *> client_npcs;
	// About 70 bits each when they move: more than a 1200 byte datagram.
	for (int i = 0; i < 250; i++) {
		TestMover *server_npc = memnew(TestMover(vformat("npc_%d", i), 1));
		server_npc->constant_direction = true;
		server_npc->direction_override = (i % 2) * 2 - 1;
		server_npcs.push_back(server_npc);
		server.register_object(server_npc);
		TestMover *client_npc = memnew(TestMover(vformat("npc_%d", i), 1));
		client_npcs.push_back(client_npc);
		client.register_object(client_npc);
	}
	REQUIRE(server.start(server_transport, 0) == OK);
	REQUIRE(client.start(client_transport, 0) == OK);
	network.connect_peers(1, 2);
	for (int i = 0; i < 300; i++) {
		network.process(1.0 / 60.0);
		server.process(1.0 / 60.0, network.get_time_usec());
		client.process(1.0 / 60.0, network.get_time_usec());
	}
	// Every delta needed more than one datagram.
	CHECK(server.get_stats().split_snapshots > 200);
	CHECK(client.get_stats().snapshots_received > 200);
	CHECK(client.get_stats().malformed_packets == 0);
	// Lost parts drop their frame, not the next ones.
	CHECK(client.get_stats().incomplete_snapshots > 0);
	// The interpolated copies follow the server (the NPCs move 5 units per second, 0.1 s of interpolation).
	for (uint32_t i = 0; i < client_npcs.size(); i++) {
		CHECK(Math::abs(client_npcs[i]->position.x - server_npcs[i]->position.x) < 1.0);
	}
	server.stop();
	client.stop();
	for (uint32_t i = 0; i < client_npcs.size(); i++) {
		memdelete(server_npcs[i]);
		memdelete(client_npcs[i]);
	}
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] The state of an object at a past frame") {
	TestWorld world(30000, 0, 0.0, 100000);
	world.server_npc.constant_direction = true;
	world.server_npc.direction_override = 1;
	world.run(3.0);

	// Server: every frame of the history, interpolated between frames.
	const uint32_t now = world.server.get_frame() - 1;
	LocalVector<Variant> at_now;
	LocalVector<Variant> past;
	LocalVector<Variant> between;
	REQUIRE(world.server.get_state_at(&world.server_npc, double(now), at_now));
	CHECK(Vector2(at_now[0]) == world.server_npc.position);
	LocalVector<Variant> next;
	REQUIRE(world.server.get_state_at(&world.server_npc, double(now - 30), past));
	REQUIRE(world.server.get_state_at(&world.server_npc, double(now - 29), next));
	// 30 ticks at 5 units per second, with the rounding of each tick to half precision.
	const real_t travelled = Vector2(at_now[0]).x - Vector2(past[0]).x;
	CHECK(travelled > 2.0);
	CHECK(travelled < 3.0);
	REQUIRE(world.server.get_state_at(&world.server_npc, double(now - 30) + 0.5, between));
	CHECK(Math::abs(Vector2(between[0]).x - (Vector2(past[0]).x + Vector2(next[0]).x) * 0.5) < 0.01);
	// Older than the history.
	CHECK_FALSE(world.server.get_state_at(&world.server_npc, double(now - 1000), past));

	// Client: the frame shown by the interpolated objects, and the state the server had at that frame.
	const double view = world.client_a.get_view_frame(world.network.get_time_usec());
	CHECK(view > double(now) - 20.0);
	CHECK(view < double(now));
	LocalVector<Variant> seen;
	LocalVector<Variant> server_seen;
	REQUIRE(world.client_a.get_state_at(&world.a_npc, view, seen));
	REQUIRE(world.server.get_state_at(&world.server_npc, view, server_seen));
	CHECK(Vector2(seen[0]).distance_to(Vector2(server_seen[0])) < 0.05);
	CHECK(Vector2(seen[0]).distance_to(world.a_npc.position) < 0.2);
}

} // namespace TestTickInterest

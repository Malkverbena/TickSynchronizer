// Tests of what game code may do from inside the engines. Game code runs inside the engines (ticks, events, signals)
// and may change the network from there: remove or register objects, or stop it. The engines go on, or stop cleanly.
//
// `RudeMover` is a body whose tick does those things; `ServerWorld` is a server alone on a simulated network.

#pragma once

#include "../source/sync/tick_mesh_core.h"
#include "test_tick_sync_core.h"

#include "tests/test_macros.h"

namespace TestTickGameCode {

using TestTickSyncCore::TestMover;

// A mover whose tick, at frame `act_at`, removes an object (possibly itself), registers others, or stops the
// network. It counts how many times it's simulated in each frame.
class RudeMover : public TestMover {
public:
	TickEngine *engine = nullptr;
	HashMap<uint32_t, int> ticks_by_frame;
	uint32_t act_at = TICK_FRAME_NONE;
	TickSyncObject *remove = nullptr;
	LocalVector<TickSyncObject *> add;
	bool stop = false;

	// A body at `p_path` that acts on `p_engine`.
	RudeMover(const String &p_path, int p_controller, TickEngine *p_engine) :
			TestMover(p_path, p_controller), engine(p_engine) {}


	// Moves as a `TestMover`, counts the tick and, at frame `act_at`, does what it was told: removes an object,
	// registers others, stops the network.
	virtual void process_tick(double p_delta, TickDataBuffer &p_input) override {
		TestMover::process_tick(p_delta, p_input);
		const uint32_t frame = engine->get_frame();
		ticks_by_frame[frame]++;
		if (frame != act_at) {
			return;
		}
		act_at = TICK_FRAME_NONE;
		if (remove) {
			engine->unregister_object(remove);
		}
		for (TickSyncObject *object : add) {
			engine->register_object(object);
		}
		if (stop) {
			engine->stop();
		}
	}


	// How many times the body was simulated in a frame.
	int get_ticks(uint32_t p_frame) const {
		const int *ticks = ticks_by_frame.getptr(p_frame);
		return ticks ? *ticks : 0;
	}
};

// A server alone on a simulated network.
struct ServerWorld {
	TickLocalNetwork network;
	Ref<TickLocalTransport> transport;
	TickSyncCore server;

	// Starts the server.
	ServerWorld() {
		transport = network.add_peer();
		REQUIRE(server.start(transport, 0) == OK);
	}


	// Runs the server for `p_frames` frames, or until it stops.
	void run(int p_frames) {
		for (int i = 0; i < p_frames && server.is_running(); i++) {
			network.process(1.0 / 60.0);
			server.process(1.0 / 60.0, network.get_time_usec());
		}
	}
};

// When an object's tick removes another object, the removed one isn't simulated in that frame nor later, and the ones
// after it are simulated exactly once.
TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] An object removed by another object's tick isn't simulated twice") {
	ServerWorld world;
	RudeMover a("a", 1, &world.server);
	RudeMover b("b", 1, &world.server);
	RudeMover c("c", 1, &world.server);
	world.server.register_object(&a);
	world.server.register_object(&b);
	world.server.register_object(&c);
	world.run(5);

	// In the middle of a tick, `a` removes `b`, which comes before `c`.
	const uint32_t frame = world.server.get_frame() + 3;
	a.remove = &b;
	a.act_at = frame;
	world.run(10);

	CHECK(a.get_ticks(frame) == 1);
	CHECK(b.get_ticks(frame) == 0);
	CHECK(b.get_ticks(frame + 1) == 0);
	for (const KeyValue<uint32_t, int> &E : c.ticks_by_frame) {
		CHECK_MESSAGE(E.value == 1, vformat("`c` was simulated %d times at frame %d.", E.value, E.key));
	}
	CHECK(c.get_ticks(frame) == 1);
	CHECK(world.server.get_net_id(&b) == 0);

	world.server.unregister_object(&a);
	world.server.unregister_object(&c);
}


// Objects registered during a tick are simulated from the next one, once per frame; an object that removes itself isn't
// simulated again.
TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Objects registered or removed by their own ticks") {
	ServerWorld world;
	RudeMover spawner("spawner", 1, &world.server);
	RudeMover leaver("leaver", 1, &world.server);
	RudeMover last("last", 1, &world.server);
	world.server.register_object(&spawner);
	world.server.register_object(&leaver);
	world.server.register_object(&last);
	world.run(5);

	// Enough new objects to reallocate the list of ids in the middle of the tick, and one removing itself.
	LocalVector<RudeMover *> spawned;
	for (int i = 0; i < 40; i++) {
		spawned.push_back(memnew(RudeMover(vformat("projectile_%d", i), 1, &world.server)));
		spawner.add.push_back(spawned[i]);
	}
	const uint32_t frame = world.server.get_frame() + 3;
	spawner.act_at = frame;
	leaver.remove = &leaver;
	leaver.act_at = frame;
	world.run(10);

	CHECK(last.get_ticks(frame) == 1);
	CHECK(leaver.get_ticks(frame) == 1);
	CHECK(leaver.get_ticks(frame + 1) == 0);
	CHECK(world.server.get_net_id(&leaver) == 0);
	for (RudeMover *projectile : spawned) {
		// Registered during the tick: simulated from the next one, once per frame.
		CHECK(world.server.get_net_id(projectile) != 0);
		CHECK(projectile->get_ticks(frame) == 0);
		CHECK(projectile->get_ticks(frame + 1) == 1);
	}

	for (RudeMover *projectile : spawned) {
		world.server.unregister_object(projectile);
		memdelete(projectile);
	}
	world.server.unregister_object(&spawner);
	world.server.unregister_object(&last);
}


// A client goes on predicting, without malformed packets, after its game removes the object it predicted; registered
// again, the object is bound and predicted again.
TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] A predicted object the game removes doesn't break the reconciliation") {
	// The bodies stand still, so the deltas carry their state over from the base.
	TestTickSyncCore::TestWorld world(30000, 0, 0.0, 0);
	world.run(2.0);
	REQUIRE(world.client_a.is_predicting());

	// The client's game removes the object it predicts (its node is freed) while the server still has it.
	world.client_a.unregister_object(&world.a_mover_a);
	world.run(1.0);
	CHECK(world.client_a.get_net_id(&world.a_mover_a) == 0);
	CHECK(world.client_a.is_predicting());
	CHECK(world.client_a.get_stats().malformed_packets == 0);

	// Back in the scene, it's bound and predicted again.
	world.client_a.register_object(&world.a_mover_a);
	world.run(1.0);
	CHECK(world.client_a.get_net_id(&world.a_mover_a) == world.server.get_net_id(&world.server_mover_a));
}


// A server and a client that stop from inside a tick stop cleanly, and can start again.
TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Game code can stop the network from a tick") {
	TickLocalNetwork network;
	network.set_latency_usec(20000);
	Ref<TickLocalTransport> server_transport = network.add_peer();
	Ref<TickLocalTransport> client_transport = network.add_peer();
	TickSyncCore server;
	TickSyncCore client;
	// The server's own object, and the client's predicted one.
	RudeMover server_npc("npc", 1, &server);
	RudeMover server_player("player", 2, &server);
	TestMover client_npc("npc", 1);
	RudeMover client_player("player", 2, &client);
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
	REQUIRE(client.is_predicting());

	// Both stop in the middle of a tick, as a game does when a match ends.
	server_npc.stop = true;
	server_npc.act_at = server.get_frame() + 2;
	client_player.stop = true;
	client_player.act_at = client.get_frame() + 2;
	for (int i = 0; i < 10; i++) {
		network.process(1.0 / 60.0);
		if (server.is_running()) {
			server.process(1.0 / 60.0, network.get_time_usec());
		}
		if (client.is_running()) {
			client.process(1.0 / 60.0, network.get_time_usec());
		}
	}
	CHECK_FALSE(server.is_running());
	CHECK_FALSE(client.is_running());
	CHECK(server.get_net_id(&server_npc) == 0);

	// Both can start again afterwards.
	CHECK(server.start(server_transport, network.get_time_usec()) == OK);
	CHECK(server.get_net_id(&server_npc) != 0);
	server.stop();
}


// In a distributed mesh, an owner can remove its own object in its tick, and an object's tick can stop the network.
TEST_CASE("[Modules][TickSynchronizer][MeshCore] Game code can remove objects and stop the network from a tick") {
	TickLocalNetwork network;
	Ref<TickLocalTransport> transport = network.add_peer();
	TickMeshCore core;
	TickEngine::Settings settings;
	settings.trusted = true;
	core.set_settings(settings);
	RudeMover leaver("leaver", 1, &core);
	RudeMover stopper("stopper", 1, &core);
	core.register_object(&leaver);
	core.register_object(&stopper);
	REQUIRE(core.start(transport, 0) == OK);
	for (int i = 0; i < 10; i++) {
		network.process(1.0 / 60.0);
		core.process(1.0 / 60.0, network.get_time_usec());
	}
	REQUIRE(core.get_net_id(&leaver) != 0);

	// The owner removes its own object in its tick, then another object stops the network.
	const uint32_t frame = core.get_frame() + 2;
	leaver.remove = &leaver;
	leaver.act_at = frame;
	stopper.stop = true;
	stopper.act_at = frame + 3;
	for (int i = 0; i < 10 && core.is_running(); i++) {
		network.process(1.0 / 60.0);
		core.process(1.0 / 60.0, network.get_time_usec());
	}
	CHECK(leaver.get_ticks(frame) == 1);
	CHECK(leaver.get_ticks(frame + 1) == 0);
	CHECK_FALSE(core.is_running());
	core.unregister_object(&stopper);
}

} // namespace TestTickGameCode

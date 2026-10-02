// Benchmarks of `TickSyncCore`: a server with hundreds of objects and many clients over the simulated network, with and
// without interest. They measure the time per tick and the bytes each client gets, and print them.
//
// Not matched by `--test-case="*TickSynchronizer*"`; run them with `--test-case="*TickSyncBenchmark*"`.
//
// `BenchmarkWorld` builds the server, the clients and their objects; `QuarterInterest` is an interest filter that
// leaves each client a quarter of the NPCs.

#pragma once

#include "test_tick_sync_core.h"

#include "core/os/os.h"
#include "tests/test_macros.h"

namespace TestTickBenchmark {

using TestTickSyncCore::TestMover;

// A server with `p_npcs` moving NPCs and `p_clients` clients, each controlling one player, over a simulated network.
struct BenchmarkWorld {
	TickLocalNetwork network;
	int client_count = 0;
	int npc_count = 0;
	TickSyncCore server;
	TickSyncCore *clients = nullptr;
	// `objects[peer]`: the NPCs, then the players; peer 0 is the server.
	LocalVector<TestMover *> objects[65];
	uint64_t server_usec = 0;
	uint64_t client_usec = 0;

	// Builds the world: every peer gets its own copy of the NPCs and of the players, then the server and the clients
	// start and connect.
	BenchmarkWorld(int p_clients, int p_npcs) :
			client_count(p_clients), npc_count(p_npcs) {
		network.set_seed(99);
		network.set_latency_usec(30000);
		network.set_jitter_usec(5000);
		clients = memnew_arr(TickSyncCore, p_clients);
		for (int peer = 0; peer <= p_clients; peer++) {
			for (int i = 0; i < p_npcs; i++) {
				TestMover *npc = memnew(TestMover(vformat("npc_%d", i), 1));
				npc->constant_direction = true;
				npc->direction_override = (i % 3) - 1;
				objects[peer].push_back(npc);
			}
			for (int client = 0; client < p_clients; client++) {
				TestMover *player = memnew(TestMover(vformat("player_%d", client), client + 2));
				player->script_length = 1000000;
				objects[peer].push_back(player);
			}
		}
		network.add_peer();
		for (TestMover *object : objects[0]) {
			server.register_object(object);
		}
		CHECK(server.start(network.get_peer(1), 0) == OK);
		for (int client = 0; client < p_clients; client++) {
			Ref<TickLocalTransport> transport = network.add_peer();
			for (TestMover *object : objects[client + 1]) {
				clients[client].register_object(object);
			}
			CHECK(clients[client].start(transport, 0) == OK);
			network.connect_peers(1, client + 2);
		}
	}


	// Stops the engines and frees the objects.
	~BenchmarkWorld() {
		server.stop();
		for (int client = 0; client < client_count; client++) {
			clients[client].stop();
		}
		memdelete_arr(clients);
		for (int peer = 0; peer <= client_count; peer++) {
			for (TestMover *object : objects[peer]) {
				memdelete(object);
			}
		}
	}


	// Runs the world for `p_seconds` at 60 frames per second; with `p_measure`, adds up the time the server and the
	// clients take.
	void run(double p_seconds, bool p_measure) {
		const int frames = int(p_seconds * 60.0);
		for (int i = 0; i < frames; i++) {
			const double delta = 1.0 / 60.0;
			network.process(delta);
			const uint64_t now = network.get_time_usec();
			uint64_t start = OS::get_singleton()->get_ticks_usec();
			server.process(delta, now);
			const uint64_t server_end = OS::get_singleton()->get_ticks_usec();
			for (int client = 0; client < client_count; client++) {
				clients[client].process(delta, now);
			}
			if (p_measure) {
				server_usec += server_end - start;
				client_usec += OS::get_singleton()->get_ticks_usec() - server_end;
			}
		}
	}
};

// 16 clients and 400 NPCs, all relevant: the clients decode every snapshot and their NPCs follow the server's. Prints
// the cost per tick and per client.
TEST_CASE("[Modules][TickSyncBenchmark] Server with many objects and clients") {
	const int clients = 16;
	const int npcs = 400;
	BenchmarkWorld world(clients, npcs);
	world.run(3.0, false);
	const uint64_t bytes_before = world.network.get_sent_bytes();
	const double seconds = 10.0;
	world.run(seconds, true);
	const double ticks = seconds * 60.0;
	const double bytes = double(world.network.get_sent_bytes() - bytes_before);
	CHECK(world.clients[0].is_predicting());
	// The clients really decode the snapshots: every frame arrives, and the interpolated NPCs follow the server.
	CHECK(world.clients[0].get_stats().snapshots_received > uint64_t(ticks * 0.9));
	CHECK(world.clients[0].get_stats().malformed_packets == 0);
	CHECK(world.objects[1][2]->position.distance_to(world.objects[0][2]->position) < 1.0);
	CHECK(world.objects[1][2]->position.length() > 10.0);
	MESSAGE(vformat("%d clients, %d NPCs: server %.1f us/tick, one client %.1f us/tick, %.1f KB/s to each client, %d rewinds, %d split snapshots",
			clients, npcs, double(world.server_usec) / ticks, double(world.client_usec) / ticks / clients,
			bytes / seconds / 1024.0 / clients, int(world.clients[0].get_stats().rewinds), int(world.server.get_stats().split_snapshots)));
}

// Each client sees a quarter of the NPCs (a stand-in for the game's distance or room checks).
class QuarterInterest : public TickSyncCore::Listener {
public:
	// Players are always relevant; an NPC is relevant to the peers whose id has the same remainder by 4 as its number.
	virtual int filter_relevance(int p_peer, TickSyncObject *p_object) override {
		const String path = p_object->get_sync_path();
		if (!path.begins_with("npc_")) {
			return 1;
		}
		return path.get_slice("_", 1).to_int() % 4 == p_peer % 4 ? 1 : 0;
	}
};

// The same world with a quarter of the NPCs relevant to each client: the relevant ones follow the server, the others
// never move.
TEST_CASE("[Modules][TickSyncBenchmark] Server with many objects and clients, with interest") {
	const int clients = 16;
	const int npcs = 400;
	BenchmarkWorld world(clients, npcs);
	QuarterInterest interest;
	world.server.set_listener(&interest);
	world.run(3.0, false);
	const uint64_t bytes_before = world.network.get_sent_bytes();
	const double seconds = 10.0;
	world.run(seconds, true);
	const double ticks = seconds * 60.0;
	const double bytes = double(world.network.get_sent_bytes() - bytes_before);
	CHECK(world.clients[0].get_stats().snapshots_received > uint64_t(ticks * 0.9));
	CHECK(world.clients[0].get_stats().malformed_packets == 0);
	// Client 0 is peer 2: it sees the NPCs 2, 6, 10...
	CHECK(world.objects[1][2]->position.distance_to(world.objects[0][2]->position) < 1.0);
	CHECK(world.objects[1][5]->position == Vector2());
	MESSAGE(vformat("%d clients, %d NPCs, a quarter relevant to each client: server %.1f us/tick, one client %.1f us/tick, %.1f KB/s to each client, %d split snapshots",
			clients, npcs, double(world.server_usec) / ticks, double(world.client_usec) / ticks / clients,
			bytes / seconds / 1024.0 / clients, int(world.server.get_stats().split_snapshots)));
}

} // namespace TestTickBenchmark

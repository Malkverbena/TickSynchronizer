#pragma once

#include "../source/sync/tick_sync_core.h"
#include "../source/transport/tick_local_transport.h"

#include "tests/test_macros.h"

namespace TestTickSyncCore {

// A body moving on the x axis. Its input is a direction in [-1, 1]; `script` gives the direction of each tick.
class TestMover : public TickSyncObject {
public:
	String path;
	int controller = 1;
	TickSchema schema;
	Vector2 position;
	Vector2 velocity;
	int ticks_collected = 0;
	int script_length = 0;
	bool constant_direction = false;
	int direction_override = 0;

	TestMover(const String &p_path, int p_controller, TickCodec::Precision p_precision = TickCodec::PRECISION_HALF) :
			path(p_path), controller(p_controller) {
		schema.add("position", TickCodec::vector2(p_precision));
		schema.add("velocity", TickCodec::vector2(p_precision));
	}

	int get_direction(int p_tick) const {
		if (constant_direction) {
			return direction_override;
		}
		if (p_tick >= script_length) {
			return 0;
		}
		return ((p_tick / 20) % 3) - 1;
	}

	virtual String get_sync_path() const override { return path; }
	virtual int get_controller_peer() const override { return controller; }
	virtual const TickSchema &get_sync_schema() const override { return schema; }

	virtual Variant get_sync_var(int p_index) const override {
		return p_index == 0 ? position : velocity;
	}

	virtual void set_sync_var(int p_index, const Variant &p_value) override {
		if (p_index == 0) {
			position = p_value;
		} else {
			velocity = p_value;
		}
	}

	virtual void collect_input(TickDataBuffer &r_input) override {
		r_input.add_int_bits(get_direction(ticks_collected++), 2);
	}

	virtual void process_tick(double p_delta, TickDataBuffer &p_input) override {
		int direction = 0;
		if (p_input.size() > 0) {
			direction = int(p_input.read_int_bits(2));
		}
		velocity = Vector2(real_t(direction) * 5.0f, 0.0f);
		position += velocity * real_t(p_delta);
	}
};

class TestListener : public TickSyncCore::Listener {
public:
	int ready_peers = 0;
	int prediction_started = 0;
	int rewinds = 0;
	int spawns = 0;
	int despawns = 0;
	uint32_t last_despawn = 0;
	String rejected;

	virtual void on_peer_ready(int p_peer) override { ready_peers++; }
	virtual void on_prediction_started(uint32_t p_frame) override { prediction_started++; }
	virtual void on_rewound(uint32_t p_frame, int p_frame_count) override { rewinds++; }
	virtual void on_rejected(const String &p_reason) override { rejected = p_reason; }
	virtual void on_spawn(const String &p_spawner, uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override { spawns++; }
	virtual void on_despawn(const String &p_spawner, uint32_t p_spawn_id) override {
		despawns++;
		last_despawn = p_spawn_id;
	}
};

// A server and two clients in a star, over a simulated network.
struct TestWorld {
	TickLocalNetwork network;
	Ref<TickLocalTransport> server_transport;
	Ref<TickLocalTransport> client_a_transport;
	Ref<TickLocalTransport> client_b_transport;
	TickSyncCore server;
	TickSyncCore client_a;
	TickSyncCore client_b;
	TestListener server_listener;
	TestListener client_a_listener;
	TestListener client_b_listener;

	// The same objects exist on every peer, as if spawned by the game.
	TestMover server_mover_a = TestMover("mover_a", 2);
	TestMover server_mover_b = TestMover("mover_b", 3);
	TestMover server_npc = TestMover("npc", 1);
	TestMover a_mover_a = TestMover("mover_a", 2);
	TestMover a_mover_b = TestMover("mover_b", 3);
	TestMover a_npc = TestMover("npc", 1);
	TestMover b_mover_a = TestMover("mover_a", 2);
	TestMover b_mover_b = TestMover("mover_b", 3);
	TestMover b_npc = TestMover("npc", 1);

	TestWorld(uint64_t p_latency_usec, uint64_t p_jitter_usec, double p_packet_loss, int p_script_length) {
		network.set_seed(12345);
		network.set_latency_usec(p_latency_usec);
		network.set_jitter_usec(p_jitter_usec);
		network.set_packet_loss(p_packet_loss);

		server_transport = network.add_peer();
		client_a_transport = network.add_peer();
		client_b_transport = network.add_peer();

		TestMover *movers[] = { &server_mover_a, &server_mover_b, &server_npc, &a_mover_a, &a_mover_b, &a_npc, &b_mover_a, &b_mover_b, &b_npc };
		for (TestMover *mover : movers) {
			mover->script_length = p_script_length;
		}

		server.set_listener(&server_listener);
		client_a.set_listener(&client_a_listener);
		client_b.set_listener(&client_b_listener);

		server.register_object(&server_mover_a);
		server.register_object(&server_mover_b);
		server.register_object(&server_npc);
		client_a.register_object(&a_mover_a);
		client_a.register_object(&a_mover_b);
		client_a.register_object(&a_npc);
		client_b.register_object(&b_mover_a);
		client_b.register_object(&b_mover_b);
		client_b.register_object(&b_npc);

		CHECK(server.start(server_transport, network.get_time_usec()) == OK);
		CHECK(client_a.start(client_a_transport, network.get_time_usec()) == OK);
		CHECK(client_b.start(client_b_transport, network.get_time_usec()) == OK);
		network.connect_peers(1, 2);
		network.connect_peers(1, 3);
	}

	// Simulates `p_seconds` of game frames at 60 FPS, with a slightly irregular frame time.
	void run(double p_seconds) {
		const int frames = int(p_seconds * 60.0);
		for (int i = 0; i < frames; i++) {
			const double delta = (i % 2 == 0) ? 0.016 : 0.0173333;
			network.process(delta);
			const uint64_t now = network.get_time_usec();
			server.process(delta, now);
			client_a.process(delta, now);
			client_b.process(delta, now);
		}
	}
};

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] The interpolation timeline doesn't jump with jitter") {
	// 40 ms each way with up to 120 ms of jitter per packet: the clock's estimate moves by milliseconds whenever its best
	// sample changes, and the frames take alternating times. The frame the client interpolates at follows smoothly
	// (ADR-071): the estimate is slewed, and the server's epoch doesn't vary with its frame time.
	TestWorld world(40000, 120000, 0.0, 0);
	world.run(3.0);
	REQUIRE(world.client_a.get_clock().is_synchronized());
	uint64_t last_usec = world.network.get_time_usec();
	double last = world.client_a.get_view_frame(last_usec);
	int64_t last_estimate = world.client_a.get_clock().get_offset_usec();
	int estimate_changes = 0;
	double worst = 0.0;
	for (int i = 0; i < 600; i++) {
		world.run(1.0 / 60.0);
		const uint64_t now = world.network.get_time_usec();
		const double frame = world.client_a.get_view_frame(now);
		// The frames the elapsed time accounts for, at 60 per second.
		const double expected = double(now - last_usec) * 60.0 / 1000000.0;
		worst = MAX(worst, Math::abs((frame - last) - expected));
		if (world.client_a.get_clock().get_offset_usec() != last_estimate) {
			last_estimate = world.client_a.get_clock().get_offset_usec();
			estimate_changes++;
		}
		last = frame;
		last_usec = now;
	}
	CHECK(estimate_changes > 0);
	// At most 5% faster or slower than the elapsed time: no jump.
	CHECK(worst < 0.06);
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Handshake, clock and registration") {
	TestWorld world(30000, 5000, 0.0, 0);
	world.run(1.0);

	CHECK(world.server.is_server());
	CHECK(world.server_listener.ready_peers == 2);
	CHECK(world.client_a_listener.ready_peers == 1);
	CHECK(world.client_a.is_welcomed());
	CHECK(world.client_a.get_clock().is_synchronized());
	CHECK(world.client_a.is_predicting());
	CHECK(world.client_a_listener.prediction_started == 1);

	// The clock estimate matches the simulated latency.
	CHECK(world.client_a.get_clock().get_rtt_usec() >= 60000);
	CHECK(world.client_a.get_clock().get_rtt_usec() <= 80000);

	// Objects are bound to the server's objects by path.
	CHECK(world.client_a.get_net_id(&world.a_mover_a) == world.server.get_net_id(&world.server_mover_a));
	CHECK(world.client_b.get_net_id(&world.b_npc) == world.server.get_net_id(&world.server_npc));
	CHECK(world.client_a.get_net_id(&world.a_mover_a) != 0);

	// The client runs ahead of the server.
	CHECK(tick_frame_after(world.client_a.get_frame(), world.server.get_frame()));
	CHECK(world.server.get_stats().full_snapshots_sent >= 2);
	CHECK(world.server.get_stats().delta_snapshots_sent > 0);
	CHECK(world.client_a.get_stats().malformed_packets == 0);
	CHECK(world.server.get_stats().malformed_packets == 0);
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Deterministic simulation never rewinds after the start") {
	TestWorld world(40000, 5000, 0.0, 100000);
	world.run(3.0);
	const uint64_t a_rewinds_after_start = world.client_a.get_stats().rewinds;
	const uint64_t b_rewinds_after_start = world.client_b.get_stats().rewinds;
	world.run(10.0);

	// Quantized, deterministic simulation: the predictions match the server exactly (zero spurious rewinds).
	CHECK(world.client_a.get_stats().rewinds == a_rewinds_after_start);
	CHECK(world.client_b.get_stats().rewinds == b_rewinds_after_start);
	CHECK(world.server.get_stats().ghost_inputs == 0);
	CHECK(world.server.get_stats().late_inputs == 0);
	// The speed control keeps the client near real time.
	CHECK(Math::abs(world.client_a.get_time_scale() - 1.0) <= 0.1);

	// The predicted object is ahead of the server's; the remote ones are interpolated behind it.
	CHECK(world.a_mover_b.position.distance_to(world.server_mover_b.position) <= 5.0 * 0.5);
	CHECK(world.a_npc.position.distance_to(world.server_npc.position) <= 5.0 * 0.5);
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Packet loss and jitter converge to the server state") {
	// 260 ticks: four whole left-stop-right cycles plus 20 ticks to the left, so the bodies end away from the origin.
	TestWorld world(50000, 20000, 0.1, 260);
	world.run(12.0);

	CHECK(world.client_a.is_predicting());
	CHECK(world.client_a.get_stats().snapshots_received > 100);
	// The scripts ended: every body stopped, and the predicted and interpolated copies agree with the server.
	const Ref<TickCodec> codec = TickCodec::vector2(TickCodec::PRECISION_HALF);
	CHECK(codec->is_equal(world.a_mover_a.position, world.server_mover_a.position));
	CHECK(codec->is_equal(world.b_mover_b.position, world.server_mover_b.position));
	CHECK(codec->is_equal(world.a_mover_b.position, world.server_mover_b.position));
	CHECK(codec->is_equal(world.b_npc.position, world.server_npc.position));
	// The bodies moved.
	CHECK(world.server_mover_a.position.length() > 1.0);
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] A diverged prediction is rewound and corrected") {
	TestWorld world(40000, 0, 0.0, 600);
	world.run(3.0);
	const uint64_t rewinds_before = world.client_a.get_stats().rewinds;

	// Something outside the synchronized simulation moves the predicted body.
	world.a_mover_a.position += Vector2(10.0, 0.0);
	world.run(0.5);
	CHECK(world.client_a.get_stats().rewinds > rewinds_before);
	CHECK(world.client_a.get_stats().rewound_frames > 0);

	world.run(10.0);
	const Ref<TickCodec> codec = TickCodec::vector2(TickCodec::PRECISION_HALF);
	CHECK(codec->is_equal(world.a_mover_a.position, world.server_mover_a.position));
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] A client can't move another client's object") {
	TestWorld world(20000, 0, 0.0, 0);
	world.run(1.5);
	const uint16_t target = world.server.get_net_id(&world.server_mover_a);
	REQUIRE(target != 0);

	// Client B (peer 3) sends inputs for client A's object (controlled by peer 2), for many frames ahead.
	for (int i = 0; i < 60; i++) {
		TickDataBuffer object_input;
		object_input.begin_write();
		object_input.add_int_bits(1, 2);

		TickDataBuffer frame_input;
		frame_input.begin_write();
		frame_input.add_uint_bits(1, 8);
		frame_input.add_uint_bits(target, 16);
		frame_input.add_data_buffer(object_input);

		TickDataBuffer message;
		message.begin_write();
		message.add_uint_bits(TICK_MESSAGE_INPUTS, 8);
		message.add_uint_bits(TICK_FRAME_NONE, 32);
		message.add_bool(false);
		message.add_uint_bits(1, 8);
		message.add_uint_bits(world.server.get_frame() + 1, 32);
		message.add_uint_bits(10, 8);
		message.add_data_buffer(frame_input);
		message.dry();
		world.client_b_transport->send(1, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, message.get_buffer().get_bytes().ptr(), int(message.get_buffer().get_bytes().size()));
		world.run(1.0 / 60.0);
	}
	world.run(0.5);
	// Client A never moved its object: the spoofed inputs were ignored.
	CHECK(world.server_mover_a.position == Vector2());
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Incompatible peers and schemas") {
	TickLocalNetwork network;
	Ref<TickLocalTransport> server_transport = network.add_peer();
	Ref<TickLocalTransport> raw_client = network.add_peer();
	Ref<TickLocalTransport> client_transport = network.add_peer();

	TickSyncCore server;
	TestMover server_mover("mover", 3, TickCodec::PRECISION_HALF);
	server.register_object(&server_mover);
	REQUIRE(server.start(server_transport, 0) == OK);

	// A client with a different codec for the same object.
	TickSyncCore client;
	TestListener client_listener;
	client.set_listener(&client_listener);
	TestMover client_mover("mover", 3, TickCodec::PRECISION_SINGLE);
	client.register_object(&client_mover);
	REQUIRE(client.start(client_transport, 0) == OK);

	network.connect_peers(1, 2);
	network.connect_peers(1, 3);

	// A peer speaking another protocol version.
	TickDataBuffer hello;
	hello.begin_write();
	hello.add_uint_bits(TICK_MESSAGE_HELLO, 8);
	hello.add_uint_bits(999, 16);
	hello.add_bool(sizeof(real_t) == sizeof(double));
	hello.dry();
	raw_client->send(1, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, hello.get_buffer().get_bytes().ptr(), int(hello.get_buffer().get_bytes().size()));

	ERR_PRINT_OFF;
	for (int i = 0; i < 150; i++) {
		network.process(1.0 / 60.0);
		server.process(1.0 / 60.0, network.get_time_usec());
		client.process(1.0 / 60.0, network.get_time_usec());
	}
	ERR_PRINT_ON;

	// The raw peer got the rejection and was disconnected.
	TickTransport::Packet packet;
	bool got_reject = false;
	while (raw_client->pop_packet(packet)) {
		got_reject = got_reject || packet.data[0] == TICK_MESSAGE_REJECT;
	}
	CHECK(got_reject);
	CHECK_FALSE(server_transport->is_peer_connected(2));

	// The compatible client joined, but the object with a different schema isn't synchronized.
	CHECK(client.is_welcomed());
	CHECK(client.get_net_id(&client_mover) == 0);
}

} // namespace TestTickSyncCore

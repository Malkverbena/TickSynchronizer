#pragma once

#include "../source/nodes/tick_object.h"
#include "test_tick_sync_core.h"

#include "tests/test_macros.h"

namespace TestTickMeshDolls {

using TestTickSyncCore::TestMover;

// A mover that the peers that don't control it simulate as a doll.
class DollMover : public TestMover {
public:
	bool doll = true;

	DollMover(const String &p_path, int p_controller) :
			TestMover(p_path, p_controller, TickCodec::PRECISION_SINGLE) {}

	virtual bool is_doll_enabled() const override { return doll; }
};

// Three players: 1 is the authority and plays too, 2 and 3 predict their own movers. Every peer has the three
// movers (dolls elsewhere) and an NPC of the authority (interpolated elsewhere). A mesh unless `p_star`.
struct DollWorld {
	TickLocalNetwork network;
	Ref<TickLocalTransport> transports[4];
	TickSyncCore cores[4];
	// `movers[peer][controller]`; `movers[peer][0]` is the NPC.
	DollMover *movers[4][4] = {};

	DollWorld(uint64_t p_latency_12, uint64_t p_latency_13, uint64_t p_latency_23, uint64_t p_jitter_usec, double p_packet_loss, int p_script_length, bool p_star = false) {
		network.set_seed(777);
		network.set_jitter_usec(p_jitter_usec);
		network.set_packet_loss(p_packet_loss);
		for (int peer = 1; peer <= 3; peer++) {
			transports[peer] = network.add_peer();
		}
		network.set_link_latency_usec(1, 2, p_latency_12);
		network.set_link_latency_usec(1, 3, p_latency_13);
		network.set_link_latency_usec(2, 3, p_latency_23);

		for (int peer = 1; peer <= 3; peer++) {
			for (int controller = 0; controller <= 3; controller++) {
				DollMover *mover = memnew(DollMover(controller == 0 ? String("npc") : vformat("mover_%d", controller), controller == 0 ? 1 : controller));
				mover->doll = controller != 0;
				mover->script_length = p_script_length;
				movers[peer][controller] = mover;
				cores[peer].register_object(mover);
			}
			TickSyncCore::Settings settings;
			cores[peer].set_settings(settings);
			CHECK(cores[peer].start(transports[peer], network.get_time_usec()) == OK);
		}
		if (p_star) {
			network.connect_peers(1, 2);
			network.connect_peers(1, 3);
		} else {
			network.connect_all();
		}
	}

	~DollWorld() {
		for (int peer = 1; peer <= 3; peer++) {
			cores[peer].stop();
			for (int controller = 0; controller <= 3; controller++) {
				memdelete(movers[peer][controller]);
			}
		}
	}

	void run(double p_seconds) {
		const int frames = int(p_seconds * 60.0);
		for (int i = 0; i < frames; i++) {
			const double delta = (i % 2 == 0) ? 0.016 : 0.0173333;
			network.process(delta);
			const uint64_t now = network.get_time_usec();
			for (int peer = 1; peer <= 3; peer++) {
				cores[peer].process(delta, now);
			}
		}
	}

	Vector2 position(int p_peer, int p_controller) const { return movers[p_peer][p_controller]->position; }
	uint64_t doll_rewinds(int p_peer) { return cores[p_peer].get_stats().doll_rewinds; }
};

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Mesh dolls follow their controller, delayed by the latency to it") {
	// The direct link between 2 and 3 is faster than the path through the authority.
	DollWorld fast(40000, 40000, 10000, 0, 0.0, 100000);
	// Here it's slower.
	DollWorld slow(40000, 40000, 90000, 0, 0.0, 100000);
	fast.run(3.0);
	slow.run(3.0);
	const uint64_t fast_rewinds = fast.doll_rewinds(2) + fast.doll_rewinds(3);
	const uint64_t slow_rewinds = slow.doll_rewinds(2) + slow.doll_rewinds(3);
	const uint64_t fast_ghosts = fast.cores[2].get_stats().doll_ghost_inputs;
	fast.run(8.0);
	slow.run(8.0);

	for (int peer = 2; peer <= 3; peer++) {
		const int other = peer == 2 ? 3 : 2;
		CHECK(fast.cores[peer].is_predicting());
		// The other player and the authority's player are dolls; the NPC is interpolated.
		CHECK(fast.cores[peer].get_doll_delay(other) >= 0);
		CHECK(fast.cores[peer].get_doll_delay(1) >= 0);
		CHECK(fast.cores[peer].get_stats().malformed_packets == 0);
		CHECK(fast.cores[peer].get_stats().rejected_inputs == 0);
	}
	// The doll's delay is the latency to its controller (in frames) plus the input buffer.
	const int fast_delay = fast.cores[2].get_doll_delay(3);
	const int slow_delay = slow.cores[2].get_doll_delay(3);
	MESSAGE("Doll delays with a 10 ms and a 90 ms link: ", fast_delay, " and ", slow_delay, " frames.");
	CHECK(fast_delay >= 2);
	CHECK(fast_delay <= 5);
	CHECK(slow_delay >= fast_delay + 3);
	CHECK(slow_delay <= fast_delay + 8);

	// A deterministic simulation: the dolls agree with the authority, with no rewinds after the start.
	CHECK(fast.doll_rewinds(2) + fast.doll_rewinds(3) == fast_rewinds);
	CHECK(slow.doll_rewinds(2) + slow.doll_rewinds(3) == slow_rewinds);
	CHECK(fast.cores[2].get_stats().doll_ghost_inputs == fast_ghosts);
	CHECK(fast.cores[2].get_stats().doll_corrections == 0);
	CHECK(fast.position(3, 3).length() > 1.0);
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Dolls converge with jitter and packet loss") {
	// 260 ticks: the bodies end away from the origin (see the core tests).
	DollWorld world(40000, 50000, 20000, 20000, 0.1, 260);
	world.run(12.0);

	const Ref<TickCodec> codec = TickCodec::vector2(TickCodec::PRECISION_SINGLE);
	for (int peer = 1; peer <= 3; peer++) {
		for (int controller = 0; controller <= 3; controller++) {
			CHECK(codec->is_equal(world.position(peer, controller), world.position(1, controller)));
		}
	}
	CHECK(world.position(1, 2).length() > 1.0);
	CHECK(world.cores[2].get_doll_delay(3) >= 0);
	CHECK(world.cores[2].get_stats().malformed_packets == 0);
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] A diverged doll is rewound to the authority's state") {
	DollWorld world(30000, 30000, 20000, 0, 0.0, 400);
	world.run(3.0);
	const uint64_t rewinds = world.doll_rewinds(2);

	// Something outside the synchronized simulation moves the doll.
	world.movers[2][3]->position += Vector2(10.0, 0.0);
	world.run(0.5);
	CHECK(world.doll_rewinds(2) > rewinds);
	CHECK(world.cores[2].get_stats().doll_rewound_frames > 0);

	world.run(6.0);
	const Ref<TickCodec> codec = TickCodec::vector2(TickCodec::PRECISION_SINGLE);
	CHECK(codec->is_equal(world.position(2, 3), world.position(1, 3)));
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] A peer can't drive another peer's doll") {
	DollWorld world(20000, 20000, 20000, 0, 0.0, 0);
	world.run(2.0);
	const uint16_t target = world.cores[1].get_net_id(world.movers[1][1]);
	REQUIRE(target != 0);
	REQUIRE(world.cores[2].get_doll_delay(1) >= 0);

	// Peer 3 sends peer 2 inputs for the authority's mover, which stands still.
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
		message.add_uint_bits(world.cores[2].get_frame() - 10, 32);
		message.add_uint_bits(20, 8);
		message.add_data_buffer(frame_input);
		message.dry();
		world.transports[3]->send(2, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, message.get_buffer().get_bytes().ptr(), int(message.get_buffer().get_bytes().size()));
		world.run(1.0 / 60.0);
		// The doll of the authority's mover never moves.
		CHECK(world.position(2, 1) == Vector2());
	}
	CHECK(world.doll_rewinds(2) == 0);
}

TEST_CASE("[Modules][TickSynchronizer][TickSyncCore] Without direct inputs, dolls are interpolated") {
	// A star: the clients only reach the server, so only the server's objects can be dolls.
	DollWorld star(30000, 30000, 30000, 0, 0.0, 200, true);
	star.run(3.0);
	CHECK(star.cores[2].get_doll_delay(1) >= 0);
	CHECK(star.cores[2].get_doll_delay(3) == -1);

	// A mesh where the link between 2 and 3 goes down: 3's mover is interpolated again on 2.
	DollWorld mesh(30000, 30000, 30000, 0, 0.0, 200);
	mesh.run(3.0);
	CHECK(mesh.cores[2].get_doll_delay(3) >= 0);
	mesh.network.disconnect_peers(2, 3);
	mesh.run(1.0);
	CHECK(mesh.cores[2].get_doll_delay(3) == -1);

	star.run(3.0);
	mesh.run(3.0);
	const Ref<TickCodec> codec = TickCodec::vector2(TickCodec::PRECISION_SINGLE);
	for (int controller = 0; controller <= 3; controller++) {
		CHECK(codec->is_equal(star.position(2, controller), star.position(1, controller)));
		CHECK(codec->is_equal(mesh.position(2, controller), mesh.position(1, controller)));
	}
	CHECK(mesh.position(1, 3).length() > 1.0);
}

TEST_CASE("[Modules][TickSynchronizer][TickObject] Remote mode") {
	TickObject *object = memnew(TickObject);
	CHECK(object->get_remote_mode() == TickObject::REMOTE_MODE_INTERPOLATE);
	CHECK_FALSE(object->is_doll_enabled());
	object->set_remote_mode(TickObject::REMOTE_MODE_DOLL);
	CHECK(object->is_doll_enabled());
	CHECK(int(object->get("remote_mode")) == int(TickObject::REMOTE_MODE_DOLL));
	memdelete(object);
}

} // namespace TestTickMeshDolls

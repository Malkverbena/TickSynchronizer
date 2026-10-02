// Tests of what a peer can't make another one pay for (audit of 2026-10-01, ADR-078): a controller that makes the
// other players simulate its doll over and over, pings without end, a client that takes snapshots and never
// acknowledges them, reals that aren't finite inside an event, a tick rate no network runs at. Each case sends what a
// program that isn't this module could send, through the transport of a peer whose engine was stopped.

#pragma once

#include "../source/sync/tick_protocol.h"
#include "../source/sync/tick_sync_core.h"
#include "test_tick_fuzz.h"

#include "core/io/marshalls.h"
#include "core/os/os.h"
#include "tests/test_macros.h"

namespace TestTickLimits {

using TestTickFuzz::FuzzBody;
using TestTickFuzz::Fuzzer;
using TestTickFuzz::SyncWorld;


// Writes an inputs message as a client sends it: `p_frames` frames from `p_first_frame`, each with an input for the
// object `p_net_id` (the input of a `FuzzBody`, chosen at random).
static void craft_inputs(LocalVector<uint8_t> &r_bytes, uint32_t p_first_frame, int p_frames, uint16_t p_net_id, Fuzzer &r_fuzzer) {
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_INPUTS, 8);
	message.add_uint_bits(TICK_FRAME_NONE, 32);
	message.add_bool(false);
	message.add_uint_bits(uint64_t(p_frames), 8);
	message.add_uint_bits(p_first_frame, 32);
	for (int i = 0; i < p_frames; i++) {
		TickDataBuffer input;
		input.begin_write();
		input.add_int_bits(r_fuzzer.range(-1, 1), 2);
		input.add_bool(r_fuzzer.chance(50));
		(void)input.add_vector2(Vector2(real_t(r_fuzzer.range(0, 6)), 1.0), TickDataBuffer::COMPRESSION_LEVEL_2);
		TickDataBuffer frame_input;
		frame_input.begin_write();
		frame_input.add_uint_bits(1, 8);
		frame_input.add_uint_bits(p_net_id, 16);
		frame_input.add_data_buffer(input);
		message.add_uint_bits(0, 8);
		message.add_data_buffer(frame_input);
	}
	message.dry();
	r_bytes = message.get_buffer().get_bytes();
}


// Takes everything a transport received, counting the full snapshots and the pongs among it.
static void drain(const Ref<TickLocalTransport> &p_transport, int *r_fulls = nullptr, int *r_pongs = nullptr, bool *r_disconnected = nullptr) {
	TickTransport::Packet packet;
	while (p_transport->pop_packet(packet)) {
		if (packet.data.is_empty()) {
			continue;
		}
		if (r_fulls && packet.data[0] == TICK_MESSAGE_SNAPSHOT_FULL) {
			(*r_fulls)++;
		}
		if (r_pongs && packet.data[0] == TICK_MESSAGE_PONG) {
			(*r_pongs)++;
		}
	}
	TickTransport::Event event;
	while (p_transport->pop_event(event)) {
		if (r_disconnected && event.type == TickTransport::EVENT_PEER_DISCONNECTED && event.peer == 1) {
			*r_disconnected = true;
		}
	}
}


// In a mesh, player 3 stops playing fair: every tick it sends player 2 inputs for frames `p_ahead` ahead of player
// 2's own, which aren't the ones it sends the host (it sends the host nothing). Returns how many times player 2
// simulated player 3's doll in 5 seconds, and what it did about it.
static int hostile_doll_ticks(int p_ahead, int *r_honest, uint64_t *r_suspensions, uint64_t *r_rejected) {
	Fuzzer fuzzer(7);
	SyncWorld world(true, false, 7);
	for (int i = 0; i < 300; i++) {
		world.step();
	}
	REQUIRE(world.cores[2].is_predicting());
	FuzzBody *doll = world.bodies[2][3];
	int before = doll->process_calls;
	for (int i = 0; i < 300; i++) {
		world.step();
	}
	*r_honest = doll->process_calls - before;
	const uint16_t net_id = world.cores[2].get_net_id(doll);
	REQUIRE(net_id != 0);

	world.cores[3].stop();
	world.live[3] = false;
	before = doll->process_calls;
	const uint64_t suspensions_before = world.cores[2].get_stats().doll_suspensions;
	const uint64_t rejected_before = world.cores[2].get_stats().rejected_inputs;
	for (int i = 0; i < 300; i++) {
		LocalVector<uint8_t> bytes;
		craft_inputs(bytes, world.cores[2].get_frame() + uint32_t(p_ahead), 7, net_id, fuzzer);
		world.raw[3]->send(2, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, bytes.ptr(), int(bytes.size()));
		drain(world.raw[3]);
		world.step();
	}
	*r_suspensions = world.cores[2].get_stats().doll_suspensions - suspensions_before;
	*r_rejected = world.cores[2].get_stats().rejected_inputs - rejected_before;
	CHECK(world.cores[2].is_running());
	return doll->process_calls - before;
}


// A doll runs on its controller's inputs, and is simulated again from the authority's state whenever the two
// disagree. A controller that sends the other players inputs that aren't the ones it sends the authority made them
// simulate its doll up to 46 times a tick. Now the frames simulated again have a budget, a doll that uses it up
// follows the snapshots for a while, and inputs far ahead of the timeline aren't taken.
TEST_CASE("[Modules][TickSynchronizer][Limits] A controller can't make the other players simulate its doll many times over") {
	ERR_PRINT_OFF;
	static const int distances[4] = { 0, 20, 30, 50 };
	for (const int ahead : distances) {
		int honest = 0;
		uint64_t suspensions = 0;
		uint64_t rejected = 0;
		const int hostile = hostile_doll_ticks(ahead, &honest, &suspensions, &rejected);
		ERR_PRINT_ON;
		// One tick per frame with an honest controller.
		CHECK(honest >= 295);
		CHECK(honest <= 305);
		// The budget is two more per frame, with two seconds' worth at once: 5 seconds cost 4.5 times at the very most.
		CHECK_MESSAGE(hostile <= honest * 9 / 2, vformat("Inputs %d frames ahead: %d doll ticks in 5 s (%d with an honest controller).", ahead, hostile, honest));
		if (ahead >= 50) {
			// Further ahead than any honest controller: the inputs aren't even kept.
			CHECK(rejected >= 1500);
			CHECK(hostile <= honest);
		} else {
			CHECK(suspensions >= 1);
		}
		ERR_PRINT_OFF;
	}
	ERR_PRINT_ON;
}


// The server answered every ping, from any connected peer: 20 000 pings a second cost it 20 000 answers. An untrusted
// client now gets as many answers as a clock needs.
TEST_CASE("[Modules][TickSynchronizer][Limits] A client gets so many pings answered") {
	ERR_PRINT_OFF;
	SyncWorld world(false, false, 3);
	for (int i = 0; i < 200; i++) {
		world.step();
	}
	world.cores[3].stop();
	world.live[3] = false;
	drain(world.raw[3]);
	const uint64_t limited_before = world.cores[1].get_stats().rate_limited_packets;

	int pongs = 0;
	for (int step = 0; step < 60; step++) {
		for (int i = 0; i < 334; i++) {
			TickDataBuffer ping;
			ping.begin_write();
			ping.add_uint_bits(TICK_MESSAGE_PING, 8);
			ping.add_uint_bits(uint64_t(i), 64);
			ping.dry();
			const LocalVector<uint8_t> &data = ping.get_buffer().get_bytes();
			world.raw[3]->send(1, TICK_CHANNEL_STATS, TickTransport::TRANSFER_MODE_UNRELIABLE, data.ptr(), int(data.size()));
		}
		world.step();
		drain(world.raw[3], nullptr, &pongs);
	}
	for (int step = 0; step < 10; step++) {
		world.step();
		drain(world.raw[3], nullptr, &pongs);
	}
	ERR_PRINT_ON;
	// 120 a second at 60 ticks per second, and as many at once; the rest are counted, not answered.
	CHECK(pongs <= 250);
	CHECK(pongs >= 60);
	CHECK(world.cores[1].get_stats().rate_limited_packets - limited_before >= 19000);
	// The honest client isn't affected.
	CHECK(world.cores[2].is_predicting());
	CHECK(world.cores[2].get_clock().is_synchronized());
}


// A client that never acknowledged a snapshot got a full one, reliable, every half second for as long as it stayed.
// Now the wait doubles each time (up to 8 seconds), nothing is sent in between once the last full snapshot is too old
// to be the base of a delta, and an untrusted client that acknowledges nothing for 30 seconds is dropped.
TEST_CASE("[Modules][TickSynchronizer][Limits] A client that never acknowledges gets fewer snapshots, then none") {
	ERR_PRINT_OFF;
	SyncWorld world(false, false, 4);
	for (int i = 0; i < 200; i++) {
		world.step();
	}
	world.cores[3].stop();
	world.live[3] = false;
	drain(world.raw[3]);

	// The first 10 seconds: a full snapshot at about 0, 0.5, 1.5, 3.5 and 7.5 seconds.
	int fulls = 0;
	bool disconnected = false;
	for (int step = 0; step < 600; step++) {
		world.step();
		drain(world.raw[3], &fulls, nullptr, &disconnected);
	}
	CHECK(fulls >= 3);
	CHECK(fulls <= 6);
	CHECK_FALSE(disconnected);
	// The next 25: one every 8 seconds, until the server gives the client up.
	for (int step = 0; step < 1500; step++) {
		world.step();
		drain(world.raw[3], &fulls, nullptr, &disconnected);
	}
	ERR_PRINT_ON;
	CHECK(fulls <= 10);
	CHECK(disconnected);
	CHECK_FALSE(world.raw[1]->is_peer_connected(3));
	// The client that plays stays.
	CHECK(world.raw[1]->is_peer_connected(2));
	CHECK(world.cores[2].is_predicting());
}


// Records the events a body receives.
class EventBody : public FuzzBody {
public:
	int received = 0;
	Variant last_payload;

	// A body controlled by peer `p_controller`.
	EventBody(const String &p_path, int p_controller) :
			FuzzBody(p_path, p_controller, false) {}


	// Keeps the payload of the event.
	virtual void on_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override {
		received++;
		last_payload = p_payload;
	}
};


// No codec carries a real that isn't finite, so it can't spread through a simulation; a payload is a `Variant`, and
// went through unchecked. Now the sender refuses it, and the receiver refuses the message of a sender that isn't this
// module.
TEST_CASE("[Modules][TickSynchronizer][Limits] Reals that aren't finite don't travel inside a payload") {
	TickLocalNetwork network;
	Ref<TickLocalTransport> server_transport = network.add_peer();
	Ref<TickLocalTransport> client_transport = network.add_peer();
	TickSyncCore server;
	TickSyncCore client;
	EventBody server_body("body", 2);
	EventBody client_body("body", 2);
	server.register_object(&server_body);
	client.register_object(&client_body);
	REQUIRE(server.start(server_transport, 0) == OK);
	REQUIRE(client.start(client_transport, 0) == OK);
	network.connect_all();
	for (int i = 0; i < 120; i++) {
		network.process(1.0 / 60.0);
		server.process(1.0 / 60.0, network.get_time_usec());
		client.process(1.0 / 60.0, network.get_time_usec());
	}
	REQUIRE(client.is_predicting());

	// With the regular API.
	Dictionary nested;
	nested["position"] = Vector3(real_t(Math::INF), 0, real_t(-Math::INF));
	PackedFloat32Array floats;
	floats.push_back(1.0f);
	floats.push_back(float(NAN));
	ERR_PRINT_OFF;
	CHECK(client.send_event(&client_body, "aim", double(NAN), TICK_FRAME_NONE, 0) == ERR_INVALID_DATA);
	CHECK(client.send_event(&client_body, "move", nested, TICK_FRAME_NONE, 0) == ERR_INVALID_DATA);
	CHECK(client.send_event(&client_body, "curve", floats, TICK_FRAME_NONE, 0) == ERR_INVALID_DATA);
	CHECK(server.spawn("Spawner", 0, "Thing", 2, Color(0, float(NAN), 0)) == 0);
	ERR_PRINT_ON;
	CHECK(client.send_event(&client_body, "fine", Vector3(1, 2, 3), TICK_FRAME_NONE, 0) == OK);
	CHECK_FALSE(TickCodec::is_sendable(Variant(double(Math::INF))));
	CHECK(TickCodec::is_sendable(Variant(1.5)));

	// As another program would send it: the payload as the engine encodes it, NaN and all.
	const uint16_t net_id = client.get_net_id(&client_body);
	REQUIRE(net_id != 0);
	const Variant payload = double(NAN);
	int length = 0;
	REQUIRE(encode_variant(payload, nullptr, length, false) == OK);
	LocalVector<uint8_t> encoded;
	encoded.resize(length);
	encode_variant(payload, encoded.ptr(), length, false);
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_EVENT, 8);
	message.add_uint_bits(net_id, 16);
	message.add_uint_bits(TICK_FRAME_NONE, 32);
	message.add_string("aim");
	message.add_uint_bits(uint64_t(length), 16);
	message.add_bits(encoded.ptr(), length * 8);
	message.dry();
	const LocalVector<uint8_t> &bytes = message.get_buffer().get_bytes();
	const uint64_t malformed_before = server.get_stats().malformed_packets;
	client_transport->send(1, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, bytes.ptr(), int(bytes.size()));
	for (int i = 0; i < 60; i++) {
		network.process(1.0 / 60.0);
		server.process(1.0 / 60.0, network.get_time_usec());
		client.process(1.0 / 60.0, network.get_time_usec());
	}
	// Only the event with finite reals arrived.
	CHECK(server_body.received == 1);
	CHECK(server_body.last_payload == Variant(Vector3(1, 2, 3)));
	CHECK(server.get_stats().malformed_packets == malformed_before + 1);
	client.stop();
	server.stop();
	client.unregister_object(&client_body);
	server.unregister_object(&server_body);
}


// The server tells the client the tick rate of the network. One no network runs at isn't followed (it would be the
// client's CPU the server spends), and no engine takes it as a setting.
TEST_CASE("[Modules][TickSynchronizer][Limits] A tick rate no network runs at isn't taken") {
	TickEngine::Settings settings;
	settings.ticks_per_second = TICK_MAX_TICKS_PER_SECOND + 1;
	TickSyncCore core;
	ERR_PRINT_OFF;
	core.set_settings(settings);
	ERR_PRINT_ON;
	CHECK(core.get_settings().ticks_per_second == 60);
	settings.ticks_per_second = TICK_MAX_TICKS_PER_SECOND;
	core.set_settings(settings);
	CHECK(core.get_settings().ticks_per_second == TICK_MAX_TICKS_PER_SECOND);

	// A client, and a server that isn't this module: its welcome says 5000 ticks per second.
	TickLocalNetwork network;
	Ref<TickLocalTransport> server_transport = network.add_peer();
	Ref<TickLocalTransport> client_transport = network.add_peer();
	TickSyncCore client;
	REQUIRE(client.start(client_transport, 0) == OK);
	network.connect_all();
	TickDataBuffer welcome;
	welcome.begin_write();
	welcome.add_uint_bits(TICK_MESSAGE_WELCOME, 8);
	welcome.add_uint_bits(5000, 16);
	welcome.add_int_bits(0, 64);
	welcome.dry();
	const LocalVector<uint8_t> &bytes = welcome.get_buffer().get_bytes();
	server_transport->send(2, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, bytes.ptr(), int(bytes.size()));
	for (int i = 0; i < 30; i++) {
		network.process(1.0 / 60.0);
		client.process(1.0 / 60.0, network.get_time_usec());
	}
	CHECK_FALSE(client.is_welcomed());
	CHECK(client.get_settings().ticks_per_second == 60);
	CHECK(client.get_stats().malformed_packets == 1);
	client.stop();
}

} // namespace TestTickLimits

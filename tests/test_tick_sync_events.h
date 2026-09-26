#pragma once

#include "test_tick_sync_core.h"

#include "tests/test_macros.h"

namespace TestTickSyncEvents {

using TestTickSyncCore::TestMover;

struct ExecutedEvent {
	int sender = 0;
	StringName name;
	Variant payload;
	uint32_t frame = 0;
	// Next frame of the core when the event ran.
	uint32_t core_frame = 0;
};

// A mover that records its events; `validator` (if not -1) overrides the network's policy.
class EventMover : public TestMover {
public:
	TickSyncCore *core = nullptr;
	int validator = -1;
	LocalVector<ExecutedEvent> events;

	EventMover(const String &p_path, int p_controller) :
			TestMover(p_path, p_controller) {}

	virtual int validate_event(int p_sender, const StringName &p_event, const Variant &p_payload) override {
		return validator;
	}

	virtual void on_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override {
		ExecutedEvent event;
		event.sender = p_sender;
		event.name = p_event;
		event.payload = p_payload;
		event.frame = p_frame;
		event.core_frame = core ? core->get_frame() : 0;
		events.push_back(event);
	}
};

// Creates movers for the spawns received, as a game's spawner would.
class SpawnListener : public TickSyncCore::Listener {
public:
	TickSyncCore *core = nullptr;
	LocalVector<EventMover *> spawned;
	LocalVector<ExecutedEvent> network_events;
	int despawned = 0;

	virtual void on_spawn(const String &p_spawner, uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override {
		EventMover *mover = memnew(EventMover(p_spawner + "/" + p_name, p_controller));
		mover->position = p_data;
		spawned.push_back(mover);
		core->register_object(mover);
	}

	virtual void on_despawn(const String &p_spawner, uint32_t p_spawn_id) override {
		despawned++;
	}

	virtual void on_network_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override {
		ExecutedEvent event;
		event.sender = p_sender;
		event.name = p_event;
		event.payload = p_payload;
		event.frame = p_frame;
		network_events.push_back(event);
	}

	~SpawnListener() {
		for (EventMover *mover : spawned) {
			core->unregister_object(mover);
			memdelete(mover);
		}
	}
};

// A server and clients (peers 2 and 3) with one object controlled by peer 2.
struct EventWorld {
	TickLocalNetwork network;
	Ref<TickLocalTransport> transports[4];
	TickSyncCore cores[4];
	SpawnListener listeners[4];
	EventMover *movers[4] = {};
	int peer_count = 0;

	EventWorld(int p_peers, bool p_trusted = false) {
		network.set_seed(99);
		network.set_latency_usec(30000);
		TickSyncCore::Settings settings;
		settings.trusted = p_trusted;
		peer_count = p_peers;
		for (int i = 1; i <= p_peers; i++) {
			transports[i] = network.add_peer();
			cores[i].set_settings(settings);
			listeners[i].core = &cores[i];
			cores[i].set_listener(&listeners[i]);
			movers[i] = memnew(EventMover("mover", 2));
			movers[i]->core = &cores[i];
			cores[i].register_object(movers[i]);
			REQUIRE(cores[i].start(transports[i], 0) == OK);
		}
		for (int i = 2; i <= p_peers; i++) {
			network.connect_peers(1, i);
		}
	}

	~EventWorld() {
		for (int i = 1; i <= peer_count; i++) {
			cores[i].unregister_object(movers[i]);
			memdelete(movers[i]);
		}
	}

	void run(double p_seconds, int p_first_peer = 1) {
		const int frames = int(p_seconds * 60.0);
		for (int f = 0; f < frames; f++) {
			network.process(1.0 / 60.0);
			for (int i = p_first_peer; i <= peer_count; i++) {
				if (cores[i].get_role() != TickSyncCore::ROLE_NONE) {
					cores[i].process(1.0 / 60.0, network.get_time_usec());
				}
			}
		}
	}

	void send_raw(int p_from, int p_to, TickChannel p_channel, TickDataBuffer &p_message) {
		p_message.dry();
		const LocalVector<uint8_t> &bytes = p_message.get_buffer().get_bytes();
		transports[p_from]->send(p_to, p_channel, TickTransport::TRANSFER_MODE_RELIABLE, bytes.ptr(), int(bytes.size()));
	}
};

TEST_CASE("[Modules][TickSynchronizer][EventSync] Client events run on the server at the predicted frame") {
	EventWorld world(3);
	world.run(2.0);
	REQUIRE(world.cores[2].is_predicting());

	// The controller sends an event; by default it's scheduled for the frame the client predicts.
	const uint32_t predicted = world.cores[2].get_frame();
	CHECK(world.cores[2].send_event(world.movers[2], "jump", 42, TICK_FRAME_NONE, 0) == OK);
	world.run(1.0);

	REQUIRE(world.movers[1]->events.size() == 1);
	const ExecutedEvent &event = world.movers[1]->events[0];
	CHECK(event.sender == 2);
	CHECK(event.name == StringName("jump"));
	CHECK(int(event.payload) == 42);
	CHECK(event.frame == predicted);
	// The client runs ahead, so the server ran it exactly at that frame.
	CHECK(event.core_frame == predicted + 1);

	// Another client isn't the controller: refused in untrusted mode.
	CHECK(world.cores[3].send_event(world.movers[3], "jump", 1, TICK_FRAME_NONE, 0) == OK);
	world.run(0.5);
	CHECK(world.movers[1]->events.size() == 1);
	CHECK(world.cores[1].get_stats().events_rejected == 1);

	// Unless the object's own validator accepts it.
	world.movers[1]->validator = 1;
	CHECK(world.cores[3].send_event(world.movers[3], "wave", Variant(), TICK_FRAME_NONE, 0) == OK);
	world.run(0.5);
	REQUIRE(world.movers[1]->events.size() == 2);
	CHECK(world.movers[1]->events[1].sender == 3);

	// Network events reach the listener with their sender.
	CHECK(world.cores[3].send_event(nullptr, "chat", String("hi"), TICK_FRAME_NONE, 0) == OK);
	world.run(0.5);
	REQUIRE(world.listeners[1].network_events.size() == 1);
	CHECK(world.listeners[1].network_events[0].sender == 3);
	CHECK(String(world.listeners[1].network_events[0].payload) == "hi");
}

TEST_CASE("[Modules][TickSynchronizer][EventSync] Trusted peers can send events to any object") {
	EventWorld world(3, true);
	world.run(2.0);
	CHECK(world.cores[3].send_event(world.movers[3], "jump", 1, TICK_FRAME_NONE, 0) == OK);
	world.run(0.5);
	CHECK(world.movers[1]->events.size() == 1);
}

TEST_CASE("[Modules][TickSynchronizer][EventSync] Server events scheduled for a frame run at that frame everywhere") {
	EventWorld world(3);
	world.run(2.0);
	REQUIRE(world.cores[2].is_predicting());
	REQUIRE(world.cores[3].is_predicting());

	// Far enough ahead for the latency and the clients' lead.
	const uint32_t frame = world.cores[1].get_event_frame(0.5);
	CHECK(world.cores[1].send_event(world.movers[1], "start_round", Variant(), frame, 0) == OK);
	world.run(1.5);

	for (int peer = 2; peer <= 3; peer++) {
		REQUIRE(world.movers[peer]->events.size() == 1);
		CHECK(world.movers[peer]->events[0].sender == 1);
		CHECK(world.movers[peer]->events[0].frame == frame);
		// Executed when the client's simulation reached the frame, not on arrival.
		CHECK(world.movers[peer]->events[0].core_frame == frame + 1);
	}

	// Without a frame, events run on arrival.
	CHECK(world.cores[1].send_event(nullptr, "notice", 7, TICK_FRAME_NONE, 3) == OK);
	world.run(0.5);
	CHECK(world.listeners[2].network_events.is_empty());
	REQUIRE(world.listeners[3].network_events.size() == 1);
	CHECK(int(world.listeners[3].network_events[0].payload) == 7);
}

TEST_CASE("[Modules][TickSynchronizer][EventSync] Untrusted clients are rate and size limited") {
	EventWorld world(2);
	world.run(2.0);

	// A burst far above the limit (30 per second by default).
	for (int i = 0; i < 200; i++) {
		world.cores[2].send_event(world.movers[2], "spam", i, TICK_FRAME_NONE, 0);
	}
	world.run(1.0);
	CHECK(world.movers[1]->events.size() <= 32);
	CHECK(world.movers[1]->events.size() >= 29);
	CHECK(world.cores[1].get_stats().events_rejected >= 160);

	// A payload above the limit is refused by the sender...
	PackedByteArray big;
	big.resize(8000);
	ERR_PRINT_OFF;
	CHECK(world.cores[2].send_event(world.movers[2], "big", big, TICK_FRAME_NONE, 0) == ERR_INVALID_DATA);
	ERR_PRINT_ON;

	// ...and by the server, when a modified client sends it anyway.
	world.run(2.0);
	const uint64_t rejected = world.cores[1].get_stats().events_rejected;
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_EVENT, 8);
	message.add_uint_bits(world.cores[1].get_net_id(world.movers[1]), 16);
	message.add_uint_bits(TICK_FRAME_NONE, 32);
	message.add_string("big");
	TickCodec::variant()->encode(big, message);
	world.send_raw(2, 1, TICK_CHANNEL_CONTROL, message);
	world.run(0.5);
	CHECK(world.cores[1].get_stats().events_rejected == rejected + 1);
}

TEST_CASE("[Modules][TickSynchronizer][EventSync] Events wait for objects the client doesn't have yet") {
	EventWorld world(2);
	world.run(2.0);

	// A new object exists on the server, but the client creates it only later (E5).
	EventMover server_door("door", 1);
	EventMover client_door("door", 1);
	world.cores[1].register_object(&server_door);
	world.run(0.5);
	CHECK(world.cores[1].send_event(&server_door, "open", Variant(), TICK_FRAME_NONE, 0) == OK);
	world.run(0.5);
	CHECK(client_door.events.is_empty());

	world.cores[2].register_object(&client_door);
	world.run(0.1);
	REQUIRE(client_door.events.size() == 1);
	CHECK(client_door.events[0].name == StringName("open"));

	world.cores[1].unregister_object(&server_door);
	world.cores[2].unregister_object(&client_door);
}

TEST_CASE("[Modules][TickSynchronizer][SpawnSync] Spawns replicate, also to late clients") {
	EventWorld world(3);
	// Peer 3 joins later.
	world.network.disconnect_peers(1, 3);
	world.run(1.5, 1);

	REQUIRE(world.cores[1].is_server());
	EventMover spawned_on_server("Spawner/Rock", 1);
	const uint32_t spawn_id = world.cores[1].spawn("Spawner", 0, "Rock", 1, Vector2(3, 4));
	CHECK(spawn_id != 0);
	world.cores[1].register_object(&spawned_on_server);
	world.run(0.5);

	REQUIRE(world.listeners[2].spawned.size() == 1);
	EventMover *rock = world.listeners[2].spawned[0];
	CHECK(rock->get_sync_path() == "Spawner/Rock");
	// Registered right after the spawn: bound to the server's object.
	CHECK(world.cores[2].get_net_id(rock) == world.cores[1].get_net_id(&spawned_on_server));

	// A client that joins later gets the live spawn before the registrations.
	world.network.connect_peers(1, 3);
	world.run(1.0);
	REQUIRE(world.listeners[3].spawned.size() == 1);
	CHECK(world.cores[3].get_net_id(world.listeners[3].spawned[0]) == world.cores[1].get_net_id(&spawned_on_server));

	world.cores[1].unregister_object(&spawned_on_server);
	world.cores[1].despawn(spawn_id);
	world.run(0.5);
	CHECK(world.listeners[2].despawned == 1);
	CHECK(world.listeners[3].despawned == 1);
}

TEST_CASE("[Modules][TickSynchronizer][SpawnSync] Released net ids are quarantined") {
	EventWorld world(1);
	world.run(0.1);
	TestMover a("a", 1);
	world.cores[1].register_object(&a);
	const uint16_t first = world.cores[1].get_net_id(&a);
	world.cores[1].unregister_object(&a);

	// Allocation keeps going forward, and wraps around skipping recent ids.
	TestMover b("b", 1);
	world.cores[1].register_object(&b);
	CHECK(world.cores[1].get_net_id(&b) != first);
	world.cores[1].unregister_object(&b);
}

TEST_CASE("[Modules][TickSynchronizer][SpawnSync] Net ids past their quarantine are reused, however many came before") {
	EventWorld world(1);
	world.run(0.1);

	// Objects come and go (projectiles, for example), all within the quarantine.
	TestMover churn("churn", 1);
	int registered = 0;
	for (int i = 0; i < 65530; i++) {
		world.cores[1].register_object(&churn);
		registered += world.cores[1].get_net_id(&churn) != 0 ? 1 : 0;
		world.cores[1].unregister_object(&churn);
	}
	CHECK(registered == 65530);

	// Past the quarantine (2 x history_size ticks), their ids can be used again.
	world.run(5.0);
	int reused = 0;
	for (int i = 0; i < 100; i++) {
		world.cores[1].register_object(&churn);
		reused += world.cores[1].get_net_id(&churn) != 0 ? 1 : 0;
		world.cores[1].unregister_object(&churn);
	}
	CHECK(reused == 100);
}

TEST_CASE("[Modules][TickSynchronizer][EventSync] Payloads that only mean something in this process aren't sent") {
	EventWorld world(2);
	world.run(2.0);
	Ref<RefCounted> object;
	object.instantiate();
	Array with_object;
	with_object.push_back(object);
	ERR_PRINT_OFF;
	CHECK(world.cores[2].send_event(world.movers[2], "give", with_object, TICK_FRAME_NONE, 0) == ERR_INVALID_DATA);
	CHECK(world.cores[1].spawn("Spawner", 0, "Thing", 1, object) == 0);
	ERR_PRINT_ON;
	CHECK(world.cores[2].get_stats().events_sent == 0);
}

TEST_CASE("[Modules][TickSynchronizer][Security] An input message can't describe more frames than a sender repeats") {
	EventWorld world(2);
	world.run(2.0);
	const uint16_t mover_id = world.cores[1].get_net_id(world.movers[1]);
	REQUIRE(mover_id != 0);

	// One group repeated 256 times, where a sender repeats at most `TICK_MAX_INPUT_FRAMES` frames.
	TickDataBuffer object_input;
	object_input.begin_write();
	object_input.add_int_bits(1, 2);
	TickDataBuffer frame_input;
	frame_input.begin_write();
	frame_input.add_uint_bits(1, 8);
	frame_input.add_uint_bits(mover_id, 16);
	frame_input.add_data_buffer(object_input);
	TickDataBuffer inputs;
	inputs.begin_write();
	inputs.add_uint_bits(TICK_MESSAGE_INPUTS, 8);
	inputs.add_uint_bits(TICK_FRAME_NONE, 32);
	inputs.add_bool(false);
	inputs.add_uint_bits(1, 8);
	inputs.add_uint_bits(world.cores[1].get_frame() - 100, 32);
	inputs.add_uint_bits(255, 8);
	inputs.add_data_buffer(frame_input);

	const TickSyncCore::Stats before = world.cores[1].get_stats();
	world.send_raw(2, 1, TICK_CHANNEL_INPUTS, inputs);
	world.run(0.2);
	const TickSyncCore::Stats &after = world.cores[1].get_stats();
	CHECK(after.malformed_packets == before.malformed_packets + 1);
	// Refused before looking at any of its frames.
	CHECK(after.late_inputs == before.late_inputs);
	CHECK(after.rejected_inputs == before.rejected_inputs);
}

TEST_CASE("[Modules][TickSynchronizer][EventSync] Event names are limited and must be valid UTF-8") {
	EventWorld world(2);
	world.run(2.0);

	// The sender refuses a name above the limit; one at the limit goes through.
	ERR_PRINT_OFF;
	CHECK(world.cores[2].send_event(world.movers[2], String("x").repeat(TICK_MAX_EVENT_NAME_BYTES + 1), Variant(), TICK_FRAME_NONE, 0) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
	const String longest = String("y").repeat(TICK_MAX_EVENT_NAME_BYTES);
	CHECK(world.cores[2].send_event(world.movers[2], longest, Variant(), TICK_FRAME_NONE, 0) == OK);
	world.run(0.5);
	REQUIRE(world.movers[1]->events.size() == 1);
	CHECK(world.movers[1]->events[0].name == StringName(longest));

	// A modified client sends a name above the limit, and one that isn't UTF-8: both messages are malformed.
	const uint16_t target = world.cores[1].get_net_id(world.movers[1]);
	const uint64_t malformed = world.cores[1].get_stats().malformed_packets;
	TickDataBuffer long_name;
	long_name.begin_write();
	long_name.add_uint_bits(TICK_MESSAGE_EVENT, 8);
	long_name.add_uint_bits(target, 16);
	long_name.add_uint_bits(TICK_FRAME_NONE, 32);
	long_name.add_string(String("z").repeat(60000));
	TickCodec::variant()->encode(Variant(), long_name);
	world.send_raw(2, 1, TICK_CHANNEL_CONTROL, long_name);

	const uint8_t invalid[4] = { 0xFF, 0xFE, 0xC0, 0x80 };
	TickDataBuffer invalid_name;
	invalid_name.begin_write();
	invalid_name.add_uint_bits(TICK_MESSAGE_EVENT, 8);
	invalid_name.add_uint_bits(target, 16);
	invalid_name.add_uint_bits(TICK_FRAME_NONE, 32);
	invalid_name.add_uint_bits(4, 16);
	invalid_name.add_bits(invalid, 32);
	TickCodec::variant()->encode(Variant(), invalid_name);
	world.send_raw(2, 1, TICK_CHANNEL_CONTROL, invalid_name);

	world.run(0.5);
	CHECK(world.cores[1].get_stats().malformed_packets == malformed + 2);
	CHECK(world.movers[1]->events.size() == 1);
}

TEST_CASE("[Modules][TickSynchronizer][Security] Spoofed input and state are ignored (H1, H2)") {
	EventWorld world(3);
	// Clients also see each other directly, as in a mesh or through a relay.
	world.network.connect_peers(2, 3);
	world.run(2.0);
	const uint16_t mover_id = world.cores[1].get_net_id(world.movers[1]);
	REQUIRE(mover_id != 0);

	// H2: peer 3 sends a full snapshot, a spawn, a registration and an event to peer 2.
	TickDataBuffer snapshot;
	snapshot.begin_write();
	snapshot.add_uint_bits(TICK_MESSAGE_SNAPSHOT_FULL, 8);
	snapshot.add_uint_bits(world.cores[2].get_latest_snapshot_frame() + 1, 32);
	snapshot.add_uint_bits(TICK_FRAME_NONE, 32);
	snapshot.add_int_bits(0, 8);
	snapshot.add_uint_bits(0, 16);
	world.send_raw(3, 2, TICK_CHANNEL_SNAPSHOT, snapshot);

	TickDataBuffer spawn;
	spawn.begin_write();
	spawn.add_uint_bits(TICK_MESSAGE_SPAWN, 8);
	spawn.add_uint_bits(999, 32);
	spawn.add_string("Spawner");
	spawn.add_int_bits(0, 16);
	spawn.add_string("Fake");
	spawn.add_int_bits(3, 32);
	TickCodec::variant()->encode(Variant(), spawn);
	world.send_raw(3, 2, TICK_CHANNEL_CONTROL, spawn);

	TickDataBuffer event;
	event.begin_write();
	event.add_uint_bits(TICK_MESSAGE_EVENT, 8);
	event.add_uint_bits(mover_id, 16);
	event.add_uint_bits(TICK_FRAME_NONE, 32);
	event.add_string("teleport");
	TickCodec::variant()->encode(Variant(), event);
	world.send_raw(3, 2, TICK_CHANNEL_CONTROL, event);

	const uint64_t malformed_before = world.cores[2].get_stats().malformed_packets;
	world.run(0.5);
	CHECK(world.cores[2].get_stats().malformed_packets == malformed_before + 3);
	CHECK(world.listeners[2].spawned.is_empty());
	CHECK(world.movers[2]->events.is_empty());
	CHECK(world.cores[2].get_stats().snapshots_dropped == 0);

	// H1: peer 3 sends input for peer 2's object; the server only takes peer 2's own input for it.
	TickDataBuffer object_input;
	object_input.begin_write();
	object_input.add_int_bits(1, 2);
	TickDataBuffer frame_input;
	frame_input.begin_write();
	frame_input.add_uint_bits(1, 8);
	frame_input.add_uint_bits(mover_id, 16);
	frame_input.add_data_buffer(object_input);
	for (int i = 0; i < 30; i++) {
		TickDataBuffer inputs;
		inputs.begin_write();
		inputs.add_uint_bits(TICK_MESSAGE_INPUTS, 8);
		inputs.add_uint_bits(TICK_FRAME_NONE, 32);
		inputs.add_bool(false);
		inputs.add_uint_bits(1, 8);
		inputs.add_uint_bits(world.cores[1].get_frame() + 1, 32);
		inputs.add_uint_bits(4, 8);
		inputs.add_data_buffer(frame_input);
		world.send_raw(3, 1, TICK_CHANNEL_INPUTS, inputs);
		world.run(1.0 / 60.0);
	}
	world.run(0.5);
	// Peer 2's script ended at tick 0: its object never moved.
	CHECK(world.movers[1]->position == Vector2());
}

} // namespace TestTickSyncEvents

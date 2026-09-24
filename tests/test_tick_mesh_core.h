#pragma once

#include "../source/sync/tick_mesh_core.h"
#include "test_tick_sync_core.h"

#include "tests/test_macros.h"

namespace TestTickMeshCore {

using TestTickSyncCore::TestMover;

// A body that always moves right while someone owns it, and records what happens to its authority.
class AuthMover : public TestMover {
public:
	int approve = -1;
	LocalVector<Vector2i> changes;
	LocalVector<int> event_senders;

	explicit AuthMover(const String &p_path, int p_controller) :
			TestMover(p_path, p_controller, TickCodec::PRECISION_SINGLE) {
		constant_direction = true;
		direction_override = 1;
	}

	virtual void on_authority_changed(int p_old_owner, int p_new_owner) override {
		changes.push_back(Vector2i(p_old_owner, p_new_owner));
	}

	virtual int approve_authority_request(int p_requester) override { return approve; }

	virtual void on_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override {
		event_senders.push_back(p_sender);
	}
};

class MeshListener : public TickEngine::Listener {
public:
	TickMeshCore *core = nullptr;
	int orphaned = 0;
	int last_orphan_owner = 0;
	int denied = 0;
	LocalVector<AuthMover *> spawned;

	virtual void on_authority_orphaned(TickSyncObject *p_object, int p_last_owner, uint32_t p_last_frame) override {
		orphaned++;
		last_orphan_owner = p_last_owner;
	}

	virtual void on_authority_request_denied(TickSyncObject *p_object) override { denied++; }

	virtual void on_spawn(const String &p_spawner, uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override {
		AuthMover *mover = memnew(AuthMover(p_spawner + "/" + p_name, p_controller));
		spawned.push_back(mover);
		core->register_object(mover);
	}

	~MeshListener() {
		for (AuthMover *mover : spawned) {
			core->unregister_object(mover);
			memdelete(mover);
		}
	}
};

// Nodes 1 to `p_nodes` in a full mesh; node 1 is the registry and clock master. Every node has "crate", owned at
// first by node 2.
struct MeshWorld {
	TickLocalNetwork network;
	Ref<TickLocalTransport> transports[5];
	TickMeshCore cores[5];
	MeshListener listeners[5];
	AuthMover *crates[5] = {};
	int count = 0;

	explicit MeshWorld(int p_nodes, bool p_connect_all = true) {
		count = p_nodes;
		network.set_seed(5);
		network.set_latency_usec(20000);
		TickEngine::Settings settings;
		settings.trusted = true;
		settings.interpolate_remote = false;
		for (int i = 1; i <= count; i++) {
			transports[i] = network.add_peer();
			cores[i].set_settings(settings);
			listeners[i].core = &cores[i];
			cores[i].set_listener(&listeners[i]);
			crates[i] = memnew(AuthMover("crate", 2));
			cores[i].register_object(crates[i]);
			REQUIRE(cores[i].start(transports[i], 0) == OK);
		}
		if (p_connect_all) {
			network.connect_all();
		}
	}

	~MeshWorld() {
		for (int i = 1; i <= count; i++) {
			cores[i].unregister_object(crates[i]);
			memdelete(crates[i]);
		}
	}

	void run(double p_seconds) {
		for (int f = 0; f < int(p_seconds * 60.0); f++) {
			network.process(1.0 / 60.0);
			for (int i = 1; i <= count; i++) {
				if (cores[i].is_running() && network.get_peer(i).is_valid()) {
					cores[i].process(1.0 / 60.0, network.get_time_usec());
				}
			}
		}
	}

	void send_raw(int p_from, int p_to, TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message) {
		p_message.dry();
		const LocalVector<uint8_t> &bytes = p_message.get_buffer().get_bytes();
		transports[p_from]->send(p_to, p_channel, p_mode, bytes.ptr(), int(bytes.size()));
	}
};

TEST_CASE("[Modules][TickSynchronizer][MeshCore] Objects register once and replicate from their owner") {
	MeshWorld world(3);
	world.run(2.0);

	const uint16_t id = world.cores[1].get_net_id(world.crates[1]);
	REQUIRE(id != 0);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_net_id(world.crates[i]) == id);
		CHECK(world.cores[i].get_owner(world.crates[i]) == 2);
		CHECK(world.cores[i].get_version(world.crates[i]) == 1);
	}
	// Only the owner moves it; the others follow its state, on the clock master's timeline.
	CHECK(world.crates[2]->position.x > 5.0);
	CHECK(Math::abs(world.crates[1]->position.x - world.crates[2]->position.x) < 0.5);
	CHECK(Math::abs(world.crates[3]->position.x - world.crates[2]->position.x) < 0.5);
	CHECK(Math::abs(int32_t(world.cores[3].get_frame() - world.cores[1].get_frame())) <= 3);
	CHECK(world.cores[1].get_stats().malformed_packets == 0);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] A requested object changes owner and keeps its state") {
	MeshWorld world(3);
	world.run(2.0);
	const float before = world.crates[2]->position.x;

	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(0.5);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 3);
		CHECK(world.cores[i].get_version(world.crates[i]) == 2);
		REQUIRE(world.crates[i]->changes.size() == 1);
		CHECK(world.crates[i]->changes[0] == Vector2i(2, 3));
	}
	// The new owner continued from the released state: no jump back.
	world.run(1.0);
	CHECK(world.crates[3]->position.x > before + 3.0);
	CHECK(Math::abs(world.crates[1]->position.x - world.crates[3]->position.x) < 0.5);
	// The former owner stopped simulating: it follows the new one.
	CHECK(Math::abs(world.crates[2]->position.x - world.crates[3]->position.x) < 0.5);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] Concurrent requests give the object to exactly one node") {
	MeshWorld world(3);
	world.run(2.0);

	// Nodes 1 and 3 ask at the same time; node 2 owns it.
	CHECK(world.cores[1].request_authority(world.crates[1]) == OK);
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(1.0);

	const int owner = world.cores[1].get_owner(world.crates[1]);
	CHECK((owner == 1 || owner == 3));
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == owner);
		// A single change of owner.
		CHECK(world.cores[i].get_version(world.crates[i]) == 2);
	}
	CHECK(world.listeners[1].denied + world.listeners[3].denied == 1);
	CHECK(world.listeners[owner].denied == 0);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] State from a former owner or an old version is discarded") {
	MeshWorld world(3);
	world.run(2.0);
	const uint16_t id = world.cores[1].get_net_id(world.crates[1]);
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(0.5);
	REQUIRE(world.cores[1].get_owner(world.crates[1]) == 3);

	// Node 2 (the former owner) sends a state with the old version, and one with the current version.
	for (uint32_t version = 1; version <= 2; version++) {
		TickDataBuffer payload;
		payload.begin_write();
		world.crates[2]->get_sync_schema().codecs[0]->encode(Vector2(9999, 0), payload);
		world.crates[2]->get_sync_schema().codecs[1]->encode(Vector2(), payload);
		TickDataBuffer message;
		message.begin_write();
		message.add_uint_bits(TICK_MESSAGE_STATE, 8);
		message.add_uint_bits(world.cores[2].get_frame() + 100, 32);
		message.add_uint_bits(1, 16);
		message.add_uint_bits(id, 16);
		message.add_uint_bits(version, 32);
		message.add_data_buffer(payload);
		world.send_raw(2, 1, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_RELIABLE, message);
	}
	const uint64_t stale_before = world.cores[1].get_stats().stale_states;
	world.run(0.2);
	CHECK(world.cores[1].get_stats().stale_states >= stale_before + 2);
	CHECK(world.crates[1]->position.x < 1000.0);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] A lost owner orphans its objects until the project assigns them") {
	MeshWorld world(3);
	world.run(2.0);
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(0.5);
	REQUIRE(world.cores[1].get_owner(world.crates[1]) == 3);

	// Node 3 leaves (it warns that it lost the registry).
	world.network.disconnect_peers(3, 1);
	world.network.disconnect_peers(3, 2);
	ERR_PRINT_OFF;
	world.run(0.5);
	ERR_PRINT_ON;
	for (int i = 1; i <= 2; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 0);
		CHECK(world.listeners[i].orphaned == 1);
		CHECK(world.listeners[i].last_orphan_owner == 3);
	}
	// Nobody simulates an orphan.
	const float frozen = world.crates[1]->position.x;
	world.run(0.5);
	CHECK(world.crates[1]->position.x == frozen);
	CHECK(world.crates[2]->position.x == frozen);

	// The project picks the new owner.
	CHECK(world.cores[1].assign_authority(world.crates[1], 2) == OK);
	world.run(1.0);
	CHECK(world.cores[1].get_owner(world.crates[1]) == 2);
	CHECK(world.cores[2].get_owner(world.crates[2]) == 2);
	CHECK(world.crates[2]->position.x > frozen + 3.0);
	CHECK(Math::abs(world.crates[1]->position.x - world.crates[2]->position.x) < 0.5);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] Owners approve requests, release objects and forward events") {
	MeshWorld world(3);
	world.run(2.0);

	// The owner refuses a request.
	world.crates[2]->approve = 0;
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(0.5);
	CHECK(world.cores[3].get_owner(world.crates[3]) == 2);
	CHECK(world.listeners[3].denied == 1);

	// An assignment isn't a request: the owner can't refuse it.
	CHECK(world.cores[3].assign_authority(world.crates[3], 3) == OK);
	world.run(0.5);
	CHECK(world.cores[1].get_owner(world.crates[1]) == 3);

	// The owner releases it to another node.
	CHECK(world.cores[3].release_authority(world.crates[3], 1) == OK);
	world.run(0.5);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 1);
	}

	// An event sent to a former owner is forwarded to the current one, with its origin.
	const uint16_t id = world.cores[1].get_net_id(world.crates[1]);
	TickDataBuffer event;
	event.begin_write();
	event.add_uint_bits(TICK_MESSAGE_MESH_EVENT, 8);
	event.add_uint_bits(id, 16);
	event.add_uint_bits(TICK_FRAME_NONE, 32);
	event.add_string("kick");
	TickCodec::variant()->encode(Variant(), event);
	event.add_int_bits(0, 32);
	event.add_uint_bits(0, 8);
	world.send_raw(3, 2, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, event);
	world.run(0.5);
	CHECK(world.cores[2].get_stats().events_forwarded == 1);
	REQUIRE(world.crates[1]->event_senders.size() == 1);
	CHECK(world.crates[1]->event_senders[0] == 3);

	// The regular way: the sender addresses the current owner directly.
	CHECK(world.cores[2].send_event(world.crates[2], "push", Variant(), TICK_FRAME_NONE, 0) == OK);
	world.run(0.5);
	REQUIRE(world.crates[1]->event_senders.size() == 2);
	CHECK(world.crates[1]->event_senders[1] == 2);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] Any node spawns, and late nodes get the registry and the spawns") {
	MeshWorld world(4, false);
	world.network.connect_peers(1, 2);
	world.network.connect_peers(1, 3);
	world.network.connect_peers(2, 3);
	world.run(1.5);

	// Node 3 spawns a body; the others create theirs, and the spawner owns it.
	AuthMover *rock = memnew(AuthMover("Spawner/Rock", 3));
	const uint32_t spawn_id = world.cores[3].spawn("Spawner", 0, "Rock", 3, Variant());
	CHECK((spawn_id >> 20) == 3);
	world.cores[3].register_object(rock);
	world.run(1.0);
	REQUIRE(world.listeners[1].spawned.size() == 1);
	REQUIRE(world.listeners[2].spawned.size() == 1);
	CHECK(world.cores[1].get_owner(world.listeners[1].spawned[0]) == 3);
	CHECK(world.listeners[1].spawned[0]->position.x > 0.5);

	// Node 4 joins late.
	world.network.connect_peers(4, 1);
	world.network.connect_peers(4, 2);
	world.network.connect_peers(4, 3);
	world.run(1.5);
	REQUIRE(world.listeners[4].spawned.size() == 1);
	CHECK(world.cores[4].get_owner(world.crates[4]) == 2);
	CHECK(world.cores[4].get_owner(world.listeners[4].spawned[0]) == 3);
	CHECK(Math::abs(world.listeners[4].spawned[0]->position.x - rock->position.x) < 0.5);

	world.cores[3].unregister_object(rock);
	memdelete(rock);
}

} // namespace TestTickMeshCore

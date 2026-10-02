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
	int roles_changes = 0;
	int quorum_changes = 0;
	bool has_quorum = true;
	int despawned = 0;
	LocalVector<AuthMover *> spawned;

	virtual void on_authority_orphaned(TickSyncObject *p_object, int p_last_owner, uint32_t p_last_frame) override {
		orphaned++;
		last_orphan_owner = p_last_owner;
	}

	virtual void on_authority_request_denied(TickSyncObject *p_object) override { denied++; }

	virtual void on_roles_changed(int p_registry, int p_clock_master) override { roles_changes++; }

	virtual void on_role_quorum_changed(bool p_has_quorum) override {
		quorum_changes++;
		has_quorum = p_has_quorum;
	}

	virtual void on_despawn(const String &p_spawner, uint32_t p_spawn_id) override { despawned++; }

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
	Ref<TickLocalTransport> transports[6];
	TickMeshCore cores[6];
	MeshListener listeners[6];
	AuthMover *crates[6] = {};
	int count = 0;
	TickEngine::Settings settings;
	// Another engine stepped with the world's: a node's process that restarted (see `RestartedNode`).
	TickMeshCore *extra = nullptr;

	explicit MeshWorld(int p_nodes, bool p_connect_all = true, const Vector<int> &p_candidates = Vector<int>(), int p_quorum = 0) {
		count = p_nodes;
		network.set_seed(5);
		network.set_latency_usec(20000);
		settings.trusted = true;
		settings.interpolate_remote = false;
		settings.role_candidates = p_candidates;
		settings.role_quorum = p_quorum;
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
			if (extra && extra->is_running()) {
				extra->process(1.0 / 60.0, network.get_time_usec());
			}
		}
	}

	// Cuts every link of a node (its machine is gone, or cut off from the others).
	void isolate(int p_node) {
		for (int i = 1; i <= count; i++) {
			if (i != p_node) {
				network.disconnect_peers(p_node, i);
			}
		}
	}

	void reconnect(int p_node) {
		for (int i = 1; i <= count; i++) {
			if (i != p_node) {
				network.connect_peers(p_node, i);
			}
		}
	}

	void send_raw(int p_from, int p_to, TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message) {
		p_message.dry();
		const LocalVector<uint8_t> &bytes = p_message.get_buffer().get_bytes();
		transports[p_from]->send(p_to, p_channel, p_mode, bytes.ptr(), int(bytes.size()));
	}
};

// The process of a node after a restart: an empty engine on the node's id, configured like the others, with its own
// "crate". The world steps it once it started.
struct RestartedNode {
	TickMeshCore core;
	MeshListener listener;
	AuthMover *crate = nullptr;
	MeshWorld *world = nullptr;

	RestartedNode(MeshWorld &r_world, int p_node) {
		world = &r_world;
		r_world.cores[p_node].stop();
		listener.core = &core;
		core.set_settings(r_world.settings);
		core.set_listener(&listener);
		crate = memnew(AuthMover("crate", 2));
		core.register_object(crate);
		REQUIRE(core.start(r_world.transports[p_node], r_world.network.get_time_usec()) == OK);
		r_world.extra = &core;
	}

	~RestartedNode() {
		world->extra = nullptr;
		core.stop();
		core.unregister_object(crate);
		memdelete(crate);
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

// Node 1 registers the objects and drops them, a frame apart; returns how many of them got an id.
static int churn_objects(MeshWorld &r_world, const LocalVector<AuthMover *> &p_movers) {
	TickMeshCore &core = r_world.cores[1];
	for (AuthMover *mover : p_movers) {
		core.register_object(mover);
	}
	r_world.run(1.0 / 60.0);
	int registered = 0;
	for (AuthMover *mover : p_movers) {
		registered += core.get_net_id(mover) != 0 ? 1 : 0;
		core.unregister_object(mover);
	}
	r_world.run(1.0 / 60.0);
	return registered;
}

// A body whose state doesn't take a whole number of bytes.
class OddMover : public AuthMover {
public:
	bool moving = false;

	OddMover(const String &p_path, int p_controller) :
			AuthMover(p_path, p_controller) {
		schema.add("moving", TickCodec::boolean());
	}

	virtual Variant get_sync_var(int p_index) const override {
		return p_index == 2 ? Variant(moving) : AuthMover::get_sync_var(p_index);
	}

	virtual void set_sync_var(int p_index, const Variant &p_value) override {
		if (p_index == 2) {
			moving = p_value;
		} else {
			AuthMover::set_sync_var(p_index, p_value);
		}
	}

	virtual void process_tick(double p_delta, TickDataBuffer &p_input) override {
		AuthMover::process_tick(p_delta, p_input);
		moving = true;
	}
};

TEST_CASE("[Modules][TickSynchronizer][MeshCore] Every object of an owner replicates, whatever the size of its state") {
	MeshWorld world(3);
	// Node 2 owns three more bodies, whose states take 129 bits each: they go in the same message, one after another.
	OddMover *movers[4][3] = {};
	for (int i = 1; i <= 3; i++) {
		for (int k = 0; k < 3; k++) {
			movers[i][k] = memnew(OddMover(vformat("odd_%d", k), 2));
			world.cores[i].register_object(movers[i][k]);
		}
	}
	world.run(3.0);
	for (int k = 0; k < 3; k++) {
		CHECK(world.cores[1].get_owner(movers[1][k]) == 2);
		CHECK(movers[2][k]->position.x > 5.0);
		for (int i = 1; i <= 3; i += 2) {
			CHECK_MESSAGE(Math::abs(movers[i][k]->position.x - movers[2][k]->position.x) < 0.5, vformat("odd_%d on node %d is at %f; on its owner, at %f.", k, i, movers[i][k]->position.x, movers[2][k]->position.x));
			CHECK(movers[i][k]->moving);
		}
	}
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_stats().malformed_packets == 0);
		for (int k = 0; k < 3; k++) {
			world.cores[i].unregister_object(movers[i][k]);
			memdelete(movers[i][k]);
		}
	}
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] Registry ids past their quarantine are reused, however many came before") {
	MeshWorld world(1);
	world.run(0.2);
	LocalVector<AuthMover *> movers;
	for (int i = 0; i < 2000; i++) {
		movers.push_back(memnew(AuthMover(vformat("churn_%d", i), 1)));
	}

	// 60,000 objects within the quarantine.
	int registered = 0;
	for (int batch = 0; batch < 30; batch++) {
		registered += churn_objects(world, movers);
	}
	CHECK(registered == 60000);

	// Past the quarantine, the registry keeps assigning ids beyond 65,535 objects in total.
	world.run(5.0);
	registered = 0;
	for (int batch = 0; batch < 5; batch++) {
		registered += churn_objects(world, movers);
	}
	CHECK(registered == 10000);

	for (AuthMover *mover : movers) {
		memdelete(mover);
	}
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

TEST_CASE("[Modules][TickSynchronizer][MeshCore] When the registry and clock node is lost, the lowest node takes both roles") {
	MeshWorld world(3);
	world.run(2.0);
	// Node 1 (registry and clock master) owns the crate.
	CHECK(world.cores[1].request_authority(world.crates[1]) == OK);
	world.run(0.5);
	REQUIRE(world.cores[2].get_owner(world.crates[2]) == 1);

	// Node 1's machine is lost.
	world.network.remove_peer(1);
	world.run(1.5);
	for (int i = 2; i <= 3; i++) {
		CHECK(world.cores[i].get_settings().registry_peer == 2);
		CHECK(world.cores[i].get_settings().clock_master == 2);
		CHECK(world.cores[i].get_roles_term() == 1);
		CHECK(world.listeners[i].roles_changes == 1);
		// The new registry orphaned the lost node's crate.
		CHECK(world.cores[i].get_owner(world.crates[i]) == 0);
		CHECK(world.listeners[i].orphaned == 1);
		CHECK(world.listeners[i].last_orphan_owner == 1);
	}

	// Ownership changes again, through node 2.
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(1.0);
	CHECK(world.cores[2].get_owner(world.crates[2]) == 3);
	CHECK(world.cores[3].get_owner(world.crates[3]) == 3);
	// Node 3 follows node 2's clock.
	CHECK(world.cores[3].get_clock().is_synchronized());
	const int64_t frames_apart = int64_t(world.cores[2].get_frame()) - int64_t(world.cores[3].get_frame());
	CHECK((frames_apart >= -3 && frames_apart <= 3));
	const float before = world.crates[2]->position.x;
	world.run(1.0);
	CHECK(world.crates[2]->position.x > before + 3.0);
	CHECK(Math::abs(world.crates[2]->position.x - world.crates[3]->position.x) < 0.5);
	CHECK(world.cores[2].get_stats().malformed_packets == 0);
	CHECK(world.cores[3].get_stats().malformed_packets == 0);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] A request the lost registry didn't answer is denied") {
	MeshWorld world(3);
	world.run(2.0);
	REQUIRE(world.cores[3].get_owner(world.crates[3]) == 2);
	// Node 3 asks for the crate, and the registry's node is lost with the request on the way.
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.network.remove_peer(1);
	world.run(1.5);
	CHECK(world.listeners[3].denied == 1);
	CHECK(world.cores[3].get_owner(world.crates[3]) == 2);
	// Asking again works, with the new registry.
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(1.0);
	CHECK(world.cores[3].get_owner(world.crates[3]) == 3);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] The registry and the clock move while the mesh runs") {
	MeshWorld world(3);
	world.run(2.0);
	// Any node moves them, to connected nodes only.
	ERR_PRINT_OFF;
	CHECK(world.cores[1].change_roles(4, 3) == ERR_UNAVAILABLE);
	ERR_PRINT_ON;
	CHECK(world.cores[2].change_roles(3, 3) == OK);
	world.run(1.5);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_settings().registry_peer == 3);
		CHECK(world.cores[i].get_settings().clock_master == 3);
		CHECK(world.listeners[i].roles_changes == 1);
		CHECK(world.cores[i].get_clock().is_synchronized());
	}

	// The new registry answers requests.
	CHECK(world.cores[1].request_authority(world.crates[1]) == OK);
	world.run(1.0);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 1);
	}
	// An object registered afterwards gets one net id everywhere, not one already in use.
	AuthMover *barrels[4] = {};
	for (int i = 1; i <= 3; i++) {
		barrels[i] = memnew(AuthMover("barrel", 1));
		world.cores[i].register_object(barrels[i]);
	}
	world.run(1.0);
	const uint16_t barrel_id = world.cores[1].get_net_id(barrels[1]);
	CHECK(barrel_id != 0);
	CHECK(barrel_id != world.cores[1].get_net_id(world.crates[1]));
	for (int i = 2; i <= 3; i++) {
		CHECK(world.cores[i].get_net_id(barrels[i]) == barrel_id);
		CHECK(world.cores[i].get_stats().malformed_packets == 0);
	}
	for (int i = 1; i <= 3; i++) {
		world.cores[i].unregister_object(barrels[i]);
		memdelete(barrels[i]);
	}
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] The registry's node comes back empty and joins as a plain node") {
	MeshWorld world(3);
	world.run(2.0);
	const uint16_t crate_id = world.cores[2].get_net_id(world.crates[2]);
	REQUIRE(crate_id != 0);
	// Node 1 loses its links (its machine restarts), and node 2 takes the roles.
	world.network.disconnect_peers(1, 2);
	world.network.disconnect_peers(1, 3);
	world.run(1.5);
	REQUIRE(world.cores[3].get_settings().registry_peer == 2);
	world.cores[1].stop();

	// The restarted process: an empty network on the same id, configured as at the start.
	TickMeshCore fresh;
	MeshListener fresh_listener;
	fresh_listener.core = &fresh;
	TickEngine::Settings settings;
	settings.trusted = true;
	settings.interpolate_remote = false;
	fresh.set_settings(settings);
	fresh.set_listener(&fresh_listener);
	AuthMover *fresh_crate = memnew(AuthMover("crate", 2));
	fresh.register_object(fresh_crate);
	REQUIRE(fresh.start(world.transports[1], world.network.get_time_usec()) == OK);
	world.network.connect_peers(1, 2);
	world.network.connect_peers(1, 3);
	for (int f = 0; f < 120; f++) {
		world.network.process(1.0 / 60.0);
		const uint64_t now = world.network.get_time_usec();
		fresh.process(1.0 / 60.0, now);
		world.cores[2].process(1.0 / 60.0, now);
		world.cores[3].process(1.0 / 60.0, now);
	}
	// It adopted the mesh's roles instead of being the registry again, and its crate has the mesh's id.
	CHECK(fresh.get_settings().registry_peer == 2);
	CHECK(fresh.get_settings().clock_master == 2);
	CHECK(fresh.get_roles_term() == 1);
	CHECK(fresh.get_net_id(fresh_crate) == crate_id);
	CHECK(fresh.get_owner(fresh_crate) == 2);
	CHECK(fresh.get_clock().is_synchronized());
	for (int i = 2; i <= 3; i++) {
		CHECK(world.cores[i].get_settings().registry_peer == 2);
		CHECK(world.cores[i].get_stats().malformed_packets == 0);
	}
	fresh.stop();
	fresh.unregister_object(fresh_crate);
	memdelete(fresh_crate);
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

TEST_CASE("[Modules][TickSynchronizer][MeshCore] The first candidate in the mesh takes the roles, not the lowest node") {
	// Node 1 has the roles and node 3 is its reserve; nodes 2 and 4 never take them.
	Vector<int> candidates;
	candidates.push_back(1);
	candidates.push_back(3);
	MeshWorld world(4, true, candidates);
	world.run(2.0);
	world.network.remove_peer(1);
	world.run(1.5);
	for (int i = 2; i <= 4; i++) {
		CHECK(world.cores[i].get_settings().registry_peer == 3);
		CHECK(world.cores[i].get_settings().clock_master == 3);
		CHECK(world.cores[i].get_roles_term() == 1);
		CHECK(world.cores[i].get_owner(world.crates[i]) == 2);
		CHECK(world.cores[i].get_stats().malformed_packets == 0);
	}
	// The reserve is lost too: no candidate is left, and the roles stay where they were.
	world.network.remove_peer(3);
	world.run(1.5);
	for (int i = 2; i <= 4; i += 2) {
		CHECK(world.cores[i].get_settings().registry_peer == 3);
		CHECK(world.cores[i].get_roles_term() == 1);
	}
	ERR_PRINT_OFF;
	CHECK(world.cores[4].request_authority(world.crates[4]) == ERR_UNAVAILABLE);
	ERR_PRINT_ON;
	// The objects go on with their owners meanwhile.
	const float before = world.crates[2]->position.x;
	world.run(1.0);
	CHECK(world.crates[2]->position.x > before + 3.0);
	CHECK(Math::abs(world.crates[4]->position.x - world.crates[2]->position.x) < 0.5);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] A node with the roles that restarts takes them again from the other nodes") {
	// Only node 1 may have the roles.
	Vector<int> candidates;
	candidates.push_back(1);
	MeshWorld world(3, true, candidates);
	world.run(2.0);
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(0.5);
	const uint16_t crate_id = world.cores[2].get_net_id(world.crates[2]);
	REQUIRE(crate_id != 0);
	REQUIRE(world.cores[2].get_owner(world.crates[2]) == 3);
	const uint32_t version = world.cores[2].get_version(world.crates[2]);

	// Node 1's machine is gone: nobody takes its roles, and the nodes go on with what they have.
	world.isolate(1);
	world.cores[1].stop();
	world.run(1.5);
	CHECK(world.cores[2].get_settings().registry_peer == 1);
	CHECK(world.cores[2].get_roles_term() == 0);
	const uint32_t frame_before = world.cores[2].get_frame();
	CHECK(frame_before > 200);

	// It restarts with nothing: it believes it's the registry and the clock, from frame 0.
	RestartedNode fresh(world, 1);
	world.reconnect(1);
	world.run(2.0);
	// The other nodes told it the roles belonged to another process: it took them again, from their views.
	CHECK(fresh.core.get_roles_term() == 1);
	CHECK(fresh.core.get_settings().registry_peer == 1);
	CHECK(fresh.core.get_net_id(fresh.crate) == crate_id);
	CHECK(fresh.core.get_owner(fresh.crate) == 3);
	for (int i = 2; i <= 3; i++) {
		CHECK(world.cores[i].get_roles_term() == 1);
		CHECK(world.cores[i].get_settings().registry_peer == 1);
		CHECK(world.cores[i].get_settings().clock_master == 1);
		CHECK(world.cores[i].get_net_id(world.crates[i]) == crate_id);
		CHECK(world.cores[i].get_owner(world.crates[i]) == 3);
		CHECK(world.cores[i].get_version(world.crates[i]) == version + 1);
		CHECK(world.listeners[i].orphaned == 0);
		CHECK(world.cores[i].get_stats().malformed_packets == 0);
	}
	// The timeline went on: the restarted clock took the mesh's frame instead of starting it over.
	CHECK(world.cores[2].get_frame() > frame_before + 100);
	const int64_t apart = int64_t(fresh.core.get_frame()) - int64_t(world.cores[2].get_frame());
	CHECK((apart >= -3 && apart <= 3));

	// A new object gets an id that wasn't in use, and ownership changes through the restarted registry.
	AuthMover *barrels[4] = {};
	for (int i = 2; i <= 3; i++) {
		barrels[i] = memnew(AuthMover("barrel", 2));
		world.cores[i].register_object(barrels[i]);
	}
	barrels[1] = memnew(AuthMover("barrel", 2));
	fresh.core.register_object(barrels[1]);
	CHECK(world.cores[2].request_authority(world.crates[2]) == OK);
	world.run(1.0);
	const uint16_t barrel_id = world.cores[2].get_net_id(barrels[2]);
	CHECK(barrel_id != 0);
	CHECK(barrel_id != crate_id);
	CHECK(world.cores[3].get_net_id(barrels[3]) == barrel_id);
	CHECK(fresh.core.get_net_id(barrels[1]) == barrel_id);
	CHECK(fresh.core.get_owner(fresh.crate) == 2);
	CHECK(world.cores[3].get_owner(world.crates[3]) == 2);
	CHECK(fresh.core.get_stats().malformed_packets == 0);
	for (int i = 2; i <= 3; i++) {
		world.cores[i].unregister_object(barrels[i]);
		memdelete(barrels[i]);
	}
	fresh.core.unregister_object(barrels[1]);
	memdelete(barrels[1]);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] A node that joins while a restarted node takes its roles again gets the mesh's view") {
	Vector<int> candidates;
	candidates.push_back(1);
	MeshWorld world(4, false, candidates);
	world.network.connect_peers(1, 2);
	world.network.connect_peers(1, 3);
	world.network.connect_peers(2, 3);
	world.run(2.0);
	const uint16_t crate_id = world.cores[2].get_net_id(world.crates[2]);
	REQUIRE(crate_id != 0);
	world.network.disconnect_peers(1, 2);
	world.network.disconnect_peers(1, 3);
	world.cores[1].stop();
	world.run(1.0);

	// Node 1 restarts, and node 4, which never met its previous process, joins at the same time: it first takes the
	// new process for the registry and the clock, until nodes 2 and 3 tell it otherwise.
	RestartedNode fresh(world, 1);
	world.network.connect_peers(1, 2);
	world.network.connect_peers(1, 3);
	world.network.connect_peers(4, 1);
	world.network.connect_peers(4, 2);
	world.network.connect_peers(4, 3);
	world.run(2.5);
	CHECK(fresh.core.get_roles_term() == 1);
	CHECK(fresh.core.get_net_id(fresh.crate) == crate_id);
	CHECK(fresh.core.get_stats().malformed_packets == 0);
	for (int i = 2; i <= 4; i++) {
		CHECK(world.cores[i].get_roles_term() == 1);
		CHECK(world.cores[i].get_settings().registry_peer == 1);
		CHECK(world.cores[i].get_net_id(world.crates[i]) == crate_id);
		CHECK(world.cores[i].get_owner(world.crates[i]) == 2);
		CHECK(world.cores[i].get_clock().is_synchronized());
		const int64_t apart = int64_t(world.cores[i].get_frame()) - int64_t(fresh.core.get_frame());
		CHECK((apart >= -3 && apart <= 3));
		CHECK(world.cores[i].get_stats().malformed_packets == 0);
	}
	CHECK(world.cores[4].get_frame() > 300);
	// The crate moves on its owner, and node 4 follows it.
	const float before = world.crates[2]->position.x;
	world.run(1.0);
	CHECK(world.crates[2]->position.x > before + 3.0);
	CHECK(Math::abs(world.crates[4]->position.x - world.crates[2]->position.x) < 0.5);
	CHECK(Math::abs(fresh.crate->position.x - world.crates[2]->position.x) < 0.5);
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] A node that only lost its link to the registry doesn't take its place") {
	MeshWorld world(3);
	world.run(2.0);
	// Node 2 would be the successor, but node 3 still sees node 1.
	world.network.disconnect_peers(1, 2);
	world.run(1.5);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_settings().registry_peer == 1);
		CHECK(world.cores[i].get_settings().clock_master == 1);
		CHECK(world.cores[i].get_roles_term() == 0);
		CHECK(world.listeners[i].roles_changes == 0);
	}
	// Node 2 can't reach the registry: it still has the crate, and can't give it away.
	ERR_PRINT_OFF;
	CHECK(world.cores[2].release_authority(world.crates[2], 3) == ERR_UNAVAILABLE);
	ERR_PRINT_ON;
	// The registry orphaned node 2's crate (it lost that node), and node 3 saw it.
	CHECK(world.cores[3].get_owner(world.crates[3]) == 0);

	// The link comes back: the same process has the roles, and node 2 goes on with it.
	world.network.connect_peers(1, 2);
	world.run(1.0);
	CHECK(world.cores[2].get_roles_term() == 0);
	CHECK(world.cores[2].get_owner(world.crates[2]) == 0);
	CHECK(world.cores[2].request_authority(world.crates[2]) == OK);
	world.run(1.0);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 2);
		CHECK(world.cores[i].get_stats().malformed_packets == 0);
	}

	// Once node 3 loses node 1 too, node 2 takes the roles.
	world.network.remove_peer(1);
	world.run(1.5);
	for (int i = 2; i <= 3; i++) {
		CHECK(world.cores[i].get_settings().registry_peer == 2);
		CHECK(world.cores[i].get_roles_term() == 1);
	}
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] A node without its quorum neither takes nor uses the roles") {
	// Two of the three nodes are needed.
	MeshWorld world(3, true, Vector<int>(), 2);
	world.run(2.0);
	REQUIRE(world.cores[1].get_owner(world.crates[1]) == 2);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].has_role_quorum());
	}

	// Node 1, with the roles, is cut off: it's told, and it doesn't orphan the objects of the nodes it lost.
	world.isolate(1);
	world.run(1.5);
	CHECK_FALSE(world.cores[1].has_role_quorum());
	CHECK_FALSE(world.listeners[1].has_quorum);
	CHECK(world.cores[1].get_settings().registry_peer == 1);
	CHECK(world.cores[1].get_owner(world.crates[1]) == 2);
	CHECK(world.listeners[1].orphaned == 0);
	// The other two have their quorum: node 2 took the roles.
	for (int i = 2; i <= 3; i++) {
		CHECK(world.cores[i].has_role_quorum());
		CHECK(world.cores[i].get_settings().registry_peer == 2);
		CHECK(world.cores[i].get_roles_term() == 1);
		CHECK(world.cores[i].get_owner(world.crates[i]) == 2);
	}
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(0.5);
	CHECK(world.cores[2].get_owner(world.crates[2]) == 3);

	// Back in the mesh, node 1 finds the roles moved and follows.
	world.reconnect(1);
	world.run(1.5);
	CHECK(world.cores[1].has_role_quorum());
	CHECK(world.listeners[1].has_quorum);
	CHECK(world.listeners[1].quorum_changes >= 2);
	CHECK(world.cores[1].get_settings().registry_peer == 2);
	CHECK(world.cores[1].get_settings().clock_master == 2);
	CHECK(world.cores[1].get_roles_term() == 1);
	CHECK(world.listeners[1].roles_changes == 1);
	CHECK(world.cores[1].get_owner(world.crates[1]) == 3);

	// A node alone never takes the roles: nodes 2 and 3 cut off from each other and from node 1.
	world.isolate(2);
	world.isolate(3);
	world.run(1.5);
	CHECK_FALSE(world.cores[3].has_role_quorum());
	CHECK(world.cores[3].get_settings().registry_peer == 2);
	CHECK(world.cores[3].get_roles_term() == 1);
	CHECK(world.cores[1].get_roles_term() == 1);
}

// Steps the world and returns the longest run of steps in which a node's frame didn't advance.
static int longest_stall(MeshWorld &r_world, int p_node, int p_steps) {
	int longest = 0;
	int stalled = 0;
	uint32_t last = r_world.cores[p_node].get_frame();
	for (int i = 0; i < p_steps; i++) {
		r_world.run(1.0 / 60.0);
		const uint32_t frame = r_world.cores[p_node].get_frame();
		stalled = frame == last ? stalled + 1 : 0;
		longest = MAX(longest, stalled);
		// The frames never go back.
		CHECK(int32_t(frame - last) >= 0);
		last = frame;
	}
	return longest;
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] The nodes go on simulating while the clock moves") {
	MeshWorld world(3);
	world.run(2.0);
	const int usual = longest_stall(world, 3, 60);

	// Moved by the project: node 3 goes on from the timeline it followed while it measures the new clock.
	const uint32_t before = world.cores[3].get_frame();
	CHECK(world.cores[1].change_roles(1, 2) == OK);
	const int moving = longest_stall(world, 3, 60);
	CHECK(moving <= usual + 1);
	CHECK(int32_t(world.cores[3].get_frame() - before) >= 58);
	CHECK(world.cores[3].get_settings().clock_master == 2);
	// The node that was the clock goes on too.
	CHECK(world.cores[1].get_clock().is_synchronized());
	world.run(1.0);
	for (int i = 1; i <= 3; i += 2) {
		const int64_t apart = int64_t(world.cores[i].get_frame()) - int64_t(world.cores[2].get_frame());
		CHECK((apart >= -3 && apart <= 3));
		CHECK_FALSE(world.cores[i].get_clock().is_holding());
	}

	// Lost with its node: the same.
	const uint32_t before_loss = world.cores[3].get_frame();
	world.network.remove_peer(2);
	const int failing = longest_stall(world, 3, 90);
	CHECK(failing <= usual + 1);
	CHECK(int32_t(world.cores[3].get_frame() - before_loss) >= 87);
	CHECK(world.cores[3].get_settings().clock_master == 1);
	world.run(1.0);
	const int64_t apart = int64_t(world.cores[3].get_frame()) - int64_t(world.cores[1].get_frame());
	CHECK((apart >= -3 && apart <= 3));
}

TEST_CASE("[Modules][TickSynchronizer][MeshCore] The spawns of a node that left belong to the registry") {
	MeshWorld world(4, false);
	world.network.connect_peers(1, 2);
	world.network.connect_peers(1, 3);
	world.network.connect_peers(2, 3);
	world.run(1.5);
	AuthMover *rock = memnew(AuthMover("Spawner/Rock", 3));
	const uint32_t spawn_id = world.cores[3].spawn("Spawner", 0, "Rock", 3, Variant());
	world.cores[3].register_object(rock);
	world.run(1.0);
	REQUIRE(world.listeners[1].spawned.size() == 1);
	CHECK(world.cores[3].owns_spawn(spawn_id));
	CHECK_FALSE(world.cores[1].owns_spawn(spawn_id));

	// Node 3 is lost: the registry has its spawn now.
	world.cores[3].unregister_object(rock);
	memdelete(rock);
	world.network.remove_peer(3);
	world.run(1.0);
	CHECK(world.cores[1].owns_spawn(spawn_id));
	CHECK_FALSE(world.cores[2].owns_spawn(spawn_id));

	// A node that joins later gets it from the registry.
	world.network.connect_peers(4, 1);
	world.network.connect_peers(4, 2);
	world.run(1.5);
	REQUIRE(world.listeners[4].spawned.size() == 1);
	CHECK(world.cores[4].get_owner(world.listeners[4].spawned[0]) == 0);
	CHECK_FALSE(world.cores[4].owns_spawn(spawn_id));

	// The registry moves: the spawn goes with it.
	CHECK(world.cores[1].change_roles(2, 1) == OK);
	world.run(1.5);
	CHECK(world.cores[2].owns_spawn(spawn_id));
	CHECK_FALSE(world.cores[1].owns_spawn(spawn_id));

	// And the registry removes it for every node.
	world.cores[2].despawn(spawn_id);
	world.run(0.5);
	CHECK(world.listeners[1].despawned == 1);
	CHECK(world.listeners[4].despawned == 1);
	CHECK_FALSE(world.cores[2].owns_spawn(spawn_id));
	for (int i = 1; i <= 4; i++) {
		if (i != 3) {
			CHECK(world.cores[i].get_stats().malformed_packets == 0);
		}
	}
}


// A release sent to a registry that is still taking over carries the version the object had before the takeover, and
// the registry announces every object again, with another version, once it finishes: the release is still taken,
// instead of leaving the object frozen with nobody simulating it.
TEST_CASE("[Modules][TickSynchronizer][MeshCore] An object released while the registry moves changes owner") {
	MeshWorld world(3);
	world.run(2.0);
	REQUIRE(world.cores[2].get_owner(world.crates[2]) == 2);

	CHECK(world.cores[1].change_roles(3, 1) == OK);
	// As soon as node 2 knows the new registry, which is still gathering every node's view of the objects.
	for (int i = 0; i < 30 && world.cores[2].get_settings().registry_peer != 3; i++) {
		world.run(1.0 / 60.0);
	}
	REQUIRE(world.cores[2].get_settings().registry_peer == 3);
	CHECK(world.cores[2].release_authority(world.crates[2], 1) == OK);
	world.run(2.0);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 1);
	}
	CHECK(world.listeners[2].denied == 0);
	// The new owner simulates it, and the others follow.
	const float before = world.crates[1]->position.x;
	world.run(1.0);
	CHECK(world.crates[1]->position.x > before + 3.0);
	CHECK(Math::abs(world.crates[2]->position.x - world.crates[1]->position.x) < 0.5);
	CHECK(Math::abs(world.crates[3]->position.x - world.crates[1]->position.x) < 0.5);
}


// A registry without its quorum changes no owner. The owner of an object it's asked to release stopped simulating
// it: the registry tells it at once that the release didn't happen, and the object goes on with its owner.
TEST_CASE("[Modules][TickSynchronizer][MeshCore] A release the registry can't take is denied, and the object stays with its owner") {
	// The three nodes are needed.
	MeshWorld world(3, true, Vector<int>(), 3);
	world.run(2.0);
	REQUIRE(world.cores[2].get_owner(world.crates[2]) == 2);

	// The registry loses node 3; node 2 still reaches both.
	world.network.disconnect_peers(1, 3);
	world.run(1.0);
	CHECK_FALSE(world.cores[1].has_role_quorum());
	CHECK(world.cores[2].release_authority(world.crates[2], 3) == OK);
	world.run(1.0);
	CHECK(world.listeners[2].denied == 1);
	CHECK(world.cores[2].get_owner(world.crates[2]) == 2);
	float before = world.crates[2]->position.x;
	world.run(1.0);
	CHECK(world.crates[2]->position.x > before + 3.0);

	// With its quorum again, the registry takes the same release.
	world.network.connect_peers(1, 3);
	world.run(1.0);
	CHECK(world.cores[1].has_role_quorum());
	CHECK(world.cores[2].release_authority(world.crates[2], 3) == OK);
	world.run(1.0);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 3);
	}
	CHECK(world.listeners[2].denied == 1);
	before = world.crates[3]->position.x;
	world.run(1.0);
	CHECK(world.crates[3]->position.x > before + 3.0);
}


// The owner stops simulating an object while it releases it. When the registry is lost before answering, and no node
// may take its place, nobody will ever answer: the object goes on with its owner, and the game is told.
TEST_CASE("[Modules][TickSynchronizer][MeshCore] A release the lost registry didn't answer leaves the object with its owner") {
	// Only node 1 may have the roles.
	Vector<int> candidates;
	candidates.push_back(1);
	MeshWorld world(3, true, candidates);
	world.run(2.0);
	REQUIRE(world.cores[2].get_owner(world.crates[2]) == 2);

	// The registry's machine goes away with the release on its way.
	world.isolate(1);
	world.cores[1].stop();
	CHECK(world.cores[2].release_authority(world.crates[2], 3) == OK);
	world.run(1.5);
	CHECK(world.cores[2].get_settings().registry_peer == 1);
	CHECK(world.listeners[2].denied == 1);
	CHECK(world.cores[2].get_owner(world.crates[2]) == 2);
	const float before = world.crates[2]->position.x;
	world.run(1.0);
	CHECK(world.crates[2]->position.x > before + 3.0);
	CHECK(Math::abs(world.crates[3]->position.x - world.crates[2]->position.x) < 0.5);
}


// Releasing an object to the node that owns it changes nothing: no announcement would ever end that release.
TEST_CASE("[Modules][TickSynchronizer][MeshCore] An object released to its own owner goes on being simulated") {
	MeshWorld world(3);
	world.run(2.0);
	REQUIRE(world.cores[2].get_owner(world.crates[2]) == 2);
	const uint32_t version = world.cores[2].get_version(world.crates[2]);
	CHECK(world.cores[2].release_authority(world.crates[2], 2) == OK);
	world.run(1.0);
	const float before = world.crates[2]->position.x;
	world.run(1.0);
	CHECK(world.crates[2]->position.x > before + 3.0);
	CHECK(Math::abs(world.crates[3]->position.x - world.crates[2]->position.x) < 0.5);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 2);
		CHECK(world.cores[i].get_version(world.crates[i]) == version);
	}
	CHECK(world.listeners[2].denied == 0);
	// It can still give the object away.
	CHECK(world.cores[2].release_authority(world.crates[2], 3) == OK);
	world.run(1.0);
	CHECK(world.cores[1].get_owner(world.crates[1]) == 3);
}


// The roles moved before (the term isn't the first one), the other candidate is down, and the process of the node
// with the roles restarts. It starts as the clock, from frame 0, and learns from the hello of the node that stayed
// that the mesh moved on: it takes the roles again from that node's frame, instead of starting the timeline over.
// With the timeline started over, the frames of the states go back, and the nodes would stop following each other.
TEST_CASE("[Modules][TickSynchronizer][MeshCore] A clock that restarts after the roles moved goes on from the mesh's frame") {
	Vector<int> candidates;
	candidates.push_back(1);
	candidates.push_back(2);
	MeshWorld world(3, true, candidates);
	// Node 3 has an object of its own, and node 1 another.
	AuthMover *barrel_3 = memnew(AuthMover("barrel", 1));
	AuthMover *barrel_1 = memnew(AuthMover("barrel", 1));
	world.cores[3].register_object(barrel_3);
	world.cores[1].register_object(barrel_1);
	world.run(2.0);
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(0.5);
	REQUIRE(world.cores[3].get_owner(world.crates[3]) == 3);
	REQUIRE(world.cores[3].get_owner(barrel_3) == 1);
	CHECK(world.cores[1].change_roles(2, 2) == OK);
	world.run(1.0);
	CHECK(world.cores[2].change_roles(1, 1) == OK);
	world.run(3.0);
	REQUIRE(world.cores[3].get_roles_term() == 2);
	REQUIRE(world.cores[3].get_settings().clock_master == 1);

	// Node 2 goes down; then node 1's process restarts. Node 3 isn't a candidate: nobody takes the roles meanwhile.
	world.isolate(2);
	world.cores[2].stop();
	world.run(1.0);
	const uint32_t frame_before = world.cores[3].get_frame();
	world.cores[1].unregister_object(barrel_1);
	world.isolate(1);
	RestartedNode fresh(world, 1);
	fresh.core.register_object(barrel_1);
	world.run(1.0);
	world.network.connect_peers(1, 3);
	world.run(4.0);

	CHECK(fresh.core.get_roles_term() == 3);
	CHECK(world.cores[3].get_roles_term() == 3);
	CHECK(fresh.core.get_settings().registry_peer == 1);
	CHECK(fresh.core.get_settings().clock_master == 1);
	// Five seconds later, on the same timeline.
	CHECK(int32_t(world.cores[3].get_frame() - frame_before) >= 290);
	CHECK(int32_t(world.cores[3].get_frame() - frame_before) <= 310);
	const int64_t apart = int64_t(fresh.core.get_frame()) - int64_t(world.cores[3].get_frame());
	CHECK((apart >= -3 && apart <= 3));
	// Each node follows the object of the other.
	REQUIRE(world.cores[3].get_owner(world.crates[3]) == 3);
	REQUIRE(fresh.core.get_owner(barrel_1) == 1);
	const float crate_before = world.crates[3]->position.x;
	const float barrel_before = barrel_1->position.x;
	world.run(1.0);
	CHECK(world.crates[3]->position.x > crate_before + 3.0);
	CHECK(Math::abs(fresh.crate->position.x - world.crates[3]->position.x) < 0.5);
	CHECK(barrel_1->position.x > barrel_before + 3.0);
	CHECK(Math::abs(barrel_3->position.x - barrel_1->position.x) < 0.5);

	fresh.core.unregister_object(barrel_1);
	memdelete(barrel_1);
	world.cores[3].unregister_object(barrel_3);
	memdelete(barrel_3);
}


// The terms of the roles wrap around, and one too far ahead isn't believed. Before, a single message with the highest
// term made the next term 0, which every node took for older: the roles could never move again.
TEST_CASE("[Modules][TickSynchronizer][MeshCore] The terms of the roles wrap around, and one too far ahead is refused") {
	MeshWorld world(4);
	world.run(2.0);
	// A mesh on its first term takes any term (a node that joins takes the mesh's): here, the highest there is.
	for (int to = 1; to <= 3; to++) {
		TickDataBuffer roles;
		roles.begin_write();
		roles.add_uint_bits(TICK_MESSAGE_ROLES, 8);
		roles.add_uint_bits(0xFFFFFFFF, 32);
		roles.add_int_bits(1, 32);
		roles.add_int_bits(1, 32);
		roles.add_uint_bits(0, 32);
		roles.add_uint_bits(0, 32);
		world.send_raw(4, to, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, roles);
	}
	world.run(1.0);
	for (int i = 1; i <= 4; i++) {
		CHECK(world.cores[i].get_roles_term() == 0xFFFFFFFF);
		CHECK(world.cores[i].get_settings().registry_peer == 1);
	}
	// The node with the roles is lost: node 2 takes them with the term after the highest, and the others follow.
	world.network.remove_peer(1);
	world.run(2.0);
	for (int i = 2; i <= 4; i++) {
		CHECK(world.cores[i].get_settings().registry_peer == 2);
		CHECK(world.cores[i].get_settings().clock_master == 2);
		CHECK(world.cores[i].get_roles_term() == 1);
		CHECK(world.cores[i].get_stats().role_changes <= 3);
	}
	CHECK(world.cores[3].request_authority(world.crates[3]) == OK);
	world.run(1.0);
	CHECK(world.cores[2].get_owner(world.crates[2]) == 3);

	// From then on, a term that far ahead isn't of this mesh: the message is counted as malformed and changes nothing.
	const uint64_t malformed = world.cores[3].get_stats().malformed_packets;
	TickDataBuffer roles;
	roles.begin_write();
	roles.add_uint_bits(TICK_MESSAGE_ROLES, 8);
	roles.add_uint_bits(0x7FFFFFF0, 32);
	roles.add_int_bits(4, 32);
	roles.add_int_bits(4, 32);
	roles.add_uint_bits(0, 32);
	roles.add_uint_bits(0, 32);
	world.send_raw(4, 3, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, roles);
	world.run(1.0);
	CHECK(world.cores[3].get_stats().malformed_packets == malformed + 1);
	for (int i = 2; i <= 4; i++) {
		CHECK(world.cores[i].get_roles_term() == 1);
		CHECK(world.cores[i].get_settings().registry_peer == 2);
	}
}


// A version too far ahead of what the registry knows isn't believed, and an announcement must name an owner and a
// version. Before, a report with the highest version made the next change of owner wrap to a version every node took
// for older, and the object stayed frozen.
TEST_CASE("[Modules][TickSynchronizer][MeshCore] Versions and owners that can't be true are refused") {
	MeshWorld world(3);
	world.run(2.0);
	const uint16_t id = world.cores[2].get_net_id(world.crates[2]);
	REQUIRE(id != 0);
	REQUIRE(world.cores[2].get_owner(world.crates[2]) == 2);
	const uint32_t version = world.cores[2].get_version(world.crates[2]);
	const uint32_t schema_hash = world.crates[2]->get_sync_schema().hash();

	// Node 3 reports the object to the registry with a version nearly at the highest.
	TickDataBuffer report;
	report.begin_write();
	report.add_uint_bits(TICK_MESSAGE_REGISTRY_REPORT, 8);
	report.add_uint_bits(world.cores[1].get_roles_term(), 32);
	report.add_bool(true);
	report.add_uint_bits(id, 16);
	report.add_uint_bits(1, 16);
	report.add_uint_bits(id, 16);
	report.add_string("crate");
	report.add_int_bits(3, 32);
	report.add_uint_bits(0xFFFFFFFE, 32);
	report.add_uint_bits(world.cores[1].get_frame(), 32);
	report.add_uint_bits(schema_hash, 32);
	world.send_raw(3, 1, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, report);
	world.run(1.0);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 2);
		CHECK(world.cores[i].get_version(world.crates[i]) == version);
	}

	// An announcement without a version, or with an owner below zero, is malformed.
	for (int bad = 0; bad < 2; bad++) {
		TickDataBuffer announce;
		announce.begin_write();
		announce.add_uint_bits(TICK_MESSAGE_ANNOUNCE, 8);
		announce.add_uint_bits(id, 16);
		announce.add_string("crate");
		announce.add_int_bits(bad == 0 ? -7 : 3, 32);
		announce.add_uint_bits(bad == 0 ? version + 1 : 0, 32);
		announce.add_uint_bits(world.cores[1].get_frame(), 32);
		announce.add_uint_bits(schema_hash, 32);
		announce.add_bool(false);
		world.send_raw(1, 3, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, announce);
	}
	world.run(0.5);
	CHECK(world.cores[3].get_stats().malformed_packets == 2);
	CHECK(world.cores[3].get_owner(world.crates[3]) == 2);
	CHECK(world.cores[3].get_version(world.crates[3]) == version);

	// Ownership still changes through the registry.
	CHECK(world.cores[1].request_authority(world.crates[1]) == OK);
	world.run(1.0);
	for (int i = 1; i <= 3; i++) {
		CHECK(world.cores[i].get_owner(world.crates[i]) == 1);
	}
	const float before = world.crates[1]->position.x;
	world.run(1.0);
	CHECK(world.crates[1]->position.x > before + 3.0);
	CHECK(Math::abs(world.crates[3]->position.x - world.crates[1]->position.x) < 0.5);
}


// A state with a frame far ahead of the node's own (from a node whose clock is off) isn't taken: it would leave every
// state that comes after it looking older. Messages for a role a node doesn't have aren't malformed: they have their
// own counter.
TEST_CASE("[Modules][TickSynchronizer][MeshCore] States far ahead of the timeline and messages for another role are set aside") {
	MeshWorld world(3);
	world.run(2.0);
	const uint16_t id = world.cores[2].get_net_id(world.crates[2]);
	REQUIRE(id != 0);
	// A few states are late in any mesh: while the clocks settle, a node may send two states of the same frame.
	const uint64_t late_before = world.cores[3].get_stats().late_states;

	// As node 2, the owner, would send it: the crate far to the left, ten minutes ahead.
	TickDataBuffer payload;
	payload.begin_write();
	world.crates[2]->get_sync_schema().codecs[0]->encode(Vector2(-500.0, 0.0), payload);
	world.crates[2]->get_sync_schema().codecs[1]->encode(Vector2(), payload);
	TickDataBuffer state;
	state.begin_write();
	state.add_uint_bits(TICK_MESSAGE_STATE, 8);
	state.add_uint_bits(world.cores[3].get_frame() + 36000, 32);
	state.add_uint_bits(1, 16);
	state.add_uint_bits(id, 16);
	state.add_uint_bits(world.cores[2].get_version(world.crates[2]), 32);
	state.add_data_buffer(payload);
	world.send_raw(2, 3, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_UNRELIABLE, state);
	world.run(1.0);
	CHECK(world.cores[3].get_stats().late_states == late_before + 1);
	CHECK(world.crates[3]->position.x > 0.0);
	CHECK(Math::abs(world.crates[3]->position.x - world.crates[2]->position.x) < 0.5);

	// A ping for the clock, sent to a node that isn't it.
	TickDataBuffer ping;
	ping.begin_write();
	ping.add_uint_bits(TICK_MESSAGE_PING, 8);
	ping.add_uint_bits(world.network.get_time_usec(), 64);
	world.send_raw(2, 3, TICK_CHANNEL_STATS, TickTransport::TRANSFER_MODE_UNRELIABLE_ORDERED, ping);
	world.run(0.5);
	CHECK(world.cores[3].get_stats().unexpected_packets == 1);
	CHECK(world.cores[3].get_stats().malformed_packets == 0);
}

} // namespace TestTickMeshCore

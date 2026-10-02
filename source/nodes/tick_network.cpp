// Implementation of `TickNetwork`: the settings it hands to its engine, the engine's callbacks turned into signals and
// into calls to the game's functions, and the node's processing, which drives the engine.

#include "tick_network.h"

#include "tick_object.h"
#include "tick_spawner.h"

#include "core/config/engine.h"
#include "core/os/os.h"

static const char *TICK_NETWORK_GROUP = "_tick_networks";

// Makes the node listen to its two engines.
TickNetwork::TickNetwork() {
	single_core.set_listener(this);
	mesh_core.set_listener(this);
}


// Stops the engine, and stops listening to both.
TickNetwork::~TickNetwork() {
	if (running) {
		engine->stop();
	}
	single_core.set_listener(nullptr);
	mesh_core.set_listener(nullptr);
}


// Joins the group the objects find a network by, stops the network when the node leaves the tree, and drives the
// engine: its ticks every physics frame, the interpolation every rendered frame.
void TickNetwork::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			add_to_group(TICK_NETWORK_GROUP);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			stop();
			remove_from_group(TICK_NETWORK_GROUP);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			advance(get_physics_process_delta_time(), OS::get_singleton()->get_ticks_usec());
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			update_interpolation(OS::get_singleton()->get_ticks_usec());
		} break;
	}
}


// Turns the node's internal processing on while the network runs and off when it stops; never in the editor.
void TickNetwork::_update_processing() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_physics_process_internal(running);
	set_process_internal(running);
}


// `TickEngine::Listener`: emits `peer_ready`.
void TickNetwork::on_peer_ready(int p_peer) {
	emit_signal(SNAME("peer_ready"), p_peer);
}


// `TickEngine::Listener`: emits `peer_left`.
void TickNetwork::on_peer_left(int p_peer) {
	emit_signal(SNAME("peer_left"), p_peer);
}


// `TickEngine::Listener`: emits `rejected`.
void TickNetwork::on_rejected(const String &p_reason) {
	emit_signal(SNAME("rejected"), p_reason);
}


// `TickEngine::Listener`: emits `prediction_started`.
void TickNetwork::on_prediction_started(uint32_t p_frame) {
	emit_signal(SNAME("prediction_started"), int64_t(p_frame));
}


// `TickEngine::Listener`: emits `rewound`.
void TickNetwork::on_rewound(uint32_t p_frame, int p_frame_count) {
	emit_signal(SNAME("rewound"), int64_t(p_frame), p_frame_count);
}


// `TickEngine::Listener`: asks `event_validator` about an event without target object; -1 when there's no
// validator.
int TickNetwork::validate_network_event(int p_sender, const StringName &p_event, const Variant &p_payload) {
	if (!event_validator.is_valid()) {
		return -1;
	}
	const Variant result = event_validator.call(p_sender, p_event, p_payload);
	return bool(result) ? 1 : 0;
}


// `TickEngine::Listener`: emits `event_received`.
void TickNetwork::on_network_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) {
	emit_signal(SNAME("event_received"), p_sender, p_event, p_payload, int64_t(p_frame));
}


// `TickEngine::Listener`: hands a spawn of another peer to the `TickSpawner` at that path.
void TickNetwork::on_spawn(const String &p_spawner, uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	Node *root = get_root_node();
	ERR_FAIL_NULL(root);
	TickSpawner *spawner = Object::cast_to<TickSpawner>(root->get_node_or_null(NodePath(p_spawner)));
	ERR_FAIL_NULL_MSG(spawner, vformat("A peer spawned with \"%s\", but there's no TickSpawner at that path.", p_spawner));
	spawner->client_spawn(p_spawn_id, p_scene, p_name, p_controller, p_data);
}


// `TickEngine::Listener`: hands a despawn of another peer to the `TickSpawner` at that path.
void TickNetwork::on_despawn(const String &p_spawner, uint32_t p_spawn_id) {
	Node *root = get_root_node();
	ERR_FAIL_NULL(root);
	TickSpawner *spawner = Object::cast_to<TickSpawner>(root->get_node_or_null(NodePath(p_spawner)));
	ERR_FAIL_NULL_MSG(spawner, vformat("A peer despawned with \"%s\", but there's no TickSpawner at that path.", p_spawner));
	spawner->client_despawn(p_spawn_id);
}


// The `Object` behind a synchronized object, to report it in signals; null when there's none.
static Object *get_instance(TickSyncObject *p_object) {
	return p_object ? p_object->get_sync_instance() : nullptr;
}


// `TickEngine::Listener`: emits `authority_changed`.
void TickNetwork::on_authority_changed(TickSyncObject *p_object, int p_old_owner, int p_new_owner) {
	emit_signal(SNAME("authority_changed"), get_instance(p_object), p_old_owner, p_new_owner);
}


// `TickEngine::Listener`: emits `authority_orphaned`.
void TickNetwork::on_authority_orphaned(TickSyncObject *p_object, int p_last_owner, uint32_t p_last_frame) {
	emit_signal(SNAME("authority_orphaned"), get_instance(p_object), p_last_owner, int64_t(p_last_frame));
}


// `TickEngine::Listener`: emits `authority_request_denied`.
void TickNetwork::on_authority_request_denied(TickSyncObject *p_object) {
	emit_signal(SNAME("authority_request_denied"), get_instance(p_object));
}


// `TickEngine::Listener`: asks `interest_filter` whether an object is relevant to a peer; -1 when there's no
// filter, or it doesn't answer with a boolean.
int TickNetwork::filter_relevance(int p_peer, TickSyncObject *p_object) {
	if (!interest_filter.is_valid()) {
		return -1;
	}
	const Variant result = interest_filter.call(p_peer, get_instance(p_object));
	return result.get_type() == Variant::BOOL ? (bool(result) ? 1 : 0) : -1;
}


// `TickEngine::Listener`: tells the object, then emits `relevance_changed`.
void TickNetwork::on_relevance_changed(TickSyncObject *p_object, bool p_relevant) {
	p_object->on_relevance_changed(p_relevant);
	emit_signal(SNAME("relevance_changed"), get_instance(p_object), p_relevant);
}


// `TickEngine::Listener`: emits `roles_changed`.
void TickNetwork::on_roles_changed(int p_registry, int p_clock_master) {
	emit_signal(SNAME("roles_changed"), p_registry, p_clock_master);
}


// `TickEngine::Listener`: emits `role_quorum_changed`.
void TickNetwork::on_role_quorum_changed(bool p_has_quorum) {
	emit_signal(SNAME("role_quorum_changed"), p_has_quorum);
}


// `TickEngine::Listener`: emits `host_migrated`.
void TickNetwork::on_host_migrated(int p_old_host, int p_new_host) {
	emit_signal(SNAME("host_migrated"), p_old_host, p_new_host);
}


// Interest (ADR-053): sets whether objects are relevant to every client until told otherwise.
void TickNetwork::set_default_relevance(bool p_relevant) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	settings.default_relevant = p_relevant;
}


// See `set_default_relevance()`.
bool TickNetwork::get_default_relevance() const {
	return settings.default_relevant;
}


// Interest: sets how often, in ticks, `interest_filter` is asked.
void TickNetwork::set_interest_interval(int p_ticks) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_ticks < 1, "The interest interval must be at least 1.");
	settings.interest_interval = p_ticks;
}


// See `set_interest_interval()`.
int TickNetwork::get_interest_interval() const {
	return settings.interest_interval;
}


// Interest, on the server: starts or stops sending an object's state to `p_peer` (0: every client).
Error TickNetwork::set_object_relevant(TickSyncObject *p_object, int p_peer, bool p_relevant) {
	ERR_FAIL_NULL_V(p_object, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	return engine->set_relevant(p_object, p_peer, p_relevant);
}


// Interest: whether an object's state is sent to `p_peer`; on a client, to this client.
bool TickNetwork::is_object_relevant(const TickSyncObject *p_object, int p_peer) const {
	return running && p_object && engine->is_relevant(p_object, p_peer);
}


// History (ADR-054): the state of an object at a frame of the authority's timeline, by variable name; empty when
// the frame isn't in the history.
Dictionary TickNetwork::get_state_at(const TickSyncObject *p_object, double p_frame) const {
	Dictionary state;
	LocalVector<Variant> values;
	if (!running || p_object == nullptr || !engine->get_state_at(p_object, p_frame, values)) {
		return state;
	}
	const TickSchema &schema = p_object->get_sync_schema();
	for (uint32_t i = 0; i < values.size() && int(i) < schema.size(); i++) {
		state[schema.names[i]] = values[i];
	}
	return state;
}


// The `TickObject` a script passed, as the engines see it; null for anything else.
static TickSyncObject *as_sync_object(Object *p_object) {
	return Object::cast_to<TickObject>(p_object);
}


// Script wrapper of `set_object_relevant()`: the object must be a `TickObject`.
Error TickNetwork::_set_object_relevant(Object *p_object, int p_peer, bool p_relevant) {
	TickSyncObject *object = as_sync_object(p_object);
	ERR_FAIL_NULL_V_MSG(object, ERR_INVALID_PARAMETER, "The object must be a TickObject.");
	return set_object_relevant(object, p_peer, p_relevant);
}


// Script wrapper of `is_object_relevant()`: the object must be a `TickObject`.
bool TickNetwork::_is_object_relevant(Object *p_object, int p_peer) const {
	const TickSyncObject *object = as_sync_object(p_object);
	ERR_FAIL_NULL_V_MSG(object, false, "The object must be a TickObject.");
	return is_object_relevant(object, p_peer);
}


// Script wrapper of `get_state_at()`: the object must be a `TickObject`.
Dictionary TickNetwork::_get_state_at(Object *p_object, double p_frame) const {
	const TickSyncObject *object = as_sync_object(p_object);
	ERR_FAIL_NULL_V_MSG(object, Dictionary(), "The object must be a TickObject.");
	return get_state_at(object, p_frame);
}


// The frame the interpolated objects show now; negative while it isn't known.
double TickNetwork::get_view_frame() const {
	return running ? engine->get_view_frame(last_update_usec) : -1.0;
}


// Chooses the engine: a single authority, or an owner per object. The registered objects move to the chosen engine.
// Not while the network runs.
void TickNetwork::set_authority_mode(AuthorityMode p_mode) {
	ERR_FAIL_COND_MSG(running, "Can't change the authority mode while the network is running.");
	if (p_mode == authority_mode) {
		return;
	}
	TickEngine *next = p_mode == AUTHORITY_DISTRIBUTED ? static_cast<TickEngine *>(&mesh_core) : static_cast<TickEngine *>(&single_core);
	for (TickSyncObject *object : registered_objects) {
		engine->unregister_object(object);
		next->register_object(object);
	}
	engine = next;
	authority_mode = p_mode;
}


// Distributed authority: sets the node that keeps the registry of owners. While the network runs, moves the role to
// that node.
void TickNetwork::set_registry_peer(int p_peer) {
	ERR_FAIL_COND_MSG(p_peer <= 0, "The registry peer must be positive.");
	if (running) {
		// The mesh moves the role while it runs (ADR-073); the configured one is for the next start.
		ERR_FAIL_COND_MSG(authority_mode != AUTHORITY_DISTRIBUTED, "Can't change the settings while the network is running.");
		engine->change_roles(p_peer, engine->get_settings().clock_master);
		return;
	}
	settings.registry_peer = p_peer;
}


// The registry's node; while the network runs, the current one.
int TickNetwork::get_registry_peer() const {
	return running ? engine->get_settings().registry_peer : settings.registry_peer;
}


// Distributed authority: sets the node whose clock the mesh follows. While the network runs, moves the role to that
// node.
void TickNetwork::set_clock_master(int p_peer) {
	ERR_FAIL_COND_MSG(p_peer <= 0, "The clock master must be positive.");
	if (running) {
		ERR_FAIL_COND_MSG(authority_mode != AUTHORITY_DISTRIBUTED, "Can't change the settings while the network is running.");
		engine->change_roles(engine->get_settings().registry_peer, p_peer);
		return;
	}
	settings.clock_master = p_peer;
}


// The clock master; while the network runs, the current one.
int TickNetwork::get_clock_master() const {
	return running ? engine->get_settings().clock_master : settings.clock_master;
}


// Distributed authority: sets the registry and the clock master at once; while the network runs, moves both roles
// together (ADR-074).
Error TickNetwork::set_roles(int p_registry_peer, int p_clock_master) {
	ERR_FAIL_COND_V_MSG(p_registry_peer <= 0 || p_clock_master <= 0, ERR_INVALID_PARAMETER, "The registry peer and the clock master must be positive.");
	if (running) {
		ERR_FAIL_COND_V_MSG(authority_mode != AUTHORITY_DISTRIBUTED, ERR_UNAVAILABLE, "Only a network with distributed authority moves its roles while it runs.");
		return engine->change_roles(p_registry_peer, p_clock_master);
	}
	settings.registry_peer = p_registry_peer;
	settings.clock_master = p_clock_master;
	return OK;
}


// Distributed authority: sets the nodes that take the roles when their node leaves, by order of preference (empty:
// any node, the lowest id first).
void TickNetwork::set_role_candidates(const PackedInt32Array &p_candidates) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	for (const int candidate : p_candidates) {
		ERR_FAIL_COND_MSG(candidate <= 0, "The role candidates must be positive.");
	}
	settings.role_candidates = p_candidates;
}


// See `set_role_candidates()`.
PackedInt32Array TickNetwork::get_role_candidates() const {
	return settings.role_candidates;
}


// Distributed authority: sets how many nodes, this one included, a node must be connected to in order to take or
// keep the roles (0: no check).
void TickNetwork::set_role_quorum(int p_nodes) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_nodes < 0, "The role quorum can't be negative.");
	settings.role_quorum = p_nodes;
}


// See `set_role_quorum()`.
int TickNetwork::get_role_quorum() const {
	return settings.role_quorum;
}


// Whether this node is connected to enough nodes to take or keep the roles; `true` while the network isn't running.
bool TickNetwork::has_role_quorum() const {
	return !running || engine->has_role_quorum();
}


// Distributed authority: sets how often, in ticks, every object's state is sent even if it didn't change.
void TickNetwork::set_keyframe_interval(int p_ticks) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_ticks < 1, "The keyframe interval must be at least 1.");
	settings.keyframe_interval = p_ticks;
}


// See `set_keyframe_interval()`.
int TickNetwork::get_keyframe_interval() const {
	return settings.keyframe_interval;
}


// The peer with the authority over an object (ADR-041): the authority peer, or the object's owner in a distributed
// mesh.
int TickNetwork::get_object_owner(const TickSyncObject *p_object) const {
	return engine->get_owner(p_object);
}


// Distributed authority: asks for the authority over an object.
Error TickNetwork::request_authority(TickSyncObject *p_object) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(authority_mode != AUTHORITY_DISTRIBUTED, ERR_UNAVAILABLE, "Authority can only change in a network with distributed authority.");
	return engine->request_authority(p_object);
}


// Distributed authority: gives up the authority over an object, to `p_to_peer` or to nobody (0).
Error TickNetwork::release_authority(TickSyncObject *p_object, int p_to_peer) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(authority_mode != AUTHORITY_DISTRIBUTED, ERR_UNAVAILABLE, "Authority can only change in a network with distributed authority.");
	return engine->release_authority(p_object, p_to_peer);
}


// Distributed authority: as the registry, gives the authority over an object to `p_peer`.
Error TickNetwork::assign_authority(TickSyncObject *p_object, int p_peer) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(authority_mode != AUTHORITY_DISTRIBUTED, ERR_UNAVAILABLE, "Authority can only change in a network with distributed authority.");
	return engine->assign_authority(p_object, p_peer);
}


// Single authority: sets the peer that simulates every object and is the clock master.
void TickNetwork::set_authority_peer(int p_peer) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_peer <= 0, "The authority peer must be positive.");
	settings.authority_peer = p_peer;
}


// The authority peer; while the network runs, the current one (it changes with a host migration).
int TickNetwork::get_authority_peer() const {
	// It changes with a host migration.
	return running ? engine->get_settings().authority_peer : settings.authority_peer;
}


// Sets whether the remote objects are interpolated, or just get the latest state (proxies on servers that relay
// them).
void TickNetwork::set_interpolate_remote(bool p_enabled) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	settings.interpolate_remote = p_enabled;
}


// See `set_interpolate_remote()`.
bool TickNetwork::is_interpolating_remote() const {
	return settings.interpolate_remote;
}


// Sets another `TickNetwork` whose timeline this one follows instead of the local delta (ADR-038). Only the peer
// that owns this network's timeline can follow another.
void TickNetwork::set_clock_network(const NodePath &p_path) {
	ERR_FAIL_COND_MSG(running, "Can't change the clock network while the network is running.");
	clock_network = p_path;
}


// Sets whether the other peers are trusted. What untrusted clients send is limited and checked.
void TickNetwork::set_trust(Trust p_trust) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	settings.trusted = p_trust == TRUST_TRUSTED;
}


// See `set_trust()`.
TickNetwork::Trust TickNetwork::get_trust() const {
	return settings.trusted ? TRUST_TRUSTED : TRUST_UNTRUSTED;
}


// Untrusted: sets how many events per second are accepted from each client.
void TickNetwork::set_max_events_per_second(int p_events) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_events < 1, "At least one event per second must be allowed.");
	settings.max_events_per_second = p_events;
}


// See `set_max_events_per_second()`.
int TickNetwork::get_max_events_per_second() const {
	return settings.max_events_per_second;
}


// Untrusted: sets the largest event payload accepted, in bytes (1 to 65535).
void TickNetwork::set_max_event_size(int p_bytes) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_bytes < 1 || p_bytes > UINT16_MAX, "The event size must be between 1 and 65535 bytes.");
	settings.max_event_bytes = p_bytes;
}


// See `set_max_event_size()`.
int TickNetwork::get_max_event_size() const {
	return settings.max_event_bytes;
}


// Sends an event without target object; `p_frame` < 0 uses the default (see `TickSyncCore::send_event()`).
Error TickNetwork::send_event(const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer) {
	return send_object_event(nullptr, p_event, p_payload, p_frame, p_peer);
}


// Sends an event to an object on other peers; the rest is as in `send_event()`.
Error TickNetwork::send_object_event(TickSyncObject *p_object, const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	return engine->send_event(p_object, p_event, p_payload, p_frame < 0 ? TICK_FRAME_NONE : uint32_t(p_frame), p_peer);
}


// The frame that is `p_seconds` from now, to schedule an event every peer runs together.
int64_t TickNetwork::get_event_frame(double p_seconds) const {
	return engine->get_event_frame(p_seconds);
}


// Called by `TickSpawner`: tells the other peers about a spawn. Returns its id; 0 when the network refused it.
uint32_t TickNetwork::spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	ERR_FAIL_COND_V_MSG(!running, 0, "The network isn't running.");
	return engine->spawn(p_spawner, p_scene, p_name, p_controller, p_data);
}


// Called by `TickSpawner`: tells the other peers a spawn was removed.
void TickNetwork::despawn(uint32_t p_spawn_id) {
	if (running) {
		engine->despawn(p_spawn_id);
	}
}


// Sets how many ticks a second has (1 to 1000). Like the other settings, it can't change while the network runs.
void TickNetwork::set_ticks_per_second(int p_ticks_per_second) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_ticks_per_second <= 0 || p_ticks_per_second > TICK_MAX_TICKS_PER_SECOND, vformat("The ticks per second must be between 1 and %d.", TICK_MAX_TICKS_PER_SECOND));
	settings.ticks_per_second = p_ticks_per_second;
}


// How many ticks a second has; while the network runs, the rate in use.
int TickNetwork::get_ticks_per_second() const {
	return running ? engine->get_settings().ticks_per_second : settings.ticks_per_second;
}


// Sets how many past inputs each input packet repeats, against packet loss.
void TickNetwork::set_input_redundancy(int p_redundancy) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_redundancy < 1 || p_redundancy > TICK_MAX_INPUT_FRAMES, vformat("The input redundancy must be between 1 and %d.", TICK_MAX_INPUT_FRAMES));
	settings.input_redundancy = p_redundancy;
}


// See `set_input_redundancy()`.
int TickNetwork::get_input_redundancy() const {
	return settings.input_redundancy;
}


// Sets how many frames of history are kept for snapshots, predictions and interpolation (at least 8).
void TickNetwork::set_history_size(int p_frames) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_frames < 8, "The history must keep at least 8 frames.");
	settings.history_size = p_frames;
}


// See `set_history_size()`.
int TickNetwork::get_history_size() const {
	return settings.history_size;
}


// Sets how many ticks pass between two snapshots.
void TickNetwork::set_snapshot_interval(int p_ticks) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_ticks < 1, "The snapshot interval must be at least 1.");
	settings.snapshot_interval = p_ticks;
}


// See `set_snapshot_interval()`.
int TickNetwork::get_snapshot_interval() const {
	return settings.snapshot_interval;
}


// Sets how far behind the authority the interpolated objects are shown, in seconds.
void TickNetwork::set_interpolation_delay(double p_seconds) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(!(p_seconds >= 0.0), "The interpolation delay can't be negative.");
	settings.interpolation_delay = p_seconds;
}


// See `set_interpolation_delay()`.
double TickNetwork::get_interpolation_delay() const {
	return settings.interpolation_delay;
}


// Sets the fewest inputs the server should have buffered ahead of its frame; raises the maximum if needed.
void TickNetwork::set_min_input_buffer(int p_frames) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_frames < 0, "The input buffer can't be negative.");
	settings.min_input_buffer = p_frames;
	settings.max_input_buffer = MAX(settings.max_input_buffer, p_frames);
}


// See `set_min_input_buffer()`.
int TickNetwork::get_min_input_buffer() const {
	return settings.min_input_buffer;
}


// Sets the most inputs the server should have buffered ahead of its frame; lowers the minimum if needed.
void TickNetwork::set_max_input_buffer(int p_frames) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_frames < 0, "The input buffer can't be negative.");
	settings.max_input_buffer = p_frames;
	settings.min_input_buffer = MIN(settings.min_input_buffer, p_frames);
}


// See `set_max_input_buffer()`.
int TickNetwork::get_max_input_buffer() const {
	return settings.max_input_buffer;
}


// Sets the node the paths of objects and spawners are relative to; every peer needs the same nodes under it.
void TickNetwork::set_root_path(const NodePath &p_path) {
	ERR_FAIL_COND_MSG(running, "Can't change the root path while the network is running.");
	root_path = p_path;
}


// See `set_root_path()`.
NodePath TickNetwork::get_root_path() const {
	return root_path;
}


// Node the object paths are relative to.
Node *TickNetwork::get_root_node() const {
	return is_inside_tree() ? get_node_or_null(root_path) : nullptr;
}


// Starts the network on a transport, with the settings and the authority mode chosen. Warns when the tick rate
// isn't the physics', and when a distributed mesh isn't marked as trusted.
Error TickNetwork::start(const Ref<TickTransport> &p_transport) {
	ERR_FAIL_COND_V_MSG(running, ERR_ALREADY_IN_USE, "The network is already running.");
	ERR_FAIL_COND_V_MSG(p_transport.is_null(), ERR_INVALID_PARAMETER, "The transport is null.");
	engine->set_settings(settings);
	const TickNetwork *source = nullptr;
	if (!clock_network.is_empty()) {
		source = Object::cast_to<TickNetwork>(get_node_or_null(clock_network));
		ERR_FAIL_COND_V_MSG(source == nullptr || source == this, ERR_INVALID_PARAMETER, "`clock_network` must point to another TickNetwork.");
		const int timeline_owner = authority_mode == AUTHORITY_DISTRIBUTED ? settings.clock_master : settings.authority_peer;
		ERR_FAIL_COND_V_MSG(p_transport->get_local_peer_id() != timeline_owner, ERR_INVALID_PARAMETER, "Only the clock master of a network can follow another network's clock.");
	}
	engine->set_clock_source(source ? &source->get_engine() : nullptr);
	const Error err = engine->start(p_transport, OS::get_singleton()->get_ticks_usec());
	ERR_FAIL_COND_V(err != OK, err);
	transport = p_transport;
	running = true;
	if (authority_mode == AUTHORITY_DISTRIBUTED && !settings.trusted) {
		WARN_PRINT("With AUTHORITY_DISTRIBUTED, every node of the mesh is trusted (events, ownership, spawns): `trust` doesn't limit anything. Use distributed authority only between servers you control, and set `trust` to TRUST_TRUSTED.");
	}
	if (Engine::get_singleton()->get_physics_ticks_per_second() != settings.ticks_per_second) {
		WARN_PRINT(vformat("TickNetwork runs at %d ticks per second, but the physics runs at %d: bodies moved with the physics delta (like move_and_slide) won't match the ticks.", settings.ticks_per_second, Engine::get_singleton()->get_physics_ticks_per_second()));
	}
	_update_processing();
	return OK;
}


// Stops the network and lets go of the transport.
void TickNetwork::stop() {
	if (!running) {
		return;
	}
	engine->stop();
	transport.unref();
	running = false;
	_update_processing();
}


// Whether this peer is the authority of a single authority network.
bool TickNetwork::is_server() const {
	return engine->is_server();
}


// Whether past frames are being simulated again right now.
bool TickNetwork::is_rewinding() const {
	return engine->is_rewinding();
}


// Client: whether the objects it controls are being predicted.
bool TickNetwork::is_predicting() const {
	return engine->is_predicting();
}


// This peer's id in the transport; 0 while the network is stopped.
int TickNetwork::get_local_peer_id() const {
	return transport.is_valid() ? transport->get_local_peer_id() : 0;
}


// The next frame to simulate.
int64_t TickNetwork::get_frame() const {
	return engine->get_frame();
}


// The lowest round trip time to the clock master among the recent pings, in seconds.
double TickNetwork::get_rtt() const {
	return double(engine->get_clock().get_rtt_usec()) / 1000000.0;
}


// The engine's counters.
Dictionary TickNetwork::get_stats() const {
	return engine->get_stats_dictionary();
}


// Starts synchronizing an object; a `TickObject` calls it when it enters the scene.
void TickNetwork::register_object(TickSyncObject *p_object) {
	if (!registered_objects.has(p_object)) {
		registered_objects.push_back(p_object);
	}
	engine->register_object(p_object);
}


// Stops synchronizing an object.
void TickNetwork::unregister_object(TickSyncObject *p_object) {
	registered_objects.erase(p_object);
	engine->unregister_object(p_object);
}


// The id the engine gave an object; 0 when it has none.
int TickNetwork::get_net_id(const TickSyncObject *p_object) const {
	return engine->get_net_id(p_object);
}


// Advances the network with an explicit time; the node calls it from the physics process. Public for tests.
void TickNetwork::advance(double p_delta, uint64_t p_now_usec) {
	if (running) {
		last_update_usec = p_now_usec;
		engine->process(p_delta, p_now_usec);
	}
}


// Updates the interpolated objects for the time given; the node calls it every rendered frame.
void TickNetwork::update_interpolation(uint64_t p_now_usec) {
	if (running) {
		last_update_usec = p_now_usec;
		engine->update_interpolation(p_now_usec);
	}
}


// Exposes the class to scripts.
void TickNetwork::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_ticks_per_second", "ticks_per_second"), &TickNetwork::set_ticks_per_second);
	ClassDB::bind_method(D_METHOD("get_ticks_per_second"), &TickNetwork::get_ticks_per_second);
	ClassDB::bind_method(D_METHOD("set_input_redundancy", "redundancy"), &TickNetwork::set_input_redundancy);
	ClassDB::bind_method(D_METHOD("get_input_redundancy"), &TickNetwork::get_input_redundancy);
	ClassDB::bind_method(D_METHOD("set_history_size", "frames"), &TickNetwork::set_history_size);
	ClassDB::bind_method(D_METHOD("get_history_size"), &TickNetwork::get_history_size);
	ClassDB::bind_method(D_METHOD("set_snapshot_interval", "ticks"), &TickNetwork::set_snapshot_interval);
	ClassDB::bind_method(D_METHOD("get_snapshot_interval"), &TickNetwork::get_snapshot_interval);
	ClassDB::bind_method(D_METHOD("set_interpolation_delay", "seconds"), &TickNetwork::set_interpolation_delay);
	ClassDB::bind_method(D_METHOD("get_interpolation_delay"), &TickNetwork::get_interpolation_delay);
	ClassDB::bind_method(D_METHOD("set_min_input_buffer", "frames"), &TickNetwork::set_min_input_buffer);
	ClassDB::bind_method(D_METHOD("get_min_input_buffer"), &TickNetwork::get_min_input_buffer);
	ClassDB::bind_method(D_METHOD("set_max_input_buffer", "frames"), &TickNetwork::set_max_input_buffer);
	ClassDB::bind_method(D_METHOD("get_max_input_buffer"), &TickNetwork::get_max_input_buffer);
	ClassDB::bind_method(D_METHOD("set_authority_mode", "mode"), &TickNetwork::set_authority_mode);
	ClassDB::bind_method(D_METHOD("get_authority_mode"), &TickNetwork::get_authority_mode);
	ClassDB::bind_method(D_METHOD("set_registry_peer", "peer"), &TickNetwork::set_registry_peer);
	ClassDB::bind_method(D_METHOD("get_registry_peer"), &TickNetwork::get_registry_peer);
	ClassDB::bind_method(D_METHOD("set_clock_master", "peer"), &TickNetwork::set_clock_master);
	ClassDB::bind_method(D_METHOD("get_clock_master"), &TickNetwork::get_clock_master);
	ClassDB::bind_method(D_METHOD("set_roles", "registry_peer", "clock_master"), &TickNetwork::set_roles);
	ClassDB::bind_method(D_METHOD("set_role_candidates", "candidates"), &TickNetwork::set_role_candidates);
	ClassDB::bind_method(D_METHOD("get_role_candidates"), &TickNetwork::get_role_candidates);
	ClassDB::bind_method(D_METHOD("set_role_quorum", "nodes"), &TickNetwork::set_role_quorum);
	ClassDB::bind_method(D_METHOD("get_role_quorum"), &TickNetwork::get_role_quorum);
	ClassDB::bind_method(D_METHOD("has_role_quorum"), &TickNetwork::has_role_quorum);
	ClassDB::bind_method(D_METHOD("set_default_relevance", "relevant"), &TickNetwork::set_default_relevance);
	ClassDB::bind_method(D_METHOD("get_default_relevance"), &TickNetwork::get_default_relevance);
	ClassDB::bind_method(D_METHOD("set_interest_interval", "ticks"), &TickNetwork::set_interest_interval);
	ClassDB::bind_method(D_METHOD("get_interest_interval"), &TickNetwork::get_interest_interval);
	ClassDB::bind_method(D_METHOD("set_interest_filter", "filter"), &TickNetwork::set_interest_filter);
	ClassDB::bind_method(D_METHOD("get_interest_filter"), &TickNetwork::get_interest_filter);
	ClassDB::bind_method(D_METHOD("set_keyframe_interval", "ticks"), &TickNetwork::set_keyframe_interval);
	ClassDB::bind_method(D_METHOD("get_keyframe_interval"), &TickNetwork::get_keyframe_interval);
	ClassDB::bind_method(D_METHOD("set_authority_peer", "peer"), &TickNetwork::set_authority_peer);
	ClassDB::bind_method(D_METHOD("get_authority_peer"), &TickNetwork::get_authority_peer);
	ClassDB::bind_method(D_METHOD("set_interpolate_remote", "enabled"), &TickNetwork::set_interpolate_remote);
	ClassDB::bind_method(D_METHOD("is_interpolating_remote"), &TickNetwork::is_interpolating_remote);
	ClassDB::bind_method(D_METHOD("set_clock_network", "path"), &TickNetwork::set_clock_network);
	ClassDB::bind_method(D_METHOD("get_clock_network"), &TickNetwork::get_clock_network);
	ClassDB::bind_method(D_METHOD("set_trust", "trust"), &TickNetwork::set_trust);
	ClassDB::bind_method(D_METHOD("get_trust"), &TickNetwork::get_trust);
	ClassDB::bind_method(D_METHOD("set_max_events_per_second", "events"), &TickNetwork::set_max_events_per_second);
	ClassDB::bind_method(D_METHOD("get_max_events_per_second"), &TickNetwork::get_max_events_per_second);
	ClassDB::bind_method(D_METHOD("set_max_event_size", "bytes"), &TickNetwork::set_max_event_size);
	ClassDB::bind_method(D_METHOD("get_max_event_size"), &TickNetwork::get_max_event_size);
	ClassDB::bind_method(D_METHOD("set_event_validator", "validator"), &TickNetwork::set_event_validator);
	ClassDB::bind_method(D_METHOD("get_event_validator"), &TickNetwork::get_event_validator);
	ClassDB::bind_method(D_METHOD("send_event", "event", "payload", "frame", "peer"), &TickNetwork::send_event, DEFVAL(Variant()), DEFVAL(-1), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_event_frame", "seconds"), &TickNetwork::get_event_frame);
	ClassDB::bind_method(D_METHOD("set_root_path", "path"), &TickNetwork::set_root_path);
	ClassDB::bind_method(D_METHOD("get_root_path"), &TickNetwork::get_root_path);

	ClassDB::bind_method(D_METHOD("start", "transport"), &TickNetwork::start);
	ClassDB::bind_method(D_METHOD("stop"), &TickNetwork::stop);
	ClassDB::bind_method(D_METHOD("is_running"), &TickNetwork::is_running);
	ClassDB::bind_method(D_METHOD("get_transport"), &TickNetwork::get_transport);
	ClassDB::bind_method(D_METHOD("is_server"), &TickNetwork::is_server);
	ClassDB::bind_method(D_METHOD("is_rewinding"), &TickNetwork::is_rewinding);
	ClassDB::bind_method(D_METHOD("is_predicting"), &TickNetwork::is_predicting);
	ClassDB::bind_method(D_METHOD("get_local_peer_id"), &TickNetwork::get_local_peer_id);
	ClassDB::bind_method(D_METHOD("get_frame"), &TickNetwork::get_frame);
	ClassDB::bind_method(D_METHOD("get_rtt"), &TickNetwork::get_rtt);
	ClassDB::bind_method(D_METHOD("set_object_relevant", "object", "peer", "relevant"), &TickNetwork::_set_object_relevant);
	ClassDB::bind_method(D_METHOD("is_object_relevant", "object", "peer"), &TickNetwork::_is_object_relevant);
	ClassDB::bind_method(D_METHOD("get_state_at", "object", "frame"), &TickNetwork::_get_state_at);
	ClassDB::bind_method(D_METHOD("get_view_frame"), &TickNetwork::get_view_frame);
	ClassDB::bind_method(D_METHOD("get_stats"), &TickNetwork::get_stats);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "ticks_per_second", PROPERTY_HINT_RANGE, "1,240,1"), "set_ticks_per_second", "get_ticks_per_second");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "input_redundancy", PROPERTY_HINT_RANGE, "1,64,1"), "set_input_redundancy", "get_input_redundancy");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "history_size", PROPERTY_HINT_RANGE, "8,1024,1"), "set_history_size", "get_history_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "snapshot_interval", PROPERTY_HINT_RANGE, "1,60,1"), "set_snapshot_interval", "get_snapshot_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interpolation_delay", PROPERTY_HINT_RANGE, "0,1,0.001,suffix:s"), "set_interpolation_delay", "get_interpolation_delay");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "min_input_buffer", PROPERTY_HINT_RANGE, "0,64,1"), "set_min_input_buffer", "get_min_input_buffer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_input_buffer", PROPERTY_HINT_RANGE, "0,64,1"), "set_max_input_buffer", "get_max_input_buffer");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "root_path"), "set_root_path", "get_root_path");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "authority_mode", PROPERTY_HINT_ENUM, "Single,Distributed"), "set_authority_mode", "get_authority_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "authority_peer", PROPERTY_HINT_RANGE, "1,2147483647,1"), "set_authority_peer", "get_authority_peer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "registry_peer", PROPERTY_HINT_RANGE, "1,2147483647,1"), "set_registry_peer", "get_registry_peer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "clock_master", PROPERTY_HINT_RANGE, "1,2147483647,1"), "set_clock_master", "get_clock_master");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "role_candidates"), "set_role_candidates", "get_role_candidates");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "role_quorum", PROPERTY_HINT_RANGE, "0,4096,1"), "set_role_quorum", "get_role_quorum");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "keyframe_interval", PROPERTY_HINT_RANGE, "1,600,1"), "set_keyframe_interval", "get_keyframe_interval");
	ADD_GROUP("Interest", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "default_relevance"), "set_default_relevance", "get_default_relevance");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "interest_interval", PROPERTY_HINT_RANGE, "1,600,1"), "set_interest_interval", "get_interest_interval");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "interest_filter"), "set_interest_filter", "get_interest_filter");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "interpolate_remote"), "set_interpolate_remote", "is_interpolating_remote");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "clock_network", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TickNetwork"), "set_clock_network", "get_clock_network");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "trust", PROPERTY_HINT_ENUM, "Untrusted,Trusted"), "set_trust", "get_trust");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_events_per_second", PROPERTY_HINT_RANGE, "1,1000,1"), "set_max_events_per_second", "get_max_events_per_second");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_event_size", PROPERTY_HINT_RANGE, "1,65535,1,suffix:B"), "set_max_event_size", "get_max_event_size");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "event_validator"), "set_event_validator", "get_event_validator");

	BIND_ENUM_CONSTANT(TRUST_UNTRUSTED);
	BIND_ENUM_CONSTANT(TRUST_TRUSTED);
	BIND_ENUM_CONSTANT(AUTHORITY_SINGLE);
	BIND_ENUM_CONSTANT(AUTHORITY_DISTRIBUTED);

	ADD_SIGNAL(MethodInfo("peer_ready", PropertyInfo(Variant::INT, "peer")));
	ADD_SIGNAL(MethodInfo("peer_left", PropertyInfo(Variant::INT, "peer")));
	ADD_SIGNAL(MethodInfo("rejected", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("prediction_started", PropertyInfo(Variant::INT, "frame")));
	ADD_SIGNAL(MethodInfo("rewound", PropertyInfo(Variant::INT, "frame"), PropertyInfo(Variant::INT, "frame_count")));
	ADD_SIGNAL(MethodInfo("authority_changed", PropertyInfo(Variant::OBJECT, "object", PROPERTY_HINT_RESOURCE_TYPE, "TickObject"), PropertyInfo(Variant::INT, "old_owner"), PropertyInfo(Variant::INT, "new_owner")));
	ADD_SIGNAL(MethodInfo("authority_orphaned", PropertyInfo(Variant::OBJECT, "object", PROPERTY_HINT_RESOURCE_TYPE, "TickObject"), PropertyInfo(Variant::INT, "last_owner"), PropertyInfo(Variant::INT, "last_frame")));
	ADD_SIGNAL(MethodInfo("authority_request_denied", PropertyInfo(Variant::OBJECT, "object", PROPERTY_HINT_RESOURCE_TYPE, "TickObject")));
	ADD_SIGNAL(MethodInfo("host_migrated", PropertyInfo(Variant::INT, "old_host"), PropertyInfo(Variant::INT, "new_host")));
	ADD_SIGNAL(MethodInfo("roles_changed", PropertyInfo(Variant::INT, "registry_peer"), PropertyInfo(Variant::INT, "clock_master")));
	ADD_SIGNAL(MethodInfo("role_quorum_changed", PropertyInfo(Variant::BOOL, "has_quorum")));
	ADD_SIGNAL(MethodInfo("relevance_changed", PropertyInfo(Variant::OBJECT, "object", PROPERTY_HINT_RESOURCE_TYPE, "TickObject"), PropertyInfo(Variant::BOOL, "relevant")));
	ADD_SIGNAL(MethodInfo("event_received", PropertyInfo(Variant::INT, "sender"), PropertyInfo(Variant::STRING_NAME, "event"), PropertyInfo(Variant::NIL, "payload", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT), PropertyInfo(Variant::INT, "frame")));
}

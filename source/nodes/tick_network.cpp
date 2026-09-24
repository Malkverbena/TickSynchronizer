#include "tick_network.h"

#include "tick_spawner.h"

#include "core/config/engine.h"
#include "core/os/os.h"

static const char *TICK_NETWORK_GROUP = "_tick_networks";

TickNetwork::TickNetwork() {
	core.set_listener(this);
}

TickNetwork::~TickNetwork() {
	core.set_listener(nullptr);
	if (running) {
		core.stop();
	}
}

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

void TickNetwork::_update_processing() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_physics_process_internal(running);
	set_process_internal(running);
}

void TickNetwork::on_peer_ready(int p_peer) {
	emit_signal(SNAME("peer_ready"), p_peer);
}

void TickNetwork::on_peer_left(int p_peer) {
	emit_signal(SNAME("peer_left"), p_peer);
}

void TickNetwork::on_rejected(const String &p_reason) {
	emit_signal(SNAME("rejected"), p_reason);
}

void TickNetwork::on_prediction_started(uint32_t p_frame) {
	emit_signal(SNAME("prediction_started"), int64_t(p_frame));
}

void TickNetwork::on_rewound(uint32_t p_frame, int p_frame_count) {
	emit_signal(SNAME("rewound"), int64_t(p_frame), p_frame_count);
}

int TickNetwork::validate_network_event(int p_sender, const StringName &p_event, const Variant &p_payload) {
	if (!event_validator.is_valid()) {
		return -1;
	}
	const Variant result = event_validator.call(p_sender, p_event, p_payload);
	return bool(result) ? 1 : 0;
}

void TickNetwork::on_network_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) {
	emit_signal(SNAME("event_received"), p_sender, p_event, p_payload, int64_t(p_frame));
}

void TickNetwork::on_spawn(const String &p_spawner, uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	Node *root = get_root_node();
	ERR_FAIL_NULL(root);
	TickSpawner *spawner = Object::cast_to<TickSpawner>(root->get_node_or_null(NodePath(p_spawner)));
	ERR_FAIL_NULL_MSG(spawner, vformat("The server spawned with \"%s\", but there's no TickSpawner at that path.", p_spawner));
	spawner->client_spawn(p_spawn_id, p_scene, p_name, p_controller, p_data);
}

void TickNetwork::on_despawn(const String &p_spawner, uint32_t p_spawn_id) {
	Node *root = get_root_node();
	ERR_FAIL_NULL(root);
	TickSpawner *spawner = Object::cast_to<TickSpawner>(root->get_node_or_null(NodePath(p_spawner)));
	ERR_FAIL_NULL_MSG(spawner, vformat("The server despawned with \"%s\", but there's no TickSpawner at that path.", p_spawner));
	spawner->client_despawn(p_spawn_id);
}

void TickNetwork::set_authority_peer(int p_peer) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_peer <= 0, "The authority peer must be positive.");
	settings.authority_peer = p_peer;
}

int TickNetwork::get_authority_peer() const {
	return settings.authority_peer;
}

void TickNetwork::set_interpolate_remote(bool p_enabled) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	settings.interpolate_remote = p_enabled;
}

bool TickNetwork::is_interpolating_remote() const {
	return settings.interpolate_remote;
}

void TickNetwork::set_clock_network(const NodePath &p_path) {
	ERR_FAIL_COND_MSG(running, "Can't change the clock network while the network is running.");
	clock_network = p_path;
}

void TickNetwork::set_trust(Trust p_trust) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	settings.trusted = p_trust == TRUST_TRUSTED;
}

TickNetwork::Trust TickNetwork::get_trust() const {
	return settings.trusted ? TRUST_TRUSTED : TRUST_UNTRUSTED;
}

void TickNetwork::set_max_events_per_second(int p_events) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_events < 1, "At least one event per second must be allowed.");
	settings.max_events_per_second = p_events;
}

int TickNetwork::get_max_events_per_second() const {
	return settings.max_events_per_second;
}

void TickNetwork::set_max_event_size(int p_bytes) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_bytes < 1 || p_bytes > UINT16_MAX, "The event size must be between 1 and 65535 bytes.");
	settings.max_event_bytes = p_bytes;
}

int TickNetwork::get_max_event_size() const {
	return settings.max_event_bytes;
}

Error TickNetwork::send_event(const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer) {
	return send_object_event(nullptr, p_event, p_payload, p_frame, p_peer);
}

Error TickNetwork::send_object_event(TickSyncObject *p_object, const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	return core.send_event(p_object, p_event, p_payload, p_frame < 0 ? TICK_FRAME_NONE : uint32_t(p_frame), p_peer);
}

int64_t TickNetwork::get_event_frame(double p_seconds) const {
	return core.get_event_frame(p_seconds);
}

uint32_t TickNetwork::spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	ERR_FAIL_COND_V_MSG(!running, 0, "The network isn't running.");
	return core.spawn(p_spawner, p_scene, p_name, p_controller, p_data);
}

void TickNetwork::despawn(uint32_t p_spawn_id) {
	if (running) {
		core.despawn(p_spawn_id);
	}
}

void TickNetwork::set_ticks_per_second(int p_ticks_per_second) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_ticks_per_second <= 0, "The ticks per second must be positive.");
	settings.ticks_per_second = p_ticks_per_second;
}

int TickNetwork::get_ticks_per_second() const {
	return running ? core.get_settings().ticks_per_second : settings.ticks_per_second;
}

void TickNetwork::set_input_redundancy(int p_redundancy) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_redundancy < 1 || p_redundancy > 64, "The input redundancy must be between 1 and 64.");
	settings.input_redundancy = p_redundancy;
}

int TickNetwork::get_input_redundancy() const {
	return settings.input_redundancy;
}

void TickNetwork::set_history_size(int p_frames) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_frames < 8, "The history must keep at least 8 frames.");
	settings.history_size = p_frames;
}

int TickNetwork::get_history_size() const {
	return settings.history_size;
}

void TickNetwork::set_snapshot_interval(int p_ticks) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_ticks < 1, "The snapshot interval must be at least 1.");
	settings.snapshot_interval = p_ticks;
}

int TickNetwork::get_snapshot_interval() const {
	return settings.snapshot_interval;
}

void TickNetwork::set_interpolation_delay(double p_seconds) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(!(p_seconds >= 0.0), "The interpolation delay can't be negative.");
	settings.interpolation_delay = p_seconds;
}

double TickNetwork::get_interpolation_delay() const {
	return settings.interpolation_delay;
}

void TickNetwork::set_min_input_buffer(int p_frames) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_frames < 0, "The input buffer can't be negative.");
	settings.min_input_buffer = p_frames;
	settings.max_input_buffer = MAX(settings.max_input_buffer, p_frames);
}

int TickNetwork::get_min_input_buffer() const {
	return settings.min_input_buffer;
}

void TickNetwork::set_max_input_buffer(int p_frames) {
	ERR_FAIL_COND_MSG(running, "Can't change the settings while the network is running.");
	ERR_FAIL_COND_MSG(p_frames < 0, "The input buffer can't be negative.");
	settings.max_input_buffer = p_frames;
	settings.min_input_buffer = MIN(settings.min_input_buffer, p_frames);
}

int TickNetwork::get_max_input_buffer() const {
	return settings.max_input_buffer;
}

void TickNetwork::set_root_path(const NodePath &p_path) {
	ERR_FAIL_COND_MSG(running, "Can't change the root path while the network is running.");
	root_path = p_path;
}

NodePath TickNetwork::get_root_path() const {
	return root_path;
}

Node *TickNetwork::get_root_node() const {
	return is_inside_tree() ? get_node_or_null(root_path) : nullptr;
}

Error TickNetwork::start(const Ref<TickTransport> &p_transport) {
	ERR_FAIL_COND_V_MSG(running, ERR_ALREADY_IN_USE, "The network is already running.");
	ERR_FAIL_COND_V_MSG(p_transport.is_null(), ERR_INVALID_PARAMETER, "The transport is null.");
	core.set_settings(settings);
	const TickNetwork *source = nullptr;
	if (!clock_network.is_empty()) {
		source = Object::cast_to<TickNetwork>(get_node_or_null(clock_network));
		ERR_FAIL_COND_V_MSG(source == nullptr || source == this, ERR_INVALID_PARAMETER, "`clock_network` must point to another TickNetwork.");
		ERR_FAIL_COND_V_MSG(p_transport->get_local_peer_id() != settings.authority_peer, ERR_INVALID_PARAMETER, "Only the authority of a network can follow another network's clock.");
	}
	core.set_clock_source(source ? &source->get_core() : nullptr);
	const Error err = core.start(p_transport, OS::get_singleton()->get_ticks_usec());
	ERR_FAIL_COND_V(err != OK, err);
	transport = p_transport;
	running = true;
	if (Engine::get_singleton()->get_physics_ticks_per_second() != settings.ticks_per_second) {
		WARN_PRINT(vformat("TickNetwork runs at %d ticks per second, but the physics runs at %d: bodies moved with the physics delta (like move_and_slide) won't match the ticks.", settings.ticks_per_second, Engine::get_singleton()->get_physics_ticks_per_second()));
	}
	_update_processing();
	return OK;
}

void TickNetwork::stop() {
	if (!running) {
		return;
	}
	core.stop();
	transport.unref();
	running = false;
	_update_processing();
}

bool TickNetwork::is_server() const {
	return core.is_server();
}

bool TickNetwork::is_rewinding() const {
	return core.is_rewinding();
}

bool TickNetwork::is_predicting() const {
	return core.is_predicting();
}

int TickNetwork::get_local_peer_id() const {
	return transport.is_valid() ? transport->get_local_peer_id() : 0;
}

int64_t TickNetwork::get_frame() const {
	return core.get_frame();
}

double TickNetwork::get_rtt() const {
	return double(core.get_clock().get_rtt_usec()) / 1000000.0;
}

Dictionary TickNetwork::get_stats() const {
	const TickSyncCore::Stats &stats = core.get_stats();
	Dictionary result;
	result["rewinds"] = stats.rewinds;
	result["rewound_frames"] = stats.rewound_frames;
	result["ghost_inputs"] = stats.ghost_inputs;
	result["late_inputs"] = stats.late_inputs;
	result["rejected_inputs"] = stats.rejected_inputs;
	result["full_snapshots_sent"] = stats.full_snapshots_sent;
	result["delta_snapshots_sent"] = stats.delta_snapshots_sent;
	result["snapshots_received"] = stats.snapshots_received;
	result["snapshots_dropped"] = stats.snapshots_dropped;
	result["malformed_packets"] = stats.malformed_packets;
	result["rate_limited_packets"] = stats.rate_limited_packets;
	result["events_sent"] = stats.events_sent;
	result["events_received"] = stats.events_received;
	result["events_rejected"] = stats.events_rejected;
	result["spawns"] = stats.spawns;
	result["despawns"] = stats.despawns;
	result["time_scale"] = core.get_time_scale();
	result["timeline_frame"] = core.get_timeline_frame(OS::get_singleton()->get_ticks_usec());
	result["latest_snapshot_frame"] = int64_t(core.get_latest_snapshot_frame());
	return result;
}

void TickNetwork::register_object(TickSyncObject *p_object) {
	core.register_object(p_object);
}

void TickNetwork::unregister_object(TickSyncObject *p_object) {
	core.unregister_object(p_object);
}

int TickNetwork::get_net_id(const TickSyncObject *p_object) const {
	return core.get_net_id(p_object);
}

void TickNetwork::advance(double p_delta, uint64_t p_now_usec) {
	if (running) {
		core.process(p_delta, p_now_usec);
	}
}

void TickNetwork::update_interpolation(uint64_t p_now_usec) {
	if (running) {
		core.update_interpolation(p_now_usec);
	}
}

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
	ClassDB::bind_method(D_METHOD("get_stats"), &TickNetwork::get_stats);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "ticks_per_second", PROPERTY_HINT_RANGE, "1,240,1"), "set_ticks_per_second", "get_ticks_per_second");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "input_redundancy", PROPERTY_HINT_RANGE, "1,64,1"), "set_input_redundancy", "get_input_redundancy");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "history_size", PROPERTY_HINT_RANGE, "8,1024,1"), "set_history_size", "get_history_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "snapshot_interval", PROPERTY_HINT_RANGE, "1,60,1"), "set_snapshot_interval", "get_snapshot_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interpolation_delay", PROPERTY_HINT_RANGE, "0,1,0.001,suffix:s"), "set_interpolation_delay", "get_interpolation_delay");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "min_input_buffer", PROPERTY_HINT_RANGE, "0,64,1"), "set_min_input_buffer", "get_min_input_buffer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_input_buffer", PROPERTY_HINT_RANGE, "0,64,1"), "set_max_input_buffer", "get_max_input_buffer");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "root_path"), "set_root_path", "get_root_path");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "authority_peer", PROPERTY_HINT_RANGE, "1,2147483647,1"), "set_authority_peer", "get_authority_peer");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "interpolate_remote"), "set_interpolate_remote", "is_interpolating_remote");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "clock_network", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TickNetwork"), "set_clock_network", "get_clock_network");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "trust", PROPERTY_HINT_ENUM, "Untrusted,Trusted"), "set_trust", "get_trust");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_events_per_second", PROPERTY_HINT_RANGE, "1,1000,1"), "set_max_events_per_second", "get_max_events_per_second");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_event_size", PROPERTY_HINT_RANGE, "1,65535,1,suffix:B"), "set_max_event_size", "get_max_event_size");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "event_validator"), "set_event_validator", "get_event_validator");

	BIND_ENUM_CONSTANT(TRUST_UNTRUSTED);
	BIND_ENUM_CONSTANT(TRUST_TRUSTED);

	ADD_SIGNAL(MethodInfo("peer_ready", PropertyInfo(Variant::INT, "peer")));
	ADD_SIGNAL(MethodInfo("peer_left", PropertyInfo(Variant::INT, "peer")));
	ADD_SIGNAL(MethodInfo("rejected", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("prediction_started", PropertyInfo(Variant::INT, "frame")));
	ADD_SIGNAL(MethodInfo("rewound", PropertyInfo(Variant::INT, "frame"), PropertyInfo(Variant::INT, "frame_count")));
	ADD_SIGNAL(MethodInfo("event_received", PropertyInfo(Variant::INT, "sender"), PropertyInfo(Variant::STRING_NAME, "event"), PropertyInfo(Variant::NIL, "payload", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT), PropertyInfo(Variant::INT, "frame")));
}

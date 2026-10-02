#include "tick_mesh_core.h"

#include "tick_net_ids.h"

#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/variant/variant.h"

// A rejected peer is disconnected after this delay, so the rejection reason reaches it.
static constexpr uint64_t MESH_REJECT_DISCONNECT_DELAY_USEC = 1000000;
// A transfer the owner doesn't answer is cancelled after this delay.
static constexpr uint64_t MESH_TRANSFER_TIMEOUT_USEC = 2000000;
// An event for an object without owner waits for one this long.
static constexpr uint64_t MESH_PENDING_EVENT_TIMEOUT_USEC = 5000000;
// Samples kept per remote object for interpolation.
static constexpr uint32_t MESH_MAX_SAMPLES = 32;
// A registry that takes over waits this long at most for the other nodes' views of the objects (ADR-073).
static constexpr uint64_t MESH_REGISTRY_SETTLE_USEC = 1000000;
// Most messages kept for a registry that can't answer yet (taking over, or without its quorum); more are dropped.
static constexpr uint32_t MESH_MAX_DEFERRED = 4096;
// A term of the roles, or a version of an object, further ahead than this isn't believed: no mesh changes its roles or
// the owner of an object this many times before a node hears of it.
static constexpr uint32_t MESH_MAX_SERIAL_JUMP = 1 << 20;
// A state stamped further ahead of this node's timeline than this many seconds isn't taken.
static constexpr int MESH_MAX_STATE_LEAD_SECONDS = 5;


// The term or the version after `p_value`. They wrap around, and 0 means "none": it's skipped.
static uint32_t mesh_next_serial(uint32_t p_value) {
	const uint32_t next = p_value + 1;
	return next == 0 ? 1 : next;
}


// Whether the term or version `p_value` replaces `p_known`: it comes after it, near enough to be believed (see
// `MESH_MAX_SERIAL_JUMP`). Anything replaces "none" (0), and "none" replaces nothing.
static bool mesh_serial_newer(uint32_t p_value, uint32_t p_known) {
	if (p_value == 0) {
		return false;
	}
	if (p_known == 0) {
		return true;
	}
	const uint32_t ahead = p_value - p_known;
	return ahead != 0 && ahead <= MESH_MAX_SERIAL_JUMP;
}


// Whether two terms or versions are so far apart that neither replaces the other: one of them isn't this mesh's.
static bool mesh_serial_apart(uint32_t p_value, uint32_t p_known) {
	return p_value != p_known && !mesh_serial_newer(p_value, p_known) && !mesh_serial_newer(p_known, p_value);
}


// Every node must have the same candidates, in the same order: checked in the handshake.
static uint32_t mesh_candidates_hash(const Vector<int> &p_candidates) {
	uint32_t hash = hash_murmur3_one_32(uint32_t(p_candidates.size()));
	for (const int candidate : p_candidates) {
		hash = hash_murmur3_one_32(uint32_t(candidate), hash);
	}
	return hash_fmix32(hash);
}

struct MeshEventOrder {
	template <typename T>
	bool operator()(const T &p_a, const T &p_b) const {
		if (p_a.frame != p_b.frame) {
			if (p_a.frame == TICK_FRAME_NONE || p_b.frame == TICK_FRAME_NONE) {
				return p_a.frame == TICK_FRAME_NONE;
			}
			return tick_frame_after(p_b.frame, p_a.frame);
		}
		return p_a.sequence < p_b.sequence;
	}
};

TickMeshCore::BusyScope::BusyScope(TickMeshCore *p_core) :
		core(p_core) {
	core->busy_depth++;
}

TickMeshCore::BusyScope::~BusyScope() {
	core->busy_depth--;
	if (core->busy_depth == 0 && core->stop_requested) {
		core->stop_now();
	}
}

void TickMeshCore::set_settings(const Settings &p_settings) {
	ERR_FAIL_COND_MSG(running, "The settings can't change while the network is running.");
	ERR_FAIL_COND_MSG(p_settings.ticks_per_second <= 0 || p_settings.ticks_per_second > TICK_MAX_TICKS_PER_SECOND, vformat("The ticks per second must be between 1 and %d.", TICK_MAX_TICKS_PER_SECOND));
	ERR_FAIL_COND_MSG(p_settings.registry_peer <= 0 || p_settings.clock_master <= 0, "The registry and clock master peers must be positive.");
	ERR_FAIL_COND_MSG(p_settings.keyframe_interval < 1, "The keyframe interval must be at least 1.");
	ERR_FAIL_COND_MSG(p_settings.role_quorum < 0, "The role quorum can't be negative.");
	for (const int candidate : p_settings.role_candidates) {
		ERR_FAIL_COND_MSG(candidate <= 0, "The role candidates must be positive.");
	}
	settings = p_settings;
}

Error TickMeshCore::start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec) {
	ERR_FAIL_COND_V_MSG(stop_requested, ERR_BUSY, "The network is stopping: start it again once its callbacks return (for example, with `call_deferred()`).");
	ERR_FAIL_COND_V_MSG(running, ERR_ALREADY_IN_USE, "The network is already running.");
	ERR_FAIL_COND_V_MSG(p_transport.is_null(), ERR_INVALID_PARAMETER, "The transport is null.");
	ERR_FAIL_COND_V_MSG(p_transport->get_channel_count() < TICK_CHANNEL_COUNT, ERR_INVALID_PARAMETER, vformat("The transport must have at least %d channels.", TICK_CHANNEL_COUNT));

	transport = p_transport;
	local_id = transport->get_local_peer_id();
	now_usec = p_now_usec;
	stats = Stats();
	running = true;
	roles_term = 0;
	// Another value for every process of this node. The scene tree seeds the random generator; the time makes the
	// value differ without one too.
	uint32_t seed = Math::rand();
	do {
		boot_id = hash_murmur3_one_64(p_now_usec, seed++);
	} while (boot_id == 0);
	// The node a role starts on has it with this process; the other nodes learn the process when they meet it.
	registry_boot = is_registry() ? boot_id : 0;
	clock_boot = is_clock_master() ? boot_id : 0;
	registry_epoch = 1;
	reported_epoch = 0;
	status_sent = false;
	sent_status = RoleStatus();
	reference_valid = false;
	timeline_trusted = is_clock_master();
	registry_orphan_check = false;
	registry_settling = false;
	registry_unreported.clear();
	registry_deferred.clear();
	pending_requests.clear();
	highest_net_id = 0;
	// The spawn ids of a process don't repeat the ones of a previous process of this node, which the registry keeps
	// (ADR-074).
	next_spawn_counter = MAX(uint32_t(1), boot_id & 0xFFFFF);

	stepper.reset();
	stepper.set_ticks_per_second(settings.ticks_per_second);
	clock.set_ticks_per_second(settings.ticks_per_second);
	stepped_usec = 0;
	if (is_clock_master()) {
		clock.set_master(true);
		clock.set_master_epoch_usec(int64_t(p_now_usec));
	} else {
		clock.set_master(false);
		clock.set_sample_window(16, settings.clock_min_samples);
	}
	last_ping_usec = 0;

	LocalVector<int> connected;
	transport->get_connected_peers(connected);
	for (const int peer : connected) {
		peers.insert(peer, PeerState());
		send_hello(peer);
	}
	had_quorum = has_role_quorum();
	// The registry registers its own objects; the others claim theirs once the registry is ready.
	claim_unbound_objects();
	return OK;
}

void TickMeshCore::stop() {
	if (busy_depth > 0) {
		// Game code the engine is running asked for it: the engine's state is freed once that code returns.
		stop_requested = true;
		return;
	}
	stop_now();
}

void TickMeshCore::stop_now() {
	stop_requested = false;
	running = false;
	transport.unref();
	peers.clear();
	entries.clear();
	ids_by_path.clear();
	registry.clear();
	registry_ids_by_path.clear();
	quarantined_ids.clear();
	spawns.clear();
	spawn_order.clear();
	pending_events.clear();
	loopback.clear();
	registry_settling = false;
	registry_unreported.clear();
	registry_deferred.clear();
	registry_orphan_check = false;
	pending_requests.clear();
}

bool TickMeshCore::is_peer_ready(int p_peer) const {
	if (p_peer == local_id) {
		return true;
	}
	const PeerState *peer = peers.getptr(p_peer);
	return peer && peer->ready;
}

int TickMeshCore::get_ready_count() const {
	int count = 0;
	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.ready) {
			count++;
		}
	}
	return count;
}

bool TickMeshCore::has_role_quorum() const {
	return settings.role_quorum <= 1 || get_ready_count() + 1 >= settings.role_quorum;
}

uint32_t TickMeshCore::get_node_boot(int p_node) const {
	if (p_node == local_id) {
		return boot_id;
	}
	const PeerState *peer = peers.getptr(p_node);
	return peer && peer->ready ? peer->boot : 0;
}

bool TickMeshCore::is_role_process_present(int p_node, uint32_t p_boot) const {
	return p_boot != 0 && get_node_boot(p_node) == p_boot;
}

int TickMeshCore::get_role_view(int p_node, uint32_t p_boot) const {
	if (p_boot == 0) {
		return ROLE_VIEW_UNKNOWN;
	}
	return is_role_process_present(p_node, p_boot) ? ROLE_VIEW_SEEN : ROLE_VIEW_LOST;
}

void TickMeshCore::forget_objects() {
	entries.clear();
	ids_by_path.clear();
	pending_requests.clear();
}

void TickMeshCore::send(int p_peer, TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message) {
	p_message.dry();
	const LocalVector<uint8_t> &bytes = p_message.get_buffer().get_bytes();
	if (p_peer == local_id) {
		TickTransport::Packet packet;
		packet.from_peer = local_id;
		packet.channel = p_channel;
		packet.mode = p_mode;
		packet.data = bytes;
		loopback.push_back(packet);
		return;
	}
	if (transport.is_null() || !transport->is_peer_connected(p_peer)) {
		return;
	}
	transport->send(p_peer, p_channel, p_mode, bytes.ptr(), int(bytes.size()));
}

void TickMeshCore::send_to_ready_peers(TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message, bool p_include_self) {
	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.ready) {
			send(E.key, p_channel, p_mode, p_message);
		}
	}
	if (p_include_self) {
		send(local_id, p_channel, p_mode, p_message);
	}
}

void TickMeshCore::read_values(TickSyncObject *p_object, LocalVector<Variant> &r_values) const {
	const int count = p_object->get_sync_schema().size();
	r_values.resize(count);
	for (int i = 0; i < count; i++) {
		r_values[i] = p_object->get_sync_var(i);
	}
}

void TickMeshCore::write_values(TickDataBuffer &r_payload, TickSyncObject *p_object, const LocalVector<Variant> &p_values) const {
	const TickSchema &schema = p_object->get_sync_schema();
	r_payload.begin_write();
	for (int i = 0; i < schema.size() && i < int(p_values.size()); i++) {
		schema.codecs[i]->encode(p_values[i], r_payload);
	}
}

bool TickMeshCore::read_payload_values(TickDataBuffer &p_payload, TickSyncObject *p_object, LocalVector<Variant> &r_values) const {
	const TickSchema &schema = p_object->get_sync_schema();
	r_values.resize(schema.size());
	for (int i = 0; i < schema.size(); i++) {
		r_values[i] = schema.codecs[i]->decode(p_payload);
	}
	return !p_payload.is_buffer_failed();
}

void TickMeshCore::quantize_object(TickSyncObject *p_object) const {
	const TickSchema &schema = p_object->get_sync_schema();
	for (int i = 0; i < schema.size(); i++) {
		p_object->set_sync_var(i, schema.codecs[i]->quantize(p_object->get_sync_var(i)));
	}
}

TickMeshCore::Entry *TickMeshCore::find_entry(const TickSyncObject *p_object, uint16_t *r_id) {
	for (KeyValue<uint16_t, Entry> &E : entries) {
		if (E.value.object == p_object) {
			if (r_id) {
				*r_id = E.key;
			}
			return &E.value;
		}
	}
	return nullptr;
}

const TickMeshCore::Entry *TickMeshCore::find_entry(const TickSyncObject *p_object, uint16_t *r_id) const {
	for (const KeyValue<uint16_t, Entry> &E : entries) {
		if (E.value.object == p_object) {
			if (r_id) {
				*r_id = E.key;
			}
			return &E.value;
		}
	}
	return nullptr;
}

void TickMeshCore::bind_entry(uint16_t p_id, Entry &r_entry) {
	if (r_entry.object) {
		return;
	}
	TickSyncObject **local = local_objects.getptr(r_entry.path);
	if (local == nullptr) {
		return;
	}
	if ((*local)->get_sync_schema().hash() != r_entry.schema_hash) {
		ERR_PRINT(vformat("The object \"%s\" declares different variables or codecs than the registry's; it's not synchronized.", r_entry.path));
		return;
	}
	r_entry.object = *local;
}

// ------------------------------------------------------------------------------------------------------ Objects

void TickMeshCore::register_object(TickSyncObject *p_object) {
	ERR_FAIL_NULL(p_object);
	const String path = p_object->get_sync_path();
	ERR_FAIL_COND_MSG(path.is_empty(), "A synchronized object needs a path.");
	if (local_objects.has(path)) {
		ERR_FAIL_COND_MSG(local_objects[path] != p_object, vformat("Another object is already registered with the path \"%s\".", path));
		return;
	}
	local_objects.insert(path, p_object);

	const uint16_t *id = ids_by_path.getptr(path);
	if (id) {
		bind_entry(*id, entries[*id]);
	}
	if (running) {
		claim(p_object);
	}
}

void TickMeshCore::unregister_object(TickSyncObject *p_object) {
	ERR_FAIL_NULL(p_object);
	const String path = p_object->get_sync_path();
	TickSyncObject **registered = local_objects.getptr(path);
	if (registered && *registered == p_object) {
		local_objects.erase(path);
	}
	uint16_t id = 0;
	Entry *entry = find_entry(p_object, &id);
	if (entry == nullptr) {
		return;
	}
	entry->object = nullptr;
	entry->samples.clear();
	if (running && entry->owner == local_id) {
		// The owner's object is gone: the registry forgets it.
		TickDataBuffer message;
		message.begin_write();
		message.add_uint_bits(TICK_MESSAGE_DROP, 8);
		message.add_uint_bits(id, 16);
		send(settings.registry_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
	}
}

void TickMeshCore::claim(TickSyncObject *p_object) {
	if (!is_registry_reachable()) {
		// Claimed once the registry is ready.
		return;
	}
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_CLAIM, 8);
	message.add_string(p_object->get_sync_path());
	message.add_int_bits(p_object->get_controller_peer(), 32);
	message.add_uint_bits(p_object->get_sync_schema().hash(), 32);
	send(settings.registry_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

void TickMeshCore::claim_unbound_objects() {
	for (const KeyValue<String, TickSyncObject *> &E : local_objects) {
		const uint16_t *id = ids_by_path.getptr(E.key);
		if (id == nullptr || entries[*id].object == nullptr) {
			claim(E.value);
		}
	}
}

uint16_t TickMeshCore::get_net_id(const TickSyncObject *p_object) const {
	uint16_t id = 0;
	find_entry(p_object, &id);
	return id;
}

int TickMeshCore::get_owner(const TickSyncObject *p_object) const {
	const Entry *entry = find_entry(p_object);
	return entry ? entry->owner : 0;
}

uint32_t TickMeshCore::get_version(const TickSyncObject *p_object) const {
	const Entry *entry = find_entry(p_object);
	return entry ? entry->version : 0;
}

// ------------------------------------------------------------------------------------------------------ Session

void TickMeshCore::send_hello(int p_peer) {
	TickDataBuffer hello;
	hello.begin_write();
	hello.add_uint_bits(TICK_MESSAGE_MESH_HELLO, 8);
	hello.add_uint_bits(TICK_PROTOCOL_VERSION, 16);
	hello.add_bool(sizeof(real_t) == sizeof(double));
	// The roles as this node knows them (ADR-073), with the processes that have them, this node's process, and the
	// candidates to the roles (ADR-074).
	hello.add_uint_bits(roles_term, 32);
	hello.add_int_bits(settings.registry_peer, 32);
	hello.add_int_bits(settings.clock_master, 32);
	hello.add_uint_bits(registry_boot, 32);
	hello.add_uint_bits(clock_boot, 32);
	hello.add_uint_bits(boot_id, 32);
	hello.add_uint_bits(mesh_candidates_hash(settings.role_candidates), 32);
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, hello);
}

void TickMeshCore::handle_hello(int p_peer, TickDataBuffer &p_message) {
	PeerState *peer = peers.getptr(p_peer);
	if (peer == nullptr || peer->ready || peer->rejected) {
		return;
	}
	const int version = int(p_message.read_uint_bits(16));
	const bool is_double = p_message.read_bool();
	const uint32_t term = uint32_t(p_message.read_uint_bits(32));
	const int new_registry = int(p_message.read_int_bits(32));
	const int new_clock = int(p_message.read_int_bits(32));
	const uint32_t new_registry_boot = uint32_t(p_message.read_uint_bits(32));
	const uint32_t new_clock_boot = uint32_t(p_message.read_uint_bits(32));
	const uint32_t boot = uint32_t(p_message.read_uint_bits(32));
	const uint32_t candidates = uint32_t(p_message.read_uint_bits(32));
	String reason;
	if (version != TICK_PROTOCOL_VERSION) {
		// Checked first: another version doesn't send the same handshake.
		reason = version == 0 ? String("Malformed handshake.") : vformat("Protocol version %d is incompatible with this node's version %d.", version, TICK_PROTOCOL_VERSION);
	} else if (p_message.is_buffer_failed() || new_registry <= 0 || new_clock <= 0 || boot == 0) {
		reason = "Malformed handshake.";
	} else if (is_double != (sizeof(real_t) == sizeof(double))) {
		reason = "The nodes of a mesh must use the same precision build.";
	} else if (term == 0 && roles_term == 0 && (new_registry != settings.registry_peer || new_clock != settings.clock_master)) {
		reason = "The nodes of a mesh must start with the same registry and clock master.";
	} else if (candidates != mesh_candidates_hash(settings.role_candidates)) {
		reason = "The nodes of a mesh must have the same role candidates.";
	} else if (mesh_serial_apart(term, roles_term)) {
		reason = "The roles of the two nodes are too far apart to be of the same mesh.";
	}
	if (!reason.is_empty()) {
		peer->rejected = true;
		peer->reject_usec = now_usec;
		TickDataBuffer reject;
		reject.begin_write();
		reject.add_uint_bits(TICK_MESSAGE_REJECT, 8);
		reject.add_string(reason);
		send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, reject);
		return;
	}
	peer->boot = boot;
	if (roles_beat(term, new_registry, new_clock, roles_term, settings.registry_peer, settings.clock_master)) {
		// The mesh moved its roles since this node knew them (it's new, or it came back).
		adopt_roles(term, new_registry, new_clock, new_registry_boot, new_clock_boot, true);
	} else if (term == roles_term && new_registry == settings.registry_peer && new_clock == settings.clock_master) {
		learn_role_boots(new_registry_boot, new_clock_boot);
	}
	if (is_active() && peers.has(p_peer)) {
		on_peer_ready(p_peer);
	}
}

void TickMeshCore::on_peer_ready(int p_peer) {
	peers[p_peer].ready = true;
	if (is_registry() && registry_boot == boot_id) {
		for (const KeyValue<uint16_t, RegistryRecord> &E : registry) {
			TickDataBuffer state;
			const bool has_state = registry_local_state(E.key, state);
			registry_send_announce(p_peer, E.key, has_state ? &state : nullptr);
		}
	}
	// A late node gets the spawns of every node from the node that made each one, and the spawns of the nodes that
	// left from the registry.
	for (const uint32_t spawn_id : spawn_order) {
		if (owns_spawn(spawn_id)) {
			send_spawn(p_peer, spawn_id);
		}
	}
	if (p_peer == settings.registry_peer) {
		sync_with_registry();
	}
	send_role_status(p_peer, make_role_status());
	if (listener) {
		listener->on_peer_ready(p_peer);
	}
}

void TickMeshCore::handle_events() {
	LocalVector<int> left;
	TickTransport::Event event;
	while (is_active() && transport->pop_event(event)) {
		if (event.type == TickTransport::EVENT_HOST_MIGRATED) {
			// The roles of a distributed mesh don't follow the transport's host: they move when their node leaves,
			// like for any other node (ADR-073).
			continue;
		}
		if (event.type == TickTransport::EVENT_PEER_CONNECTED) {
			if (!peers.has(event.peer)) {
				peers.insert(event.peer, PeerState());
				send_hello(event.peer);
			}
			continue;
		}
		const bool was_ready = peers.has(event.peer) && peers[event.peer].ready;
		peers.erase(event.peer);
		if (!was_ready) {
			continue;
		}
		left.push_back(event.peer);
		// Its spawns belong to the registry from now on (ADR-074).
		for (KeyValue<uint32_t, SpawnRecord> &E : spawns) {
			if (E.value.origin == event.peer) {
				E.value.adopted = true;
			}
		}
		if (listener) {
			listener->on_peer_left(event.peer);
		}
	}
	// A role of a node that left is taken in `update_roles()`, once the other nodes say they lost it too. The
	// registry orphans the objects of the nodes that left once every event of this step is in: with its quorum it
	// does it now, without it (this node may be the one cut off) when it has it again.
	if (left.is_empty() || !is_active() || !is_registry()) {
		return;
	}
	for (const int peer : left) {
		registry_unreported.erase(peer);
	}
	if (registry_boot != boot_id) {
		return;
	}
	if (!has_role_quorum()) {
		registry_orphan_check = true;
		return;
	}
	for (const int peer : left) {
		registry_on_peer_left(peer);
	}
}

void TickMeshCore::handle_packet(const TickTransport::Packet &p_packet) {
	if (p_packet.data.is_empty()) {
		stats.malformed_packets++;
		return;
	}
	TickDataBuffer message(TickBitArray(p_packet.data.ptr(), int(p_packet.data.size())));
	message.begin_read();
	const int type = int(message.read_uint_bits(8));
	const int from = p_packet.from_peer;

	if (type == TICK_MESSAGE_MESH_HELLO) {
		handle_hello(from, message);
		return;
	}
	if (type == TICK_MESSAGE_REJECT) {
		if (!peers.has(from)) {
			stats.unexpected_packets++;
			return;
		}
		const String reason = message.read_string();
		ERR_PRINT(vformat("A mesh node refused this node: %s", reason));
		if (listener) {
			listener->on_rejected(reason);
		}
		return;
	}
	// Everything else only from ready nodes (or this node).
	if (!is_peer_ready(from)) {
		stats.unexpected_packets++;
		return;
	}
	// The registry's messages only from the registry (H2).
	const bool from_registry = from == settings.registry_peer;
	switch (type) {
		case TICK_MESSAGE_PING:
			handle_ping(from, message);
			break;
		case TICK_MESSAGE_PONG:
			handle_pong(from, message);
			break;
		case TICK_MESSAGE_STATE:
			handle_state(from, message);
			break;
		case TICK_MESSAGE_SPAWN:
			handle_spawn(from, message);
			break;
		case TICK_MESSAGE_DESPAWN:
			handle_despawn(from, message);
			break;
		case TICK_MESSAGE_MESH_EVENT:
			handle_mesh_event(from, message);
			break;
		case TICK_MESSAGE_ROLES:
			handle_roles(from, message);
			break;
		case TICK_MESSAGE_ROLE_STATUS:
			handle_role_status(from, message);
			break;
		case TICK_MESSAGE_REGISTRY_REPORT:
			if (!is_registry()) {
				// The registry moved while the report was on its way.
				stats.unexpected_packets++;
			} else {
				handle_registry_report(from, message);
			}
			break;
		case TICK_MESSAGE_ANNOUNCE:
		case TICK_MESSAGE_UNREGISTER:
		case TICK_MESSAGE_AUTH_TRANSFER:
		case TICK_MESSAGE_AUTH_DENIED:
			if (!from_registry) {
				// From a node that had the registry, or that this node doesn't know as the registry yet.
				stats.unexpected_packets++;
			} else if (!is_registry_reachable()) {
				// The node of the registry, but not the process that has it (it restarted): it doesn't know the mesh's
				// objects until it takes the role again (ADR-074).
			} else if (type == TICK_MESSAGE_ANNOUNCE) {
				handle_announce(from, message);
			} else if (type == TICK_MESSAGE_UNREGISTER) {
				handle_unregister(from, message);
			} else if (type == TICK_MESSAGE_AUTH_TRANSFER) {
				handle_transfer(from, message);
			} else {
				handle_denied(from, message);
			}
			break;
		case TICK_MESSAGE_CLAIM:
		case TICK_MESSAGE_DROP:
		case TICK_MESSAGE_AUTH_REQUEST:
		case TICK_MESSAGE_AUTH_ASSIGN:
		case TICK_MESSAGE_AUTH_RELEASE: {
			if (!is_registry()) {
				// For the registry this node was.
				stats.unexpected_packets++;
				break;
			}
			if (!registry_can_act()) {
				if (type == TICK_MESSAGE_AUTH_RELEASE && !registry_settling) {
					// Without its quorum, or with its role in doubt, this registry may not answer for long, and the owner
					// doesn't simulate an object it's releasing: it takes it back.
					registry_refuse_release(p_packet);
					break;
				}
				// Answered once this registry has every node's view (ADR-073), its role and its quorum (ADR-074).
				registry_defer(p_packet);
				break;
			}
			if (type == TICK_MESSAGE_AUTH_RELEASE) {
				registry_handle_release(from, message);
				break;
			}
			if (type == TICK_MESSAGE_CLAIM) {
				const String path = message.read_string();
				const int owner = int(message.read_int_bits(32));
				const uint32_t schema_hash = uint32_t(message.read_uint_bits(32));
				if (message.is_buffer_failed() || path.is_empty()) {
					stats.malformed_packets++;
				} else {
					registry_handle_claim(from, path, owner, schema_hash);
				}
				break;
			}
			const uint16_t id = uint16_t(message.read_uint_bits(16));
			const int target = type == TICK_MESSAGE_AUTH_ASSIGN ? int(message.read_int_bits(32)) : 0;
			if (message.is_buffer_failed()) {
				stats.malformed_packets++;
			} else if (type == TICK_MESSAGE_DROP) {
				registry_handle_drop(from, id);
			} else if (type == TICK_MESSAGE_AUTH_REQUEST) {
				registry_handle_request(from, id);
			} else {
				registry_handle_assign(from, id, target);
			}
		} break;
		default:
			stats.malformed_packets++;
			break;
	}
}

// ------------------------------------------------------------------------------------------------------ Clock

int64_t TickMeshCore::compute_epoch() const {
	const double elapsed_frames = double(stepper.get_next_frame_index()) + stepper.get_interpolation_fraction();
	const uint64_t stepped_at = stepped_usec != 0 ? stepped_usec : now_usec;
	return int64_t(stepped_at) - int64_t(elapsed_frames * 1000000.0 * get_tick_delta());
}

void TickMeshCore::handle_ping(int p_peer, TickDataBuffer &p_message) {
	const uint64_t client_time = p_message.read_uint_bits(64);
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	if (!is_clock_master()) {
		// For the clock this node was.
		stats.unexpected_packets++;
		return;
	}
	if (clock_boot != boot_id) {
		// This process isn't the one with the clock (it restarted): its frames aren't the mesh's timeline.
		return;
	}
	TickDataBuffer pong;
	pong.begin_write();
	pong.add_uint_bits(TICK_MESSAGE_PONG, 8);
	pong.add_uint_bits(client_time, 64);
	pong.add_uint_bits(now_usec, 64);
	pong.add_int_bits(compute_epoch(), 64);
	send(p_peer, TICK_CHANNEL_STATS, TickTransport::TRANSFER_MODE_UNRELIABLE_ORDERED, pong);
}

void TickMeshCore::handle_pong(int p_peer, TickDataBuffer &p_message) {
	const uint64_t client_time = p_message.read_uint_bits(64);
	const uint64_t master_time = p_message.read_uint_bits(64);
	const int64_t epoch = p_message.read_int_bits(64);
	// A time or an epoch no running node has makes the pong malformed: nothing of it is taken.
	if (p_message.is_buffer_failed() || !TickClock::is_plausible_time(epoch) || !TickClock::is_plausible_sample(client_time, master_time, now_usec)) {
		stats.malformed_packets++;
		return;
	}
	if (p_peer != settings.clock_master || is_clock_master()) {
		// From the clock a node was.
		stats.unexpected_packets++;
		return;
	}
	if (is_clock_lost()) {
		return;
	}
	// The epoch first: a clock that holds its previous timeline takes it with the samples.
	clock.set_master_epoch_usec(epoch);
	clock.add_sample(client_time, master_time, now_usec);
	timeline_trusted = true;
}

double TickMeshCore::get_timeline_frame(uint64_t p_now_usec) const {
	if (!running) {
		return -1.0;
	}
	if (is_clock_master()) {
		if (clock_source) {
			return clock_source->get_timeline_frame(p_now_usec);
		}
		return double(stepper.get_next_frame_index()) + stepper.get_interpolation_fraction();
	}
	return clock.is_synchronized() ? clock.get_master_frame_time(p_now_usec) : -1.0;
}

void TickMeshCore::follow_timeline(double p_target_frame) {
	if (p_target_frame < 0.0) {
		// The timeline isn't known yet: nothing is simulated.
		return;
	}
	const uint32_t target = tick_frame_at(p_target_frame);
	const int32_t behind = int32_t(target - stepper.get_next_frame_index()) + 1;
	const int32_t limit = stepper.get_max_ticks_per_advance();
	if (behind > limit * 4 || behind < -limit * 4) {
		// Too far off (start, a long hitch, or another clock): jump to the timeline's frame. The states received before
		// have frames of where the timeline was.
		stepper.set_next_frame_index(target);
		reset_received_states();
		tick(stepper.step_frame());
		return;
	}
	for (int32_t i = 0; i < MIN(behind, limit) && is_active(); i++) {
		tick(stepper.step_frame());
	}
}

// ------------------------------------------------------------------------------------------------------ Process

void TickMeshCore::process(double p_delta, uint64_t p_now_usec) {
	ERR_FAIL_COND_MSG(!is_active(), "The network isn't running.");
	BusyScope busy(this);
	now_usec = p_now_usec;

	transport->poll();
	handle_events();
	TickTransport::Packet packet;
	while (is_active() && transport->pop_packet(packet)) {
		handle_packet(packet);
	}
	if (is_active()) {
		update_roles();
	}
	if (is_active() && is_registry()) {
		if (registry_settling && (registry_unreported.is_empty() || now_usec >= registry_settle_usec)) {
			registry_finish_take_over();
		}
		if (registry_can_act() && (registry_orphan_check || !registry_deferred.is_empty())) {
			registry_resume();
		}
		registry_check_timeouts();
	}
	// Messages to itself, including the ones they cause (bounded).
	for (int round = 0; round < 16 && !loopback.is_empty() && is_active(); round++) {
		LocalVector<TickTransport::Packet> packets;
		packets = loopback;
		loopback.clear();
		for (uint32_t i = 0; i < packets.size() && is_active(); i++) {
			handle_packet(packets[i]);
		}
	}
	if (!is_active()) {
		return;
	}

	if (is_clock_master()) {
		if (clock_source) {
			follow_timeline(clock_source->get_timeline_frame(now_usec));
		} else {
			stepper.advance(p_delta);
			while (stepper.get_pending_ticks() > 0 && is_active()) {
				tick(stepper.pop_tick());
			}
		}
		stepped_usec = now_usec;
	} else {
		follow_timeline(clock.is_synchronized() ? clock.get_master_frame_time(now_usec) : -1.0);
		if (get_role_view(settings.clock_master, clock_boot) == ROLE_VIEW_SEEN) {
			const uint64_t interval = clock.needs_samples() ? uint64_t(1000000.0 * get_tick_delta()) : uint64_t(settings.ping_interval * 1000000.0);
			if (last_ping_usec == 0 || now_usec - last_ping_usec >= interval) {
				last_ping_usec = now_usec;
				TickDataBuffer ping;
				ping.begin_write();
				ping.add_uint_bits(TICK_MESSAGE_PING, 8);
				ping.add_uint_bits(now_usec, 64);
				send(settings.clock_master, TICK_CHANNEL_STATS, TickTransport::TRANSFER_MODE_UNRELIABLE_ORDERED, ping);
			}
		}
	}
	if (!is_active()) {
		return;
	}

	LocalVector<int> to_disconnect;
	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.rejected && now_usec - E.value.reject_usec >= MESH_REJECT_DISCONNECT_DELAY_USEC) {
			to_disconnect.push_back(E.key);
		}
	}
	for (const int peer : to_disconnect) {
		transport->disconnect_peer(peer);
		peers.erase(peer);
	}

	update_interpolation(now_usec);
	transport->poll();
}

void TickMeshCore::tick(uint32_t p_frame) {
	run_events(p_frame);
	const double delta = get_tick_delta();
	for (KeyValue<uint16_t, Entry> &E : entries) {
		Entry &entry = E.value;
		if (entry.owner != local_id || entry.object == nullptr || entry.frozen) {
			continue;
		}
		// The owner drives the object with its own input. The object's code may remove objects (removing one
		// clears its entry's object), so the entry is checked again after it.
		TickDataBuffer input;
		input.begin_write();
		entry.object->collect_input(input);
		if (entry.object == nullptr) {
			continue;
		}
		input.begin_read();
		entry.object->process_tick(delta, input);
		if (entry.object == nullptr) {
			continue;
		}
		quantize_object(entry.object);
	}
	if (p_frame % uint32_t(settings.snapshot_interval) == 0) {
		send_states(p_frame);
	}
}

// The count's place in a state message, written once known.
static constexpr int MESH_STATE_COUNT_OFFSET = 8 + 32;

void TickMeshCore::send_state_message(TickDataBuffer &r_message, int p_count) {
	const int end = r_message.total_size();
	r_message.seek(MESH_STATE_COUNT_OFFSET);
	r_message.add_uint_bits(uint64_t(p_count), 16);
	r_message.seek(end);
	send_to_ready_peers(TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_UNRELIABLE, r_message, false);
	stats.states_sent += uint64_t(p_count);
}

void TickMeshCore::send_states(uint32_t p_frame) {
	const bool keyframe = p_frame % uint32_t(settings.keyframe_interval) == 0;
	const int limit_bits = MAX(64, transport->get_max_payload_size()) * 8;

	LocalVector<uint16_t> ids;
	for (KeyValue<uint16_t, Entry> &E : entries) {
		if (E.value.owner == local_id && E.value.object && !E.value.frozen) {
			ids.push_back(E.key);
		}
	}
	ids.sort();

	TickDataBuffer message;
	int count = 0;
	for (const uint16_t id : ids) {
		Entry &entry = entries[id];
		LocalVector<Variant> values;
		read_values(entry.object, values);
		bool changed = entry.last_sent.size() != values.size();
		for (uint32_t v = 0; !changed && v < values.size(); v++) {
			changed = !(values[v] == entry.last_sent[v]);
		}
		if (!changed && !keyframe) {
			continue;
		}
		entry.last_sent = values;
		TickDataBuffer payload;
		write_values(payload, entry.object, values);
		// Every entry is written straight into the message: a nested buffer is aligned to the bytes of the buffer it's
		// written in, so an entry built apart and moved to another bit offset would be read shifted.
		for (int attempt = 0; attempt < 2; attempt++) {
			if (count == 0) {
				message.begin_write();
				message.add_uint_bits(TICK_MESSAGE_STATE, 8);
				message.add_uint_bits(p_frame, 32);
				message.add_uint_bits(0, 16);
			}
			const int before = message.total_size();
			message.add_uint_bits(id, 16);
			message.add_uint_bits(entry.version, 32);
			message.add_data_buffer(payload);
			if (count == 0 || message.total_size() <= limit_bits) {
				count++;
				break;
			}
			// Too big with this entry: the message goes without it, and the entry starts the next one.
			message.shrink_to(0, before);
			message.seek(before);
			send_state_message(message, count);
			count = 0;
		}
	}
	if (count > 0) {
		send_state_message(message, count);
	}
}

void TickMeshCore::handle_state(int p_peer, TickDataBuffer &p_message) {
	const uint32_t frame = uint32_t(p_message.read_uint_bits(32));
	const int count = int(p_message.read_uint_bits(16));
	// A frame far ahead of this node's own is of a node whose clock is off (or of a timeline this node doesn't follow
	// yet): taking it would leave every state that comes after it looking old.
	const bool too_far_ahead = int32_t(frame - stepper.get_next_frame_index()) > settings.ticks_per_second * MESH_MAX_STATE_LEAD_SECONDS;
	for (int i = 0; i < count && !p_message.is_buffer_failed(); i++) {
		const uint16_t id = uint16_t(p_message.read_uint_bits(16));
		const uint32_t version = uint32_t(p_message.read_uint_bits(32));
		TickDataBuffer payload;
		p_message.read_data_buffer(payload);
		if (p_message.is_buffer_failed()) {
			break;
		}
		Entry *entry = entries.getptr(id);
		// Only the registered owner, with the current version: a packet sent before a change of owner is dropped.
		if (entry == nullptr || entry->owner != p_peer || entry->version != version) {
			stats.stale_states++;
			continue;
		}
		if (entry->object == nullptr) {
			continue;
		}
		if (too_far_ahead || (entry->last_state_frame != TICK_FRAME_NONE && !tick_frame_after(frame, entry->last_state_frame))) {
			stats.late_states++;
			continue;
		}
		Sample sample;
		sample.frame = frame;
		if (!read_payload_values(payload, entry->object, sample.values)) {
			stats.malformed_packets++;
			continue;
		}
		entry->last_state_frame = frame;
		entry->samples.push_back(sample);
		if (entry->samples.size() > MESH_MAX_SAMPLES) {
			entry->samples.remove_at(0);
		}
		stats.states_received++;
	}
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
	}
}

void TickMeshCore::update_interpolation(uint64_t p_now_usec) {
	if (!is_active()) {
		return;
	}
	// Objects may apply the state with their own code.
	BusyScope busy(this);
	now_usec = p_now_usec;
	const double timeline = get_timeline_frame(p_now_usec);
	const bool interpolate = settings.interpolate_remote && timeline >= 0.0;
	const double render_frame = timeline - settings.interpolation_delay * double(settings.ticks_per_second);

	LocalVector<Variant> values;
	for (KeyValue<uint16_t, Entry> &E : entries) {
		Entry &entry = E.value;
		if (entry.owner == local_id || entry.object == nullptr || entry.samples.is_empty()) {
			continue;
		}
		const Sample &latest = entry.samples[entry.samples.size() - 1];
		if (!interpolate) {
			entry.object->apply_interpolated_state(latest.values);
			continue;
		}
		const Sample *past = nullptr;
		const Sample *future = nullptr;
		for (const Sample &sample : entry.samples) {
			if (double(sample.frame) <= render_frame) {
				past = &sample;
			} else if (future == nullptr) {
				future = &sample;
			}
		}
		if (past && future && past->values.size() == future->values.size()) {
			const double weight = CLAMP((render_frame - double(past->frame)) / double(future->frame - past->frame), 0.0, 1.0);
			const TickSchema &schema = entry.object->get_sync_schema();
			values.resize(past->values.size());
			for (uint32_t i = 0; i < values.size() && int(i) < schema.size(); i++) {
				values[i] = schema.codecs[i]->interpolate(past->values[i], future->values[i], weight);
			}
			entry.object->apply_interpolated_state(values);
		} else if (past) {
			entry.object->apply_interpolated_state(past->values);
		} else {
			entry.object->apply_interpolated_state(entry.samples[0].values);
		}
	}
}

// ------------------------------------------------------------------------------------------------------ Registry

void TickMeshCore::registry_handle_claim(int p_peer, const String &p_path, int p_owner, uint32_t p_schema_hash) {
	const uint16_t *existing = registry_ids_by_path.getptr(p_path);
	if (existing) {
		// Already registered: the claimant gets the current entry.
		TickDataBuffer state;
		const bool has_state = registry_local_state(*existing, state);
		registry_send_announce(p_peer, *existing, has_state ? &state : nullptr);
		return;
	}
	const uint32_t current = stepper.get_next_frame_index();
	const uint32_t quarantine = uint32_t(settings.history_size) * 2;
	if (registry.size() + quarantined_ids.size() >= UINT16_MAX - 1) {
		// Released ids are only forgotten when reused: the ones past their quarantine don't count.
		tick_prune_quarantine(quarantined_ids, current, quarantine);
	}
	ERR_FAIL_COND_MSG(registry.size() + quarantined_ids.size() >= UINT16_MAX - 1, "Too many synchronized objects.");
	for (int attempt = 0; attempt < UINT16_MAX; attempt++) {
		if (next_net_id == 0 || registry.has(next_net_id)) {
			next_net_id++;
			continue;
		}
		const uint32_t *released = quarantined_ids.getptr(next_net_id);
		if (released && current - *released < quarantine) {
			next_net_id++;
			continue;
		}
		quarantined_ids.erase(next_net_id);
		break;
	}
	const uint16_t id = next_net_id++;
	RegistryRecord record;
	record.path = p_path;
	// The first claim decides the initial owner: the object's controller, or the claimant.
	record.owner = p_owner > 0 ? p_owner : p_peer;
	record.version = 1;
	record.frame = current;
	record.schema_hash = p_schema_hash;
	registry.insert(id, record);
	registry_ids_by_path.insert(p_path, id);

	registry_send_announce(0, id, nullptr);
}

void TickMeshCore::registry_handle_drop(int p_peer, uint16_t p_id) {
	RegistryRecord *record = registry.getptr(p_id);
	if (record == nullptr || record->owner != p_peer) {
		return;
	}
	registry_ids_by_path.erase(record->path);
	registry.erase(p_id);
	quarantined_ids.insert(p_id, stepper.get_next_frame_index());
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_UNREGISTER, 8);
	message.add_uint_bits(p_id, 16);
	send_to_ready_peers(TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message, true);
}

bool TickMeshCore::registry_local_state(uint16_t p_id, TickDataBuffer &r_state) const {
	const Entry *entry = entries.getptr(p_id);
	if (entry == nullptr || entry->object == nullptr) {
		return false;
	}
	LocalVector<Variant> values;
	read_values(entry->object, values);
	write_values(r_state, entry->object, values);
	return true;
}

void TickMeshCore::registry_send_announce(int p_peer, uint16_t p_id, const TickDataBuffer *p_state) {
	const RegistryRecord &record = registry[p_id];
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_ANNOUNCE, 8);
	message.add_uint_bits(p_id, 16);
	message.add_string(record.path);
	message.add_int_bits(record.owner, 32);
	message.add_uint_bits(record.version, 32);
	message.add_uint_bits(record.frame, 32);
	message.add_uint_bits(record.schema_hash, 32);
	message.add_bool(p_state != nullptr);
	if (p_state) {
		message.add_data_buffer(*p_state);
	}
	if (p_peer == 0) {
		send_to_ready_peers(TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message, true);
	} else {
		send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
	}
}

void TickMeshCore::registry_deny(int p_peer, uint16_t p_id) {
	if (p_peer <= 0 || !is_peer_ready(p_peer)) {
		return;
	}
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_AUTH_DENIED, 8);
	message.add_uint_bits(p_id, 16);
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}


// Answers a release this registry can't take now (it has no quorum, its role is in doubt, or too many messages wait
// for it) with a denial: the owner stopped simulating the object to release it, and takes it back.
void TickMeshCore::registry_refuse_release(const TickTransport::Packet &p_packet) {
	TickDataBuffer message(TickBitArray(p_packet.data.ptr(), int(p_packet.data.size())));
	message.begin_read();
	message.read_uint_bits(8);
	const uint16_t id = uint16_t(message.read_uint_bits(16));
	message.read_uint_bits(32);
	const int to = int(message.read_int_bits(32));
	if (message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	// The owner's refusal of a transfer (`to` below 0) froze nothing.
	if (to >= 0) {
		registry_deny(p_packet.from_peer, id);
	}
}


void TickMeshCore::registry_change_owner(uint16_t p_id, int p_new_owner, uint32_t p_frame, const TickDataBuffer *p_state) {
	RegistryRecord &record = registry[p_id];
	if (record.pending_to >= 0 && record.pending_to != p_new_owner) {
		// The transfer in progress doesn't happen: whoever asked for it is told.
		registry_deny(record.pending_requester, p_id);
	}
	// Announced again with the owner it had (by a registry that took over, or after a late report): the release its
	// owner may have sent for the version it knew is still good.
	record.previous_version = record.owner == p_new_owner ? record.version : 0;
	record.owner = p_new_owner;
	record.version = mesh_next_serial(record.version);
	record.frame = p_frame;
	record.pending_to = -1;
	record.pending_requester = 0;
	registry_send_announce(0, p_id, p_state);
}

void TickMeshCore::registry_handle_request(int p_peer, uint16_t p_id) {
	RegistryRecord *record = registry.getptr(p_id);
	if (record == nullptr || record->pending_to >= 0) {
		// Unknown, or another transfer is in progress (ADR-041).
		registry_deny(p_peer, p_id);
		return;
	}
	if (record->owner == p_peer) {
		return;
	}
	if (record->owner == 0) {
		// An orphan goes to the first node that asks, from its last known state.
		TickDataBuffer state;
		const bool has_state = registry_local_state(p_id, state);
		registry_change_owner(p_id, p_peer, stepper.get_next_frame_index(), has_state ? &state : nullptr);
		return;
	}
	record->pending_to = p_peer;
	record->pending_requester = p_peer;
	record->pending_forced = false;
	record->pending_timeout_usec = now_usec + MESH_TRANSFER_TIMEOUT_USEC;
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_AUTH_TRANSFER, 8);
	message.add_uint_bits(p_id, 16);
	message.add_uint_bits(record->version, 32);
	message.add_int_bits(p_peer, 32);
	message.add_bool(false);
	send(record->owner, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

void TickMeshCore::registry_handle_assign(int p_peer, uint16_t p_id, int p_target) {
	RegistryRecord *record = registry.getptr(p_id);
	if (record == nullptr || record->pending_to >= 0 || p_target < 0 || (p_target > 0 && !is_peer_ready(p_target))) {
		registry_deny(p_peer, p_id);
		return;
	}
	if (record->owner == p_target) {
		return;
	}
	if (record->owner == 0) {
		TickDataBuffer state;
		const bool has_state = registry_local_state(p_id, state);
		registry_change_owner(p_id, p_target, stepper.get_next_frame_index(), has_state ? &state : nullptr);
		return;
	}
	// Assignments are forced: the owner can't refuse them.
	record->pending_to = p_target;
	record->pending_requester = p_peer;
	record->pending_forced = true;
	record->pending_timeout_usec = now_usec + MESH_TRANSFER_TIMEOUT_USEC;
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_AUTH_TRANSFER, 8);
	message.add_uint_bits(p_id, 16);
	message.add_uint_bits(record->version, 32);
	message.add_int_bits(p_target, 32);
	message.add_bool(true);
	send(record->owner, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

void TickMeshCore::registry_handle_release(int p_peer, TickDataBuffer &p_message) {
	const uint16_t id = uint16_t(p_message.read_uint_bits(16));
	const uint32_t version = uint32_t(p_message.read_uint_bits(32));
	const int to = int(p_message.read_int_bits(32));
	const uint32_t frame = uint32_t(p_message.read_uint_bits(32));
	const bool has_state = p_message.read_bool();
	TickDataBuffer state;
	if (has_state) {
		p_message.read_data_buffer(state);
	}
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	RegistryRecord *record = registry.getptr(id);
	// Only the current owner, for the current version, or for the one it had before this registry announced the object
	// again without changing its owner.
	const bool current = record && record->owner == p_peer && (record->version == version || (record->previous_version != 0 && record->previous_version == version));
	if (!current) {
		if (to >= 0) {
			// The node froze the object to release it: it's told the release didn't happen, and takes it back.
			registry_deny(p_peer, id);
		}
		return;
	}
	if (to < 0) {
		// The owner refused the request.
		const int requester = record->pending_requester;
		record->pending_to = -1;
		record->pending_requester = 0;
		registry_deny(requester, id);
		return;
	}
	if (to == record->owner) {
		// Released to its own owner: no announcement would say the owner changed, so the release is answered here.
		registry_deny(p_peer, id);
		return;
	}
	// Released somewhere else than a pending transfer asked: `registry_change_owner()` tells who asked for it.
	if (to > 0 && to != local_id && !is_peer_ready(to)) {
		// The new owner is gone: the object is orphaned instead.
		registry_change_owner(id, 0, frame, has_state ? &state : nullptr);
		return;
	}
	registry_change_owner(id, to, frame, has_state ? &state : nullptr);
}

void TickMeshCore::registry_on_peer_left(int p_peer) {
	LocalVector<uint16_t> ids;
	for (const KeyValue<uint16_t, RegistryRecord> &E : registry) {
		ids.push_back(E.key);
	}
	ids.sort();
	for (const uint16_t id : ids) {
		RegistryRecord &record = registry[id];
		if (record.owner == p_peer) {
			// Orphaned (ADR-010): the last state known here goes with the announcement. Whoever asked for a transfer of
			// it is told it won't happen (`registry_change_owner()`).
			const Entry *entry = entries.getptr(id);
			const uint32_t frame = entry && entry->last_state_frame != TICK_FRAME_NONE ? entry->last_state_frame : stepper.get_next_frame_index();
			TickDataBuffer state;
			const bool has_state = registry_local_state(id, state);
			registry_change_owner(id, 0, frame, has_state ? &state : nullptr);
		} else if (record.pending_to >= 0 && (record.pending_to == p_peer || record.pending_requester == p_peer)) {
			const int requester = record.pending_requester;
			record.pending_to = -1;
			record.pending_requester = 0;
			if (requester != p_peer) {
				registry_deny(requester, id);
			}
		}
	}
}

void TickMeshCore::registry_check_timeouts() {
	for (KeyValue<uint16_t, RegistryRecord> &E : registry) {
		RegistryRecord &record = E.value;
		if (record.pending_to >= 0 && now_usec >= record.pending_timeout_usec) {
			const int requester = record.pending_requester;
			record.pending_to = -1;
			record.pending_requester = 0;
			registry_deny(requester, E.key);
		}
	}
}

// ------------------------------------------------------------------------------------------------------ Replica

void TickMeshCore::handle_announce(int p_peer, TickDataBuffer &p_message) {
	const uint16_t id = uint16_t(p_message.read_uint_bits(16));
	const String path = p_message.read_string();
	const int owner = int(p_message.read_int_bits(32));
	const uint32_t version = uint32_t(p_message.read_uint_bits(32));
	const uint32_t frame = uint32_t(p_message.read_uint_bits(32));
	const uint32_t schema_hash = uint32_t(p_message.read_uint_bits(32));
	const bool has_state = p_message.read_bool();
	TickDataBuffer state;
	if (has_state) {
		p_message.read_data_buffer(state);
	}
	if (p_message.is_buffer_failed() || id == 0 || path.is_empty() || owner < 0 || version == 0) {
		stats.malformed_packets++;
		return;
	}

	// Announcements come from the current registry (see `handle_packet()`), and what it says wins over what another
	// registry did, whatever the versions: two registries of a mesh that split count their versions apart. Its own
	// versions only go up.
	Entry *existing = entries.getptr(id);
	if (existing && existing->registry_epoch == registry_epoch && existing->version != version && !mesh_serial_newer(version, existing->version)) {
		return;
	}
	// A path has one entry: if it had another id here (given by another registry), that entry is gone.
	const uint16_t *named_before = ids_by_path.getptr(path);
	if (named_before && *named_before != id) {
		const uint16_t other_id = *named_before;
		pending_requests.erase(other_id);
		ids_by_path.erase(path);
		entries.erase(other_id);
		existing = entries.getptr(id);
	}
	if (existing == nullptr) {
		Entry fresh;
		fresh.path = path;
		entries.insert(id, fresh);
		ids_by_path.insert(path, id);
	}
	Entry &entry = entries[id];
	if (entry.path != path) {
		// The id names another object here (learned from a process that wasn't the registry): it starts over, and the
		// object that had the id here is claimed again.
		const String previous_path = entry.path;
		const uint16_t *named = ids_by_path.getptr(previous_path);
		if (named && *named == id) {
			ids_by_path.erase(previous_path);
		}
		entry = Entry();
		entry.path = path;
		ids_by_path.insert(path, id);
		TickSyncObject **unbound = local_objects.getptr(previous_path);
		if (unbound && !ids_by_path.has(previous_path)) {
			claim(*unbound);
		}
	}
	const bool first = entry.version == 0;
	const int old_owner = entry.owner;
	highest_net_id = MAX(highest_net_id, id);
	if (old_owner != owner) {
		// A request or an assignment for it was answered.
		pending_requests.erase(id);
	}
	entry.path = path;
	entry.owner = owner;
	entry.version = version;
	entry.registry_epoch = registry_epoch;
	entry.frame = frame;
	entry.schema_hash = schema_hash;
	bind_entry(id, entry);

	if (old_owner != owner || first) {
		entry.frozen = false;
		entry.release_own = false;
		entry.samples.clear();
		entry.last_state_frame = TICK_FRAME_NONE;
		entry.last_sent.clear();
	}
	if (has_state && entry.object) {
		LocalVector<Variant> values;
		state.begin_read();
		if (read_payload_values(state, entry.object, values)) {
			if (owner == local_id) {
				// The new owner continues from the state the previous one released.
				for (uint32_t i = 0; i < values.size(); i++) {
					entry.object->set_sync_var(int(i), values[i]);
				}
			} else {
				// The state is of the frame the owner changed at. On a timeline that started over since then, that frame is
				// ahead of this node's: the state counts as of now, or the owner's next states would look older than it.
				const uint32_t current = stepper.get_next_frame_index();
				Sample sample;
				sample.frame = tick_frame_after(frame, current) ? current : frame;
				sample.values = values;
				entry.samples.push_back(sample);
				entry.last_state_frame = sample.frame;
			}
		}
	}
	if (first || old_owner == owner || entry.object == nullptr) {
		return;
	}
	stats.transfers++;
	entry.object->on_authority_changed(old_owner, owner);
	if (listener) {
		listener->on_authority_changed(entry.object, old_owner, owner);
	}
	if (owner == 0) {
		stats.orphans++;
		if (listener) {
			listener->on_authority_orphaned(entry.object, old_owner, frame);
		}
	}
	// Events waiting for an owner may go now (those already due).
	run_events(stepper.get_next_frame_index() - 1);
}

void TickMeshCore::handle_unregister(int p_peer, TickDataBuffer &p_message) {
	const uint16_t id = uint16_t(p_message.read_uint_bits(16));
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	Entry *entry = entries.getptr(id);
	if (entry) {
		ids_by_path.erase(entry->path);
		entries.erase(id);
	}
}

void TickMeshCore::release_frozen(uint16_t p_id, Entry &r_entry, int p_to) {
	r_entry.frozen = true;
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_AUTH_RELEASE, 8);
	message.add_uint_bits(p_id, 16);
	message.add_uint_bits(r_entry.version, 32);
	message.add_int_bits(p_to, 32);
	// The last simulated frame, and the state at it.
	message.add_uint_bits(stepper.get_next_frame_index() - 1, 32);
	message.add_bool(r_entry.object != nullptr);
	if (r_entry.object) {
		LocalVector<Variant> values;
		read_values(r_entry.object, values);
		TickDataBuffer state;
		write_values(state, r_entry.object, values);
		message.add_data_buffer(state);
	}
	send(settings.registry_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

void TickMeshCore::handle_transfer(int p_peer, TickDataBuffer &p_message) {
	const uint16_t id = uint16_t(p_message.read_uint_bits(16));
	const uint32_t version = uint32_t(p_message.read_uint_bits(32));
	const int to = int(p_message.read_int_bits(32));
	const bool forced = p_message.read_bool();
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	Entry *entry = entries.getptr(id);
	if (entry == nullptr || entry->owner != local_id || entry->version != version || entry->frozen) {
		// Not the owner of this version (anymore): the registry's timeout cancels the transfer.
		return;
	}
	if (!forced && entry->object && entry->object->approve_authority_request(to) == 0) {
		TickDataBuffer message;
		message.begin_write();
		message.add_uint_bits(TICK_MESSAGE_AUTH_RELEASE, 8);
		message.add_uint_bits(id, 16);
		message.add_uint_bits(entry->version, 32);
		message.add_int_bits(-1, 32);
		message.add_uint_bits(stepper.get_next_frame_index() - 1, 32);
		message.add_bool(false);
		send(settings.registry_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
		return;
	}
	release_frozen(id, *entry, to);
}

void TickMeshCore::handle_denied(int p_peer, TickDataBuffer &p_message) {
	const uint16_t id = uint16_t(p_message.read_uint_bits(16));
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	Entry *entry = entries.getptr(id);
	// The game is told about what it asked for: a request, an assignment, or a release of its own. A release a
	// transfer asked for isn't its doing.
	bool tell = true;
	if (entry && entry->owner == local_id && entry->frozen) {
		// The registry didn't take the release: the object stays here, simulated again.
		tell = entry->release_own || pending_requests.has(id);
		entry->frozen = false;
		entry->release_own = false;
	}
	pending_requests.erase(id);
	if (!tell) {
		return;
	}
	stats.denied_requests++;
	if (listener && entry && entry->object) {
		listener->on_authority_request_denied(entry->object);
	}
}

Error TickMeshCore::request_authority(TickSyncObject *p_object) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	uint16_t id = 0;
	Entry *entry = find_entry(p_object, &id);
	ERR_FAIL_NULL_V_MSG(entry, ERR_UNAVAILABLE, "The object isn't registered in the mesh yet.");
	if (entry->owner == local_id) {
		return OK;
	}
	ERR_FAIL_COND_V_MSG(!is_registry_reachable(), ERR_UNAVAILABLE, "The registry node isn't connected.");
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_AUTH_REQUEST, 8);
	message.add_uint_bits(id, 16);
	send(settings.registry_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
	pending_requests.insert(id);
	return OK;
}

Error TickMeshCore::release_authority(TickSyncObject *p_object, int p_to_peer) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(p_to_peer < 0, ERR_INVALID_PARAMETER, "The new owner can't be negative (0 leaves the object orphaned).");
	uint16_t id = 0;
	Entry *entry = find_entry(p_object, &id);
	ERR_FAIL_NULL_V_MSG(entry, ERR_UNAVAILABLE, "The object isn't registered in the mesh yet.");
	ERR_FAIL_COND_V_MSG(entry->owner != local_id, ERR_UNAUTHORIZED, "Only the owner can release an object.");
	ERR_FAIL_COND_V_MSG(entry->frozen, ERR_BUSY, "The object is already being released.");
	if (p_to_peer == local_id) {
		// Released to its own owner: there's nothing to change, and no announcement would ever end the release.
		return OK;
	}
	ERR_FAIL_COND_V_MSG(!is_registry_reachable(), ERR_UNAVAILABLE, "The registry node isn't connected.");
	release_frozen(id, *entry, p_to_peer);
	entry->release_own = true;
	return OK;
}

Error TickMeshCore::assign_authority(TickSyncObject *p_object, int p_peer) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(p_peer < 0, ERR_INVALID_PARAMETER, "The new owner can't be negative.");
	uint16_t id = 0;
	Entry *entry = find_entry(p_object, &id);
	ERR_FAIL_NULL_V_MSG(entry, ERR_UNAVAILABLE, "The object isn't registered in the mesh yet.");
	ERR_FAIL_COND_V_MSG(!is_registry_reachable(), ERR_UNAVAILABLE, "The registry node isn't connected.");
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_AUTH_ASSIGN, 8);
	message.add_uint_bits(id, 16);
	message.add_int_bits(p_peer, 32);
	send(settings.registry_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
	if (entry->owner != p_peer) {
		pending_requests.insert(id);
	}
	return OK;
}

Error TickMeshCore::change_roles(int p_registry, int p_clock_master) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(!is_peer_ready(p_registry) || !is_peer_ready(p_clock_master), ERR_UNAVAILABLE, "The registry and the clock master must be nodes connected to this one.");
	if (p_registry == settings.registry_peer && p_clock_master == settings.clock_master) {
		return OK;
	}
	BusyScope busy(this);
	const uint32_t term = mesh_next_serial(roles_term);
	// A role that moves goes to the node's current process; one that stays keeps the process it had.
	const uint32_t new_registry_boot = p_registry == settings.registry_peer ? registry_boot : get_node_boot(p_registry);
	const uint32_t new_clock_boot = p_clock_master == settings.clock_master ? clock_boot : get_node_boot(p_clock_master);
	send_roles(0, term, p_registry, p_clock_master, new_registry_boot, new_clock_boot);
	adopt_roles(term, p_registry, p_clock_master, new_registry_boot, new_clock_boot, false);
	return OK;
}

// ------------------------------------------------------------------------------------------------------ Roles

bool TickMeshCore::roles_beat(uint32_t p_term, int p_registry, int p_clock, uint32_t p_other_term, int p_other_registry, int p_other_clock) {
	if (p_term != p_other_term) {
		// Terms wrap around; one too far ahead to be this mesh's beats nothing.
		return mesh_serial_newer(p_term, p_other_term);
	}
	if (p_registry != p_other_registry) {
		return p_registry < p_other_registry;
	}
	return p_clock < p_other_clock;
}

void TickMeshCore::send_roles(int p_peer, uint32_t p_term, int p_registry, int p_clock, uint32_t p_registry_boot, uint32_t p_clock_boot) {
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_ROLES, 8);
	message.add_uint_bits(p_term, 32);
	message.add_int_bits(p_registry, 32);
	message.add_int_bits(p_clock, 32);
	message.add_uint_bits(p_registry_boot, 32);
	message.add_uint_bits(p_clock_boot, 32);
	if (p_peer == 0) {
		send_to_ready_peers(TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message, false);
	} else {
		send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
	}
}

void TickMeshCore::handle_roles(int p_peer, TickDataBuffer &p_message) {
	const uint32_t term = uint32_t(p_message.read_uint_bits(32));
	const int new_registry = int(p_message.read_int_bits(32));
	const int new_clock = int(p_message.read_int_bits(32));
	const uint32_t new_registry_boot = uint32_t(p_message.read_uint_bits(32));
	const uint32_t new_clock_boot = uint32_t(p_message.read_uint_bits(32));
	// A term no node of this mesh gets to by changing its roles makes the message malformed.
	if (p_message.is_buffer_failed() || new_registry <= 0 || new_clock <= 0 || mesh_serial_apart(term, roles_term)) {
		stats.malformed_packets++;
		return;
	}
	if (!roles_beat(term, new_registry, new_clock, roles_term, settings.registry_peer, settings.clock_master)) {
		if (roles_beat(roles_term, settings.registry_peer, settings.clock_master, term, new_registry, new_clock)) {
			// The sender knows older roles: it gets the current ones.
			send_roles(p_peer, roles_term, settings.registry_peer, settings.clock_master, registry_boot, clock_boot);
		}
		return;
	}
	// Passed on first, so every node gets them even without a link to the one that changed them, and the new registry
	// gets them before this node's view of the objects.
	send_roles(0, term, new_registry, new_clock, new_registry_boot, new_clock_boot);
	adopt_roles(term, new_registry, new_clock, new_registry_boot, new_clock_boot, false);
}

int TickMeshCore::get_role_successor() const {
	if (settings.role_candidates.is_empty()) {
		// Any node: the lowest id in the mesh.
		int successor = local_id;
		for (const KeyValue<int, PeerState> &E : peers) {
			if (E.value.ready && E.key < successor) {
				successor = E.key;
			}
		}
		return successor;
	}
	for (const int candidate : settings.role_candidates) {
		if (is_peer_ready(candidate)) {
			return candidate;
		}
	}
	// The node of a lost role that isn't a candidate takes it again when it comes back.
	if (is_registry_lost() && is_peer_ready(settings.registry_peer)) {
		return settings.registry_peer;
	}
	if (is_clock_lost() && is_peer_ready(settings.clock_master)) {
		return settings.clock_master;
	}
	return 0;
}

void TickMeshCore::fill_vacant_roles() {
	const bool registry_lost = is_registry_lost();
	const bool clock_lost = is_clock_lost();
	if (!registry_lost && !clock_lost) {
		return;
	}
	// Every node picks the same successor, and only it announces the roles: the others adopt them from its message
	// (and meanwhile ask nothing of the role's node). Without its quorum, this node may be the one cut off.
	if (get_role_successor() != local_id || !has_role_quorum()) {
		return;
	}
	// Not while a node connected to this one still sees the process with the role: then only this node lost it. Every
	// connected node must have said what it sees, for these roles.
	for (const KeyValue<int, PeerState> &E : peers) {
		if (!E.value.ready) {
			continue;
		}
		const RoleStatus &status = E.value.status;
		if (!E.value.status_known || status.term != roles_term || status.registry != settings.registry_peer || status.clock != settings.clock_master) {
			return;
		}
		if (registry_lost && status.registry_view == ROLE_VIEW_SEEN && status.registry_boot == registry_boot) {
			return;
		}
		if (clock_lost && status.clock_view == ROLE_VIEW_SEEN && status.clock_boot == clock_boot) {
			return;
		}
	}
	const uint32_t term = mesh_next_serial(roles_term);
	const int new_registry = registry_lost ? local_id : settings.registry_peer;
	const int new_clock = clock_lost ? local_id : settings.clock_master;
	const uint32_t new_registry_boot = registry_lost ? boot_id : registry_boot;
	const uint32_t new_clock_boot = clock_lost ? boot_id : clock_boot;
	send_roles(0, term, new_registry, new_clock, new_registry_boot, new_clock_boot);
	adopt_roles(term, new_registry, new_clock, new_registry_boot, new_clock_boot, false);
}

void TickMeshCore::learn_role_boots(uint32_t p_registry_boot, uint32_t p_clock_boot) {
	// Two nodes with the same roles know different processes as the holder of one: its node restarted, and no node
	// took the role since. The process that had the role is the one that isn't here anymore; a node that believed in
	// the one that is here (the node itself, if it's the one that restarted) met it later, and gives way. What it
	// learned from that process isn't the mesh's: the role is lost for it too, until a successor takes it.
	if (p_registry_boot != 0 && p_registry_boot != registry_boot && (registry_boot == 0 || is_role_process_present(settings.registry_peer, registry_boot))) {
		const bool knew_another = registry_boot != 0;
		registry_boot = p_registry_boot;
		registry_epoch++;
		if (knew_another) {
			if (is_registry()) {
				registry.clear();
				registry_ids_by_path.clear();
				quarantined_ids.clear();
				registry_settling = false;
				registry_unreported.clear();
				registry_deferred.clear();
				registry_orphan_check = false;
			}
			forget_objects();
		}
		// The registry's process is known now: if it's in reach, it gets this node's view and claims.
		sync_with_registry();
	}
	if (p_clock_boot != 0 && p_clock_boot != clock_boot && (clock_boot == 0 || is_role_process_present(settings.clock_master, clock_boot))) {
		const bool knew_another = clock_boot != 0;
		clock_boot = p_clock_boot;
		if (knew_another) {
			timeline_trusted = false;
			if (!clock.is_master()) {
				clock.clear_samples();
			}
		}
	}
}

TickMeshCore::RoleStatus TickMeshCore::make_role_status() const {
	RoleStatus status;
	status.term = roles_term;
	status.registry = settings.registry_peer;
	status.clock = settings.clock_master;
	status.registry_boot = registry_boot;
	status.registry_view = get_role_view(settings.registry_peer, registry_boot);
	status.clock_boot = clock_boot;
	status.clock_view = get_role_view(settings.clock_master, clock_boot);
	return status;
}

void TickMeshCore::send_role_status(int p_peer, const RoleStatus &p_status) {
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_ROLE_STATUS, 8);
	message.add_uint_bits(p_status.term, 32);
	message.add_int_bits(p_status.registry, 32);
	message.add_int_bits(p_status.clock, 32);
	message.add_uint_bits(p_status.registry_boot, 32);
	message.add_uint_bits(uint64_t(p_status.registry_view), 2);
	message.add_uint_bits(p_status.clock_boot, 32);
	message.add_uint_bits(uint64_t(p_status.clock_view), 2);
	// The mesh's timeline as this node follows it, for a clock master that has to take it from the other nodes.
	const double frame = timeline_trusted ? get_timeline_frame(now_usec) : -1.0;
	message.add_bool(frame >= 0.0);
	if (frame >= 0.0) {
		message.add_uint_bits(tick_frame_at(frame), 32);
		message.add_uint_bits(uint64_t((frame - Math::floor(frame)) * 256.0) & 0xFF, 8);
	}
	if (p_peer == 0) {
		send_to_ready_peers(TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message, false);
	} else {
		send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
	}
}

void TickMeshCore::handle_role_status(int p_peer, TickDataBuffer &p_message) {
	RoleStatus status;
	status.term = uint32_t(p_message.read_uint_bits(32));
	status.registry = int(p_message.read_int_bits(32));
	status.clock = int(p_message.read_int_bits(32));
	status.registry_boot = uint32_t(p_message.read_uint_bits(32));
	status.registry_view = int(p_message.read_uint_bits(2));
	status.clock_boot = uint32_t(p_message.read_uint_bits(32));
	status.clock_view = int(p_message.read_uint_bits(2));
	const bool has_frame = p_message.read_bool();
	const uint32_t frame = has_frame ? uint32_t(p_message.read_uint_bits(32)) : 0;
	const int fraction = has_frame ? int(p_message.read_uint_bits(8)) : 0;
	PeerState *peer = peers.getptr(p_peer);
	if (p_message.is_buffer_failed() || peer == nullptr || status.registry <= 0 || status.clock <= 0 || status.registry_view > ROLE_VIEW_LOST || status.clock_view > ROLE_VIEW_LOST || mesh_serial_apart(status.term, roles_term)) {
		stats.malformed_packets++;
		return;
	}
	peer->status_known = true;
	peer->status = status;
	if (has_frame) {
		reference_valid = true;
		reference_frame = double(frame) + double(fraction) / 256.0;
		reference_usec = now_usec;
	}
	if (roles_beat(roles_term, settings.registry_peer, settings.clock_master, status.term, status.registry, status.clock)) {
		// It knows older roles: it gets the current ones.
		send_roles(p_peer, roles_term, settings.registry_peer, settings.clock_master, registry_boot, clock_boot);
		return;
	}
	if (status.term == roles_term && status.registry == settings.registry_peer && status.clock == settings.clock_master) {
		learn_role_boots(status.registry_boot, status.clock_boot);
	}
}

void TickMeshCore::update_roles() {
	if (settings.role_quorum > 1) {
		const bool quorum = has_role_quorum();
		if (quorum != had_quorum) {
			had_quorum = quorum;
			if (listener) {
				listener->on_role_quorum_changed(quorum);
			}
			if (!is_active()) {
				return;
			}
		}
	}
	// The other nodes decide with what this node sees: it tells them whenever that changes.
	const RoleStatus status = make_role_status();
	if (!status_sent || !(status == sent_status)) {
		status_sent = true;
		sent_status = status;
		send_role_status(0, status);
	}
	if (is_registry_lost()) {
		// Nobody answers the releases in progress while the registry is gone: the objects go on with this node.
		LocalVector<String> denied_paths;
		cancel_releases(denied_paths);
		notify_denied(denied_paths);
		if (!is_active()) {
			return;
		}
	}
	fill_vacant_roles();
}

void TickMeshCore::adopt_roles(uint32_t p_term, int p_registry, int p_clock, uint32_t p_registry_boot, uint32_t p_clock_boot, bool p_resync) {
	// A node that never met the process with a role announces the role without it (0). The role's node takes it as its
	// own process; the others keep the one they knew for that node, or learn it later from the nodes that know it.
	// Without this, nobody would know the process, and the role could never be told lost.
	if (p_registry_boot == 0) {
		p_registry_boot = p_registry == local_id ? boot_id : (p_registry == settings.registry_peer ? registry_boot : 0);
	}
	if (p_clock_boot == 0) {
		p_clock_boot = p_clock == local_id ? boot_id : (p_clock == settings.clock_master ? clock_boot : 0);
	}
	const int old_registry = settings.registry_peer;
	// A role moved if another node has it, or another process of the same node: it restarted, and took the role again
	// knowing nothing of what its previous process knew.
	const bool registry_moved = p_registry != old_registry || (registry_boot != 0 && p_registry_boot != registry_boot);
	const bool clock_moved = p_clock != settings.clock_master || (clock_boot != 0 && p_clock_boot != clock_boot);
	// A node that joins a mesh that moved on learns the objects again from its registry.
	const bool resync = p_resync && p_registry != local_id;
	roles_term = p_term;
	settings.registry_peer = p_registry;
	settings.clock_master = p_clock;
	registry_boot = p_registry_boot;
	clock_boot = p_clock_boot;
	stats.role_changes++;
	if (registry_moved) {
		registry_epoch++;
	}
	if (p_resync && clock.is_master()) {
		// This node joins a mesh that moved on while it ran its own clock (it restarted as the clock master, or it was
		// cut off): its frames aren't the mesh's timeline.
		timeline_trusted = false;
	}

	if (clock_moved) {
		if (p_clock == local_id && p_clock_boot != boot_id) {
			// The mesh knows another process of this node as its clock: this one restarted, and nobody took the role. A
			// process that started as the clock has frames of its own, which aren't the mesh's timeline until it takes the
			// role again (then it takes the frame the other nodes tell it). One that followed the mesh goes on as it was.
			if (clock.is_master()) {
				timeline_trusted = false;
			}
		} else if (p_clock == local_id) {
			// A node that followed the mesh's timeline goes on from its current frame, which becomes the reference. One
			// that didn't (it restarted as the clock, or it just joined) takes the frame the other nodes told it.
			if (!(timeline_trusted && clock.is_synchronized()) && reference_valid) {
				const double frame = reference_frame + double(now_usec - reference_usec) * double(settings.ticks_per_second) / 1000000.0;
				stepper.set_next_frame_index(tick_frame_at(frame));
				reset_received_states();
			}
			clock.set_master(true);
			stepped_usec = 0;
			clock.set_master_epoch_usec(compute_epoch());
			timeline_trusted = true;
		} else {
			// Another clock: the samples measured the previous one. The frames go on from the timeline followed until
			// now while the new clock's samples arrive (ADR-074); a node without a timeline waits for them.
			const bool had_timeline = timeline_trusted && clock.is_synchronized();
			const bool was_master = clock.is_master();
			const int64_t offset = was_master ? 0 : clock.get_timeline_offset_usec(now_usec);
			const int64_t epoch = was_master ? compute_epoch() : clock.get_timeline_epoch_usec();
			if (was_master) {
				clock.set_master(false);
				clock.set_sample_window(16, settings.clock_min_samples);
			}
			if (had_timeline) {
				clock.hold(offset, epoch);
			} else {
				clock.clear_samples();
				timeline_trusted = false;
			}
			last_ping_usec = 0;
		}
	}

	// The objects of the requests the previous registry won't answer.
	LocalVector<String> denied_paths;
	if (registry_moved || resync) {
		for (const uint16_t id : pending_requests) {
			const Entry *entry = entries.getptr(id);
			if (entry) {
				denied_paths.push_back(entry->path);
			}
		}
		pending_requests.clear();
		// The objects this node was releasing stay its own.
		cancel_releases(denied_paths);
		if (old_registry == local_id) {
			registry.clear();
			registry_ids_by_path.clear();
			quarantined_ids.clear();
			registry_settling = false;
			registry_unreported.clear();
			registry_deferred.clear();
			registry_orphan_check = false;
		}
		if (resync) {
			forget_objects();
		}
		if (p_registry == local_id) {
			registry_take_over();
		} else if (!resync) {
			send_registry_report();
		}
		// The claims the previous registry didn't answer (sent once the new one is ready).
		claim_unbound_objects();
	}

	// Game code, last.
	notify_denied(denied_paths);
	if (listener) {
		listener->on_roles_changed(p_registry, p_clock);
	}
}


// Gives up the releases in progress: nobody will answer them (the registry moved, or it's gone), so the objects stay
// with this node, simulated again. Adds the paths of the ones the game asked to release to `r_denied_paths`.
void TickMeshCore::cancel_releases(LocalVector<String> &r_denied_paths) {
	for (KeyValue<uint16_t, Entry> &E : entries) {
		Entry &entry = E.value;
		if (!entry.frozen) {
			continue;
		}
		if (entry.release_own) {
			r_denied_paths.push_back(entry.path);
		}
		entry.frozen = false;
		entry.release_own = false;
	}
}


// Tells the game that what it asked for these objects (a request, an assignment or a release) won't happen.
void TickMeshCore::notify_denied(const LocalVector<String> &p_paths) {
	for (const String &path : p_paths) {
		stats.denied_requests++;
		TickSyncObject **object = local_objects.getptr(path);
		if (listener && object) {
			listener->on_authority_request_denied(*object);
		}
	}
}


// Forgets the states received from the other nodes: the timeline's frames jumped, so their frames are of another
// stretch of it, and the next states would look older than them.
void TickMeshCore::reset_received_states() {
	for (KeyValue<uint16_t, Entry> &E : entries) {
		E.value.samples.clear();
		E.value.last_state_frame = TICK_FRAME_NONE;
	}
}

void TickMeshCore::registry_defer(const TickTransport::Packet &p_packet) {
	if (registry_deferred.size() >= MESH_MAX_DEFERRED) {
		WARN_PRINT_ONCE("The registry of the mesh can't answer (it's taking over, or it lost its quorum) and too many messages wait for it: the new ones are dropped.");
		if (!p_packet.data.is_empty() && p_packet.data[0] == TICK_MESSAGE_AUTH_RELEASE) {
			registry_refuse_release(p_packet);
		}
		return;
	}
	registry_deferred.push_back(p_packet);
}

void TickMeshCore::registry_resume() {
	if (registry_orphan_check) {
		registry_orphan_check = false;
		// The nodes that left while this registry couldn't act.
		LocalVector<int> gone;
		for (const KeyValue<uint16_t, RegistryRecord> &E : registry) {
			const int nodes[3] = { E.value.owner, E.value.pending_to, E.value.pending_requester };
			for (const int node : nodes) {
				if (node > 0 && !is_peer_ready(node) && !gone.has(node)) {
					gone.push_back(node);
				}
			}
		}
		for (const int node : gone) {
			registry_on_peer_left(node);
		}
	}
	// The messages that waited, from the nodes still here.
	LocalVector<TickTransport::Packet> deferred;
	deferred = registry_deferred;
	registry_deferred.clear();
	for (const TickTransport::Packet &packet : deferred) {
		if (is_peer_ready(packet.from_peer) && is_active()) {
			handle_packet(packet);
		}
	}
}

void TickMeshCore::send_registry_report() {
	const int registry_node = settings.registry_peer;
	if (registry_node == local_id || !is_registry_reachable()) {
		return;
	}
	reported_epoch = registry_epoch;
	// Split in messages the transport takes; the last one says so (an empty view is one empty message).
	const int max_bytes = MAX(64, transport->get_max_payload_size());
	LocalVector<uint16_t> ids;
	for (const KeyValue<uint16_t, Entry> &E : entries) {
		ids.push_back(E.key);
	}
	ids.sort();
	TickDataBuffer body;
	body.begin_write();
	int count = 0;
	for (uint32_t i = 0; i <= ids.size(); i++) {
		const bool last = i == ids.size();
		TickDataBuffer part;
		if (!last) {
			const Entry &entry = entries[ids[i]];
			part.begin_write();
			part.add_uint_bits(ids[i], 16);
			part.add_string(entry.path);
			part.add_int_bits(entry.owner, 32);
			part.add_uint_bits(entry.version, 32);
			part.add_uint_bits(entry.frame, 32);
			part.add_uint_bits(entry.schema_hash, 32);
		}
		const bool too_big = !last && count > 0 && (body.total_size() + part.total_size() + 128) / 8 > max_bytes;
		if (last || too_big) {
			TickDataBuffer message;
			message.begin_write();
			message.add_uint_bits(TICK_MESSAGE_REGISTRY_REPORT, 8);
			message.add_uint_bits(roles_term, 32);
			message.add_bool(last);
			message.add_uint_bits(highest_net_id, 16);
			message.add_uint_bits(uint64_t(count), 16);
			if (count > 0) {
				body.begin_read();
				body.slice(message, 0, body.total_size());
			}
			send(registry_node, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
			body.begin_write();
			count = 0;
		}
		if (!last) {
			part.begin_read();
			part.slice(body, 0, part.total_size());
			count++;
		}
	}
}

// Sends this node's view of the objects to a registry it reaches and didn't report to yet, so the registry learns
// what only this node knows (the registry answers a late report with its own view), then claims the objects of this
// node that have no id. A node that knows no object has nothing to report.
void TickMeshCore::sync_with_registry() {
	if (is_registry() || !is_registry_reachable()) {
		return;
	}
	if (reported_epoch != registry_epoch) {
		if (entries.is_empty()) {
			reported_epoch = registry_epoch;
		} else {
			send_registry_report();
		}
	}
	claim_unbound_objects();
}


void TickMeshCore::handle_registry_report(int p_peer, TickDataBuffer &p_message) {
	const uint32_t term = uint32_t(p_message.read_uint_bits(32));
	const bool last = p_message.read_bool();
	const uint16_t highest = uint16_t(p_message.read_uint_bits(16));
	const int count = int(p_message.read_uint_bits(16));
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	if (term != roles_term || registry_boot != boot_id || (!registry_settling && !registry_can_act())) {
		// For another takeover, or this registry can't act on it.
		return;
	}
	// After the settle time (a node that took long, or that only heard of the roles now), a newer record is announced
	// as it comes, and the node gets the registry's view back.
	const bool late = !registry_settling;
	highest_net_id = MAX(highest_net_id, highest);
	if (late && highest_net_id >= next_net_id) {
		next_net_id = uint16_t(highest_net_id + 1);
	}
	for (int i = 0; i < count; i++) {
		const uint16_t id = uint16_t(p_message.read_uint_bits(16));
		const String path = p_message.read_string();
		const int owner = int(p_message.read_int_bits(32));
		const uint32_t version = uint32_t(p_message.read_uint_bits(32));
		const uint32_t frame = uint32_t(p_message.read_uint_bits(32));
		const uint32_t schema_hash = uint32_t(p_message.read_uint_bits(32));
		if (p_message.is_buffer_failed() || id == 0 || path.is_empty() || owner < 0 || version == 0) {
			stats.malformed_packets++;
			return;
		}
		// The newest version any node saw is the truth: the previous registry's last announcements may have reached
		// only some nodes. Versions wrap around, and one too far ahead of what this registry knows isn't believed.
		RegistryRecord *record = registry.getptr(id);
		if (record && !mesh_serial_newer(version, record->version)) {
			continue;
		}
		const uint16_t *other = registry_ids_by_path.getptr(path);
		if (other && *other != id) {
			RegistryRecord &named = registry[*other];
			if (!mesh_serial_newer(version, named.version)) {
				continue;
			}
			if (named.pending_to >= 0) {
				registry_deny(named.pending_requester, *other);
			}
			registry.erase(*other);
		}
		if (record) {
			if (record->pending_to >= 0) {
				// The record starts over: the transfer in progress doesn't happen.
				registry_deny(record->pending_requester, id);
			}
			registry_ids_by_path.erase(record->path);
		}
		RegistryRecord merged;
		merged.path = path;
		merged.owner = owner;
		merged.version = version;
		merged.frame = frame;
		merged.schema_hash = schema_hash;
		registry.insert(id, merged);
		registry_ids_by_path.insert(path, id);
		highest_net_id = MAX(highest_net_id, id);
		if (late) {
			if (id >= next_net_id) {
				next_net_id = uint16_t(id + 1);
			}
			const bool gone = owner > 0 && !is_peer_ready(owner);
			registry_change_owner(id, gone ? 0 : owner, frame, nullptr);
		}
	}
	if (!last) {
		return;
	}
	registry_unreported.erase(p_peer);
	if (late) {
		LocalVector<uint16_t> ids;
		for (const KeyValue<uint16_t, RegistryRecord> &E : registry) {
			ids.push_back(E.key);
		}
		ids.sort();
		for (const uint16_t id : ids) {
			TickDataBuffer state;
			const bool has_state = registry_local_state(id, state);
			registry_send_announce(p_peer, id, has_state ? &state : nullptr);
		}
	}
}

void TickMeshCore::registry_take_over() {
	// This node's view of the objects is where the registry starts from; the other nodes' views complete it.
	registry.clear();
	registry_ids_by_path.clear();
	quarantined_ids.clear();
	for (const KeyValue<uint16_t, Entry> &E : entries) {
		RegistryRecord record;
		record.path = E.value.path;
		record.owner = E.value.owner;
		record.version = E.value.version;
		record.frame = E.value.frame;
		record.schema_hash = E.value.schema_hash;
		registry.insert(E.key, record);
		registry_ids_by_path.insert(record.path, E.key);
		highest_net_id = MAX(highest_net_id, E.key);
	}
	registry_settling = true;
	registry_settle_usec = now_usec + MESH_REGISTRY_SETTLE_USEC;
	registry_unreported.clear();
	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.ready) {
			registry_unreported.insert(E.key);
		}
	}
	registry_deferred.clear();
}

void TickMeshCore::registry_finish_take_over() {
	registry_settling = false;
	registry_unreported.clear();
	// New ids never repeat one any node heard of (the claim loop skips 0 if this wraps).
	next_net_id = uint16_t(highest_net_id + 1);
	// Every object is announced again with a new version, so every node ends with the same view; the ones whose owner
	// left (the previous registry's, for example) are orphaned, with the last state known here.
	LocalVector<uint16_t> ids;
	for (const KeyValue<uint16_t, RegistryRecord> &E : registry) {
		ids.push_back(E.key);
	}
	ids.sort();
	for (const uint16_t id : ids) {
		RegistryRecord *record = registry.getptr(id);
		if (record == nullptr) {
			continue;
		}
		const bool gone = record->owner > 0 && !is_peer_ready(record->owner);
		if (!gone) {
			registry_change_owner(id, record->owner, record->frame, nullptr);
			continue;
		}
		const Entry *entry = entries.getptr(id);
		const uint32_t frame = entry && entry->last_state_frame != TICK_FRAME_NONE ? entry->last_state_frame : stepper.get_next_frame_index();
		TickDataBuffer state;
		const bool has_state = registry_local_state(id, state);
		registry_change_owner(id, 0, frame, has_state ? &state : nullptr);
	}
	// The spawns of the nodes that left are this registry's now: the nodes that joined meanwhile get them.
	for (const uint32_t spawn_id : spawn_order) {
		if (spawns[spawn_id].adopted) {
			for (const KeyValue<int, PeerState> &E : peers) {
				if (E.value.ready) {
					send_spawn(E.key, spawn_id);
				}
			}
		}
	}
	// The messages that waited (unless this registry still waits for its quorum).
	registry_orphan_check = false;
	if (registry_can_act()) {
		registry_resume();
	}
}

// ------------------------------------------------------------------------------------------------------ Spawns

uint32_t TickMeshCore::get_next_spawn_id() const {
	return (uint32_t(local_id & 0xFFF) << 20) | (next_spawn_counter & 0xFFFFF);
}

uint32_t TickMeshCore::spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	ERR_FAIL_COND_V_MSG(!running, 0, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(local_id >= 4096, 0, "Only mesh nodes with an id below 4096 can spawn.");
	ERR_FAIL_COND_V_MSG(p_name.is_empty(), 0, "A spawned node needs a name.");
	ERR_FAIL_COND_V_MSG(!TickCodec::is_sendable(p_data), 0, "The spawn data can't contain objects, callables, signals, RIDs or reals that aren't finite.");
	// Not an id this node's previous process used (the registry keeps its spawns).
	for (int attempt = 0; attempt < 0x100000 && spawns.has(get_next_spawn_id()); attempt++) {
		next_spawn_counter++;
	}
	const uint32_t spawn_id = get_next_spawn_id();
	ERR_FAIL_COND_V_MSG(spawns.has(spawn_id), 0, "Too many spawns.");
	next_spawn_counter++;
	SpawnRecord record;
	record.spawner = p_spawner;
	record.scene = p_scene;
	record.name = p_name;
	record.controller = p_controller;
	record.data = p_data;
	record.origin = local_id;
	spawns.insert(spawn_id, record);
	spawn_order.push_back(spawn_id);
	stats.spawns++;
	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.ready) {
			send_spawn(E.key, spawn_id);
		}
	}
	return spawn_id;
}

void TickMeshCore::send_spawn(int p_peer, uint32_t p_spawn_id) {
	const SpawnRecord &record = spawns[p_spawn_id];
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_SPAWN, 8);
	message.add_uint_bits(p_spawn_id, 32);
	message.add_string(record.spawner);
	message.add_int_bits(record.scene, 16);
	message.add_string(record.name);
	message.add_int_bits(record.controller, 32);
	TickCodec::variant()->encode(record.data, message);
	message.add_bool(record.adopted);
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

bool TickMeshCore::owns_spawn(uint32_t p_spawn_id) const {
	if (!running) {
		return false;
	}
	const SpawnRecord *record = spawns.getptr(p_spawn_id);
	if (record == nullptr) {
		return false;
	}
	// The spawns of the nodes that left belong to the registry (ADR-074).
	return record->adopted ? (is_registry() && registry_boot == boot_id) : record->origin == local_id;
}

void TickMeshCore::despawn(uint32_t p_spawn_id) {
	const SpawnRecord *record = spawns.getptr(p_spawn_id);
	ERR_FAIL_COND_MSG(record == nullptr || !owns_spawn(p_spawn_id), vformat("Spawn %d doesn't belong to this node.", p_spawn_id));
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_DESPAWN, 8);
	message.add_uint_bits(p_spawn_id, 32);
	message.add_string(record->spawner);
	send_to_ready_peers(TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message, false);
	spawns.erase(p_spawn_id);
	spawn_order.erase(p_spawn_id);
	stats.despawns++;
}

void TickMeshCore::handle_spawn(int p_peer, TickDataBuffer &p_message) {
	const uint32_t spawn_id = uint32_t(p_message.read_uint_bits(32));
	const String spawner = p_message.read_string();
	const int scene = int(p_message.read_int_bits(16));
	const String name = p_message.read_string();
	const int controller = int(p_message.read_int_bits(32));
	const Variant data = TickCodec::variant()->decode(p_message);
	const bool adopted = p_message.read_bool();
	// A node spawns with its own id; the registry sends the spawns of the nodes that left (ADR-074).
	const bool from_origin = int(spawn_id >> 20) == (p_peer & 0xFFF);
	if (p_message.is_buffer_failed() || name.is_empty()) {
		stats.malformed_packets++;
		return;
	}
	if (!from_origin && !(adopted && p_peer == settings.registry_peer)) {
		// From a node that had the registry.
		stats.unexpected_packets++;
		return;
	}
	if (!from_origin && !is_registry_reachable()) {
		// Not from the process that has the registry.
		return;
	}
	SpawnRecord *known = spawns.getptr(spawn_id);
	if (known) {
		// Sent again: by the registry that took it over, or by its node, back in the mesh.
		known->adopted = adopted;
		return;
	}
	SpawnRecord record;
	record.spawner = spawner;
	record.scene = scene;
	record.name = name;
	record.controller = controller;
	record.data = data;
	record.origin = int(spawn_id >> 20);
	record.adopted = adopted;
	spawns.insert(spawn_id, record);
	spawn_order.push_back(spawn_id);
	stats.spawns++;
	if (listener) {
		listener->on_spawn(spawner, spawn_id, scene, name, controller, data);
	}
}

void TickMeshCore::handle_despawn(int p_peer, TickDataBuffer &p_message) {
	const uint32_t spawn_id = uint32_t(p_message.read_uint_bits(32));
	const String spawner = p_message.read_string();
	// From the node that spawned it, or from the registry (the spawns of the nodes that left are its own).
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	if (int(spawn_id >> 20) != (p_peer & 0xFFF) && p_peer != settings.registry_peer) {
		stats.unexpected_packets++;
		return;
	}
	spawns.erase(spawn_id);
	spawn_order.erase(spawn_id);
	stats.despawns++;
	if (listener) {
		listener->on_despawn(spawner, spawn_id);
	}
}

// ------------------------------------------------------------------------------------------------------ Events

uint32_t TickMeshCore::get_event_frame(double p_seconds) const {
	return stepper.get_next_frame_index() + uint32_t(Math::ceil(MAX(p_seconds, 0.0) * double(settings.ticks_per_second)));
}

void TickMeshCore::queue_event(const PendingEvent &p_event) {
	PendingEvent event = p_event;
	event.sequence = next_event_sequence++;
	pending_events.push_back(event);
	pending_events.sort_custom<MeshEventOrder>();
}

void TickMeshCore::forward_event(int p_peer, const PendingEvent &p_event) {
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_MESH_EVENT, 8);
	message.add_uint_bits(p_event.target, 16);
	message.add_uint_bits(p_event.requested_frame, 32);
	message.add_string(p_event.name);
	TickCodec::variant()->encode(p_event.payload, message);
	message.add_int_bits(p_event.sender, 32);
	message.add_uint_bits(uint64_t(p_event.hops), 8);
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

bool TickMeshCore::dispatch_event(PendingEvent &r_event) {
	const uint32_t frame = r_event.requested_frame != TICK_FRAME_NONE ? r_event.requested_frame : r_event.frame;
	if (r_event.target == 0) {
		if (listener) {
			listener->on_network_event(r_event.sender, r_event.name, r_event.payload, frame);
		}
		return true;
	}
	Entry *entry = entries.getptr(r_event.target);
	if (entry == nullptr) {
		stats.events_rejected++;
		return true;
	}
	if (entry->owner == local_id) {
		if (entry->object == nullptr) {
			return true;
		}
		if (entry->object->validate_event(r_event.sender, r_event.name, r_event.payload) == 0) {
			stats.events_rejected++;
			return true;
		}
		// The validator is game code, which may have removed the object.
		if (entry->object) {
			entry->object->on_event(r_event.sender, r_event.name, r_event.payload, frame);
		}
		return true;
	}
	if (entry->owner == 0) {
		// Waits for an owner, until it expires.
		return now_usec >= r_event.expire_usec;
	}
	// The target changed owner: forward it (ADR-044).
	if (r_event.hops >= MAX_EVENT_FORWARDS) {
		stats.events_rejected++;
		return true;
	}
	r_event.hops++;
	forward_event(entry->owner, r_event);
	stats.events_forwarded++;
	return true;
}

void TickMeshCore::run_events(uint32_t p_frame) {
	if (pending_events.is_empty()) {
		return;
	}
	LocalVector<PendingEvent> events;
	events = pending_events;
	pending_events.clear();
	LocalVector<PendingEvent> kept;
	for (PendingEvent &event : events) {
		const bool due = event.frame == TICK_FRAME_NONE || !tick_frame_after(event.frame, p_frame);
		if (!due || !dispatch_event(event)) {
			kept.push_back(event);
		}
	}
	for (const PendingEvent &event : pending_events) {
		kept.push_back(event);
	}
	pending_events = kept;
	pending_events.sort_custom<MeshEventOrder>();
}

void TickMeshCore::handle_mesh_event(int p_peer, TickDataBuffer &p_message) {
	PendingEvent event;
	event.target = uint16_t(p_message.read_uint_bits(16));
	event.requested_frame = uint32_t(p_message.read_uint_bits(32));
	event.name = p_message.read_string(TICK_MAX_EVENT_NAME_BYTES);
	event.payload = TickCodec::variant()->decode(p_message);
	const int origin = int(p_message.read_int_bits(32));
	event.hops = int(p_message.read_uint_bits(8));
	if (p_message.is_buffer_failed() || String(event.name).is_empty()) {
		stats.malformed_packets++;
		return;
	}
	// The transport identifies the sender; a forwarded event carries its origin (ADR-044).
	event.sender = event.hops > 0 ? origin : p_peer;
	event.expire_usec = now_usec + MESH_PENDING_EVENT_TIMEOUT_USEC;
	stats.events_received++;
	if (event.target == 0 && listener && listener->validate_network_event(event.sender, event.name, event.payload) == 0) {
		stats.events_rejected++;
		return;
	}
	const uint32_t current = stepper.get_next_frame_index();
	if (event.requested_frame == TICK_FRAME_NONE || !tick_frame_after(event.requested_frame, current)) {
		event.frame = current;
	} else {
		event.frame = event.requested_frame;
	}
	queue_event(event);
}

Error TickMeshCore::send_event(TickSyncObject *p_target, const StringName &p_name, const Variant &p_payload, uint32_t p_frame, int p_peer) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(String(p_name).is_empty(), ERR_INVALID_PARAMETER, "The event needs a name.");
	ERR_FAIL_COND_V_MSG(String(p_name).utf8().length() > TICK_MAX_EVENT_NAME_BYTES, ERR_INVALID_PARAMETER, vformat("An event name can't be longer than %d bytes in UTF-8.", TICK_MAX_EVENT_NAME_BYTES));
	ERR_FAIL_COND_V_MSG(!TickCodec::is_sendable(p_payload), ERR_INVALID_DATA, "The event payload can't contain objects, callables, signals, RIDs or reals that aren't finite.");
	int payload_bytes = 0;
	ERR_FAIL_COND_V_MSG(encode_variant(p_payload, nullptr, payload_bytes, false) != OK, ERR_INVALID_DATA, "The event payload can't be encoded.");
	ERR_FAIL_COND_V_MSG(payload_bytes > settings.max_event_bytes, ERR_INVALID_DATA, vformat("The event payload takes %d bytes; the limit is %d.", payload_bytes, settings.max_event_bytes));

	PendingEvent event;
	event.name = p_name;
	event.payload = p_payload;
	event.requested_frame = p_frame;
	event.sender = local_id;
	event.expire_usec = now_usec + MESH_PENDING_EVENT_TIMEOUT_USEC;
	if (p_target) {
		uint16_t id = 0;
		Entry *entry = find_entry(p_target, &id);
		ERR_FAIL_NULL_V_MSG(entry, ERR_UNAVAILABLE, "The event's target object isn't registered in the mesh yet.");
		event.target = id;
		stats.events_sent++;
		if (entry->owner == local_id || entry->owner == 0) {
			// Runs here, or waits for an owner.
			event.frame = p_frame == TICK_FRAME_NONE ? stepper.get_next_frame_index() : p_frame;
			queue_event(event);
			return OK;
		}
		forward_event(entry->owner, event);
		return OK;
	}
	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.ready && (p_peer == TickTransport::PEER_BROADCAST || p_peer == E.key)) {
			forward_event(E.key, event);
			stats.events_sent++;
		}
	}
	return OK;
}

Dictionary TickMeshCore::get_stats_dictionary() const {
	Dictionary result;
	result["states_sent"] = stats.states_sent;
	result["states_received"] = stats.states_received;
	result["stale_states"] = stats.stale_states;
	result["late_states"] = stats.late_states;
	result["malformed_packets"] = stats.malformed_packets;
	result["unexpected_packets"] = stats.unexpected_packets;
	result["transfers"] = stats.transfers;
	result["orphans"] = stats.orphans;
	result["denied_requests"] = stats.denied_requests;
	result["events_sent"] = stats.events_sent;
	result["events_received"] = stats.events_received;
	result["events_forwarded"] = stats.events_forwarded;
	result["events_rejected"] = stats.events_rejected;
	result["spawns"] = stats.spawns;
	result["despawns"] = stats.despawns;
	result["role_changes"] = stats.role_changes;
	result["roles_term"] = roles_term;
	result["has_quorum"] = has_role_quorum();
	result["timeline_frame"] = get_timeline_frame(now_usec);
	return result;
}

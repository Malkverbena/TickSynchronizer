#include "tick_mesh_core.h"

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

void TickMeshCore::set_settings(const Settings &p_settings) {
	ERR_FAIL_COND_MSG(running, "The settings can't change while the network is running.");
	ERR_FAIL_COND_MSG(p_settings.ticks_per_second <= 0, "The ticks per second must be positive.");
	ERR_FAIL_COND_MSG(p_settings.registry_peer <= 0 || p_settings.clock_master <= 0, "The registry and clock master peers must be positive.");
	ERR_FAIL_COND_MSG(p_settings.keyframe_interval < 1, "The keyframe interval must be at least 1.");
	settings = p_settings;
}

Error TickMeshCore::start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec) {
	ERR_FAIL_COND_V_MSG(running, ERR_ALREADY_IN_USE, "The network is already running.");
	ERR_FAIL_COND_V_MSG(p_transport.is_null(), ERR_INVALID_PARAMETER, "The transport is null.");
	ERR_FAIL_COND_V_MSG(p_transport->get_channel_count() < TICK_CHANNEL_COUNT, ERR_INVALID_PARAMETER, vformat("The transport must have at least %d channels.", TICK_CHANNEL_COUNT));

	transport = p_transport;
	local_id = transport->get_local_peer_id();
	now_usec = p_now_usec;
	stats = Stats();
	running = true;

	stepper.reset();
	stepper.set_ticks_per_second(settings.ticks_per_second);
	clock.set_ticks_per_second(settings.ticks_per_second);
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
	// The registry registers its own objects; the others claim theirs once the registry is ready.
	claim_unbound_objects();
	return OK;
}

void TickMeshCore::stop() {
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
}

bool TickMeshCore::is_peer_ready(int p_peer) const {
	if (p_peer == local_id) {
		return true;
	}
	const PeerState *peer = peers.getptr(p_peer);
	return peer && peer->ready;
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
	if (!is_peer_ready(settings.registry_peer)) {
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
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, hello);
}

void TickMeshCore::handle_hello(int p_peer, TickDataBuffer &p_message) {
	PeerState *peer = peers.getptr(p_peer);
	if (peer == nullptr || peer->ready || peer->rejected) {
		return;
	}
	const int version = int(p_message.read_uint_bits(16));
	const bool is_double = p_message.read_bool();
	String reason;
	if (p_message.is_buffer_failed()) {
		reason = "Malformed handshake.";
	} else if (version != TICK_PROTOCOL_VERSION) {
		reason = vformat("Protocol version %d is incompatible with this node's version %d.", version, TICK_PROTOCOL_VERSION);
	} else if (is_double != (sizeof(real_t) == sizeof(double))) {
		reason = "The nodes of a mesh must use the same precision build.";
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
	on_peer_ready(p_peer);
}

void TickMeshCore::on_peer_ready(int p_peer) {
	peers[p_peer].ready = true;
	if (is_registry()) {
		for (const KeyValue<uint16_t, RegistryRecord> &E : registry) {
			TickDataBuffer state;
			const bool has_state = registry_local_state(E.key, state);
			registry_send_announce(p_peer, E.key, has_state ? &state : nullptr);
		}
	}
	// A late node gets the spawns of every node, each from its origin.
	for (const uint32_t spawn_id : spawn_order) {
		send_spawn(p_peer, spawn_id);
	}
	if (p_peer == settings.registry_peer) {
		claim_unbound_objects();
	}
	if (listener) {
		listener->on_peer_ready(p_peer);
	}
}

void TickMeshCore::handle_events() {
	TickTransport::Event event;
	while (transport->pop_event(event)) {
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
		if (is_registry()) {
			registry_on_peer_left(event.peer);
		}
		if (event.peer == settings.registry_peer) {
			WARN_PRINT("The registry node left the mesh: ownership can't change until it's back.");
		}
		if (listener) {
			listener->on_peer_left(event.peer);
		}
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
		const String reason = message.read_string();
		ERR_PRINT(vformat("A mesh node refused this node: %s", reason));
		if (listener) {
			listener->on_rejected(reason);
		}
		return;
	}
	// Everything else only from ready nodes (or this node).
	if (!is_peer_ready(from)) {
		stats.malformed_packets++;
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
		case TICK_MESSAGE_ANNOUNCE:
		case TICK_MESSAGE_UNREGISTER:
		case TICK_MESSAGE_AUTH_TRANSFER:
		case TICK_MESSAGE_AUTH_DENIED:
			if (!from_registry) {
				stats.malformed_packets++;
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
				stats.malformed_packets++;
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
	return int64_t(now_usec) - int64_t(elapsed_frames * 1000000.0 * get_tick_delta());
}

void TickMeshCore::handle_ping(int p_peer, TickDataBuffer &p_message) {
	const uint64_t client_time = p_message.read_uint_bits(64);
	if (p_message.is_buffer_failed() || !is_clock_master()) {
		stats.malformed_packets++;
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
	if (p_message.is_buffer_failed() || p_peer != settings.clock_master || is_clock_master()) {
		stats.malformed_packets++;
		return;
	}
	clock.add_sample(client_time, master_time, now_usec);
	clock.set_master_epoch_usec(epoch);
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
	const uint32_t target = uint32_t(Math::floor(p_target_frame));
	const int32_t behind = int32_t(target - stepper.get_next_frame_index()) + 1;
	const int32_t limit = stepper.get_max_ticks_per_advance();
	if (behind > limit * 4 || behind < -limit * 4) {
		// Too far off (start, or a long hitch): jump to the timeline's frame.
		stepper.set_next_frame_index(target);
		tick(stepper.step_frame());
		return;
	}
	for (int32_t i = 0; i < MIN(behind, limit); i++) {
		tick(stepper.step_frame());
	}
}

// ------------------------------------------------------------------------------------------------------ Process

void TickMeshCore::process(double p_delta, uint64_t p_now_usec) {
	ERR_FAIL_COND_MSG(!running, "The network isn't running.");
	now_usec = p_now_usec;

	transport->poll();
	handle_events();
	TickTransport::Packet packet;
	while (running && transport->pop_packet(packet)) {
		handle_packet(packet);
	}
	// Messages to itself, including the ones they cause (bounded).
	for (int round = 0; round < 16 && !loopback.is_empty(); round++) {
		LocalVector<TickTransport::Packet> packets;
		packets = loopback;
		loopback.clear();
		for (const TickTransport::Packet &local : packets) {
			handle_packet(local);
		}
	}
	if (!running) {
		return;
	}
	if (is_registry()) {
		registry_check_timeouts();
	}

	if (is_clock_master()) {
		if (clock_source) {
			follow_timeline(clock_source->get_timeline_frame(now_usec));
		} else {
			stepper.advance(p_delta);
			while (stepper.get_pending_ticks() > 0) {
				tick(stepper.pop_tick());
			}
		}
	} else {
		follow_timeline(clock.is_synchronized() ? clock.get_master_frame_time(now_usec) : -1.0);
		if (is_peer_ready(settings.clock_master)) {
			const uint64_t interval = clock.is_synchronized() ? uint64_t(settings.ping_interval * 1000000.0) : uint64_t(1000000.0 * get_tick_delta());
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
		// The owner drives the object with its own input.
		TickDataBuffer input;
		input.begin_write();
		entry.object->collect_input(input);
		input.begin_read();
		entry.object->process_tick(delta, input);
		quantize_object(entry.object);
	}
	if (p_frame % uint32_t(settings.snapshot_interval) == 0) {
		send_states(p_frame);
	}
}

void TickMeshCore::send_states(uint32_t p_frame) {
	const bool keyframe = p_frame % uint32_t(settings.keyframe_interval) == 0;
	const int max_bytes = MAX(64, transport->get_max_payload_size());

	TickDataBuffer body;
	body.begin_write();
	int count = 0;
	LocalVector<uint16_t> ids;
	for (KeyValue<uint16_t, Entry> &E : entries) {
		if (E.value.owner == local_id && E.value.object && !E.value.frozen) {
			ids.push_back(E.key);
		}
	}
	ids.sort();

	for (uint32_t i = 0; i <= ids.size(); i++) {
		const bool last = i == ids.size();
		TickDataBuffer object_part;
		if (!last) {
			Entry &entry = entries[ids[i]];
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
			object_part.begin_write();
			object_part.add_uint_bits(ids[i], 16);
			object_part.add_uint_bits(entry.version, 32);
			object_part.add_data_buffer(payload);
		}
		// Flushes when the packet would get too big, and at the end.
		const bool too_big = !last && count > 0 && (body.total_size() + object_part.total_size() + 64) / 8 > max_bytes;
		if ((last || too_big) && count > 0) {
			TickDataBuffer message;
			message.begin_write();
			message.add_uint_bits(TICK_MESSAGE_STATE, 8);
			message.add_uint_bits(p_frame, 32);
			message.add_uint_bits(uint64_t(count), 16);
			body.begin_read();
			body.slice(message, 0, body.total_size());
			send_to_ready_peers(TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_UNRELIABLE, message, false);
			stats.states_sent += uint64_t(count);
			body.begin_write();
			count = 0;
		}
		if (!last) {
			object_part.begin_read();
			object_part.slice(body, 0, object_part.total_size());
			count++;
		}
	}
}

void TickMeshCore::handle_state(int p_peer, TickDataBuffer &p_message) {
	const uint32_t frame = uint32_t(p_message.read_uint_bits(32));
	const int count = int(p_message.read_uint_bits(16));
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
		if (entry->last_state_frame != TICK_FRAME_NONE && !tick_frame_after(frame, entry->last_state_frame)) {
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
	if (!running) {
		return;
	}
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
	ERR_FAIL_COND_MSG(registry.size() + quarantined_ids.size() >= UINT16_MAX - 1, "Too many synchronized objects.");
	const uint32_t current = stepper.get_next_frame_index();
	const uint32_t quarantine = uint32_t(settings.history_size) * 2;
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

void TickMeshCore::registry_change_owner(uint16_t p_id, int p_new_owner, uint32_t p_frame, const TickDataBuffer *p_state) {
	RegistryRecord &record = registry[p_id];
	record.owner = p_new_owner;
	record.version++;
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
	// Only the current owner, for the current version.
	if (record == nullptr || record->owner != p_peer || record->version != version) {
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
	if (record->pending_to >= 0 && record->pending_to != to) {
		// Released somewhere else than the pending transfer asked.
		registry_deny(record->pending_requester, id);
	}
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
			// Orphaned (ADR-010): the last state known here goes with the announcement.
			const int requester = record.pending_requester;
			const bool had_pending = record.pending_to >= 0;
			const Entry *entry = entries.getptr(id);
			const uint32_t frame = entry && entry->last_state_frame != TICK_FRAME_NONE ? entry->last_state_frame : stepper.get_next_frame_index();
			TickDataBuffer state;
			const bool has_state = registry_local_state(id, state);
			registry_change_owner(id, 0, frame, has_state ? &state : nullptr);
			if (had_pending && requester != p_peer) {
				registry_deny(requester, id);
			}
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
	if (p_message.is_buffer_failed() || id == 0 || path.is_empty()) {
		stats.malformed_packets++;
		return;
	}

	Entry *existing = entries.getptr(id);
	if (existing && version < existing->version) {
		return;
	}
	if (existing == nullptr) {
		Entry fresh;
		fresh.path = path;
		entries.insert(id, fresh);
		ids_by_path.insert(path, id);
	}
	Entry &entry = entries[id];
	const bool first = entry.version == 0;
	const int old_owner = entry.owner;
	entry.path = path;
	entry.owner = owner;
	entry.version = version;
	entry.frame = frame;
	entry.schema_hash = schema_hash;
	bind_entry(id, entry);

	if (old_owner != owner || first) {
		entry.frozen = false;
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
				Sample sample;
				sample.frame = frame;
				sample.values = values;
				entry.samples.push_back(sample);
				entry.last_state_frame = frame;
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
	stats.denied_requests++;
	Entry *entry = entries.getptr(id);
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
	ERR_FAIL_COND_V_MSG(!is_peer_ready(settings.registry_peer), ERR_UNAVAILABLE, "The registry node isn't connected.");
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_AUTH_REQUEST, 8);
	message.add_uint_bits(id, 16);
	send(settings.registry_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
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
	ERR_FAIL_COND_V_MSG(!is_peer_ready(settings.registry_peer), ERR_UNAVAILABLE, "The registry node isn't connected.");
	release_frozen(id, *entry, p_to_peer);
	return OK;
}

Error TickMeshCore::assign_authority(TickSyncObject *p_object, int p_peer) {
	ERR_FAIL_COND_V_MSG(!running, ERR_UNCONFIGURED, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(p_peer < 0, ERR_INVALID_PARAMETER, "The new owner can't be negative.");
	uint16_t id = 0;
	Entry *entry = find_entry(p_object, &id);
	ERR_FAIL_NULL_V_MSG(entry, ERR_UNAVAILABLE, "The object isn't registered in the mesh yet.");
	ERR_FAIL_COND_V_MSG(!is_peer_ready(settings.registry_peer), ERR_UNAVAILABLE, "The registry node isn't connected.");
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_AUTH_ASSIGN, 8);
	message.add_uint_bits(id, 16);
	message.add_int_bits(p_peer, 32);
	send(settings.registry_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
	return OK;
}

// ------------------------------------------------------------------------------------------------------ Spawns

uint32_t TickMeshCore::get_next_spawn_id() const {
	return (uint32_t(local_id & 0xFFF) << 20) | (next_spawn_counter & 0xFFFFF);
}

uint32_t TickMeshCore::spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	ERR_FAIL_COND_V_MSG(!running, 0, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(local_id >= 4096, 0, "Only mesh nodes with an id below 4096 can spawn.");
	ERR_FAIL_COND_V_MSG(p_name.is_empty(), 0, "A spawned node needs a name.");
	const uint32_t spawn_id = get_next_spawn_id();
	next_spawn_counter++;
	SpawnRecord record;
	record.spawner = p_spawner;
	record.scene = p_scene;
	record.name = p_name;
	record.controller = p_controller;
	record.data = p_data;
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
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

void TickMeshCore::despawn(uint32_t p_spawn_id) {
	const SpawnRecord *record = spawns.getptr(p_spawn_id);
	ERR_FAIL_NULL_MSG(record, vformat("Spawn %d wasn't made by this node.", p_spawn_id));
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
	// A node only spawns with its own id.
	if (p_message.is_buffer_failed() || name.is_empty() || int(spawn_id >> 20) != (p_peer & 0xFFF)) {
		stats.malformed_packets++;
		return;
	}
	stats.spawns++;
	if (listener) {
		listener->on_spawn(spawner, spawn_id, scene, name, controller, data);
	}
}

void TickMeshCore::handle_despawn(int p_peer, TickDataBuffer &p_message) {
	const uint32_t spawn_id = uint32_t(p_message.read_uint_bits(32));
	const String spawner = p_message.read_string();
	if (p_message.is_buffer_failed() || int(spawn_id >> 20) != (p_peer & 0xFFF)) {
		stats.malformed_packets++;
		return;
	}
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
		entry->object->on_event(r_event.sender, r_event.name, r_event.payload, frame);
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
	event.name = p_message.read_string();
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
	int payload_bytes = 0;
	ERR_FAIL_COND_V_MSG(encode_variant(p_payload, nullptr, payload_bytes, false) != OK, ERR_INVALID_DATA, "The event payload can't be encoded (objects aren't allowed).");
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
	result["malformed_packets"] = stats.malformed_packets;
	result["transfers"] = stats.transfers;
	result["orphans"] = stats.orphans;
	result["denied_requests"] = stats.denied_requests;
	result["events_sent"] = stats.events_sent;
	result["events_received"] = stats.events_received;
	result["events_forwarded"] = stats.events_forwarded;
	result["events_rejected"] = stats.events_rejected;
	result["spawns"] = stats.spawns;
	result["despawns"] = stats.despawns;
	result["timeline_frame"] = get_timeline_frame(now_usec);
	return result;
}

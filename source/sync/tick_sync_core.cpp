#include "tick_sync_core.h"

#include "tick_net_ids.h"

#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/variant/variant.h"

// A rejected peer is disconnected after this delay, so the rejection reason reaches it.
static constexpr uint64_t REJECT_DISCONNECT_DELAY_USEC = 1000000;
// A full snapshot is sent again if it wasn't acknowledged within this delay.
static constexpr uint64_t FULL_SNAPSHOT_RESEND_USEC = 500000;
// A client keeps an event for an object it doesn't know yet for this long (E5).
static constexpr uint64_t PENDING_EVENT_TIMEOUT_USEC = 5000000;
// A doll whose controller sent no input for this long is interpolated again.
static constexpr uint64_t DOLL_STALE_USEC = 500000;
// Arrivals kept to estimate the jitter of a doll's inputs.
static constexpr uint32_t DOLL_ARRIVAL_WINDOW = 32;
// Speed control of a doll's timeline: speed change per frame of buffer error, and the largest change.
static constexpr double DOLL_SPEED_GAIN = 0.05;
static constexpr double DOLL_MAX_SPEED_DELTA = 0.2;
// Most frames a doll simulates in one local tick.
static constexpr int DOLL_MAX_STEPS = 3;

struct PendingEventOrder {
	template <typename T>
	bool operator()(const T &p_a, const T &p_b) const {
		// Events without frame first, then by frame, then in arrival order.
		if (p_a.frame != p_b.frame) {
			if (p_a.frame == TICK_FRAME_NONE || p_b.frame == TICK_FRAME_NONE) {
				return p_a.frame == TICK_FRAME_NONE;
			}
			return tick_frame_after(p_b.frame, p_a.frame);
		}
		return p_a.sequence < p_b.sequence;
	}
};

static bool sorted_contains(const LocalVector<uint16_t> &p_sorted, uint16_t p_value) {
	uint32_t low = 0;
	uint32_t high = p_sorted.size();
	while (low < high) {
		const uint32_t middle = (low + high) / 2;
		if (p_sorted[middle] < p_value) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low < p_sorted.size() && p_sorted[low] == p_value;
}

TickSyncCore::BusyScope::BusyScope(TickSyncCore *p_core) :
		core(p_core) {
	core->busy_depth++;
}

TickSyncCore::BusyScope::~BusyScope() {
	core->busy_depth--;
	if (core->busy_depth == 0 && core->stop_requested) {
		core->stop_now();
	}
}

bool TickSyncCore::RateLimiter::take(double p_rate, uint64_t p_now_usec) {
	if (tokens < 0.0) {
		tokens = p_rate;
		last_usec = p_now_usec;
	}
	tokens = MIN(p_rate, tokens + double(p_now_usec - last_usec) * p_rate / 1000000.0);
	last_usec = p_now_usec;
	if (tokens < 1.0) {
		return false;
	}
	tokens -= 1.0;
	return true;
}

void TickSyncCore::set_settings(const Settings &p_settings) {
	ERR_FAIL_COND_MSG(role != ROLE_NONE, "The settings can't change while the network is running.");
	ERR_FAIL_COND_MSG(p_settings.ticks_per_second <= 0, "The ticks per second must be positive.");
	ERR_FAIL_COND_MSG(p_settings.history_size < 8, "The history must keep at least 8 frames.");
	ERR_FAIL_COND_MSG(p_settings.input_redundancy < 1 || p_settings.input_redundancy > 64, "The input redundancy must be between 1 and 64.");
	ERR_FAIL_COND_MSG(p_settings.snapshot_interval < 1, "The snapshot interval must be at least 1.");
	ERR_FAIL_COND_MSG(p_settings.min_input_buffer < 0 || p_settings.max_input_buffer < p_settings.min_input_buffer, "The input buffer bounds are invalid.");
	ERR_FAIL_COND_MSG(p_settings.interest_interval < 1, "The interest interval must be at least 1.");
	settings = p_settings;
}

Error TickSyncCore::start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec) {
	ERR_FAIL_COND_V_MSG(stop_requested, ERR_BUSY, "The network is stopping: start it again once its callbacks return (for example, with `call_deferred()`).");
	ERR_FAIL_COND_V_MSG(role != ROLE_NONE, ERR_ALREADY_IN_USE, "The network is already running.");
	ERR_FAIL_COND_V_MSG(p_transport.is_null(), ERR_INVALID_PARAMETER, "The transport is null.");
	ERR_FAIL_COND_V_MSG(p_transport->get_channel_count() < TICK_CHANNEL_COUNT, ERR_INVALID_PARAMETER, vformat("The transport must have at least %d channels.", TICK_CHANNEL_COUNT));

	BusyScope busy(this);
	transport = p_transport;
	now_usec = p_now_usec;
	stats = Stats();
	role = transport->get_local_peer_id() == settings.authority_peer ? ROLE_SERVER : ROLE_CLIENT;

	stepper.reset();
	stepper.set_ticks_per_second(settings.ticks_per_second);
	stepper.set_time_scale(1.0);
	clock.set_ticks_per_second(settings.ticks_per_second);

	if (role == ROLE_SERVER) {
		clock.set_master(true);
		server_epoch_usec = int64_t(p_now_usec);
		clock.set_master_epoch_usec(server_epoch_usec);
		server_history.clear();
		server_history.resize(settings.history_size);
		authority_inputs.clear();
		authority_inputs.resize(settings.history_size);

		next_net_id = 1;
		for (const KeyValue<String, TickSyncObject *> &E : local_objects) {
			server_add_object(E.value);
		}

		LocalVector<int> connected;
		transport->get_connected_peers(connected);
		for (const int peer : connected) {
			PeerState state;
			state.inputs.resize(settings.history_size);
			peers.insert(peer, state);
		}
	} else {
		clock.set_master(false);
		clock.set_sample_window(16, settings.clock_min_samples);
		received.clear();
		received.resize(settings.history_size);
		predictions.clear();
		predictions.resize(settings.history_size);
		dolls.clear();
		shares_inputs = false;
		pending_snapshot = PendingSnapshot();
		welcomed = false;
		rejected = false;
		predicting = false;
		needs_full = false;
		latest_snapshot = TICK_FRAME_NONE;
		last_reconciled = TICK_FRAME_NONE;
		last_ping_usec = 0;
		if (transport->is_peer_connected(settings.authority_peer)) {
			TickDataBuffer hello;
			hello.begin_write();
			hello.add_uint_bits(TICK_MESSAGE_HELLO, 8);
			hello.add_uint_bits(TICK_PROTOCOL_VERSION, 16);
			hello.add_bool(sizeof(real_t) == sizeof(double));
			send(settings.authority_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, hello);
		}
	}
	return OK;
}

void TickSyncCore::stop() {
	if (busy_depth > 0) {
		// Game code the engine is running asked for it: the engine's state is freed once that code returns.
		stop_requested = true;
		return;
	}
	stop_now();
}

void TickSyncCore::stop_now() {
	stop_requested = false;
	role = ROLE_NONE;
	transport.unref();
	peers.clear();
	server_history.clear();
	server_objects.clear();
	server_object_ids.clear();
	server_ids_by_object.clear();
	quarantined_ids.clear();
	spawns.clear();
	spawn_order.clear();
	pending_events.clear();
	received.clear();
	pending_snapshot = PendingSnapshot();
	predictions.clear();
	authority_inputs.clear();
	dolls.clear();
	shares_inputs = false;
	for (KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
		E.value.object = nullptr;
	}
	remote_objects.clear();
	predicted_ids.clear();
	predicted_ids_dirty = true;
	welcomed = false;
	predicting = false;
	rewinding = false;
}

void TickSyncCore::register_object(TickSyncObject *p_object) {
	ERR_FAIL_NULL(p_object);
	const String path = p_object->get_sync_path();
	ERR_FAIL_COND_MSG(path.is_empty(), "A synchronized object needs a path.");

	// Objects can be registered before the network starts; the role decides what happens to them in `start()`.
	if (local_objects.has(path)) {
		ERR_FAIL_COND_MSG(local_objects[path] != p_object, vformat("Another object is already registered with the path \"%s\".", path));
		return;
	}
	local_objects.insert(path, p_object);

	// The interest filter and the events of a bound object are game code.
	BusyScope busy(this);
	if (role == ROLE_SERVER) {
		server_add_object(p_object);
	} else if (role == ROLE_CLIENT) {
		for (KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
			if (E.value.path == path) {
				client_bind(E.key, E.value);
			}
		}
	}
}

void TickSyncCore::server_add_object(TickSyncObject *p_object) {
	const uint32_t current = stepper.get_next_frame_index();
	const uint32_t quarantine = uint32_t(settings.history_size) * 2;
	if (server_objects.size() + quarantined_ids.size() >= UINT16_MAX - 1) {
		// Released ids are only forgotten when reused: the ones past their quarantine don't count.
		tick_prune_quarantine(quarantined_ids, current, quarantine);
	}
	ERR_FAIL_COND_MSG(server_objects.size() + quarantined_ids.size() >= UINT16_MAX - 1, "Too many synchronized objects.");
	for (int attempt = 0; attempt < UINT16_MAX; attempt++) {
		if (next_net_id == 0 || server_objects.has(next_net_id)) {
			next_net_id++;
			continue;
		}
		// A released id is reused only once no snapshot in flight can refer to it (ADR-034).
		const uint32_t *released = quarantined_ids.getptr(next_net_id);
		if (released && current - *released < quarantine) {
			next_net_id++;
			continue;
		}
		quarantined_ids.erase(next_net_id);
		break;
	}
	const uint16_t net_id = next_net_id++;
	ServerObject object;
	object.object = p_object;
	object.path = p_object->get_sync_path();
	object.controller = p_object->get_controller_peer();
	object.schema_hash = p_object->get_sync_schema().hash();
	server_objects.insert(net_id, object);
	server_ids_by_object.insert(p_object, net_id);
	server_object_ids.push_back(net_id);
	server_object_ids.sort();

	for (KeyValue<int, PeerState> &E : peers) {
		if (E.value.accepted && server_objects.has(net_id)) {
			server_register_for_peer(E.key, E.value, net_id);
		}
	}
}

void TickSyncCore::unregister_object(TickSyncObject *p_object) {
	ERR_FAIL_NULL(p_object);
	const String path = p_object->get_sync_path();
	TickSyncObject **registered = local_objects.getptr(path);
	if (registered && *registered == p_object) {
		local_objects.erase(path);
	}

	const uint16_t *net_id = server_ids_by_object.getptr(p_object);
	if (net_id) {
		const uint16_t id = *net_id;
		server_ids_by_object.erase(p_object);
		server_objects.erase(id);
		server_object_ids.erase(id);
		quarantined_ids.insert(id, stepper.get_next_frame_index());
		for (KeyValue<int, PeerState> &E : peers) {
			E.value.relevant.erase(id);
			E.value.relevant_since.erase(id);
		}
		for (SnapshotRecord &record : server_history) {
			record.states.erase(id);
		}
		if (role == ROLE_SERVER) {
			for (const KeyValue<int, PeerState> &E : peers) {
				if (E.value.accepted) {
					TickDataBuffer message;
					message.begin_write();
					message.add_uint_bits(TICK_MESSAGE_UNREGISTER, 8);
					message.add_uint_bits(id, 16);
					send(E.key, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
				}
			}
		}
	}

	for (KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
		if (E.value.object == p_object) {
			E.value.object = nullptr;
			predicted_ids_dirty = true;
		}
	}
}

uint16_t TickSyncCore::get_net_id(const TickSyncObject *p_object) const {
	if (role == ROLE_SERVER) {
		const uint16_t *net_id = server_ids_by_object.getptr(const_cast<TickSyncObject *>(p_object));
		return net_id ? *net_id : 0;
	}
	for (const KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
		if (E.value.object == p_object) {
			return E.key;
		}
	}
	return 0;
}

void TickSyncCore::send(int p_peer, TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message) {
	ERR_FAIL_COND(transport.is_null());
	if (p_peer != TickTransport::PEER_BROADCAST && !transport->is_peer_connected(p_peer)) {
		// The peer left; its disconnection event is processed next.
		return;
	}
	p_message.dry();
	const LocalVector<uint8_t> &bytes = p_message.get_buffer().get_bytes();
	transport->send(p_peer, p_channel, p_mode, bytes.ptr(), int(bytes.size()));
}

void TickSyncCore::read_states(TickSyncObject *p_object, LocalVector<Variant> &r_values) const {
	const int count = p_object->get_sync_schema().size();
	r_values.resize(count);
	for (int i = 0; i < count; i++) {
		r_values[i] = p_object->get_sync_var(i);
	}
}

void TickSyncCore::quantize_object(TickSyncObject *p_object) const {
	const TickSchema &schema = p_object->get_sync_schema();
	for (int i = 0; i < schema.size(); i++) {
		p_object->set_sync_var(i, schema.codecs[i]->quantize(p_object->get_sync_var(i)));
	}
}

bool TickSyncCore::parse_frame_input(TickDataBuffer &p_frame_input, HashMap<uint16_t, TickDataBuffer> &r_inputs) {
	r_inputs.clear();
	const int count = int(p_frame_input.read_uint_bits(8));
	for (int i = 0; i < count && !p_frame_input.is_buffer_failed(); i++) {
		const uint16_t net_id = uint16_t(p_frame_input.read_uint_bits(16));
		TickDataBuffer input;
		p_frame_input.read_data_buffer(input);
		r_inputs.insert(net_id, input);
	}
	if (p_frame_input.is_buffer_failed()) {
		r_inputs.clear();
		return false;
	}
	return true;
}

void TickSyncCore::write_input_groups(TickDataBuffer &r_message, uint32_t p_first_frame, const LocalVector<const TickDataBuffer *> &p_frames) {
	LocalVector<const TickDataBuffer *> groups;
	LocalVector<int> duplicates;
	for (const TickDataBuffer *input : p_frames) {
		if (!groups.is_empty() && *groups[groups.size() - 1] == *input && duplicates[duplicates.size() - 1] < 255) {
			duplicates[duplicates.size() - 1]++;
		} else {
			groups.push_back(input);
			duplicates.push_back(0);
		}
	}
	r_message.add_uint_bits(groups.size(), 8);
	r_message.add_uint_bits(p_first_frame, 32);
	for (uint32_t i = 0; i < groups.size(); i++) {
		r_message.add_uint_bits(uint64_t(duplicates[i]), 8);
		r_message.add_data_buffer(*groups[i]);
	}
}

void TickSyncCore::process(double p_delta, uint64_t p_now_usec) {
	ERR_FAIL_COND_MSG(!is_active(), "The network isn't running.");
	BusyScope busy(this);
	now_usec = p_now_usec;

	transport->poll();
	handle_events();
	TickTransport::Packet packet;
	while (is_active() && transport->pop_packet(packet)) {
		handle_packet(packet);
	}
	if (!is_active()) {
		return;
	}

	if (role == ROLE_SERVER) {
		if (clock_source) {
			// Follows the source's timeline: simulates every frame up to the source's current one.
			const double source_frame = clock_source->get_timeline_frame(now_usec);
			if (source_frame >= 0.0) {
				const uint32_t target = uint32_t(Math::floor(source_frame));
				const uint32_t next = stepper.get_next_frame_index();
				const int32_t behind = int32_t(target - next) + 1;
				if (behind > stepper.get_max_ticks_per_advance() * 4 || behind < -stepper.get_max_ticks_per_advance() * 4) {
					// Too far off (start, or a long hitch): jump to the source's frame.
					stepper.set_next_frame_index(target);
					server_tick(stepper.step_frame());
				} else {
					for (int32_t i = 0; i < MIN(behind, int32_t(stepper.get_max_ticks_per_advance())) && is_active(); i++) {
						server_tick(stepper.step_frame());
					}
				}
			}
		} else {
			const uint64_t dropped_before = stepper.get_dropped_ticks();
			stepper.advance(p_delta);
			// Dropped ticks shift the timeline: the frames keep their duration.
			server_epoch_usec += int64_t(double(stepper.get_dropped_ticks() - dropped_before) * 1000000.0 * get_tick_delta());
			while (stepper.get_pending_ticks() > 0 && is_active()) {
				server_tick(stepper.pop_tick());
			}
		}
		if (!is_active()) {
			return;
		}

		LocalVector<int> to_disconnect;
		for (const KeyValue<int, PeerState> &E : peers) {
			if (E.value.rejected && now_usec - E.value.reject_usec >= REJECT_DISCONNECT_DELAY_USEC) {
				to_disconnect.push_back(E.key);
			}
		}
		for (const int peer : to_disconnect) {
			transport->disconnect_peer(peer);
			peers.erase(peer);
		}
	} else {
		if (!predicting && welcomed && clock.is_synchronized() && latest_snapshot != TICK_FRAME_NONE) {
			client_start_prediction();
		}
		bool ticked = false;
		if (predicting) {
			stepper.advance(p_delta);
			while (stepper.get_pending_ticks() > 0 && is_active()) {
				client_tick(stepper.pop_tick());
				ticked = true;
			}
		}
		if (!is_active()) {
			return;
		}
		if (transport->is_peer_connected(settings.authority_peer)) {
			if (ticked || ack_pending) {
				client_send_inputs();
				ack_pending = false;
			}
			const uint64_t ping_interval = clock.is_synchronized() ? uint64_t(settings.ping_interval * 1000000.0) : uint64_t(1000000.0 * get_tick_delta());
			if (last_ping_usec == 0 || now_usec - last_ping_usec >= ping_interval) {
				client_send_ping();
			}
		}
		if (!predicting) {
			// Events waiting for their object (E5), or expiring.
			run_events(TICK_FRAME_NONE);
		}
		client_update_interpolation();
	}

	// Sends what was queued during this process.
	transport->poll();
}

void TickSyncCore::handle_events() {
	TickTransport::Event event;
	while (is_active() && transport->pop_event(event)) {
		if (event.type == TickTransport::EVENT_HOST_MIGRATED) {
			handle_host_migrated(event.peer);
			continue;
		}
		if (role == ROLE_SERVER) {
			if (event.type == TickTransport::EVENT_PEER_CONNECTED) {
				if (!peers.has(event.peer)) {
					PeerState state;
					state.inputs.resize(settings.history_size);
					peers.insert(event.peer, state);
				}
			} else {
				const bool was_accepted = peers.has(event.peer) && peers[event.peer].accepted;
				peers.erase(event.peer);
				if (was_accepted && listener) {
					listener->on_peer_left(event.peer);
				}
			}
		} else if (event.peer == settings.authority_peer) {
			if (event.type == TickTransport::EVENT_PEER_CONNECTED) {
				TickDataBuffer hello;
				hello.begin_write();
				hello.add_uint_bits(TICK_MESSAGE_HELLO, 8);
				hello.add_uint_bits(TICK_PROTOCOL_VERSION, 16);
				hello.add_bool(sizeof(real_t) == sizeof(double));
				send(settings.authority_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, hello);
			} else {
				welcomed = false;
				predicting = false;
				for (KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
					E.value.object = nullptr;
				}
				remote_objects.clear();
				dolls.clear();
				predicted_ids_dirty = true;
				if (listener) {
					listener->on_peer_left(settings.authority_peer);
				}
			}
		} else if (event.type == TickTransport::EVENT_PEER_DISCONNECTED) {
			// Another peer of a mesh: its dolls are interpolated again.
			dolls.erase(event.peer);
			predicted_ids_dirty = true;
		}
	}
}

void TickSyncCore::handle_packet(const TickTransport::Packet &p_packet) {
	if (p_packet.data.is_empty()) {
		stats.malformed_packets++;
		return;
	}
	TickDataBuffer message(TickBitArray(p_packet.data.ptr(), int(p_packet.data.size())));
	message.begin_read();
	const int type = int(message.read_uint_bits(8));

	if (role == ROLE_SERVER) {
		switch (type) {
			case TICK_MESSAGE_HELLO:
				server_handle_hello(p_packet.from_peer, message);
				break;
			case TICK_MESSAGE_INPUTS:
				server_handle_inputs(p_packet.from_peer, message);
				break;
			case TICK_MESSAGE_PING:
				server_handle_ping(p_packet.from_peer, message);
				break;
			case TICK_MESSAGE_EVENT:
				server_handle_event(p_packet.from_peer, message);
				break;
			default:
				stats.malformed_packets++;
				break;
		}
		return;
	}

	if (type == TICK_MESSAGE_INPUTS) {
		// Inputs for dolls, from their controller (any peer of a mesh, or the server for its own objects).
		client_handle_doll_inputs(p_packet.from_peer, message);
		return;
	}
	// The client only trusts the server (H2): state never comes from another peer.
	if (p_packet.from_peer != settings.authority_peer) {
		stats.malformed_packets++;
		return;
	}
	switch (type) {
		case TICK_MESSAGE_WELCOME:
			client_handle_welcome(message);
			break;
		case TICK_MESSAGE_REJECT:
			client_handle_reject(message);
			break;
		case TICK_MESSAGE_REGISTER:
			client_handle_register(message);
			break;
		case TICK_MESSAGE_UNREGISTER:
			client_handle_unregister(message);
			break;
		case TICK_MESSAGE_SNAPSHOT_DELTA:
			client_handle_snapshot(message, false);
			break;
		case TICK_MESSAGE_SNAPSHOT_FULL:
			client_handle_snapshot(message, true);
			break;
		case TICK_MESSAGE_PONG:
			client_handle_pong(message);
			break;
		case TICK_MESSAGE_SPAWN:
			client_handle_spawn(message);
			break;
		case TICK_MESSAGE_DESPAWN:
			client_handle_despawn(message);
			break;
		case TICK_MESSAGE_EVENT:
			client_handle_event(message);
			break;
		case TICK_MESSAGE_RELEVANCE:
			client_handle_relevance(message);
			break;
		default:
			stats.malformed_packets++;
			break;
	}
}

// ------------------------------------------------------------------------------------------------------ Server

void TickSyncCore::server_handle_hello(int p_peer, TickDataBuffer &p_message) {
	PeerState *peer = peers.getptr(p_peer);
	if (peer == nullptr || peer->accepted || peer->rejected) {
		return;
	}
	const int version = int(p_message.read_uint_bits(16));
	const bool is_double = p_message.read_bool();
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		server_reject_peer(p_peer, "Malformed handshake.");
		return;
	}
	if (version != TICK_PROTOCOL_VERSION) {
		server_reject_peer(p_peer, vformat("Protocol version %d is incompatible with the server's version %d.", version, TICK_PROTOCOL_VERSION));
		return;
	}
	const bool server_is_double = sizeof(real_t) == sizeof(double);
	if (is_double != server_is_double) {
		server_reject_peer(p_peer, vformat("The client is a %s precision build, but the server is %s precision.", is_double ? "double" : "single", server_is_double ? "double" : "single"));
		return;
	}
	server_accept_peer(p_peer);
}

void TickSyncCore::server_accept_peer(int p_peer) {
	PeerState &peer = peers[p_peer];
	peer.accepted = true;
	peer.needs_full = true;

	TickDataBuffer welcome;
	welcome.begin_write();
	welcome.add_uint_bits(TICK_MESSAGE_WELCOME, 8);
	welcome.add_uint_bits(uint64_t(settings.ticks_per_second), 16);
	welcome.add_int_bits(server_compute_epoch(), 64);
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, welcome);

	// Late join (E4): the live spawns first, so the objects exist when their registration arrives.
	for (const uint32_t spawn_id : spawn_order) {
		server_send_spawn(p_peer, spawn_id);
	}
	// The interest filter is game code, which may register or remove objects: a copy of the ids.
	LocalVector<uint16_t> ids;
	ids = server_object_ids;
	for (const uint16_t net_id : ids) {
		if (server_objects.has(net_id)) {
			server_register_for_peer(p_peer, peer, net_id);
		}
	}
	if (listener) {
		listener->on_peer_ready(p_peer);
	}
}

void TickSyncCore::server_reject_peer(int p_peer, const String &p_reason) {
	PeerState &peer = peers[p_peer];
	peer.rejected = true;
	peer.reject_usec = now_usec;

	TickDataBuffer reject;
	reject.begin_write();
	reject.add_uint_bits(TICK_MESSAGE_REJECT, 8);
	reject.add_string(p_reason);
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, reject);
}

void TickSyncCore::server_send_register(int p_peer, uint16_t p_net_id) {
	const ServerObject &object = server_objects[p_net_id];
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_REGISTER, 8);
	message.add_uint_bits(p_net_id, 16);
	message.add_string(object.path);
	message.add_int_bits(object.controller, 32);
	message.add_uint_bits(object.schema_hash, 32);
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

void TickSyncCore::server_handle_inputs(int p_peer, TickDataBuffer &p_message) {
	PeerState *peer = peers.getptr(p_peer);
	if (peer == nullptr || !peer->accepted) {
		return;
	}
	if (!settings.trusted && !peer->input_limiter.take(settings.max_input_packets_per_second, now_usec)) {
		stats.rate_limited_packets++;
		return;
	}

	const uint32_t ack = uint32_t(p_message.read_uint_bits(32));
	const bool wants_full = p_message.read_bool();
	const int group_count = int(p_message.read_uint_bits(8));
	const uint32_t first_frame = uint32_t(p_message.read_uint_bits(32));
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}

	if (ack != TICK_FRAME_NONE && (peer->acked_snapshot == TICK_FRAME_NONE || tick_frame_after(ack, peer->acked_snapshot))) {
		peer->acked_snapshot = ack;
	}
	if (wants_full) {
		peer->needs_full = true;
	}

	const uint32_t current = stepper.get_next_frame_index();
	const uint32_t ring = uint32_t(settings.history_size);
	uint32_t index = 0;
	for (int group = 0; group < group_count; group++) {
		const int duplicates = int(p_message.read_uint_bits(8));
		TickDataBuffer input;
		p_message.read_data_buffer(input);
		if (p_message.is_buffer_failed()) {
			stats.malformed_packets++;
			return;
		}
		for (int copy = 0; copy <= duplicates; copy++) {
			const uint32_t frame = first_frame + index++;
			const bool is_new = peer->last_received_frame == TICK_FRAME_NONE || tick_frame_after(frame, peer->last_received_frame);
			if (tick_frame_after(current, frame)) {
				// Already simulated: the input arrived too late.
				if (is_new) {
					stats.late_inputs++;
					peer->last_received_frame = frame;
				}
				continue;
			}
			if (!tick_frame_after(current + ring, frame)) {
				// Too far in the future for the buffer.
				stats.rejected_inputs++;
				continue;
			}
			InputRecord &record = peer->inputs[frame % ring];
			if (record.frame == frame) {
				continue;
			}
			record.frame = frame;
			record.input = input;
			if (is_new) {
				peer->last_received_frame = frame;
			}
		}
	}
}

void TickSyncCore::server_handle_ping(int p_peer, TickDataBuffer &p_message) {
	const uint64_t client_time = p_message.read_uint_bits(64);
	if (p_message.is_buffer_failed() || !peers.has(p_peer)) {
		stats.malformed_packets++;
		return;
	}
	TickDataBuffer pong;
	pong.begin_write();
	pong.add_uint_bits(TICK_MESSAGE_PONG, 8);
	pong.add_uint_bits(client_time, 64);
	pong.add_uint_bits(now_usec, 64);
	pong.add_int_bits(server_compute_epoch(), 64);
	send(p_peer, TICK_CHANNEL_STATS, TickTransport::TRANSFER_MODE_UNRELIABLE_ORDERED, pong);
}

int64_t TickSyncCore::server_compute_epoch() const {
	// The epoch that matches the frame the server is really at, including the part of the next frame already
	// accumulated: the server's frames can drift from its start time (startup, hitches, another network's clock).
	const double elapsed_frames = double(stepper.get_next_frame_index()) + stepper.get_interpolation_fraction();
	return int64_t(now_usec) - int64_t(elapsed_frames * 1000000.0 * get_tick_delta());
}

void TickSyncCore::server_resolve_input(PeerState &r_peer, uint32_t p_frame) {
	r_peer.tick_inputs.clear();
	InputRecord &record = r_peer.inputs[p_frame % uint32_t(settings.history_size)];
	if (record.frame == p_frame) {
		r_peer.last_input = record.input;
		r_peer.has_last_input = true;
		record.frame = TICK_FRAME_NONE;
	} else if (r_peer.has_last_input) {
		// The input is missing: repeat the last one (ghost input), as the NetworkSynchronizer does.
		stats.ghost_inputs++;
	} else {
		return;
	}
	TickDataBuffer input = r_peer.last_input;
	input.begin_read();
	if (!parse_frame_input(input, r_peer.tick_inputs)) {
		stats.malformed_packets++;
	}
	if (!settings.trusted) {
		LocalVector<uint16_t> oversized;
		for (const KeyValue<uint16_t, TickDataBuffer> &E : r_peer.tick_inputs) {
			if (E.value.total_size() > settings.max_input_bits) {
				oversized.push_back(E.key);
			}
		}
		for (const uint16_t net_id : oversized) {
			r_peer.tick_inputs.erase(net_id);
			stats.rejected_inputs++;
		}
	}
}

void TickSyncCore::server_tick(uint32_t p_frame) {
	for (KeyValue<int, PeerState> &E : peers) {
		if (E.value.accepted) {
			server_resolve_input(E.value, p_frame);
		}
	}
	// Events scheduled for this frame run before the simulation.
	run_events(p_frame);

	const double delta = get_tick_delta();
	// The record is reused: every object's slot is overwritten (removed objects are erased in `unregister_object()`).
	SnapshotRecord &record = server_history[history_index(p_frame)];
	record.frame = p_frame;

	LocalVector<uint16_t> own_doll_ids;
	LocalVector<TickDataBuffer> own_doll_inputs;
	// The objects' code may register or remove objects (spawn a projectile, remove a node): the loop goes over a
	// copy of the ids and looks every object up again after running it. Objects registered during the tick are
	// simulated from the next one.
	LocalVector<uint16_t> ids;
	ids = server_object_ids;
	for (const uint16_t net_id : ids) {
		const ServerObject *object = server_objects.getptr(net_id);
		if (object == nullptr) {
			// Removed earlier in this tick.
			continue;
		}
		TickSyncObject *sync_object = object->object;
		TickDataBuffer input;
		input.begin_write();
		if (object->controller == settings.authority_peer) {
			sync_object->collect_input(input);
			if (sync_object->is_doll_enabled() && own_doll_ids.size() < 255) {
				own_doll_ids.push_back(net_id);
				own_doll_inputs.push_back(input);
			}
		} else {
			// Only the controller's own input moves the object (H1): inputs are looked up per sending peer.
			PeerState *peer = peers.getptr(object->controller);
			if (peer && peer->accepted) {
				TickDataBuffer *peer_input = peer->tick_inputs.getptr(net_id);
				if (peer_input) {
					input = *peer_input;
				}
			}
		}
		input.begin_read();
		sync_object->process_tick(delta, input);
		object = server_objects.getptr(net_id);
		if (object == nullptr || object->object != sync_object) {
			// Removed by its own code.
			continue;
		}
		quantize_object(sync_object);
		LocalVector<Variant> *values = record.states.getptr(net_id);
		if (values == nullptr) {
			values = &record.states.insert(net_id, LocalVector<Variant>())->value;
		}
		read_states(sync_object, *values);
	}

	if (p_frame % uint32_t(settings.interest_interval) == 0) {
		server_update_relevance();
	}

	InputRecord &own_inputs = authority_inputs[history_index(p_frame)];
	if (own_doll_ids.is_empty()) {
		own_inputs.frame = TICK_FRAME_NONE;
	} else {
		own_inputs.frame = p_frame;
		own_inputs.input.begin_write();
		own_inputs.input.add_uint_bits(own_doll_ids.size(), 8);
		for (uint32_t i = 0; i < own_doll_ids.size(); i++) {
			own_inputs.input.add_uint_bits(own_doll_ids[i], 16);
			own_inputs.input.add_data_buffer(own_doll_inputs[i]);
		}
		server_send_own_inputs(p_frame);
	}

	if (p_frame % uint32_t(settings.snapshot_interval) == 0) {
		for (KeyValue<int, PeerState> &E : peers) {
			if (E.value.accepted) {
				server_send_snapshot(E.key, E.value, p_frame);
			}
		}
	}
}

void TickSyncCore::server_send_own_inputs(uint32_t p_frame) {
	// The same format as a client's inputs, with the last `input_redundancy` frames.
	uint32_t first_frame = p_frame;
	for (int i = 1; i < settings.input_redundancy; i++) {
		const uint32_t frame = p_frame - uint32_t(i);
		// `TICK_FRAME_NONE` (before frame 0) would match the empty records.
		if (frame == TICK_FRAME_NONE || authority_inputs[history_index(frame)].frame != frame) {
			break;
		}
		first_frame = frame;
	}
	LocalVector<const TickDataBuffer *> frames;
	for (uint32_t frame = first_frame; frame != p_frame + 1; frame++) {
		frames.push_back(&authority_inputs[history_index(frame)].input);
	}
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_INPUTS, 8);
	message.add_uint_bits(TICK_FRAME_NONE, 32);
	message.add_bool(false);
	write_input_groups(message, first_frame, frames);
	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.accepted) {
			send(E.key, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, message);
		}
	}
}

void TickSyncCore::write_object_state(TickDataBuffer &r_message, const ServerObject &p_object, const LocalVector<Variant> &p_values, const LocalVector<Variant> *p_base) const {
	const TickSchema &schema = p_object.object->get_sync_schema();
	TickDataBuffer &payload = scratch_payload;
	payload.begin_write();
	for (int i = 0; i < schema.size() && i < int(p_values.size()); i++) {
		// Exact comparison: both sides hold the same quantized values.
		const bool changed = p_base == nullptr || i >= int(p_base->size()) || !(p_values[i] == (*p_base)[i]);
		payload.add_bool(changed);
		if (changed) {
			schema.codecs[i]->encode(p_values[i], payload);
		}
	}
	r_message.add_data_buffer(payload);
}

void TickSyncCore::server_send_snapshot(int p_peer, PeerState &r_peer, uint32_t p_frame) {
	const SnapshotRecord &record = server_history[history_index(p_frame)];

	const bool acked_available = r_peer.acked_snapshot != TICK_FRAME_NONE && server_history[history_index(r_peer.acked_snapshot)].frame == r_peer.acked_snapshot;
	const bool full_available = r_peer.last_full_frame != TICK_FRAME_NONE && server_history[history_index(r_peer.last_full_frame)].frame == r_peer.last_full_frame;
	const bool full_recent = r_peer.last_full_frame != TICK_FRAME_NONE && now_usec - r_peer.last_full_usec < FULL_SNAPSHOT_RESEND_USEC;

	uint32_t base = TICK_FRAME_NONE;
	bool full = false;
	if (r_peer.needs_full && !full_recent) {
		full = true;
	} else if (acked_available && (r_peer.last_full_frame == TICK_FRAME_NONE || !tick_frame_after(r_peer.last_full_frame, r_peer.acked_snapshot))) {
		base = r_peer.acked_snapshot;
	} else if (full_available && full_recent) {
		// The last full snapshot may still be on its way: the client decodes the deltas once it arrives.
		base = r_peer.last_full_frame;
	} else if (acked_available) {
		base = r_peer.acked_snapshot;
	} else {
		full = true;
	}

	const SnapshotRecord *base_record = base == TICK_FRAME_NONE ? nullptr : &server_history[history_index(base)];
	int server_buffer = 0;
	if (r_peer.last_received_frame != TICK_FRAME_NONE) {
		server_buffer = CLAMP(int(int32_t(r_peer.last_received_frame - p_frame)), -127, 127);
	}

	// A delta bigger than a datagram is split between objects, and each part is decoded on its own. Full snapshots are
	// reliable, and ENet fragments them. Entries are written straight into the parts: a nested buffer is aligned to
	// the bytes of the message, so an entry can't be moved to another offset.
	static constexpr int PART_FIELDS_OFFSET = 8 + 32 + 32 + 8;
	static constexpr int MAX_PARTS = 64;
	const int limit_bits = (transport->get_max_payload_size() - 1) * 8;
	LocalVector<TickDataBuffer> parts;
	LocalVector<int> counts;
	for (const uint16_t net_id : server_object_ids) {
		if (!r_peer.relevant.has(net_id)) {
			continue;
		}
		const LocalVector<Variant> *values = record.states.getptr(net_id);
		const LocalVector<Variant> *base_values = base_record ? base_record->states.getptr(net_id) : nullptr;
		const uint32_t *since = r_peer.relevant_since.getptr(net_id);
		if (since) {
			if (r_peer.acked_snapshot != TICK_FRAME_NONE && !tick_frame_after(*since, r_peer.acked_snapshot)) {
				r_peer.relevant_since.erase(net_id);
			} else if (base == TICK_FRAME_NONE || tick_frame_after(*since, base)) {
				// Relevant again after the base was sent: the client doesn't have it in the base.
				base_values = nullptr;
			}
		}
		for (int attempt = 0; attempt < 2; attempt++) {
			if (parts.is_empty() || attempt == 1) {
				parts.push_back(TickDataBuffer());
				counts.push_back(0);
				TickDataBuffer &message = parts[parts.size() - 1];
				message.begin_write();
				message.add_uint_bits(full ? TICK_MESSAGE_SNAPSHOT_FULL : TICK_MESSAGE_SNAPSHOT_DELTA, 8);
				message.add_uint_bits(p_frame, 32);
				message.add_uint_bits(base, 32);
				message.add_int_bits(server_buffer, 8);
				// Part, part count and object count, written once known.
				message.add_uint_bits(0, 8 + 8 + 16);
			}
			TickDataBuffer &message = parts[parts.size() - 1];
			const int before = message.total_size();
			write_snapshot_entry(message, net_id, values, base_values);
			if (full || attempt == 1 || message.total_size() <= limit_bits || counts[counts.size() - 1] == 0 || parts.size() >= MAX_PARTS) {
				counts[counts.size() - 1]++;
				break;
			}
			// Too big: the entry goes to a new part.
			message.shrink_to(0, before);
			message.seek(before);
		}
	}
	if (parts.is_empty()) {
		parts.push_back(TickDataBuffer());
		counts.push_back(0);
		TickDataBuffer &message = parts[0];
		message.begin_write();
		message.add_uint_bits(full ? TICK_MESSAGE_SNAPSHOT_FULL : TICK_MESSAGE_SNAPSHOT_DELTA, 8);
		message.add_uint_bits(p_frame, 32);
		message.add_uint_bits(base, 32);
		message.add_int_bits(server_buffer, 8);
		message.add_uint_bits(0, 8 + 8 + 16);
	}
	for (uint32_t part = 0; part < parts.size(); part++) {
		TickDataBuffer &message = parts[part];
		const int end = message.total_size();
		message.seek(PART_FIELDS_OFFSET);
		message.add_uint_bits(part, 8);
		message.add_uint_bits(parts.size(), 8);
		message.add_uint_bits(uint64_t(counts[part]), 16);
		message.seek(end);
		if (full) {
			send(p_peer, TICK_CHANNEL_SNAPSHOT, TickTransport::TRANSFER_MODE_RELIABLE, message);
		} else {
			send(p_peer, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_UNRELIABLE, message);
		}
	}

	if (full) {
		r_peer.needs_full = false;
		r_peer.last_full_frame = p_frame;
		r_peer.last_full_usec = now_usec;
		stats.full_snapshots_sent++;
	} else {
		stats.delta_snapshots_sent++;
		stats.split_snapshots += parts.size() > 1 ? 1 : 0;
	}
}

void TickSyncCore::write_snapshot_entry(TickDataBuffer &r_message, uint16_t p_net_id, const LocalVector<Variant> *p_values, const LocalVector<Variant> *p_base) const {
	r_message.add_uint_bits(p_net_id, 16);
	if (p_values == nullptr) {
		// Registered after this frame was simulated.
		r_message.add_bool(false);
		return;
	}
	bool changed = p_base == nullptr || p_base->size() != p_values->size();
	for (uint32_t i = 0; !changed && i < p_values->size(); i++) {
		changed = !((*p_values)[i] == (*p_base)[i]);
	}
	r_message.add_bool(changed);
	if (changed) {
		write_object_state(r_message, server_objects[p_net_id], *p_values, p_base);
	}
}

void TickSyncCore::server_register_for_peer(int p_peer, PeerState &r_peer, uint16_t p_net_id) {
	const ServerObject *object = server_objects.getptr(p_net_id);
	ERR_FAIL_NULL(object);
	server_send_register(p_peer, p_net_id);
	const int controller = object->controller;
	bool relevant = settings.default_relevant;
	// The filter is game code, which may register or remove objects: `object` isn't used after it.
	const int verdict = listener ? listener->filter_relevance(p_peer, object->object) : -1;
	if (verdict >= 0) {
		relevant = verdict == 1;
	}
	if (!server_objects.has(p_net_id)) {
		return;
	}
	if (relevant || controller == p_peer) {
		r_peer.relevant.insert(p_net_id);
		return;
	}
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_RELEVANCE, 8);
	message.add_uint_bits(p_net_id, 16);
	message.add_bool(false);
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

void TickSyncCore::server_set_peer_relevant(int p_peer, PeerState &r_peer, uint16_t p_net_id, bool p_relevant) {
	const ServerObject *object = server_objects.getptr(p_net_id);
	if (object == nullptr) {
		return;
	}
	// A client always gets the objects it controls.
	const bool relevant = p_relevant || object->controller == p_peer;
	if (relevant == r_peer.relevant.has(p_net_id)) {
		return;
	}
	if (relevant) {
		r_peer.relevant.insert(p_net_id);
		r_peer.relevant_since.insert(p_net_id, stepper.get_next_frame_index());
	} else {
		r_peer.relevant.erase(p_net_id);
		r_peer.relevant_since.erase(p_net_id);
	}
	stats.relevance_changes++;
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_RELEVANCE, 8);
	message.add_uint_bits(p_net_id, 16);
	message.add_bool(relevant);
	send(p_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
}

void TickSyncCore::server_update_relevance() {
	if (listener == nullptr) {
		return;
	}
	// The filter is game code, which may register or remove objects: a copy of the ids.
	LocalVector<uint16_t> ids;
	ids = server_object_ids;
	for (KeyValue<int, PeerState> &E : peers) {
		if (!E.value.accepted) {
			continue;
		}
		for (const uint16_t net_id : ids) {
			const ServerObject *object = server_objects.getptr(net_id);
			if (object == nullptr) {
				continue;
			}
			const int verdict = listener->filter_relevance(E.key, object->object);
			if (verdict >= 0) {
				server_set_peer_relevant(E.key, E.value, net_id, verdict == 1);
			}
		}
	}
}

Error TickSyncCore::set_relevant(TickSyncObject *p_object, int p_peer, bool p_relevant) {
	ERR_FAIL_COND_V_MSG(role != ROLE_SERVER, ERR_UNAVAILABLE, "Only the server decides what's relevant to each client.");
	const uint16_t *net_id = server_ids_by_object.getptr(p_object);
	ERR_FAIL_NULL_V_MSG(net_id, ERR_INVALID_PARAMETER, "The object isn't synchronized.");
	if (p_peer == TickTransport::PEER_BROADCAST) {
		for (KeyValue<int, PeerState> &E : peers) {
			if (E.value.accepted) {
				server_set_peer_relevant(E.key, E.value, *net_id, p_relevant);
			}
		}
		return OK;
	}
	PeerState *peer = peers.getptr(p_peer);
	ERR_FAIL_COND_V_MSG(peer == nullptr || !peer->accepted, ERR_INVALID_PARAMETER, vformat("Peer %d isn't connected.", p_peer));
	server_set_peer_relevant(p_peer, *peer, *net_id, p_relevant);
	return OK;
}

bool TickSyncCore::is_relevant(const TickSyncObject *p_object, int p_peer) const {
	if (role == ROLE_SERVER) {
		const uint16_t *net_id = server_ids_by_object.getptr(const_cast<TickSyncObject *>(p_object));
		const PeerState *peer = peers.getptr(p_peer);
		return net_id && peer && peer->relevant.has(*net_id);
	}
	// Client: whether the server sends it to this client.
	for (const KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
		if (E.value.object == p_object) {
			return E.value.relevant;
		}
	}
	return false;
}

// ------------------------------------------------------------------------------------------------------ Client

void TickSyncCore::client_handle_welcome(TickDataBuffer &p_message) {
	const int ticks_per_second = int(p_message.read_uint_bits(16));
	const int64_t epoch = p_message.read_int_bits(64);
	if (p_message.is_buffer_failed() || ticks_per_second <= 0) {
		stats.malformed_packets++;
		return;
	}
	settings.ticks_per_second = ticks_per_second;
	stepper.set_ticks_per_second(ticks_per_second);
	clock.set_ticks_per_second(ticks_per_second);
	clock.set_master_epoch_usec(epoch);
	welcomed = true;
	if (listener) {
		listener->on_peer_ready(settings.authority_peer);
	}
}

void TickSyncCore::client_handle_reject(TickDataBuffer &p_message) {
	const String reason = p_message.read_string();
	rejected = true;
	ERR_PRINT(vformat("The server refused the connection: %s", reason));
	if (listener) {
		listener->on_rejected(reason);
	}
}

void TickSyncCore::client_handle_register(TickDataBuffer &p_message) {
	const uint16_t net_id = uint16_t(p_message.read_uint_bits(16));
	RemoteObject remote;
	remote.path = p_message.read_string();
	remote.controller = int(p_message.read_int_bits(32));
	remote.schema_hash = uint32_t(p_message.read_uint_bits(32));
	if (p_message.is_buffer_failed() || net_id == 0) {
		stats.malformed_packets++;
		return;
	}
	remote_objects.insert(net_id, remote);
	predicted_ids_dirty = true;
	TickSyncObject **local = local_objects.getptr(remote.path);
	if (local) {
		client_bind(net_id, remote_objects[net_id]);
	}
}

void TickSyncCore::client_handle_unregister(TickDataBuffer &p_message) {
	const uint16_t net_id = uint16_t(p_message.read_uint_bits(16));
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	remote_objects.erase(net_id);
	predicted_ids_dirty = true;
}

void TickSyncCore::client_bind(uint16_t p_net_id, RemoteObject &r_remote) {
	TickSyncObject **local = local_objects.getptr(r_remote.path);
	ERR_FAIL_NULL(local);
	const uint32_t local_hash = (*local)->get_sync_schema().hash();
	if (local_hash != r_remote.schema_hash) {
		ERR_PRINT(vformat("The object \"%s\" declares different variables or codecs than the server's; it's not synchronized.", r_remote.path));
		return;
	}
	TickSyncObject *object = *local;
	r_remote.object = object;
	predicted_ids_dirty = true;
	// Its state may be unknown in the current snapshot: ask for a full one.
	needs_full = true;
	if (!r_remote.relevant && listener) {
		listener->on_relevance_changed(object, false);
	}
	// Events that arrived before the object (E5).
	run_events(predicting ? stepper.get_next_frame_index() - 1 : TICK_FRAME_NONE);
	// The game's code above may have removed the object again.
	if (r_remote.object != object) {
		return;
	}
	if (predicting && r_remote.controller == transport->get_local_peer_id() && latest_snapshot != TICK_FRAME_NONE) {
		// A predicted object joining late starts from the server's state.
		SnapshotRecord *record = client_get_received(latest_snapshot);
		const LocalVector<Variant> *values = record ? record->states.getptr(p_net_id) : nullptr;
		if (values) {
			for (uint32_t i = 0; i < values->size(); i++) {
				object->set_sync_var(int(i), (*values)[i]);
			}
		}
	}
}

void TickSyncCore::client_update_predicted_ids() {
	if (!predicted_ids_dirty) {
		return;
	}
	predicted_ids_dirty = false;
	predicted_ids.clear();
	shares_inputs = false;
	for (KeyValue<int, DollPeer> &E : dolls) {
		E.value.ids.clear();
	}
	const int local_peer = transport.is_valid() ? transport->get_local_peer_id() : 0;
	for (const KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
		if (E.value.object == nullptr) {
			continue;
		}
		if (E.value.controller == local_peer) {
			predicted_ids.push_back(E.key);
			shares_inputs = shares_inputs || E.value.object->is_doll_enabled();
		} else if (E.value.object->is_doll_enabled() && E.value.relevant) {
			client_get_doll(E.value.controller).ids.push_back(E.key);
		}
	}
	predicted_ids.sort();
	for (KeyValue<int, DollPeer> &E : dolls) {
		E.value.ids.sort();
		if (E.value.ids.is_empty()) {
			E.value.started = false;
		}
	}
}

bool TickSyncCore::client_is_predicted(uint16_t p_net_id) const {
	if (!predicting) {
		return false;
	}
	for (const uint16_t id : predicted_ids) {
		if (id == p_net_id) {
			return true;
		}
	}
	return false;
}

const TickSyncCore::SnapshotRecord *TickSyncCore::client_get_received(uint32_t p_frame) const {
	if (p_frame == TICK_FRAME_NONE || received.is_empty()) {
		return nullptr;
	}
	const SnapshotRecord &record = received[history_index(p_frame)];
	return record.frame == p_frame ? &record : nullptr;
}

const LocalVector<Variant> *TickSyncCore::find_state(uint16_t p_net_id, uint32_t p_frame) const {
	if (p_frame == TICK_FRAME_NONE) {
		return nullptr;
	}
	if (role == ROLE_SERVER) {
		const SnapshotRecord &record = server_history[history_index(p_frame)];
		return record.frame == p_frame ? record.states.getptr(p_net_id) : nullptr;
	}
	// Client: the predicted objects' own history, the dolls' one, or the authority's snapshots.
	if (client_is_predicted(p_net_id)) {
		const PredictionRecord &record = predictions[history_index(p_frame)];
		return record.frame == p_frame ? record.states.getptr(p_net_id) : nullptr;
	}
	if (client_is_active_doll(p_net_id)) {
		const DollPeer *doll = dolls.getptr(remote_objects.getptr(p_net_id)->controller);
		const DollRecord &record = doll->history[history_index(p_frame)];
		return record.frame == p_frame ? record.states.getptr(p_net_id) : nullptr;
	}
	const SnapshotRecord *record = client_get_received(p_frame);
	return record ? record->states.getptr(p_net_id) : nullptr;
}

bool TickSyncCore::get_state_at(const TickSyncObject *p_object, double p_frame, LocalVector<Variant> &r_values) const {
	if (role == ROLE_NONE || !(p_frame >= 0.0)) {
		return false;
	}
	const uint16_t net_id = get_net_id(p_object);
	if (net_id == 0) {
		return false;
	}
	// The nearest known states around the frame (a client only has the snapshots it received).
	static constexpr uint32_t MAX_GAP = 16;
	const uint32_t frame = uint32_t(Math::floor(p_frame));
	const LocalVector<Variant> *past = nullptr;
	const LocalVector<Variant> *future = nullptr;
	uint32_t past_frame = frame;
	uint32_t future_frame = frame + 1;
	for (uint32_t back = 0; back <= MAX_GAP && past == nullptr; back++) {
		past_frame = frame - back;
		past = find_state(net_id, past_frame);
	}
	for (uint32_t ahead = 1; ahead <= MAX_GAP && future == nullptr; ahead++) {
		future_frame = frame + ahead;
		future = find_state(net_id, future_frame);
	}
	if (past == nullptr) {
		return false;
	}
	if (future == nullptr || future->size() != past->size() || p_frame <= double(past_frame)) {
		r_values = *past;
		return true;
	}
	const double weight = CLAMP((p_frame - double(past_frame)) / double(future_frame - past_frame), 0.0, 1.0);
	const TickSchema &schema = p_object->get_sync_schema();
	r_values.resize(past->size());
	for (uint32_t i = 0; i < past->size(); i++) {
		r_values[i] = int(i) < schema.size() ? schema.codecs[i]->interpolate((*past)[i], (*future)[i], weight) : (*past)[i];
	}
	return true;
}

double TickSyncCore::get_view_frame(uint64_t p_now_usec) const {
	if (role == ROLE_CLIENT) {
		if (latest_snapshot == TICK_FRAME_NONE) {
			return -1.0;
		}
		if (settings.interpolate_remote && clock.is_synchronized() && welcomed) {
			return clock.get_master_frame_time(p_now_usec) - settings.interpolation_delay * double(settings.ticks_per_second);
		}
		return double(latest_snapshot);
	}
	return get_timeline_frame(p_now_usec);
}

uint32_t TickSyncCore::client_find_snapshot_before(uint32_t p_frame) const {
	uint32_t found = TICK_FRAME_NONE;
	for (const SnapshotRecord &record : received) {
		if (record.frame == TICK_FRAME_NONE || !tick_frame_after(p_frame, record.frame)) {
			continue;
		}
		if (found == TICK_FRAME_NONE || tick_frame_after(record.frame, found)) {
			found = record.frame;
		}
	}
	return found;
}

TickSyncCore::SnapshotRecord *TickSyncCore::client_get_received(uint32_t p_frame) {
	if (p_frame == TICK_FRAME_NONE || received.is_empty()) {
		return nullptr;
	}
	SnapshotRecord &record = received[history_index(p_frame)];
	return record.frame == p_frame ? &record : nullptr;
}

void TickSyncCore::client_handle_snapshot(TickDataBuffer &p_message, bool p_full) {
	const uint32_t frame = uint32_t(p_message.read_uint_bits(32));
	const uint32_t base = uint32_t(p_message.read_uint_bits(32));
	const int server_buffer = int(p_message.read_int_bits(8));
	const int part = int(p_message.read_uint_bits(8));
	const int part_count = int(p_message.read_uint_bits(8));
	const int count = int(p_message.read_uint_bits(16));
	if (p_message.is_buffer_failed() || frame == TICK_FRAME_NONE || part_count < 1 || part_count > 64 || part >= part_count) {
		stats.malformed_packets++;
		return;
	}
	if (latest_snapshot != TICK_FRAME_NONE && !tick_frame_after(frame, latest_snapshot)) {
		// Older than what's already known.
		stats.snapshots_dropped++;
		return;
	}
	const SnapshotRecord *base_record = nullptr;
	if (base != TICK_FRAME_NONE) {
		base_record = client_get_received(base);
		if (base_record == nullptr) {
			// The base isn't known (yet): can't decode.
			stats.snapshots_dropped++;
			ack_pending = true;
			return;
		}
	}

	// Decoded straight into the record the frame will take in `received` (the oldest one), which is only valid once
	// every part arrived. A newer frame drops an incomplete one.
	SnapshotRecord &record = received[history_index(frame)];
	if (base_record == &record) {
		// The base is a whole history old: ask for a full snapshot.
		stats.snapshots_dropped++;
		needs_full = true;
		return;
	}
	if (pending_snapshot.frame != frame) {
		if (pending_snapshot.frame != TICK_FRAME_NONE) {
			if (!tick_frame_after(frame, pending_snapshot.frame)) {
				stats.snapshots_dropped++;
				return;
			}
			stats.incomplete_snapshots++;
		}
		pending_snapshot = PendingSnapshot();
		pending_snapshot.frame = frame;
		pending_snapshot.part_count = part_count;
		pending_snapshot.server_buffer = server_buffer;
		written_ids.clear();
		record.frame = TICK_FRAME_NONE;
	}
	const uint64_t part_bit = uint64_t(1) << part;
	if (pending_snapshot.part_count != part_count || (pending_snapshot.parts_received & part_bit)) {
		return;
	}
	if (!client_read_snapshot_objects(p_message, count, base_record, record.states, pending_snapshot.missing_state)) {
		stats.malformed_packets++;
		pending_snapshot = PendingSnapshot();
		return;
	}
	pending_snapshot.parts_received |= part_bit;
	if (pending_snapshot.parts_received != (part_count == 64 ? ~uint64_t(0) : (uint64_t(1) << part_count) - 1)) {
		return;
	}

	// Complete: the objects the snapshot didn't have are removed from the reused record.
	if (record.states.size() != written_ids.size()) {
		written_ids.sort();
		LocalVector<uint16_t> stale;
		for (const KeyValue<uint16_t, LocalVector<Variant>> &E : record.states) {
			if (!sorted_contains(written_ids, E.key)) {
				stale.push_back(E.key);
			}
		}
		for (const uint16_t net_id : stale) {
			record.states.erase(net_id);
		}
	}
	record.frame = frame;
	record.complete = !pending_snapshot.missing_state;
	const PendingSnapshot complete = pending_snapshot;
	pending_snapshot = PendingSnapshot();
	client_finish_snapshot(frame, p_full, complete.server_buffer, complete.missing_state);
}

bool TickSyncCore::client_read_snapshot_objects(TickDataBuffer &p_message, int p_count, const SnapshotRecord *p_base, ObjectStates &r_states, bool &r_missing_state) {
	for (int i = 0; i < p_count; i++) {
		const uint16_t net_id = uint16_t(p_message.read_uint_bits(16));
		const bool changed = p_message.read_bool();
		if (p_message.is_buffer_failed()) {
			return false;
		}
		const RemoteObject *remote = remote_objects.getptr(net_id);
		const bool bound = remote && remote->object;
		const LocalVector<Variant> *base_values = p_base ? p_base->states.getptr(net_id) : nullptr;

		if (!changed) {
			if (base_values) {
				LocalVector<Variant> *values = r_states.getptr(net_id);
				if (values == nullptr) {
					values = &r_states.insert(net_id, LocalVector<Variant>())->value;
				}
				*values = *base_values;
				written_ids.push_back(net_id);
			} else if (bound) {
				r_missing_state = true;
			}
			continue;
		}

		p_message.read_data_buffer(read_payload);
		if (p_message.is_buffer_failed()) {
			return false;
		}
		if (!bound) {
			continue;
		}

		const TickSchema &schema = remote->object->get_sync_schema();
		LocalVector<Variant> *values = r_states.getptr(net_id);
		if (values == nullptr) {
			values = &r_states.insert(net_id, LocalVector<Variant>())->value;
		}
		values->resize(schema.size());
		bool complete = true;
		for (int v = 0; v < schema.size(); v++) {
			if (read_payload.read_bool()) {
				(*values)[v] = schema.codecs[v]->decode(read_payload);
			} else if (base_values && v < int(base_values->size())) {
				(*values)[v] = (*base_values)[v];
			} else {
				complete = false;
			}
		}
		if (complete && !read_payload.is_buffer_failed()) {
			written_ids.push_back(net_id);
		} else {
			r_states.erase(net_id);
			r_missing_state = true;
		}
	}
	return !p_message.is_buffer_failed();
}

void TickSyncCore::client_finish_snapshot(uint32_t p_frame, bool p_full, int p_server_buffer, bool p_missing_state) {
	if (p_full && !p_missing_state) {
		needs_full = false;
	} else if (p_missing_state) {
		needs_full = true;
	}

	latest_snapshot = p_frame;
	stats.snapshots_received++;
	ack_pending = true;

	client_adjust_speed(p_server_buffer);
	client_reconcile(p_frame);
	client_reconcile_dolls(p_frame);
}

void TickSyncCore::client_handle_relevance(TickDataBuffer &p_message) {
	const uint16_t net_id = uint16_t(p_message.read_uint_bits(16));
	const bool relevant = p_message.read_bool();
	RemoteObject *remote = remote_objects.getptr(net_id);
	if (p_message.is_buffer_failed() || remote == nullptr) {
		stats.malformed_packets++;
		return;
	}
	if (remote->relevant == relevant) {
		return;
	}
	remote->relevant = relevant;
	predicted_ids_dirty = true;
	stats.relevance_changes++;
	if (remote->object && listener) {
		listener->on_relevance_changed(remote->object, relevant);
	}
}

void TickSyncCore::client_handle_pong(TickDataBuffer &p_message) {
	const uint64_t client_time = p_message.read_uint_bits(64);
	const uint64_t server_time = p_message.read_uint_bits(64);
	const int64_t epoch = p_message.read_int_bits(64);
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	clock.add_sample(client_time, server_time, now_usec);
	clock.set_master_epoch_usec(epoch);
}

void TickSyncCore::client_send_ping() {
	last_ping_usec = now_usec;
	TickDataBuffer ping;
	ping.begin_write();
	ping.add_uint_bits(TICK_MESSAGE_PING, 8);
	ping.add_uint_bits(now_usec, 64);
	send(settings.authority_peer, TICK_CHANNEL_STATS, TickTransport::TRANSFER_MODE_UNRELIABLE_ORDERED, ping);
}

uint32_t TickSyncCore::client_compute_start_frame() const {
	const double tick_usec = 1000000.0 * get_tick_delta();
	const double jitter_frames = double(clock.get_rtt_spread_usec()) / tick_usec;
	const int target = CLAMP(settings.min_input_buffer + int(Math::ceil(jitter_frames)), settings.min_input_buffer, settings.max_input_buffer);
	const int travel = int(Math::ceil(double(clock.get_rtt_usec()) * 0.5 / tick_usec));
	return clock.get_master_frame(now_usec) + uint32_t(travel + target + 1);
}

void TickSyncCore::client_start_prediction() {
	client_update_predicted_ids();
	predicting = true;
	stepper.reset();
	stepper.set_time_scale(1.0);
	const uint32_t start_frame = client_compute_start_frame();
	stepper.set_next_frame_index(start_frame);
	for (PredictionRecord &record : predictions) {
		record.frame = TICK_FRAME_NONE;
	}
	last_reconciled = TICK_FRAME_NONE;
	client_reset_dolls();

	// The predicted objects start from the server's latest state.
	SnapshotRecord *record = client_get_received(latest_snapshot);
	if (record) {
		for (const uint16_t net_id : predicted_ids) {
			const LocalVector<Variant> *values = record->states.getptr(net_id);
			const RemoteObject *remote = remote_objects.getptr(net_id);
			// The objects' setters are game code: one of them may have removed a later object.
			TickSyncObject *object = remote ? remote->object : nullptr;
			if (values && object) {
				for (uint32_t i = 0; i < values->size(); i++) {
					object->set_sync_var(int(i), (*values)[i]);
				}
			}
		}
	}
	if (listener) {
		listener->on_prediction_started(start_frame);
	}
}

void TickSyncCore::client_adjust_speed(int p_server_buffer) {
	if (!predicting) {
		return;
	}
	const double tick_usec = 1000000.0 * get_tick_delta();
	const double jitter_frames = double(clock.get_rtt_spread_usec()) / tick_usec;
	const int target = CLAMP(settings.min_input_buffer + int(Math::ceil(jitter_frames)), settings.min_input_buffer, settings.max_input_buffer);
	const int error = target - p_server_buffer;

	const int resync_threshold = MAX(20, settings.ticks_per_second / 2);
	if (Math::abs(error) > resync_threshold) {
		// Too far off to recover by speeding up: jump to the right frame.
		stepper.set_time_scale(1.0);
		stepper.set_next_frame_index(client_compute_start_frame());
		for (PredictionRecord &record : predictions) {
			record.frame = TICK_FRAME_NONE;
		}
		// The local timeline jumped: the dolls' delays are measured again.
		client_reset_dolls();
		return;
	}
	const double delta = CLAMP(double(error) * settings.time_scale_gain, -settings.max_time_scale_delta, settings.max_time_scale_delta);
	stepper.set_time_scale(1.0 + delta);
}

void TickSyncCore::client_reconcile(uint32_t p_frame) {
	// The game may have removed a predicted object since the last tick.
	client_update_predicted_ids();
	if (!predicting || predicted_ids.is_empty()) {
		return;
	}
	if (last_reconciled != TICK_FRAME_NONE && !tick_frame_after(p_frame, last_reconciled)) {
		return;
	}
	const uint32_t next_frame = stepper.get_next_frame_index();
	if (!tick_frame_after(next_frame, p_frame)) {
		// This frame wasn't predicted yet.
		return;
	}
	PredictionRecord &prediction = predictions[history_index(p_frame)];
	SnapshotRecord *snapshot = client_get_received(p_frame);
	if (prediction.frame != p_frame || snapshot == nullptr) {
		return;
	}
	last_reconciled = p_frame;

	bool diverged = false;
	for (const uint16_t net_id : predicted_ids) {
		const LocalVector<Variant> *server_values = snapshot->states.getptr(net_id);
		if (server_values == nullptr) {
			continue;
		}
		const LocalVector<Variant> *local_values = prediction.states.getptr(net_id);
		const TickSchema &schema = remote_objects[net_id].object->get_sync_schema();
		if (local_values == nullptr || local_values->size() != server_values->size()) {
			diverged = true;
			break;
		}
		for (uint32_t i = 0; i < server_values->size() && int(i) < schema.size(); i++) {
			if (!schema.codecs[i]->is_equal((*local_values)[i], (*server_values)[i])) {
				diverged = true;
				break;
			}
		}
		if (diverged) {
			break;
		}
	}
	if (!diverged) {
		return;
	}

	// Rewind: apply the server's state at `p_frame`, then simulate again the frames predicted after it. The objects'
	// code runs here and may remove objects: each one is looked up again before it's used.
	rewinding = true;
	for (const uint16_t net_id : predicted_ids) {
		const LocalVector<Variant> *server_values = snapshot->states.getptr(net_id);
		const RemoteObject *remote = remote_objects.getptr(net_id);
		TickSyncObject *object = remote ? remote->object : nullptr;
		if (server_values == nullptr || object == nullptr) {
			continue;
		}
		for (uint32_t i = 0; i < server_values->size(); i++) {
			object->set_sync_var(int(i), (*server_values)[i]);
		}
		prediction.states.insert(net_id, *server_values);
	}

	const double delta = get_tick_delta();
	int frame_count = 0;
	HashMap<uint16_t, TickDataBuffer> inputs;
	for (uint32_t frame = p_frame + 1; tick_frame_after(next_frame, frame); frame++) {
		PredictionRecord &record = predictions[history_index(frame)];
		if (record.frame != frame) {
			break;
		}
		TickDataBuffer frame_input = record.input;
		frame_input.begin_read();
		parse_frame_input(frame_input, inputs);
		for (const uint16_t net_id : predicted_ids) {
			const RemoteObject *remote = remote_objects.getptr(net_id);
			TickSyncObject *object = remote ? remote->object : nullptr;
			if (object == nullptr) {
				continue;
			}
			TickDataBuffer input;
			TickDataBuffer *stored = inputs.getptr(net_id);
			if (stored) {
				input = *stored;
			} else {
				input.begin_write();
			}
			input.begin_read();
			object->process_tick(delta, input);
			if (remote->object != object) {
				// Removed by its own code.
				continue;
			}
			quantize_object(object);
			LocalVector<Variant> values;
			read_states(object, values);
			record.states.insert(net_id, values);
		}
		frame_count++;
	}
	rewinding = false;

	stats.rewinds++;
	stats.rewound_frames += uint64_t(frame_count);
	if (listener) {
		listener->on_rewound(p_frame, frame_count);
	}
}

void TickSyncCore::client_tick(uint32_t p_frame) {
	client_update_predicted_ids();
	run_events(p_frame);
	const double delta = get_tick_delta();

	PredictionRecord &record = predictions[history_index(p_frame)];
	record.frame = p_frame;
	record.states.clear();
	record.input.begin_write();
	// The object count, written once known.
	record.input.add_uint_bits(0, 8);

	// The objects' code (and the events above) may remove objects: each one is looked up again before it's used.
	int count = 0;
	for (const uint16_t net_id : predicted_ids) {
		if (count >= 255) {
			break;
		}
		const RemoteObject *remote = remote_objects.getptr(net_id);
		TickSyncObject *object = remote ? remote->object : nullptr;
		if (object == nullptr) {
			continue;
		}
		count++;
		TickDataBuffer input;
		input.begin_write();
		object->collect_input(input);
		record.input.add_uint_bits(net_id, 16);
		record.input.add_data_buffer(input);

		input.begin_read();
		object->process_tick(delta, input);
		if (remote->object != object) {
			// Removed by its own code.
			continue;
		}
		quantize_object(object);
		LocalVector<Variant> values;
		read_states(object, values);
		record.states.insert(net_id, values);
	}
	const int end = record.input.total_size();
	record.input.seek(0);
	record.input.add_uint_bits(uint64_t(count), 8);
	record.input.seek(end);

	client_advance_dolls();
}

void TickSyncCore::client_send_inputs() {
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_INPUTS, 8);
	message.add_uint_bits(latest_snapshot, 32);
	message.add_bool(needs_full);

	if (!predicting) {
		message.add_uint_bits(0, 8);
		message.add_uint_bits(TICK_FRAME_NONE, 32);
		send(settings.authority_peer, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, message);
		return;
	}

	// The last `input_redundancy` predicted frames; consecutive identical inputs are sent once with a duplicate
	// count (NetworkSynchronizer `encode_inputs`).
	const uint32_t last_frame = stepper.get_next_frame_index() - 1;
	uint32_t first_frame = last_frame;
	for (int i = 1; i < settings.input_redundancy; i++) {
		const uint32_t frame = last_frame - uint32_t(i);
		if (frame == TICK_FRAME_NONE || predictions[history_index(frame)].frame != frame) {
			break;
		}
		first_frame = frame;
	}
	if (predictions[history_index(last_frame)].frame != last_frame) {
		message.add_uint_bits(0, 8);
		message.add_uint_bits(TICK_FRAME_NONE, 32);
		send(settings.authority_peer, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, message);
		return;
	}

	LocalVector<const TickDataBuffer *> frames;
	for (uint32_t frame = first_frame; frame != last_frame + 1; frame++) {
		frames.push_back(&predictions[history_index(frame)].input);
	}
	write_input_groups(message, first_frame, frames);
	send(settings.authority_peer, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, message);

	if (shares_inputs) {
		// Mesh: the other peers simulate this peer's objects as dolls (ADR-045). A star client is only connected to
		// the server.
		LocalVector<int> connected;
		transport->get_connected_peers(connected);
		for (const int peer : connected) {
			if (peer != settings.authority_peer) {
				send(peer, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, message);
			}
		}
	}
}

double TickSyncCore::get_timeline_frame(uint64_t p_now_usec) const {
	if (role == ROLE_SERVER) {
		return double(stepper.get_next_frame_index()) + stepper.get_interpolation_fraction();
	}
	if (role == ROLE_CLIENT && welcomed && clock.is_synchronized()) {
		return clock.get_master_frame_time(p_now_usec);
	}
	return -1.0;
}

void TickSyncCore::update_interpolation(uint64_t p_now_usec) {
	if (role != ROLE_CLIENT || stop_requested) {
		return;
	}
	// Objects may apply the state with their own code.
	BusyScope busy(this);
	now_usec = p_now_usec;
	client_update_interpolation();
}

void TickSyncCore::client_update_interpolation() {
	if (latest_snapshot == TICK_FRAME_NONE) {
		return;
	}

	// Render time, in frames: behind the server's current frame by the interpolation delay; the latest state
	// without interpolation.
	const double render_frame = get_view_frame(now_usec);

	const SnapshotRecord *past = nullptr;
	const SnapshotRecord *future = nullptr;
	for (const SnapshotRecord &record : received) {
		if (record.frame == TICK_FRAME_NONE) {
			continue;
		}
		const double frame = double(record.frame);
		if (frame <= render_frame) {
			if (past == nullptr || record.frame > past->frame) {
				past = &record;
			}
		} else if (future == nullptr || record.frame < future->frame) {
			future = &record;
		}
	}
	if (past == nullptr && future == nullptr) {
		return;
	}

	LocalVector<Variant> values;
	for (const KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
		TickSyncObject *object = E.value.object;
		if (object == nullptr || !E.value.relevant || client_is_predicted(E.key) || client_is_active_doll(E.key)) {
			continue;
		}
		const LocalVector<Variant> *past_values = past ? past->states.getptr(E.key) : nullptr;
		const LocalVector<Variant> *future_values = future ? future->states.getptr(E.key) : nullptr;
		if (past_values && future_values && past_values->size() == future_values->size()) {
			const double weight = CLAMP((render_frame - double(past->frame)) / double(future->frame - past->frame), 0.0, 1.0);
			const TickSchema &schema = object->get_sync_schema();
			values.resize(past_values->size());
			for (uint32_t i = 0; i < values.size() && int(i) < schema.size(); i++) {
				values[i] = schema.codecs[i]->interpolate((*past_values)[i], (*future_values)[i], weight);
			}
			object->apply_interpolated_state(values);
		} else if (past_values) {
			// No newer state: hold the last one.
			object->apply_interpolated_state(*past_values);
		} else if (future_values) {
			object->apply_interpolated_state(*future_values);
		}
	}
}

// ------------------------------------------------------------------------------------------------------ Dolls

TickSyncCore::DollPeer &TickSyncCore::client_get_doll(int p_peer) {
	DollPeer *doll = dolls.getptr(p_peer);
	if (doll) {
		return *doll;
	}
	DollPeer created;
	created.inputs.resize(settings.history_size);
	created.history.resize(settings.history_size);
	dolls.insert(p_peer, created);
	return dolls[p_peer];
}

void TickSyncCore::client_reset_dolls() {
	for (KeyValue<int, DollPeer> &E : dolls) {
		E.value.started = false;
		E.value.arrival_offsets.clear();
		E.value.next_offset = 0;
	}
}

void TickSyncCore::client_handle_doll_inputs(int p_peer, TickDataBuffer &p_message) {
	if (!welcomed) {
		return;
	}
	DollPeer &doll = client_get_doll(p_peer);
	if (!settings.trusted && !doll.input_limiter.take(settings.max_input_packets_per_second, now_usec)) {
		stats.rate_limited_packets++;
		return;
	}
	// The acknowledgment and the full snapshot request are for the server.
	p_message.read_uint_bits(32);
	p_message.read_bool();
	const int group_count = int(p_message.read_uint_bits(8));
	const uint32_t first_frame = uint32_t(p_message.read_uint_bits(32));
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	if (group_count == 0) {
		return;
	}

	// Frames are kept only near the local timeline (the controller predicts about as far ahead as this peer).
	const uint32_t ring = uint32_t(settings.history_size);
	const uint32_t reference = predicting ? stepper.get_next_frame_index() : first_frame;
	uint32_t newest = TICK_FRAME_NONE;
	uint32_t index = 0;
	for (int group = 0; group < group_count; group++) {
		const int duplicates = int(p_message.read_uint_bits(8));
		TickDataBuffer input;
		p_message.read_data_buffer(input);
		if (p_message.is_buffer_failed()) {
			stats.malformed_packets++;
			return;
		}
		for (int copy = 0; copy <= duplicates; copy++) {
			const uint32_t frame = first_frame + index++;
			if (frame == TICK_FRAME_NONE) {
				stats.rejected_inputs++;
				continue;
			}
			const int32_t distance = int32_t(frame - reference);
			if (distance >= int32_t(ring / 2)) {
				stats.rejected_inputs++;
				continue;
			}
			if (distance <= -int32_t(ring / 2)) {
				continue;
			}
			InputRecord &record = doll.inputs[frame % ring];
			if (record.frame != frame) {
				record.frame = frame;
				record.input = input;
			}
			if (newest == TICK_FRAME_NONE || tick_frame_after(frame, newest)) {
				newest = frame;
			}
		}
	}
	if (newest == TICK_FRAME_NONE || (doll.last_received_frame != TICK_FRAME_NONE && !tick_frame_after(newest, doll.last_received_frame))) {
		return;
	}
	doll.last_received_frame = newest;
	doll.last_received_usec = now_usec;
	if (predicting) {
		const double offset = double(int32_t(stepper.get_next_frame_index() - newest)) + stepper.get_interpolation_fraction();
		if (doll.arrival_offsets.size() < DOLL_ARRIVAL_WINDOW) {
			doll.arrival_offsets.push_back(offset);
		} else {
			doll.arrival_offsets[doll.next_offset] = offset;
		}
		doll.next_offset = (doll.next_offset + 1) % DOLL_ARRIVAL_WINDOW;
	}
}

int TickSyncCore::client_get_doll_target(const DollPeer &p_doll) const {
	// Inputs kept ahead of the doll: the minimum plus the jitter of their arrival.
	double spread = 0.0;
	if (!p_doll.arrival_offsets.is_empty()) {
		double lowest = p_doll.arrival_offsets[0];
		double highest = lowest;
		for (const double offset : p_doll.arrival_offsets) {
			lowest = MIN(lowest, offset);
			highest = MAX(highest, offset);
		}
		spread = highest - lowest;
	}
	return CLAMP(settings.min_input_buffer + int(Math::ceil(spread)), settings.min_input_buffer, settings.max_input_buffer);
}

bool TickSyncCore::client_is_active_doll(uint16_t p_net_id) const {
	if (dolls.is_empty()) {
		return false;
	}
	const RemoteObject *remote = remote_objects.getptr(p_net_id);
	if (remote == nullptr || remote->object == nullptr || !remote->object->is_doll_enabled()) {
		return false;
	}
	const DollPeer *doll = dolls.getptr(remote->controller);
	return doll && doll->started && predicting;
}

int TickSyncCore::get_doll_delay(int p_peer) const {
	const DollPeer *doll = dolls.getptr(p_peer);
	if (doll == nullptr || !doll->started || !predicting || doll->ids.is_empty()) {
		return -1;
	}
	return int(int32_t(stepper.get_next_frame_index() - doll->next_frame));
}

void TickSyncCore::client_simulate_doll(DollPeer &r_doll, uint32_t p_frame) {
	// The controller's input for the frame, or its latest one before it (ghost input).
	const uint32_t ring = uint32_t(settings.history_size);
	const uint32_t lookback = uint32_t(settings.max_input_buffer + settings.input_redundancy);
	TickDataBuffer frame_input;
	bool found = false;
	for (uint32_t back = 0; back <= lookback && !found; back++) {
		const uint32_t frame = p_frame - back;
		const InputRecord &record = r_doll.inputs[frame % ring];
		if (frame != TICK_FRAME_NONE && record.frame == frame) {
			frame_input = record.input;
			found = true;
			if (back > 0 && !rewinding) {
				stats.doll_ghost_inputs++;
			}
		}
	}
	HashMap<uint16_t, TickDataBuffer> inputs;
	if (found) {
		frame_input.begin_read();
		if (!parse_frame_input(frame_input, inputs)) {
			stats.malformed_packets++;
		}
	} else if (!rewinding) {
		stats.doll_ghost_inputs++;
	}

	const double delta = get_tick_delta();
	DollRecord &record = r_doll.history[p_frame % ring];
	record.frame = p_frame;
	record.states.clear();
	for (const uint16_t net_id : r_doll.ids) {
		RemoteObject *remote = remote_objects.getptr(net_id);
		if (remote == nullptr || remote->object == nullptr) {
			continue;
		}
		TickSyncObject *object = remote->object;
		// The inputs came from the object's controller: dolls are grouped by the peer that sent them (H1).
		TickDataBuffer input;
		const TickDataBuffer *stored = inputs.getptr(net_id);
		if (stored && (settings.trusted || stored->total_size() <= settings.max_input_bits)) {
			input = *stored;
		} else {
			input.begin_write();
		}
		input.begin_read();
		object->process_tick(delta, input);
		if (remote->object != object) {
			// Removed by its own code.
			continue;
		}
		quantize_object(object);
		LocalVector<Variant> values;
		read_states(object, values);
		record.states.insert(net_id, values);
	}

	// A doll behind the authority: the snapshot of this frame arrived before the doll simulated it.
	const SnapshotRecord *snapshot = client_get_received(p_frame);
	if (snapshot == nullptr || client_doll_matches(r_doll, record.states, *snapshot)) {
		return;
	}
	for (const uint16_t net_id : r_doll.ids) {
		const LocalVector<Variant> *values = snapshot->states.getptr(net_id);
		const RemoteObject *remote = remote_objects.getptr(net_id);
		TickSyncObject *object = remote ? remote->object : nullptr;
		if (values == nullptr || object == nullptr) {
			continue;
		}
		for (uint32_t i = 0; i < values->size(); i++) {
			object->set_sync_var(int(i), (*values)[i]);
		}
		record.states.insert(net_id, *values);
	}
	stats.doll_corrections++;
}

bool TickSyncCore::client_doll_matches(const DollPeer &p_doll, const ObjectStates &p_doll_states, const SnapshotRecord &p_snapshot) const {
	for (const uint16_t net_id : p_doll.ids) {
		const LocalVector<Variant> *authority_values = p_snapshot.states.getptr(net_id);
		const RemoteObject *remote = remote_objects.getptr(net_id);
		if (authority_values == nullptr || remote == nullptr || remote->object == nullptr) {
			continue;
		}
		const LocalVector<Variant> *doll_values = p_doll_states.getptr(net_id);
		if (doll_values == nullptr || doll_values->size() != authority_values->size()) {
			return false;
		}
		const TickSchema &schema = remote->object->get_sync_schema();
		for (uint32_t v = 0; v < authority_values->size() && int(v) < schema.size(); v++) {
			if (!schema.codecs[v]->is_equal((*doll_values)[v], (*authority_values)[v])) {
				return false;
			}
		}
	}
	return true;
}

bool TickSyncCore::client_restore_doll(DollPeer &r_doll, uint32_t p_snapshot_frame, uint32_t p_next_frame) {
	SnapshotRecord *snapshot = client_get_received(p_snapshot_frame);
	if (snapshot == nullptr) {
		return false;
	}
	const uint32_t ring = uint32_t(settings.history_size);
	uint32_t next_frame = p_next_frame;
	if (!tick_frame_after(next_frame, p_snapshot_frame) || next_frame - p_snapshot_frame >= ring / 2) {
		// Too far ahead of the state to simulate the frames in between: the doll continues from the state.
		next_frame = p_snapshot_frame + 1;
	}

	rewinding = true;
	DollRecord &record = r_doll.history[p_snapshot_frame % ring];
	record.frame = p_snapshot_frame;
	record.states.clear();
	for (const uint16_t net_id : r_doll.ids) {
		const LocalVector<Variant> *values = snapshot->states.getptr(net_id);
		const RemoteObject *remote = remote_objects.getptr(net_id);
		TickSyncObject *object = remote ? remote->object : nullptr;
		if (values == nullptr || object == nullptr) {
			continue;
		}
		for (uint32_t i = 0; i < values->size(); i++) {
			object->set_sync_var(int(i), (*values)[i]);
		}
		record.states.insert(net_id, *values);
	}
	for (uint32_t frame = p_snapshot_frame + 1; frame != next_frame; frame++) {
		client_simulate_doll(r_doll, frame);
	}
	rewinding = false;

	r_doll.next_frame = next_frame;
	r_doll.started = true;
	return true;
}

void TickSyncCore::client_advance_dolls() {
	const int resync_threshold = MAX(20, settings.ticks_per_second / 2);
	for (KeyValue<int, DollPeer> &E : dolls) {
		DollPeer &doll = E.value;
		if (doll.ids.is_empty()) {
			continue;
		}
		if (doll.last_received_frame == TICK_FRAME_NONE || now_usec - doll.last_received_usec >= DOLL_STALE_USEC) {
			// The controller stopped sending inputs: the doll is interpolated from the snapshots.
			doll.started = false;
			continue;
		}
		const int target = client_get_doll_target(doll);
		if (!doll.started) {
			// Starts `target` inputs behind the newest one, from the authority's latest state before that frame.
			const uint32_t start_frame = doll.last_received_frame - uint32_t(target) + 1;
			doll.step_accumulator = 0.0;
			client_restore_doll(doll, client_find_snapshot_before(start_frame), start_frame);
			continue;
		}

		const int buffered = int(int32_t(doll.last_received_frame - doll.next_frame)) + 1;
		const int error = buffered - target;
		if (Math::abs(error) > resync_threshold) {
			// Too far off to recover by changing the speed: start again.
			doll.started = false;
			stats.doll_resyncs++;
			continue;
		}
		// Faster when too many inputs are waiting, slower when they run out, so the buffer stays at the target.
		doll.step_accumulator += 1.0 + CLAMP(double(error) * DOLL_SPEED_GAIN, -DOLL_MAX_SPEED_DELTA, DOLL_MAX_SPEED_DELTA);
		for (int step = 0; step < DOLL_MAX_STEPS && doll.step_accumulator >= 1.0; step++) {
			client_simulate_doll(doll, doll.next_frame);
			doll.next_frame++;
			doll.step_accumulator -= 1.0;
		}
		doll.step_accumulator = MIN(doll.step_accumulator, 1.0);
	}
}

void TickSyncCore::client_reconcile_dolls(uint32_t p_frame) {
	if (!predicting || dolls.is_empty()) {
		return;
	}
	const SnapshotRecord *snapshot = client_get_received(p_frame);
	if (snapshot == nullptr) {
		return;
	}
	const uint32_t ring = uint32_t(settings.history_size);
	for (KeyValue<int, DollPeer> &E : dolls) {
		DollPeer &doll = E.value;
		// A doll behind the authority checks this state when it simulates the frame (`client_simulate_doll()`).
		if (!doll.started || doll.ids.is_empty() || !tick_frame_after(doll.next_frame, p_frame)) {
			continue;
		}
		const DollRecord &record = doll.history[p_frame % ring];
		if (record.frame == p_frame && client_doll_matches(doll, record.states, *snapshot)) {
			continue;
		}
		// Rewind: the authority's state at `p_frame`, then the doll's frames after it again.
		const uint32_t next_frame = doll.next_frame;
		client_restore_doll(doll, p_frame, next_frame);
		stats.doll_rewinds++;
		stats.doll_rewound_frames += uint64_t(next_frame - p_frame - 1);
	}
}

// ------------------------------------------------------------------------------------------------------ Migration

void TickSyncCore::handle_host_migrated(int p_new_host) {
	if (role != ROLE_CLIENT || p_new_host == settings.authority_peer) {
		return;
	}
	const int old_authority = settings.authority_peer;
	if (p_new_host == transport->get_local_peer_id()) {
		client_become_server(old_authority);
	} else {
		client_follow_authority(p_new_host);
	}
	if (listener) {
		listener->on_host_migrated(old_authority, p_new_host);
	}
}

void TickSyncCore::client_follow_authority(int p_new_authority) {
	// A new handshake with the new authority; the objects stay bound (it keeps the net ids).
	settings.authority_peer = p_new_authority;
	welcomed = false;
	rejected = false;
	predicting = false;
	needs_full = false;
	ack_pending = false;
	latest_snapshot = TICK_FRAME_NONE;
	last_reconciled = TICK_FRAME_NONE;
	pending_snapshot = PendingSnapshot();
	for (SnapshotRecord &record : received) {
		record.frame = TICK_FRAME_NONE;
	}
	for (PredictionRecord &record : predictions) {
		record.frame = TICK_FRAME_NONE;
	}
	client_reset_dolls();
	clock.clear_samples();
	last_ping_usec = 0;
	predicted_ids_dirty = true;
	if (transport->is_peer_connected(p_new_authority)) {
		TickDataBuffer hello;
		hello.begin_write();
		hello.add_uint_bits(TICK_MESSAGE_HELLO, 8);
		hello.add_uint_bits(TICK_PROTOCOL_VERSION, 16);
		hello.add_bool(sizeof(real_t) == sizeof(double));
		send(p_new_authority, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, hello);
	}
}

void TickSyncCore::client_become_server(int p_old_authority) {
	const int local_peer = transport->get_local_peer_id();
	// The freshest authoritative state of each object: the last snapshot, except for the objects this peer predicted.
	client_update_predicted_ids();
	LocalVector<uint16_t> predicted;
	predicted = predicted_ids;
	const SnapshotRecord *latest = client_get_received(latest_snapshot);
	// The timeline goes on from the old authority's current frame.
	const bool synchronized = welcomed && clock.is_synchronized();
	const uint32_t frame = synchronized ? clock.get_master_frame(now_usec) : stepper.get_next_frame_index();

	role = ROLE_SERVER;
	settings.authority_peer = local_peer;
	stepper.reset();
	stepper.set_ticks_per_second(settings.ticks_per_second);
	stepper.set_time_scale(1.0);
	stepper.set_next_frame_index(frame);
	clock.set_master(true);
	server_epoch_usec = int64_t(now_usec);
	clock.set_master_epoch_usec(server_compute_epoch());
	server_history.clear();
	server_history.resize(settings.history_size);
	authority_inputs.clear();
	authority_inputs.resize(settings.history_size);
	server_objects.clear();
	server_object_ids.clear();
	server_ids_by_object.clear();
	quarantined_ids.clear();

	// The objects keep their net ids; the old authority's own objects (NPCs, its player) are now this peer's.
	LocalVector<uint16_t> ids;
	for (const KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
		if (E.value.object) {
			ids.push_back(E.key);
		}
	}
	ids.sort();
	uint16_t highest = 0;
	for (const uint16_t net_id : ids) {
		const RemoteObject &remote = remote_objects[net_id];
		ServerObject object;
		object.object = remote.object;
		object.path = remote.path;
		object.controller = remote.controller == p_old_authority ? local_peer : remote.controller;
		object.schema_hash = remote.schema_hash;
		server_objects.insert(net_id, object);
		server_ids_by_object.insert(object.object, net_id);
		server_object_ids.push_back(net_id);
		highest = MAX(highest, net_id);
	}
	next_net_id = highest + 1;
	// The objects' setters are game code, which may remove objects: set once every object is registered.
	for (const uint16_t net_id : ids) {
		const LocalVector<Variant> *values = latest ? latest->states.getptr(net_id) : nullptr;
		const ServerObject *object = server_objects.getptr(net_id);
		if (values && object && !predicted.has(net_id)) {
			TickSyncObject *sync_object = object->object;
			for (uint32_t i = 0; i < values->size(); i++) {
				sync_object->set_sync_var(int(i), (*values)[i]);
			}
		}
	}

	// The client's state is over.
	remote_objects.clear();
	received.clear();
	predictions.clear();
	dolls.clear();
	predicted_ids.clear();
	predicted_ids_dirty = true;
	pending_snapshot = PendingSnapshot();
	welcomed = false;
	predicting = false;
	needs_full = false;
	latest_snapshot = TICK_FRAME_NONE;
	last_reconciled = TICK_FRAME_NONE;

	// Every other peer joins again with a handshake.
	peers.clear();
	LocalVector<int> connected;
	transport->get_connected_peers(connected);
	for (const int peer : connected) {
		PeerState state;
		state.inputs.resize(settings.history_size);
		peers.insert(peer, state);
	}
	// Objects registered here that the old authority didn't know.
	for (const KeyValue<String, TickSyncObject *> &E : local_objects) {
		if (!server_ids_by_object.has(E.value)) {
			server_add_object(E.value);
		}
	}
}

// ------------------------------------------------------------------------------------------------------ Spawns

uint32_t TickSyncCore::spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	ERR_FAIL_COND_V_MSG(role != ROLE_SERVER, 0, "Only the server can spawn.");
	ERR_FAIL_COND_V_MSG(p_name.is_empty(), 0, "A spawned node needs a name.");
	const uint32_t spawn_id = next_spawn_id++;
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
		if (E.value.accepted) {
			server_send_spawn(E.key, spawn_id);
		}
	}
	return spawn_id;
}

void TickSyncCore::despawn(uint32_t p_spawn_id) {
	ERR_FAIL_COND_MSG(role != ROLE_SERVER, "Only the server can despawn.");
	const SpawnRecord *record = spawns.getptr(p_spawn_id);
	ERR_FAIL_NULL_MSG(record, vformat("Spawn %d doesn't exist.", p_spawn_id));
	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(TICK_MESSAGE_DESPAWN, 8);
	message.add_uint_bits(p_spawn_id, 32);
	message.add_string(record->spawner);
	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.accepted) {
			send(E.key, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
		}
	}
	spawns.erase(p_spawn_id);
	spawn_order.erase(p_spawn_id);
	stats.despawns++;
}

void TickSyncCore::server_send_spawn(int p_peer, uint32_t p_spawn_id) {
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

void TickSyncCore::client_handle_spawn(TickDataBuffer &p_message) {
	const uint32_t spawn_id = uint32_t(p_message.read_uint_bits(32));
	const String spawner = p_message.read_string();
	const int scene = int(p_message.read_int_bits(16));
	const String name = p_message.read_string();
	const int controller = int(p_message.read_int_bits(32));
	const Variant data = TickCodec::variant()->decode(p_message);
	if (p_message.is_buffer_failed() || name.is_empty()) {
		stats.malformed_packets++;
		return;
	}
	stats.spawns++;
	// Kept, so this client can take over the spawns if it becomes the authority (ADR-062).
	if (!spawns.has(spawn_id)) {
		SpawnRecord record;
		record.spawner = spawner;
		record.scene = scene;
		record.name = name;
		record.controller = controller;
		record.data = data;
		spawns.insert(spawn_id, record);
		spawn_order.push_back(spawn_id);
		next_spawn_id = MAX(next_spawn_id, spawn_id + 1);
	}
	if (listener) {
		listener->on_spawn(spawner, spawn_id, scene, name, controller, data);
	}
}

void TickSyncCore::client_handle_despawn(TickDataBuffer &p_message) {
	const uint32_t spawn_id = uint32_t(p_message.read_uint_bits(32));
	const String spawner = p_message.read_string();
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}
	stats.despawns++;
	spawns.erase(spawn_id);
	spawn_order.erase(spawn_id);
	if (listener) {
		listener->on_despawn(spawner, spawn_id);
	}
}

// ------------------------------------------------------------------------------------------------------ Events

void TickSyncCore::write_event(TickDataBuffer &r_message, uint16_t p_target, uint32_t p_frame, const StringName &p_name, const Variant &p_payload) {
	r_message.begin_write();
	r_message.add_uint_bits(TICK_MESSAGE_EVENT, 8);
	r_message.add_uint_bits(p_target, 16);
	r_message.add_uint_bits(p_frame, 32);
	r_message.add_string(p_name);
	TickCodec::variant()->encode(p_payload, r_message);
}

bool TickSyncCore::read_event(TickDataBuffer &p_message, PendingEvent &r_event, int &r_payload_bytes) {
	r_event.target = uint16_t(p_message.read_uint_bits(16));
	r_event.requested_frame = uint32_t(p_message.read_uint_bits(32));
	r_event.name = p_message.read_string(TICK_MAX_EVENT_NAME_BYTES);
	// The payload size comes first (see `TickCodec::variant()`); checked before decoding anything big.
	const int offset = p_message.get_bit_offset();
	r_payload_bytes = int(p_message.read_uint_bits(16));
	p_message.seek(offset);
	if (p_message.is_buffer_failed() || String(r_event.name).is_empty()) {
		return false;
	}
	if (!settings.trusted && role == ROLE_SERVER && r_payload_bytes > settings.max_event_bytes) {
		return true;
	}
	r_event.payload = TickCodec::variant()->decode(p_message);
	return !p_message.is_buffer_failed();
}

void TickSyncCore::queue_event(const PendingEvent &p_event) {
	PendingEvent event = p_event;
	event.sequence = next_event_sequence++;
	pending_events.push_back(event);
	pending_events.sort_custom<PendingEventOrder>();
}

bool TickSyncCore::execute_event(const PendingEvent &p_event) {
	const uint32_t frame = p_event.requested_frame != TICK_FRAME_NONE ? p_event.requested_frame : p_event.frame;
	if (p_event.target == 0) {
		if (listener) {
			listener->on_network_event(p_event.sender, p_event.name, p_event.payload, frame);
		}
		return true;
	}
	TickSyncObject *object = nullptr;
	if (role == ROLE_SERVER) {
		ServerObject *server_object = server_objects.getptr(p_event.target);
		object = server_object ? server_object->object : nullptr;
		if (object == nullptr) {
			// The object is gone.
			return true;
		}
	} else {
		RemoteObject *remote = remote_objects.getptr(p_event.target);
		object = remote ? remote->object : nullptr;
		if (object == nullptr) {
			// Not created or bound yet: keep it until it expires (E5).
			return now_usec >= p_event.expire_usec;
		}
	}
	object->on_event(p_event.sender, p_event.name, p_event.payload, frame);
	return true;
}

void TickSyncCore::run_events(uint32_t p_frame) {
	if (pending_events.is_empty()) {
		return;
	}
	// Executing an event can queue others; work on a copy.
	LocalVector<PendingEvent> events;
	events = pending_events;
	pending_events.clear();
	LocalVector<PendingEvent> kept;
	for (const PendingEvent &event : events) {
		const bool due = event.frame == TICK_FRAME_NONE || (p_frame != TICK_FRAME_NONE && !tick_frame_after(event.frame, p_frame));
		if (!due || !execute_event(event)) {
			kept.push_back(event);
		}
	}
	for (const PendingEvent &event : pending_events) {
		kept.push_back(event);
	}
	pending_events = kept;
	pending_events.sort_custom<PendingEventOrder>();
}

void TickSyncCore::server_handle_event(int p_peer, TickDataBuffer &p_message) {
	PeerState *peer = peers.getptr(p_peer);
	if (peer == nullptr || !peer->accepted) {
		return;
	}
	if (!settings.trusted && !peer->event_limiter.take(settings.max_events_per_second, now_usec)) {
		stats.rate_limited_packets++;
		stats.events_rejected++;
		return;
	}
	PendingEvent event;
	int payload_bytes = 0;
	if (!read_event(p_message, event, payload_bytes)) {
		stats.malformed_packets++;
		return;
	}
	if (!settings.trusted && payload_bytes > settings.max_event_bytes) {
		stats.events_rejected++;
		return;
	}
	event.sender = p_peer;

	// Sender rules (ADR-033).
	int verdict = -1;
	if (event.target == 0) {
		verdict = listener ? listener->validate_network_event(p_peer, event.name, event.payload) : -1;
		if (verdict < 0) {
			verdict = 1;
		}
	} else {
		const ServerObject *object = server_objects.getptr(event.target);
		if (object == nullptr) {
			stats.events_rejected++;
			return;
		}
		// The validator is game code, which may remove the object: `object` isn't used after it.
		const int controller = object->controller;
		verdict = object->object->validate_event(p_peer, event.name, event.payload);
		if (verdict < 0) {
			verdict = (settings.trusted || controller == p_peer) ? 1 : 0;
		}
	}
	if (verdict == 0) {
		stats.events_rejected++;
		return;
	}

	// Scheduling: the requested frame, or the next one if it already passed.
	const uint32_t current = stepper.get_next_frame_index();
	const uint32_t max_delay = uint32_t(settings.max_event_delay * double(settings.ticks_per_second));
	if (event.requested_frame != TICK_FRAME_NONE && tick_frame_after(event.requested_frame, current + max_delay)) {
		stats.events_rejected++;
		return;
	}
	if (event.requested_frame == TICK_FRAME_NONE || !tick_frame_after(event.requested_frame, current)) {
		event.frame = current;
	} else {
		event.frame = event.requested_frame;
	}
	stats.events_received++;
	queue_event(event);
}

void TickSyncCore::client_handle_event(TickDataBuffer &p_message) {
	PendingEvent event;
	int payload_bytes = 0;
	if (!read_event(p_message, event, payload_bytes)) {
		stats.malformed_packets++;
		return;
	}
	event.sender = settings.authority_peer;
	event.expire_usec = now_usec + PENDING_EVENT_TIMEOUT_USEC;
	stats.events_received++;

	// A frame not simulated yet (or any frame, before the prediction starts) waits for the simulation to reach it;
	// otherwise it runs now.
	const uint32_t next_frame = stepper.get_next_frame_index();
	if (event.requested_frame != TICK_FRAME_NONE && (!predicting || !tick_frame_after(next_frame, event.requested_frame))) {
		event.frame = event.requested_frame;
		queue_event(event);
		return;
	}
	event.frame = TICK_FRAME_NONE;
	if (!execute_event(event)) {
		queue_event(event);
	}
}

Error TickSyncCore::send_event(TickSyncObject *p_target, const StringName &p_name, const Variant &p_payload, uint32_t p_frame, int p_peer) {
	ERR_FAIL_COND_V_MSG(role == ROLE_NONE, ERR_UNCONFIGURED, "The network isn't running.");
	ERR_FAIL_COND_V_MSG(String(p_name).is_empty(), ERR_INVALID_PARAMETER, "The event needs a name.");
	ERR_FAIL_COND_V_MSG(String(p_name).utf8().length() > TICK_MAX_EVENT_NAME_BYTES, ERR_INVALID_PARAMETER, vformat("An event name can't be longer than %d bytes in UTF-8.", TICK_MAX_EVENT_NAME_BYTES));
	int payload_bytes = 0;
	ERR_FAIL_COND_V_MSG(encode_variant(p_payload, nullptr, payload_bytes, false) != OK, ERR_INVALID_DATA, "The event payload can't be encoded (objects aren't allowed).");
	ERR_FAIL_COND_V_MSG(payload_bytes > settings.max_event_bytes, ERR_INVALID_DATA, vformat("The event payload takes %d bytes; the limit is %d.", payload_bytes, settings.max_event_bytes));

	uint16_t target = 0;
	if (p_target) {
		target = get_net_id(p_target);
		ERR_FAIL_COND_V_MSG(target == 0, ERR_UNAVAILABLE, "The event's target object isn't synchronized yet.");
	}

	TickDataBuffer message;
	if (role == ROLE_CLIENT) {
		ERR_FAIL_COND_V_MSG(!welcomed, ERR_UNAVAILABLE, "The client isn't connected to the server yet.");
		uint32_t frame = p_frame;
		if (frame == TICK_FRAME_NONE && predicting) {
			// By default, the frame the client is predicting.
			frame = stepper.get_next_frame_index();
		}
		write_event(message, target, frame, p_name, p_payload);
		send(settings.authority_peer, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
		stats.events_sent++;
		return OK;
	}

	write_event(message, target, p_frame, p_name, p_payload);
	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.accepted && (p_peer == TickTransport::PEER_BROADCAST || p_peer == E.key)) {
			send(E.key, TICK_CHANNEL_CONTROL, TickTransport::TRANSFER_MODE_RELIABLE, message);
			stats.events_sent++;
		}
	}
	return OK;
}

uint32_t TickSyncCore::get_event_frame(double p_seconds) const {
	return stepper.get_next_frame_index() + uint32_t(Math::ceil(MAX(p_seconds, 0.0) * double(settings.ticks_per_second)));
}

Dictionary TickSyncCore::get_stats_dictionary() const {
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
	result["doll_rewinds"] = stats.doll_rewinds;
	result["doll_rewound_frames"] = stats.doll_rewound_frames;
	result["doll_corrections"] = stats.doll_corrections;
	result["doll_resyncs"] = stats.doll_resyncs;
	result["doll_ghost_inputs"] = stats.doll_ghost_inputs;
	result["split_snapshots"] = stats.split_snapshots;
	result["incomplete_snapshots"] = stats.incomplete_snapshots;
	result["relevance_changes"] = stats.relevance_changes;
	Dictionary doll_delays;
	for (const KeyValue<int, DollPeer> &E : dolls) {
		const int delay = get_doll_delay(E.key);
		if (delay >= 0) {
			doll_delays[E.key] = delay;
		}
	}
	result["doll_delays"] = doll_delays;
	result["time_scale"] = stepper.get_time_scale();
	result["timeline_frame"] = get_timeline_frame(now_usec);
	result["latest_snapshot_frame"] = int64_t(latest_snapshot);
	return result;
}

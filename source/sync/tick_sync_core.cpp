#include "tick_sync_core.h"

#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/variant/variant.h"

// A rejected peer is disconnected after this delay, so the rejection reason reaches it.
static constexpr uint64_t REJECT_DISCONNECT_DELAY_USEC = 1000000;
// A full snapshot is sent again if it wasn't acknowledged within this delay.
static constexpr uint64_t FULL_SNAPSHOT_RESEND_USEC = 500000;
// A client keeps an event for an object it doesn't know yet for this long (E5).
static constexpr uint64_t PENDING_EVENT_TIMEOUT_USEC = 5000000;

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
	settings = p_settings;
}

Error TickSyncCore::start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec) {
	ERR_FAIL_COND_V_MSG(role != ROLE_NONE, ERR_ALREADY_IN_USE, "The network is already running.");
	ERR_FAIL_COND_V_MSG(p_transport.is_null(), ERR_INVALID_PARAMETER, "The transport is null.");
	ERR_FAIL_COND_V_MSG(p_transport->get_channel_count() < TICK_CHANNEL_COUNT, ERR_INVALID_PARAMETER, vformat("The transport must have at least %d channels.", TICK_CHANNEL_COUNT));

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
	predictions.clear();
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
	ERR_FAIL_COND_MSG(server_objects.size() + quarantined_ids.size() >= UINT16_MAX - 1, "Too many synchronized objects.");
	const uint32_t current = stepper.get_next_frame_index();
	const uint32_t quarantine = uint32_t(settings.history_size) * 2;
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

	for (const KeyValue<int, PeerState> &E : peers) {
		if (E.value.accepted) {
			server_send_register(E.key, net_id);
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

void TickSyncCore::process(double p_delta, uint64_t p_now_usec) {
	ERR_FAIL_COND_MSG(role == ROLE_NONE, "The network isn't running.");
	now_usec = p_now_usec;

	transport->poll();
	handle_events();
	TickTransport::Packet packet;
	while (role != ROLE_NONE && transport->pop_packet(packet)) {
		handle_packet(packet);
	}
	if (role == ROLE_NONE) {
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
					for (int32_t i = 0; i < MIN(behind, int32_t(stepper.get_max_ticks_per_advance())); i++) {
						server_tick(stepper.step_frame());
					}
				}
			}
		} else {
			const uint64_t dropped_before = stepper.get_dropped_ticks();
			stepper.advance(p_delta);
			// Dropped ticks shift the timeline: the frames keep their duration.
			server_epoch_usec += int64_t(double(stepper.get_dropped_ticks() - dropped_before) * 1000000.0 * get_tick_delta());
			while (stepper.get_pending_ticks() > 0) {
				server_tick(stepper.pop_tick());
			}
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
			while (stepper.get_pending_ticks() > 0) {
				client_tick(stepper.pop_tick());
				ticked = true;
			}
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
	while (transport->pop_event(event)) {
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
				predicted_ids_dirty = true;
				if (listener) {
					listener->on_peer_left(settings.authority_peer);
				}
			}
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
	for (const uint16_t net_id : server_object_ids) {
		server_send_register(p_peer, net_id);
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
	SnapshotRecord &record = server_history[history_index(p_frame)];
	record.frame = p_frame;
	record.states.clear();

	for (const uint16_t net_id : server_object_ids) {
		ServerObject &object = server_objects[net_id];
		TickDataBuffer input;
		input.begin_write();
		if (object.controller == settings.authority_peer) {
			object.object->collect_input(input);
		} else {
			// Only the controller's own input moves the object (H1): inputs are looked up per sending peer.
			PeerState *peer = peers.getptr(object.controller);
			if (peer && peer->accepted) {
				TickDataBuffer *peer_input = peer->tick_inputs.getptr(net_id);
				if (peer_input) {
					input = *peer_input;
				}
			}
		}
		input.begin_read();
		object.object->process_tick(delta, input);
		quantize_object(object.object);
		LocalVector<Variant> values;
		read_states(object.object, values);
		record.states.insert(net_id, values);
	}

	if (p_frame % uint32_t(settings.snapshot_interval) == 0) {
		for (KeyValue<int, PeerState> &E : peers) {
			if (E.value.accepted) {
				server_send_snapshot(E.key, E.value, p_frame);
			}
		}
	}
}

void TickSyncCore::write_object_state(TickDataBuffer &r_message, const ServerObject &p_object, const LocalVector<Variant> &p_values, const LocalVector<Variant> *p_base) const {
	const TickSchema &schema = p_object.object->get_sync_schema();
	TickDataBuffer payload;
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

	TickDataBuffer message;
	message.begin_write();
	message.add_uint_bits(full ? TICK_MESSAGE_SNAPSHOT_FULL : TICK_MESSAGE_SNAPSHOT_DELTA, 8);
	message.add_uint_bits(p_frame, 32);
	message.add_uint_bits(base, 32);
	int server_buffer = 0;
	if (r_peer.last_received_frame != TICK_FRAME_NONE) {
		server_buffer = CLAMP(int(int32_t(r_peer.last_received_frame - p_frame)), -127, 127);
	}
	message.add_int_bits(server_buffer, 8);
	message.add_uint_bits(server_object_ids.size(), 16);
	for (const uint16_t net_id : server_object_ids) {
		const LocalVector<Variant> *values = record.states.getptr(net_id);
		const LocalVector<Variant> *base_values = base_record ? base_record->states.getptr(net_id) : nullptr;
		message.add_uint_bits(net_id, 16);
		if (values == nullptr) {
			// Registered after this frame was simulated.
			message.add_bool(false);
			continue;
		}
		bool changed = base_values == nullptr || base_values->size() != values->size();
		for (uint32_t i = 0; !changed && i < values->size(); i++) {
			changed = !((*values)[i] == (*base_values)[i]);
		}
		message.add_bool(changed);
		if (changed) {
			write_object_state(message, server_objects[net_id], *values, base_values);
		}
	}

	if (full) {
		r_peer.needs_full = false;
		r_peer.last_full_frame = p_frame;
		r_peer.last_full_usec = now_usec;
		stats.full_snapshots_sent++;
		send(p_peer, TICK_CHANNEL_SNAPSHOT, TickTransport::TRANSFER_MODE_RELIABLE, message);
	} else {
		stats.delta_snapshots_sent++;
		send(p_peer, TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_UNRELIABLE, message);
	}
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
	r_remote.object = *local;
	predicted_ids_dirty = true;
	// Events that arrived before the object (E5).
	run_events(predicting ? stepper.get_next_frame_index() - 1 : TICK_FRAME_NONE);
	// Its state may be unknown in the current snapshot: ask for a full one.
	needs_full = true;
	if (predicting && r_remote.controller == transport->get_local_peer_id() && latest_snapshot != TICK_FRAME_NONE) {
		// A predicted object joining late starts from the server's state.
		SnapshotRecord *record = client_get_received(latest_snapshot);
		const LocalVector<Variant> *values = record ? record->states.getptr(p_net_id) : nullptr;
		if (values) {
			for (uint32_t i = 0; i < values->size(); i++) {
				r_remote.object->set_sync_var(int(i), (*values)[i]);
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
	const int local_peer = transport.is_valid() ? transport->get_local_peer_id() : 0;
	for (const KeyValue<uint16_t, RemoteObject> &E : remote_objects) {
		if (E.value.object && E.value.controller == local_peer) {
			predicted_ids.push_back(E.key);
		}
	}
	predicted_ids.sort();
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
	const int count = int(p_message.read_uint_bits(16));
	if (p_message.is_buffer_failed() || frame == TICK_FRAME_NONE) {
		stats.malformed_packets++;
		return;
	}
	if (latest_snapshot != TICK_FRAME_NONE && !tick_frame_after(frame, latest_snapshot)) {
		// Older than what's already known.
		stats.snapshots_dropped++;
		return;
	}
	SnapshotRecord *base_record = nullptr;
	if (base != TICK_FRAME_NONE) {
		base_record = client_get_received(base);
		if (base_record == nullptr) {
			// The base isn't known (yet): can't decode.
			stats.snapshots_dropped++;
			ack_pending = true;
			return;
		}
	}

	ObjectStates states;
	bool missing_state = false;
	for (int i = 0; i < count; i++) {
		const uint16_t net_id = uint16_t(p_message.read_uint_bits(16));
		const bool changed = p_message.read_bool();
		if (p_message.is_buffer_failed()) {
			break;
		}
		const RemoteObject *remote = remote_objects.getptr(net_id);
		const bool bound = remote && remote->object;
		const LocalVector<Variant> *base_values = base_record ? base_record->states.getptr(net_id) : nullptr;

		if (!changed) {
			if (base_values) {
				states.insert(net_id, *base_values);
			} else if (bound) {
				missing_state = true;
			}
			continue;
		}

		TickDataBuffer payload;
		p_message.read_data_buffer(payload);
		if (p_message.is_buffer_failed() || !bound) {
			continue;
		}

		const TickSchema &schema = remote->object->get_sync_schema();
		LocalVector<Variant> values;
		values.resize(schema.size());
		bool complete = true;
		for (int v = 0; v < schema.size(); v++) {
			if (payload.read_bool()) {
				values[v] = schema.codecs[v]->decode(payload);
			} else if (base_values && v < int(base_values->size())) {
				values[v] = (*base_values)[v];
			} else {
				complete = false;
			}
		}
		if (complete && !payload.is_buffer_failed()) {
			states.insert(net_id, values);
		} else {
			missing_state = true;
		}
	}
	if (p_message.is_buffer_failed()) {
		stats.malformed_packets++;
		return;
	}

	if (p_full && !missing_state) {
		needs_full = false;
	} else if (missing_state) {
		needs_full = true;
	}

	SnapshotRecord &record = received[history_index(frame)];
	record.frame = frame;
	record.states = states;
	record.complete = !missing_state;
	latest_snapshot = frame;
	stats.snapshots_received++;
	ack_pending = true;

	client_adjust_speed(server_buffer);
	client_reconcile(frame);
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

	// The predicted objects start from the server's latest state.
	SnapshotRecord *record = client_get_received(latest_snapshot);
	if (record) {
		for (const uint16_t net_id : predicted_ids) {
			const LocalVector<Variant> *values = record->states.getptr(net_id);
			if (values) {
				TickSyncObject *object = remote_objects[net_id].object;
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
		return;
	}
	const double delta = CLAMP(double(error) * settings.time_scale_gain, -settings.max_time_scale_delta, settings.max_time_scale_delta);
	stepper.set_time_scale(1.0 + delta);
}

void TickSyncCore::client_reconcile(uint32_t p_frame) {
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

	// Rewind: apply the server's state at `p_frame`, then simulate again the frames predicted after it.
	rewinding = true;
	for (const uint16_t net_id : predicted_ids) {
		const LocalVector<Variant> *server_values = snapshot->states.getptr(net_id);
		if (server_values == nullptr) {
			continue;
		}
		TickSyncObject *object = remote_objects[net_id].object;
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
			TickSyncObject *object = remote_objects[net_id].object;
			TickDataBuffer input;
			TickDataBuffer *stored = inputs.getptr(net_id);
			if (stored) {
				input = *stored;
			} else {
				input.begin_write();
			}
			input.begin_read();
			object->process_tick(delta, input);
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
	record.input.add_uint_bits(MIN(predicted_ids.size(), 255u), 8);

	int count = 0;
	for (const uint16_t net_id : predicted_ids) {
		if (count++ >= 255) {
			break;
		}
		TickSyncObject *object = remote_objects[net_id].object;
		TickDataBuffer input;
		input.begin_write();
		object->collect_input(input);
		record.input.add_uint_bits(net_id, 16);
		record.input.add_data_buffer(input);

		input.begin_read();
		object->process_tick(delta, input);
		quantize_object(object);
		LocalVector<Variant> values;
		read_states(object, values);
		record.states.insert(net_id, values);
	}
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
		if (predictions[history_index(frame)].frame != frame) {
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

	LocalVector<const TickDataBuffer *> groups;
	LocalVector<int> duplicates;
	for (uint32_t frame = first_frame; frame != last_frame + 1; frame++) {
		const TickDataBuffer &input = predictions[history_index(frame)].input;
		if (!groups.is_empty() && *groups[groups.size() - 1] == input && duplicates[duplicates.size() - 1] < 255) {
			duplicates[duplicates.size() - 1]++;
		} else {
			groups.push_back(&input);
			duplicates.push_back(0);
		}
	}

	message.add_uint_bits(groups.size(), 8);
	message.add_uint_bits(first_frame, 32);
	for (uint32_t i = 0; i < groups.size(); i++) {
		message.add_uint_bits(uint64_t(duplicates[i]), 8);
		message.add_data_buffer(*groups[i]);
	}
	send(settings.authority_peer, TICK_CHANNEL_INPUTS, TickTransport::TRANSFER_MODE_UNRELIABLE, message);
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
	if (role != ROLE_CLIENT) {
		return;
	}
	now_usec = p_now_usec;
	client_update_interpolation();
}

void TickSyncCore::client_update_interpolation() {
	if (latest_snapshot == TICK_FRAME_NONE) {
		return;
	}

	// Render time, in frames: behind the server's current frame by the interpolation delay; the latest state
	// without interpolation.
	double render_frame = double(latest_snapshot);
	if (settings.interpolate_remote && clock.is_synchronized() && welcomed) {
		render_frame = clock.get_master_frame_time(now_usec) - settings.interpolation_delay * double(settings.ticks_per_second);
	}

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
		if (object == nullptr || client_is_predicted(E.key)) {
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
	r_event.name = p_message.read_string();
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
		ServerObject *object = server_objects.getptr(event.target);
		if (object == nullptr) {
			stats.events_rejected++;
			return;
		}
		verdict = object->object->validate_event(p_peer, event.name, event.payload);
		if (verdict < 0) {
			verdict = (settings.trusted || object->controller == p_peer) ? 1 : 0;
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
	result["time_scale"] = stepper.get_time_scale();
	result["timeline_frame"] = get_timeline_frame(now_usec);
	result["latest_snapshot_frame"] = int64_t(latest_snapshot);
	return result;
}

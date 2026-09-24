#pragma once

#include "../common/tick_data_buffer.h"
#include "../tick/tick_clock.h"
#include "../tick/tick_fixed_stepper.h"
#include "../transport/tick_transport.h"
#include "tick_protocol.h"
#include "tick_sync_object.h"

#include "core/templates/hash_map.h"

// Star network with a single authority (the server, peer 1): the F2 engine. Design in `notes/f2-design.md`.
//
// - Server: simulates every object; the input of each client's objects comes from that client, and the input of
//   the server's objects is collected locally. Sends snapshots (delta over the last acknowledged one).
// - Client: predicts the objects it controls and rewinds them when the server disagrees; interpolates every
//   other object between snapshots.
//
// The core doesn't read the clock: `process()` receives the current time, so it runs the same with a real
// network and with the simulated one of the tests.
class TickSyncCore {
public:
	enum Role {
		ROLE_NONE,
		ROLE_SERVER,
		ROLE_CLIENT,
	};

	class Listener {
	public:
		virtual ~Listener() {}
		// Server: a client passed the handshake. Client: the server accepted this client.
		virtual void on_peer_ready(int p_peer) {}
		virtual void on_peer_left(int p_peer) {}
		// Client: the server refused the connection.
		virtual void on_rejected(const String &p_reason) {}
		// Client: the clock is synchronized and the local objects are now predicted.
		virtual void on_prediction_started(uint32_t p_frame) {}
		// Client: the predicted objects diverged at `p_frame` and `p_frame_count` frames were simulated again.
		virtual void on_rewound(uint32_t p_frame, int p_frame_count) {}
	};

	struct Settings {
		int ticks_per_second = 60;
		// Frames of history kept for snapshots, predictions and interpolation.
		int history_size = 128;
		// Past inputs repeated in each input packet, against packet loss.
		int input_redundancy = 5;
		// A snapshot is sent every `snapshot_interval` ticks.
		int snapshot_interval = 1;
		// How far behind the server the interpolated objects are rendered, in seconds.
		double interpolation_delay = 0.1;
		// Inputs the server should have buffered ahead of its frame; the client adapts its speed to keep it
		// between these bounds.
		int min_input_buffer = 2;
		int max_input_buffer = 8;
		// Largest speed change of the client, and how much of the buffer error is corrected per frame.
		double max_time_scale_delta = 0.1;
		double time_scale_gain = 0.02;
		// Seconds between pings once the clock is synchronized.
		double ping_interval = 0.5;
		int clock_min_samples = 4;
	};

	struct Stats {
		uint64_t rewinds = 0;
		uint64_t rewound_frames = 0;
		uint64_t ghost_inputs = 0;
		uint64_t late_inputs = 0;
		uint64_t rejected_inputs = 0;
		uint64_t full_snapshots_sent = 0;
		uint64_t delta_snapshots_sent = 0;
		uint64_t snapshots_received = 0;
		uint64_t snapshots_dropped = 0;
		uint64_t malformed_packets = 0;
	};

private:
	typedef HashMap<uint16_t, LocalVector<Variant>> ObjectStates;

	struct SnapshotRecord {
		uint32_t frame = TICK_FRAME_NONE;
		ObjectStates states;
		// Client: objects whose state is known only partially (not decodable yet).
		bool complete = true;
	};

	struct PredictionRecord {
		uint32_t frame = TICK_FRAME_NONE;
		TickDataBuffer input;
		ObjectStates states;
	};

	struct InputRecord {
		uint32_t frame = TICK_FRAME_NONE;
		TickDataBuffer input;
	};

	struct ServerObject {
		TickSyncObject *object = nullptr;
		String path;
		int controller = 0;
		uint32_t schema_hash = 0;
	};

	struct RemoteObject {
		String path;
		int controller = 0;
		uint32_t schema_hash = 0;
		TickSyncObject *object = nullptr;
	};

	struct PeerState {
		bool accepted = false;
		uint64_t reject_usec = 0;
		bool rejected = false;
		uint32_t acked_snapshot = TICK_FRAME_NONE;
		bool needs_full = true;
		uint32_t last_full_frame = TICK_FRAME_NONE;
		uint64_t last_full_usec = 0;
		LocalVector<InputRecord> inputs;
		TickDataBuffer last_input;
		bool has_last_input = false;
		uint32_t last_received_frame = TICK_FRAME_NONE;
		HashMap<uint16_t, TickDataBuffer> tick_inputs;
	};

	Settings settings;
	Stats stats;
	Listener *listener = nullptr;
	Role role = ROLE_NONE;
	Ref<TickTransport> transport;
	TickFixedStepper stepper;
	TickClock clock;
	uint64_t now_usec = 0;
	bool rewinding = false;

	// Server.
	HashMap<uint16_t, ServerObject> server_objects;
	LocalVector<uint16_t> server_object_ids;
	HashMap<TickSyncObject *, uint16_t> server_ids_by_object;
	uint16_t next_net_id = 1;
	HashMap<int, PeerState> peers;
	LocalVector<SnapshotRecord> server_history;
	uint64_t server_epoch_usec = 0;

	// Client.
	HashMap<String, TickSyncObject *> local_objects;
	HashMap<uint16_t, RemoteObject> remote_objects;
	LocalVector<uint16_t> predicted_ids;
	bool predicted_ids_dirty = true;
	bool welcomed = false;
	bool rejected = false;
	bool predicting = false;
	bool needs_full = false;
	uint64_t last_ping_usec = 0;
	uint32_t latest_snapshot = TICK_FRAME_NONE;
	uint32_t last_reconciled = TICK_FRAME_NONE;
	bool ack_pending = false;
	LocalVector<SnapshotRecord> received;
	LocalVector<PredictionRecord> predictions;

	int history_index(uint32_t p_frame) const { return int(p_frame % uint32_t(settings.history_size)); }
	double get_tick_delta() const { return 1.0 / double(settings.ticks_per_second); }

	void send(int p_peer, TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message);
	void read_states(TickSyncObject *p_object, LocalVector<Variant> &r_values) const;
	void quantize_object(TickSyncObject *p_object) const;
	static bool parse_frame_input(TickDataBuffer &p_frame_input, HashMap<uint16_t, TickDataBuffer> &r_inputs);

	void handle_events();
	void handle_packet(const TickTransport::Packet &p_packet);

	// Server.
	void server_handle_hello(int p_peer, TickDataBuffer &p_message);
	void server_handle_inputs(int p_peer, TickDataBuffer &p_message);
	void server_handle_ping(int p_peer, TickDataBuffer &p_message);
	void server_accept_peer(int p_peer);
	void server_reject_peer(int p_peer, const String &p_reason);
	void server_send_register(int p_peer, uint16_t p_net_id);
	void server_add_object(TickSyncObject *p_object);
	void server_tick(uint32_t p_frame);
	void server_resolve_input(PeerState &r_peer, uint32_t p_frame);
	void server_send_snapshot(int p_peer, PeerState &r_peer, uint32_t p_frame);
	void write_object_state(TickDataBuffer &r_message, const ServerObject &p_object, const LocalVector<Variant> &p_values, const LocalVector<Variant> *p_base) const;

	// Client.
	void client_handle_welcome(TickDataBuffer &p_message);
	void client_handle_reject(TickDataBuffer &p_message);
	void client_handle_register(TickDataBuffer &p_message);
	void client_handle_unregister(TickDataBuffer &p_message);
	void client_handle_snapshot(TickDataBuffer &p_message, bool p_full);
	void client_handle_pong(TickDataBuffer &p_message);
	void client_bind(uint16_t p_net_id, RemoteObject &r_remote);
	void client_update_predicted_ids();
	bool client_is_predicted(uint16_t p_net_id) const;
	void client_start_prediction();
	uint32_t client_compute_start_frame() const;
	void client_adjust_speed(int p_server_buffer);
	void client_reconcile(uint32_t p_frame);
	void client_tick(uint32_t p_frame);
	void client_send_inputs();
	void client_send_ping();
	void client_update_interpolation();
	SnapshotRecord *client_get_received(uint32_t p_frame);

public:
	void set_settings(const Settings &p_settings);
	const Settings &get_settings() const { return settings; }
	const Stats &get_stats() const { return stats; }
	void set_listener(Listener *p_listener) { listener = p_listener; }

	// The role comes from the transport: peer 1 is the server.
	Error start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec);
	void stop();
	Role get_role() const { return role; }
	bool is_server() const { return role == ROLE_SERVER; }

	// Server: objects are simulated and replicated. Client: objects are bound to the server's objects with the
	// same path, then predicted (controlled by this client) or interpolated.
	void register_object(TickSyncObject *p_object);
	void unregister_object(TickSyncObject *p_object);

	// Receives, simulates the pending ticks, sends, and updates the interpolated objects.
	void process(double p_delta, uint64_t p_now_usec);

	// Next frame to simulate.
	uint32_t get_frame() const { return stepper.get_next_frame_index(); }
	bool is_rewinding() const { return rewinding; }
	bool is_predicting() const { return predicting; }
	bool is_welcomed() const { return welcomed; }
	const TickClock &get_clock() const { return clock; }
	double get_time_scale() const { return stepper.get_time_scale(); }
	uint32_t get_latest_snapshot_frame() const { return latest_snapshot; }
	// Net id of an object, or 0 when unknown.
	uint16_t get_net_id(const TickSyncObject *p_object) const;
};

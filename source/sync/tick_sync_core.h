#pragma once

#include "../common/tick_data_buffer.h"
#include "../tick/tick_fixed_stepper.h"
#include "tick_engine.h"
#include "tick_protocol.h"

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
class TickSyncCore : public TickEngine {
public:
	enum Role {
		ROLE_NONE,
		ROLE_SERVER,
		ROLE_CLIENT,
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
		uint64_t rate_limited_packets = 0;
		uint64_t events_sent = 0;
		uint64_t events_received = 0;
		uint64_t events_rejected = 0;
		uint64_t spawns = 0;
		uint64_t despawns = 0;
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

	struct RateLimiter {
		double tokens = -1.0;
		uint64_t last_usec = 0;

		// Takes one token; tokens refill at `p_rate` per second, up to `p_rate`.
		bool take(double p_rate, uint64_t p_now_usec);
	};

	struct PendingEvent {
		uint32_t frame = TICK_FRAME_NONE;
		uint32_t requested_frame = TICK_FRAME_NONE;
		uint64_t sequence = 0;
		uint64_t expire_usec = 0;
		int sender = 0;
		uint16_t target = 0;
		StringName name;
		Variant payload;
	};

	struct SpawnRecord {
		String spawner;
		int scene = -1;
		String name;
		int controller = 0;
		Variant data;
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
		RateLimiter input_limiter;
		RateLimiter event_limiter;
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
	const TickEngine *clock_source = nullptr;

	// Server.
	HashMap<uint16_t, ServerObject> server_objects;
	LocalVector<uint16_t> server_object_ids;
	HashMap<TickSyncObject *, uint16_t> server_ids_by_object;
	uint16_t next_net_id = 1;
	HashMap<int, PeerState> peers;
	LocalVector<SnapshotRecord> server_history;
	int64_t server_epoch_usec = 0;
	// Net ids released recently, with the frame they were released at (ADR-034).
	HashMap<uint16_t, uint32_t> quarantined_ids;
	HashMap<uint32_t, SpawnRecord> spawns;
	LocalVector<uint32_t> spawn_order;
	uint32_t next_spawn_id = 1;

	// Events waiting for their frame (both roles) or for their target object (client).
	LocalVector<PendingEvent> pending_events;
	uint64_t next_event_sequence = 0;

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
	void server_handle_event(int p_peer, TickDataBuffer &p_message);
	void server_send_spawn(int p_peer, uint32_t p_spawn_id);
	// Local time of frame 0 of this server's timeline, from the frame it's at now (signed: a server following another
	// network's clock has frames older than its process).
	int64_t server_compute_epoch() const;
	void write_object_state(TickDataBuffer &r_message, const ServerObject &p_object, const LocalVector<Variant> &p_values, const LocalVector<Variant> *p_base) const;

	// Client.
	void client_handle_welcome(TickDataBuffer &p_message);
	void client_handle_reject(TickDataBuffer &p_message);
	void client_handle_register(TickDataBuffer &p_message);
	void client_handle_unregister(TickDataBuffer &p_message);
	void client_handle_snapshot(TickDataBuffer &p_message, bool p_full);
	void client_handle_pong(TickDataBuffer &p_message);
	void client_handle_spawn(TickDataBuffer &p_message);
	void client_handle_despawn(TickDataBuffer &p_message);
	void client_handle_event(TickDataBuffer &p_message);
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

	// Events.
	bool read_event(TickDataBuffer &p_message, PendingEvent &r_event, int &r_payload_bytes);
	void write_event(TickDataBuffer &r_message, uint16_t p_target, uint32_t p_frame, const StringName &p_name, const Variant &p_payload);
	void queue_event(const PendingEvent &p_event);
	// Executes the events due at `p_frame`, or every event whose target is available when `p_frame` is
	// `TICK_FRAME_NONE`.
	void run_events(uint32_t p_frame);
	bool execute_event(const PendingEvent &p_event);

public:
	virtual void set_settings(const Settings &p_settings) override;
	virtual const Settings &get_settings() const override { return settings; }
	const Stats &get_stats() const { return stats; }
	virtual Dictionary get_stats_dictionary() const override;
	virtual void set_listener(Listener *p_listener) override { listener = p_listener; }

	// The role comes from the transport: the `authority_peer` is the server.
	virtual Error start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec) override;
	virtual void stop() override;
	virtual bool is_running() const override { return role != ROLE_NONE; }
	Role get_role() const { return role; }
	virtual bool is_server() const override { return role == ROLE_SERVER; }
	virtual bool can_spawn() const override { return role == ROLE_SERVER; }

	// Server: objects are simulated and replicated. Client: objects are bound to the server's objects with the
	// same path, then predicted (controlled by this client) or interpolated.
	virtual void register_object(TickSyncObject *p_object) override;
	virtual void unregister_object(TickSyncObject *p_object) override;

	virtual void process(double p_delta, uint64_t p_now_usec) override;
	virtual void update_interpolation(uint64_t p_now_usec) override;

	virtual double get_timeline_frame(uint64_t p_now_usec) const override;
	virtual void set_clock_source(const TickEngine *p_source) override { clock_source = p_source; }

	virtual uint32_t get_frame() const override { return stepper.get_next_frame_index(); }
	virtual bool is_rewinding() const override { return rewinding; }
	virtual bool is_predicting() const override { return predicting; }
	bool is_welcomed() const { return welcomed; }
	virtual const TickClock &get_clock() const override { return clock; }
	double get_time_scale() const { return stepper.get_time_scale(); }
	uint32_t get_latest_snapshot_frame() const { return latest_snapshot; }
	virtual uint16_t get_net_id(const TickSyncObject *p_object) const override;
	// The authority simulates every object.
	virtual int get_owner(const TickSyncObject *p_object) const override { return settings.authority_peer; }

	// Server: records a spawn and sends it to the clients (and to the ones joining later). Call it before the
	// spawned objects are registered, so the clients create them before binding them.
	virtual uint32_t spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override;
	virtual void despawn(uint32_t p_spawn_id) override;
	virtual uint32_t get_next_spawn_id() const override { return next_spawn_id; }

	// Sends an event to `p_target` (or the network when null). Client: to the server. Server: to `p_peer`, or
	// every client with 0. `p_frame` schedules it (`TICK_FRAME_NONE`: see `notes/f3-design.md`).
	virtual Error send_event(TickSyncObject *p_target, const StringName &p_name, const Variant &p_payload, uint32_t p_frame, int p_peer) override;
	// A frame `p_seconds` after the current one, to schedule events that every peer runs at the same frame.
	virtual uint32_t get_event_frame(double p_seconds) const override;
};

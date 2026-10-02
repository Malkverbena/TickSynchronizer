// The engine of a network with a single authority: `TickSyncCore`.
//
// A star network with a single authority (the server, peer 1): the F2 engine. Design in `notes/f2-design.md`.
//
// - Server: simulates every object; the input of each client's objects comes from that client, and the input of the
//   server's objects is collected locally. Sends snapshots (delta over the last acknowledged one).
// - Client: predicts the objects it controls and rewinds them when the server disagrees; interpolates every other
//   object between snapshots, or simulates it as a doll with its controller's inputs (F6, ADR-045: in a mesh, the
//   controllers send their inputs to every peer).
//
// The core doesn't read the clock: `process()` receives the current time, so it runs the same with a real network and
// with the simulated one of the tests.
//
// When the network isn't trusted, what a peer costs the others is bounded (ADR-078): the rates of inputs, events and
// pings, their sizes, the frames a doll is simulated again, and how long a client that acknowledges nothing is served.

#pragma once

#include "../common/tick_data_buffer.h"
#include "../tick/tick_fixed_stepper.h"
#include "tick_engine.h"
#include "tick_protocol.h"

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

class TickSyncCore : public TickEngine {
public:
	enum Role {
		ROLE_NONE,
		ROLE_SERVER,
		ROLE_CLIENT,
	};

	// Counters of what happened since the engine started.
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
		// Dolls: rewinds after an authority snapshot of a frame already simulated disagreed; corrections of a frame
		// simulated after its snapshot arrived; restarts of the doll's timeline; frames simulated without the
		// controller's input.
		uint64_t doll_rewinds = 0;
		uint64_t doll_rewound_frames = 0;
		uint64_t doll_corrections = 0;
		uint64_t doll_resyncs = 0;
		uint64_t doll_ghost_inputs = 0;
		// Dolls left to the snapshots for a while: their controller's inputs disagreed with the authority more than
		// this peer pays to simulate again.
		uint64_t doll_suspensions = 0;
		// Delta snapshots sent in more than one part (bigger than a datagram), and partial ones the client dropped.
		uint64_t split_snapshots = 0;
		uint64_t incomplete_snapshots = 0;
		uint64_t relevance_changes = 0;
	};

private:
	typedef HashMap<uint16_t, LocalVector<Variant>> ObjectStates;

	// The state of every object at one frame: an entry of the server's history, or a snapshot a client received.
	struct SnapshotRecord {
		uint32_t frame = TICK_FRAME_NONE;
		ObjectStates states;
		// Client: objects whose state is known only partially (not decodable yet).
		bool complete = true;
	};

	// Client: the input it collected at a frame, and the state its predicted objects reached with it.
	struct PredictionRecord {
		uint32_t frame = TICK_FRAME_NONE;
		TickDataBuffer input;
		ObjectStates states;
	};

	// The input of one frame as it travels: the inputs of every object of its sender.
	struct InputRecord {
		uint32_t frame = TICK_FRAME_NONE;
		TickDataBuffer input;
	};

	// Server: an object it simulates and replicates.
	struct ServerObject {
		TickSyncObject *object = nullptr;
		String path;
		int controller = 0;
		uint32_t schema_hash = 0;
	};

	// Client: an object the server registered, bound to the local object with the same path once there is one.
	struct RemoteObject {
		String path;
		int controller = 0;
		uint32_t schema_hash = 0;
		TickSyncObject *object = nullptr;
		// Whether the server sends its state to this client (interest, ADR-053).
		bool relevant = true;
	};

	// Client: the snapshot being decoded into its record of `received`: in parts, when it was split.
	struct PendingSnapshot {
		uint32_t frame = TICK_FRAME_NONE;
		int part_count = 0;
		uint64_t parts_received = 0;
		int server_buffer = 0;
		bool missing_state = false;
	};

	// A token bucket for what an untrusted peer sends.
	struct RateLimiter {
		double tokens = -1.0;
		uint64_t last_usec = 0;

		// Takes one token; tokens refill at `p_rate` per second, up to `p_rate`.
		bool take(double p_rate, uint64_t p_now_usec);
	};

	// An event waiting for its frame, or for its target object.
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

	// A live spawn, kept for the clients that join later, and for a client that becomes the server.
	struct SpawnRecord {
		String spawner;
		int scene = -1;
		String name;
		int controller = 0;
		Variant data;
	};

	// The state of a peer's dolls at one frame of their timeline.
	struct DollRecord {
		uint32_t frame = TICK_FRAME_NONE;
		ObjectStates states;
	};

	// Client: the objects controlled by another peer that are simulated here as dolls, on a timeline of their own:
	// behind the local one by the latency to that peer plus an input buffer that absorbs its jitter (ADR-046).
	struct DollPeer {
		LocalVector<uint16_t> ids;
		LocalVector<InputRecord> inputs;
		LocalVector<DollRecord> history;
		bool started = false;
		uint32_t next_frame = TICK_FRAME_NONE;
		uint32_t last_received_frame = TICK_FRAME_NONE;
		uint64_t last_received_usec = 0;
		double step_accumulator = 0.0;
		// Local timeline frame minus the frame of the newest input, when it arrived; their spread is the jitter.
		LocalVector<double> arrival_offsets;
		uint32_t next_offset = 0;
		RateLimiter input_limiter;
		// The frames this peer still simulates again for the doll when the authority disagrees with it (refilled over
		// time); without them the doll is interpolated from the snapshots until `suspended_until_usec` (ADR-078).
		double rewind_budget = -1.0;
		uint64_t rewind_budget_usec = 0;
		uint64_t suspended_until_usec = 0;
	};

	// Server: what it knows about a client.
	struct PeerState {
		bool accepted = false;
		uint64_t reject_usec = 0;
		bool rejected = false;
		uint32_t acked_snapshot = TICK_FRAME_NONE;
		bool needs_full = true;
		uint32_t last_full_frame = TICK_FRAME_NONE;
		uint64_t last_full_usec = 0;
		// How long until a full snapshot the client didn't acknowledge is sent again: doubled at each one, back to the
		// shortest when an acknowledgment comes. `last_ack_usec`: when the client was accepted, or last acknowledged a
		// newer snapshot; a client that acknowledges nothing for long is dropped (ADR-078).
		uint64_t full_resend_usec = 0;
		uint64_t last_ack_usec = 0;
		LocalVector<InputRecord> inputs;
		TickDataBuffer last_input;
		bool has_last_input = false;
		uint32_t last_received_frame = TICK_FRAME_NONE;
		HashMap<uint16_t, TickDataBuffer> tick_inputs;
		RateLimiter input_limiter;
		RateLimiter event_limiter;
		RateLimiter ping_limiter;
		// Interest (ADR-053): the objects whose state this client gets, and the frame each became relevant at, until
		// the client acknowledges a snapshot that has it (the deltas need a base the client knows).
		HashSet<uint16_t> relevant;
		HashMap<uint16_t, uint32_t> relevant_since;
	};

	// Marks a call into the engine that may run game code (ticks, events, the listener). Game code may stop the
	// engine: `stop()` then waits until the outermost call returns, so nothing the engine is working on is freed
	// under it.
	class BusyScope {
		TickSyncCore *core = nullptr;

	public:
		// Enters a call that may run game code.
		explicit BusyScope(TickSyncCore *p_core);


		// Leaves the call; the outermost one stops the engine if game code asked for it.
		~BusyScope();
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
	int busy_depth = 0;
	bool stop_requested = false;

	// Server.
	HashMap<uint16_t, ServerObject> server_objects;
	LocalVector<uint16_t> server_object_ids;
	HashMap<TickSyncObject *, uint16_t> server_ids_by_object;
	uint16_t next_net_id = 1;
	HashMap<int, PeerState> peers;
	LocalVector<SnapshotRecord> server_history;
	int64_t server_epoch_usec = 0;
	// When the server's frames last advanced. The epoch is computed at that time: pings are answered before the frames
	// advance, and the time of the current frame would make the epoch vary with the frame time (ADR-071).
	uint64_t server_stepped_usec = 0;
	// Net ids released recently, with the frame they were released at (ADR-034).
	HashMap<uint16_t, uint32_t> quarantined_ids;
	HashMap<uint32_t, SpawnRecord> spawns;
	LocalVector<uint32_t> spawn_order;
	uint32_t next_spawn_id = 1;
	// The server's input for the objects it controls that are dolls on the clients.
	LocalVector<InputRecord> authority_inputs;

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
	PendingSnapshot pending_snapshot;
	// Objects written in the pending snapshot's record; the others are stale and removed once it's complete.
	LocalVector<uint16_t> written_ids;
	// Scratch buffers, kept to avoid allocating for every object of every snapshot.
	mutable TickDataBuffer scratch_payload;
	TickDataBuffer read_payload;
	LocalVector<PredictionRecord> predictions;
	// Whether the local predicted objects are dolls on the other peers: their inputs go to every peer.
	bool shares_inputs = false;
	HashMap<int, DollPeer> dolls;

	// The slot of a frame in the rings of history.
	int history_index(uint32_t p_frame) const { return int(p_frame % uint32_t(settings.history_size)); }


	// Seconds a tick lasts.
	double get_tick_delta() const { return 1.0 / double(settings.ticks_per_second); }


	// Running, and not asked to stop.
	bool is_active() const { return role != ROLE_NONE && !stop_requested; }


	// Frees the session's state: peers, histories, remote objects, spawns, pending events. The registered objects stay
	// for the next start.
	void stop_now();


	// Sends a message to a peer, or to all of them with `PEER_BROADCAST`, trimmed to what was written; nothing if the
	// peer already left.
	void send(int p_peer, TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message);


	// Reads every synchronized variable of an object into `r_values`.
	void read_states(TickSyncObject *p_object, LocalVector<Variant> &r_values) const;


	// Sets every variable of an object to the value its codec delivers, so every peer simulates from the same values.
	void quantize_object(TickSyncObject *p_object) const;


	// Splits the input of a frame into the input of each object, by net id; `false` if it's malformed.
	static bool parse_frame_input(TickDataBuffer &p_frame_input, HashMap<uint16_t, TickDataBuffer> &r_inputs);


	// Writes the inputs of consecutive frames starting at `p_first_frame`; consecutive identical inputs are sent once
	// with a duplicate count (NetworkSynchronizer `encode_inputs`).
	static void write_input_groups(TickDataBuffer &r_message, uint32_t p_first_frame, const LocalVector<const TickDataBuffer *> &p_frames);


	// Handles what the transport reported: connections, disconnections and a host migration.
	void handle_events();


	// Hands a received message to the handler of its type, for this peer's role. A client only takes state from the
	// server.
	void handle_packet(const TickTransport::Packet &p_packet);


	// Server: a client's handshake: accepts it if its protocol version and its precision match, rejects it otherwise.
	void server_handle_hello(int p_peer, TickDataBuffer &p_message);


	// Server: a client's inputs message: the snapshot it acknowledges, whether it wants a full one, and the inputs of
	// its recent frames, kept for the frames still to simulate.
	void server_handle_inputs(int p_peer, TickDataBuffer &p_message);


	// Server: answers a ping with its time and the epoch of its timeline. An untrusted client gets a limited number of
	// answers.
	void server_handle_ping(int p_peer, TickDataBuffer &p_message);


	// Server: welcomes a client: the tick rate and the epoch, the live spawns, then every object.
	void server_accept_peer(int p_peer);


	// Server: tells a peer why it's refused. It's disconnected a moment later, so the reason reaches it.
	void server_reject_peer(int p_peer, const String &p_reason);


	// Server: tells a client about an object: its id, path, controller and schema hash.
	void server_send_register(int p_peer, uint16_t p_net_id);


	// Server: gives an object a net id (never one still in quarantine) and registers it with every accepted client.
	void server_add_object(TickSyncObject *p_object);


	// Server: simulates one frame: resolves the clients' inputs, runs the events due, ticks every object with its
	// controller's input, records the states, updates the interest and sends the snapshots.
	void server_tick(uint32_t p_frame);


	// Server: takes a client's input for a frame, or repeats its last one (a ghost input), and splits it by object.
	// Oversized inputs of untrusted clients are dropped.
	void server_resolve_input(PeerState &r_peer, uint32_t p_frame);


	// Server: sends a client the state, at a frame, of the objects relevant to it: a delta over a snapshot the client
	// has, or a full one. A delta bigger than a datagram is split in parts.
	void server_send_snapshot(int p_peer, PeerState &r_peer, uint32_t p_frame);


	// Server: an event from a client: checks its rate, its size, who may send it and its frame, then queues it.
	void server_handle_event(int p_peer, TickDataBuffer &p_message);


	// Server: tells a client about a live spawn.
	void server_send_spawn(int p_peer, uint32_t p_spawn_id);


	// Server: sends the clients the inputs of the objects it controls that are dolls on them.
	void server_send_own_inputs(uint32_t p_frame);


	// Registers an object with a client and decides whether it's relevant to it.
	void server_register_for_peer(int p_peer, PeerState &r_peer, uint16_t p_net_id);


	// Server: starts or stops sending an object's state to a client, and tells it. An object stays relevant to its
	// controller.
	void server_set_peer_relevant(int p_peer, PeerState &r_peer, uint16_t p_net_id, bool p_relevant);


	// Server: asks the listener's filter about every object and every client.
	void server_update_relevance();


	// Local time of frame 0 of this server's timeline, from the frame it's at now (signed: a server following another
	// network's clock has frames older than its process).
	int64_t server_compute_epoch() const;


	// Writes an object's values, each after a bit that tells whether it changed from the base.
	void write_object_state(TickDataBuffer &r_message, const ServerObject &p_object, const LocalVector<Variant> &p_values, const LocalVector<Variant> *p_base) const;


	// The entry of one object in a snapshot: its id, whether it changed from the base, and the changed values.
	void write_snapshot_entry(TickDataBuffer &r_message, uint16_t p_net_id, const LocalVector<Variant> *p_values, const LocalVector<Variant> *p_base) const;


	// Client: the server accepted it: takes the tick rate and the epoch of the timeline.
	void client_handle_welcome(TickDataBuffer &p_message);


	// Client: the server refused it: reports the reason.
	void client_handle_reject(TickDataBuffer &p_message);


	// Client: the server registered an object: binds it to the local object with the same path, if there is one.
	void client_handle_register(TickDataBuffer &p_message);


	// Client: the server removed an object.
	void client_handle_unregister(TickDataBuffer &p_message);


	// Client: a snapshot, or a part of one: decodes it over its base into the history, and finishes it once every part
	// arrived.
	void client_handle_snapshot(TickDataBuffer &p_message, bool p_full);


	// Reads `p_count` object states of a snapshot into `r_states`; `false` if the message is malformed.
	bool client_read_snapshot_objects(TickDataBuffer &p_message, int p_count, const SnapshotRecord *p_base, ObjectStates &r_states, bool &r_missing_state);


	// Client: a snapshot is complete: it's acknowledged, the speed is adjusted, and the predicted objects and the dolls
	// are reconciled with it.
	void client_finish_snapshot(uint32_t p_frame, bool p_full, int p_server_buffer, bool p_missing_state);


	// Client: the server started or stopped sending an object's state.
	void client_handle_relevance(TickDataBuffer &p_message);


	// Client: the answer to a ping: a sample for the clock and the epoch of the timeline, when they're plausible.
	void client_handle_pong(TickDataBuffer &p_message);


	// Client: the server spawned something: keeps the record and tells the listener.
	void client_handle_spawn(TickDataBuffer &p_message);


	// Client: the server removed a spawn.
	void client_handle_despawn(TickDataBuffer &p_message);


	// Client: an event from the server: runs it now, or queues it for its frame, or until its target exists.
	void client_handle_event(TickDataBuffer &p_message);


	// Client: binds a registered object to the local object with the same path, if their schemas match, and runs the
	// events that waited for it.
	void client_bind(uint16_t p_net_id, RemoteObject &r_remote);


	// Client: works out again, when something changed, which objects it predicts and which are dolls of each peer.
	void client_update_predicted_ids();


	// Client: whether it's predicting the object.
	bool client_is_predicted(uint16_t p_net_id) const;


	// Client: starts predicting: jumps to the frame its inputs must be for, and puts the predicted objects at the
	// server's latest state.
	void client_start_prediction();


	// Client: the frame to predict now so that its inputs reach the server ahead of time: the server's frame, plus the
	// travel time and the input buffer.
	uint32_t client_compute_start_frame() const;


	// Client: speeds up or slows down so that the server keeps the wanted number of its inputs buffered; jumps when
	// it's too far off.
	void client_adjust_speed(int p_server_buffer);


	// Client: compares its prediction of a frame with the server's state; when they differ, takes the server's and
	// simulates the later frames again.
	void client_reconcile(uint32_t p_frame);


	// Client: predicts one frame: runs the events due, collects the inputs and ticks the predicted objects, then
	// advances the dolls.
	void client_tick(uint32_t p_frame);


	// Client: sends the server the newest snapshot it has, whether it needs a full one, and the inputs of its last
	// frames. In a mesh the inputs go to the other peers too, for their dolls.
	void client_send_inputs();


	// Client: sends a ping with its time.
	void client_send_ping();


	// Client: shows the objects it neither predicts nor simulates as dolls between the two snapshots around the view
	// frame.
	void client_update_interpolation();


	// Client: the snapshot received for a frame, or null.
	SnapshotRecord *client_get_received(uint32_t p_frame);


	// Client: the snapshot received for a frame, or null.
	const SnapshotRecord *client_get_received(uint32_t p_frame) const;


	// The state of an object at an exact frame, from the history of this peer (ADR-054), or null.
	const LocalVector<Variant> *find_state(uint16_t p_net_id, uint32_t p_frame) const;


	// The newest snapshot received before `p_frame`, or `TICK_FRAME_NONE`.
	uint32_t client_find_snapshot_before(uint32_t p_frame) const;


	// Client: the dolls of a peer (ADR-045, ADR-046), made when first needed.
	DollPeer &client_get_doll(int p_peer);


	// Client: inputs from the controller of dolls: keeps the ones near the local timeline, and measures the jitter of
	// their arrival.
	void client_handle_doll_inputs(int p_peer, TickDataBuffer &p_message);


	// Client: how many inputs to keep ahead of a doll: the minimum plus the jitter of their arrival.
	int client_get_doll_target(const DollPeer &p_doll) const;


	// Client: whether the object is being simulated as a doll right now.
	bool client_is_active_doll(uint16_t p_net_id) const;


	// Applies the snapshot of `p_snapshot_frame` to the doll, then simulates it up to `p_next_frame`.
	bool client_restore_doll(DollPeer &r_doll, uint32_t p_snapshot_frame, uint32_t p_next_frame);


	// Simulates one frame of the doll; if the authority's state of that frame already arrived, the doll takes it.
	void client_simulate_doll(DollPeer &r_doll, uint32_t p_frame);


	// Client: whether a doll's states at a frame equal the authority's, within the tolerance of the codecs.
	bool client_doll_matches(const DollPeer &p_doll, const ObjectStates &p_doll_states, const SnapshotRecord &p_snapshot) const;


	// Takes `p_frames` of the frames this peer simulates again for a doll; `false` when the doll doesn't have them.
	bool client_take_doll_budget(DollPeer &r_doll, int p_frames);


	// Client: advances each peer's dolls on their timeline, a little faster or slower to keep their input buffer;
	// starts, restarts or suspends them as needed.
	void client_advance_dolls();


	// Client: compares the dolls with the authority's state of a frame they already simulated, and rewinds the ones
	// that differ, within their budget.
	void client_reconcile_dolls(uint32_t p_frame);


	// Client: makes every doll start again, with its delay measured anew.
	void client_reset_dolls();


	// Host migration (ADR-062): the transport's host changed: this client becomes the server, or follows the new one.
	void handle_host_migrated(int p_new_host);


	// Client: becomes the server: keeps the net ids and the spawns, puts the objects at the freshest authoritative
	// state, and waits for the handshakes of the other peers.
	void client_become_server(int p_old_authority);


	// Client: starts over with the new authority: a new handshake and a new clock, with the objects still bound.
	void client_follow_authority(int p_new_authority);


	// Reads an event from a message; `false` if it's malformed. `r_payload_bytes` gets the size of the payload, which a
	// server checks before decoding it.
	bool read_event(TickDataBuffer &p_message, PendingEvent &r_event, int &r_payload_bytes);


	// Writes an event message.
	void write_event(TickDataBuffer &r_message, uint16_t p_target, uint32_t p_frame, const StringName &p_name, const Variant &p_payload);


	// Keeps an event until its frame, in order.
	void queue_event(const PendingEvent &p_event);


	// Executes the events due at `p_frame`, or every event whose target is available when `p_frame` is
	// `TICK_FRAME_NONE`.
	void run_events(uint32_t p_frame);


	// Runs an event on its target, or on the listener. `false` when the target doesn't exist yet and the event should
	// wait.
	bool execute_event(const PendingEvent &p_event);


public:
	// `TickEngine`: takes the settings if they're valid; not while running.
	virtual void set_settings(const Settings &p_settings) override;


	// `TickEngine`: the settings in use.
	virtual const Settings &get_settings() const override { return settings; }


	// The counters.
	const Stats &get_stats() const { return stats; }


	// `TickEngine`: the counters, the dolls' delays, the time scale and the frames, by name.
	virtual Dictionary get_stats_dictionary() const override;


	// `TickEngine`: sets who hears what happens.
	virtual void set_listener(Listener *p_listener) override { listener = p_listener; }


	// `TickEngine`: starts as the server or as a client. The role comes from the transport: the `authority_peer` is the
	// server.
	virtual Error start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec) override;


	// `TickEngine`: stops the engine. Called from game code the engine is running (a tick, an event, a signal), it
	// stops once that call returns; it isn't running anymore from now on.
	virtual void stop() override;


	// `TickEngine`: whether the engine is running and wasn't asked to stop.
	virtual bool is_running() const override { return is_active(); }


	// Whether this peer is the server, a client, or not running.
	Role get_role() const { return role; }


	// `TickEngine`: whether this peer is the server.
	virtual bool is_server() const override { return role == ROLE_SERVER; }


	// `TickEngine`: only the server spawns.
	virtual bool can_spawn() const override { return role == ROLE_SERVER; }


	// `TickEngine`: starts synchronizing an object, also before the engine starts. Server: objects are simulated and
	// replicated. Client: objects are bound to the server's objects with the same path, then predicted (controlled by
	// this client) or interpolated.
	virtual void register_object(TickSyncObject *p_object) override;


	// `TickEngine`: stops synchronizing an object. The server tells the clients and keeps the object's id in
	// quarantine.
	virtual void unregister_object(TickSyncObject *p_object) override;


	// `TickEngine`: receives, simulates the pending ticks, sends, and updates the interpolated objects.
	virtual void process(double p_delta, uint64_t p_now_usec) override;


	// `TickEngine`: client: updates the interpolated objects for the time given.
	virtual void update_interpolation(uint64_t p_now_usec) override;


	// `TickEngine`: the server's frame at the time given, with its fraction; negative on a client that doesn't know it
	// yet.
	virtual double get_timeline_frame(uint64_t p_now_usec) const override;


	// `TickEngine`: makes a server follow another engine's timeline instead of the local delta.
	virtual void set_clock_source(const TickEngine *p_source) override { clock_source = p_source; }


	// `TickEngine`: the next frame to simulate.
	virtual uint32_t get_frame() const override { return stepper.get_next_frame_index(); }


	// `TickEngine`: whether past frames are being simulated again.
	virtual bool is_rewinding() const override { return rewinding; }


	// `TickEngine`: client: whether it's predicting.
	virtual bool is_predicting() const override { return predicting; }


	// Client: whether the server accepted it.
	bool is_welcomed() const { return welcomed; }


	// `TickEngine`: the clock.
	virtual const TickClock &get_clock() const override { return clock; }


	// Client: the speed of its ticks, around 1.
	double get_time_scale() const { return stepper.get_time_scale(); }


	// Client: the frame of the newest snapshot received.
	uint32_t get_latest_snapshot_frame() const { return latest_snapshot; }


	// `TickEngine`: the net id of an object, or 0.
	virtual uint16_t get_net_id(const TickSyncObject *p_object) const override;


	// The authority simulates every object.
	virtual int get_owner(const TickSyncObject *p_object) const override { return settings.authority_peer; }


	// Client: how many frames the dolls of `p_peer` are behind the local timeline, or -1 when there are none.
	int get_doll_delay(int p_peer) const;


	// Interest (ADR-053): server only; `p_peer` 0 changes it for every client. An object is always relevant to its
	// controller.
	virtual Error set_relevant(TickSyncObject *p_object, int p_peer, bool p_relevant) override;


	// `TickEngine`: server: whether the object's state goes to `p_peer`; client: whether the server sends it here.
	virtual bool is_relevant(const TickSyncObject *p_object, int p_peer) const override;


	// `TickEngine`: history (ADR-054): the state of an object at a frame, interpolated between the nearest states
	// known.
	virtual bool get_state_at(const TickSyncObject *p_object, double p_frame, LocalVector<Variant> &r_values) const override;


	// `TickEngine`: the frame the interpolated objects show; on the server, the timeline's.
	virtual double get_view_frame(uint64_t p_now_usec) const override;


	// Server: records a spawn and sends it to the clients (and to the ones joining later). Call it before the
	// spawned objects are registered, so the clients create them before binding them.
	virtual uint32_t spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override;


	// `TickEngine`: server: removes a spawn and tells the clients.
	virtual void despawn(uint32_t p_spawn_id) override;


	// `TickEngine`: the id the next spawn gets.
	virtual uint32_t get_next_spawn_id() const override { return next_spawn_id; }


	// The server owns every spawn: a client that becomes the server takes over the old one's (it kept their records).
	virtual bool owns_spawn(uint32_t p_spawn_id) const override { return role == ROLE_SERVER && spawns.has(p_spawn_id); }


	// Sends an event to `p_target` (or the network when null). Client: to the server. Server: to `p_peer`, or
	// every client with 0. `p_frame` schedules it (`TICK_FRAME_NONE`: see `notes/f3-design.md`).
	virtual Error send_event(TickSyncObject *p_target, const StringName &p_name, const Variant &p_payload, uint32_t p_frame, int p_peer) override;


	// A frame `p_seconds` after the current one, to schedule events that every peer runs at the same frame.
	virtual uint32_t get_event_frame(double p_seconds) const override;
};

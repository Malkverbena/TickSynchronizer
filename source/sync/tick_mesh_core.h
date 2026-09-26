#pragma once

#include "../common/tick_data_buffer.h"
#include "../tick/tick_fixed_stepper.h"
#include "tick_engine.h"
#include "tick_protocol.h"

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

// Mesh with an owner per object (MESH + DISTRIBUTED, F5). Design in `notes/f5-design.md`.
//
// - Every object has an owner node, which simulates it and sends its state to the other nodes; the others
//   receive it (interpolated, or the latest state with `interpolate_remote` disabled).
// - The registry node (`registry_peer`) is the only one that writes the owners: it assigns the net ids, and every
//   change of owner, requested, released or assigned, goes through it and increases the object's version. State
//   with another version than the current one is discarded (ADR-041).
// - When an owner leaves, the registry marks its objects as orphaned; the project decides who adopts them.
// - The nodes follow the timeline of the clock master (`clock_master`, ADR-042).
//
// Meant for trusted networks of servers: there's no prediction, and forwarded events carry their origin (ADR-044).
class TickMeshCore : public TickEngine {
public:
	struct Stats {
		uint64_t states_sent = 0;
		uint64_t states_received = 0;
		uint64_t stale_states = 0;
		uint64_t malformed_packets = 0;
		uint64_t transfers = 0;
		uint64_t orphans = 0;
		uint64_t denied_requests = 0;
		uint64_t events_sent = 0;
		uint64_t events_received = 0;
		uint64_t events_forwarded = 0;
		uint64_t events_rejected = 0;
		uint64_t spawns = 0;
		uint64_t despawns = 0;
	};

	// Largest number of times an event is forwarded to the current owner of its target (ADR-044).
	static constexpr int MAX_EVENT_FORWARDS = 3;

private:
	struct Sample {
		uint32_t frame = TICK_FRAME_NONE;
		LocalVector<Variant> values;
	};

	// Every node's view of an object, updated only by the registry's announcements.
	struct Entry {
		String path;
		int owner = 0;
		uint32_t version = 0;
		// Frame of the last change of owner.
		uint32_t frame = 0;
		uint32_t schema_hash = 0;
		TickSyncObject *object = nullptr;
		// Owner: released to the registry, waiting for its announcement; not simulated nor sent.
		bool frozen = false;
		// Owner: the state last sent, to send only changes between keyframes.
		LocalVector<Variant> last_sent;
		// Receiver: newest state received, and the recent ones for interpolation (oldest first).
		uint32_t last_state_frame = TICK_FRAME_NONE;
		LocalVector<Sample> samples;
	};

	// The registry node's records: the truth about owners and versions.
	struct RegistryRecord {
		String path;
		int owner = 0;
		uint32_t version = 0;
		uint32_t frame = 0;
		uint32_t schema_hash = 0;
		// A transfer in progress: the new owner, who asked for it, whether the owner may refuse it, and when it
		// times out.
		int pending_to = -1;
		int pending_requester = 0;
		bool pending_forced = false;
		uint64_t pending_timeout_usec = 0;
	};

	struct PeerState {
		bool ready = false;
		bool rejected = false;
		uint64_t reject_usec = 0;
	};

	struct PendingEvent {
		uint32_t frame = TICK_FRAME_NONE;
		uint32_t requested_frame = TICK_FRAME_NONE;
		uint64_t sequence = 0;
		uint64_t expire_usec = 0;
		int sender = 0;
		int hops = 0;
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

	// Marks a call into the engine that may run game code (ticks, events, the listener). Game code may stop the
	// engine: `stop()` then waits until the outermost call returns, so nothing the engine is working on is freed
	// under it.
	class BusyScope {
		TickMeshCore *core = nullptr;

	public:
		explicit BusyScope(TickMeshCore *p_core);
		~BusyScope();
	};

	Settings settings;
	Stats stats;
	Listener *listener = nullptr;
	bool running = false;
	int busy_depth = 0;
	bool stop_requested = false;
	Ref<TickTransport> transport;
	int local_id = 0;
	TickFixedStepper stepper;
	TickClock clock;
	uint64_t now_usec = 0;
	const TickEngine *clock_source = nullptr;
	uint64_t last_ping_usec = 0;

	HashMap<String, TickSyncObject *> local_objects;
	HashMap<uint16_t, Entry> entries;
	HashMap<String, uint16_t> ids_by_path;
	HashMap<int, PeerState> peers;
	// Messages this node sends to itself (the registry's announcements to its own replica, for example).
	LocalVector<TickTransport::Packet> loopback;

	// Registry node.
	HashMap<uint16_t, RegistryRecord> registry;
	HashMap<String, uint16_t> registry_ids_by_path;
	uint16_t next_net_id = 1;
	HashMap<uint16_t, uint32_t> quarantined_ids;

	// Spawns made by this node.
	HashMap<uint32_t, SpawnRecord> spawns;
	LocalVector<uint32_t> spawn_order;
	uint32_t next_spawn_counter = 1;

	LocalVector<PendingEvent> pending_events;
	uint64_t next_event_sequence = 0;

	bool is_registry() const { return local_id == settings.registry_peer; }
	bool is_clock_master() const { return local_id == settings.clock_master; }
	double get_tick_delta() const { return 1.0 / double(settings.ticks_per_second); }
	bool is_peer_ready(int p_peer) const;
	// Running, and not asked to stop.
	bool is_active() const { return running && !stop_requested; }
	void stop_now();

	void send(int p_peer, TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message);
	void send_to_ready_peers(TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message, bool p_include_self);
	void read_values(TickSyncObject *p_object, LocalVector<Variant> &r_values) const;
	void write_values(TickDataBuffer &r_payload, TickSyncObject *p_object, const LocalVector<Variant> &p_values) const;
	bool read_payload_values(TickDataBuffer &p_payload, TickSyncObject *p_object, LocalVector<Variant> &r_values) const;
	void quantize_object(TickSyncObject *p_object) const;
	Entry *find_entry(const TickSyncObject *p_object, uint16_t *r_id = nullptr);
	const Entry *find_entry(const TickSyncObject *p_object, uint16_t *r_id = nullptr) const;
	void bind_entry(uint16_t p_id, Entry &r_entry);
	void claim(TickSyncObject *p_object);
	void claim_unbound_objects();

	void handle_events();
	void handle_packet(const TickTransport::Packet &p_packet);
	void handle_hello(int p_peer, TickDataBuffer &p_message);
	void send_hello(int p_peer);
	void on_peer_ready(int p_peer);
	void handle_ping(int p_peer, TickDataBuffer &p_message);
	void handle_pong(int p_peer, TickDataBuffer &p_message);
	void handle_announce(int p_peer, TickDataBuffer &p_message);
	void handle_unregister(int p_peer, TickDataBuffer &p_message);
	void handle_state(int p_peer, TickDataBuffer &p_message);
	void handle_transfer(int p_peer, TickDataBuffer &p_message);
	void handle_denied(int p_peer, TickDataBuffer &p_message);
	void handle_spawn(int p_peer, TickDataBuffer &p_message);
	void handle_despawn(int p_peer, TickDataBuffer &p_message);
	void handle_mesh_event(int p_peer, TickDataBuffer &p_message);
	void send_spawn(int p_peer, uint32_t p_spawn_id);
	int64_t compute_epoch() const;

	// Registry node.
	void registry_handle_claim(int p_peer, const String &p_path, int p_owner, uint32_t p_schema_hash);
	void registry_handle_drop(int p_peer, uint16_t p_id);
	void registry_handle_request(int p_peer, uint16_t p_id);
	void registry_handle_assign(int p_peer, uint16_t p_id, int p_target);
	void registry_handle_release(int p_peer, TickDataBuffer &p_message);
	void registry_change_owner(uint16_t p_id, int p_new_owner, uint32_t p_frame, const TickDataBuffer *p_state);
	void registry_send_announce(int p_peer, uint16_t p_id, const TickDataBuffer *p_state);
	void registry_deny(int p_peer, uint16_t p_id);
	void registry_on_peer_left(int p_peer);
	void registry_check_timeouts();
	bool registry_local_state(uint16_t p_id, TickDataBuffer &r_state) const;

	void tick(uint32_t p_frame);
	void follow_timeline(double p_target_frame);
	void send_states(uint32_t p_frame);
	void release_frozen(uint16_t p_id, Entry &r_entry, int p_to);

	// Events.
	void queue_event(const PendingEvent &p_event);
	void run_events(uint32_t p_frame);
	// Runs, forwards or keeps an event; returns `false` to keep it.
	bool dispatch_event(PendingEvent &r_event);
	void forward_event(int p_peer, const PendingEvent &p_event);

public:
	virtual void set_settings(const Settings &p_settings) override;
	virtual const Settings &get_settings() const override { return settings; }
	const Stats &get_stats() const { return stats; }
	virtual Dictionary get_stats_dictionary() const override;
	virtual void set_listener(Listener *p_listener) override { listener = p_listener; }

	virtual Error start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec) override;
	// Called from game code the engine is running (a tick, an event, a signal), the engine stops once that call
	// returns; it isn't running anymore from now on.
	virtual void stop() override;
	virtual bool is_running() const override { return is_active(); }

	virtual void register_object(TickSyncObject *p_object) override;
	virtual void unregister_object(TickSyncObject *p_object) override;
	virtual uint16_t get_net_id(const TickSyncObject *p_object) const override;

	virtual void process(double p_delta, uint64_t p_now_usec) override;
	virtual void update_interpolation(uint64_t p_now_usec) override;

	virtual uint32_t get_frame() const override { return stepper.get_next_frame_index(); }
	virtual double get_timeline_frame(uint64_t p_now_usec) const override;
	virtual void set_clock_source(const TickEngine *p_source) override { clock_source = p_source; }
	virtual const TickClock &get_clock() const override { return clock; }
	virtual bool can_spawn() const override { return running; }

	// Spawn ids carry the id of the node that spawned (below 4096) in their high bits.
	virtual uint32_t spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override;
	virtual void despawn(uint32_t p_spawn_id) override;
	virtual uint32_t get_next_spawn_id() const override;

	// Object events go to the target's current owner (forwarded if it changed on the way); events without target
	// go to `p_peer`, or every node with 0.
	virtual Error send_event(TickSyncObject *p_target, const StringName &p_name, const Variant &p_payload, uint32_t p_frame, int p_peer) override;
	virtual uint32_t get_event_frame(double p_seconds) const override;

	// Owner of an object, 0 when it has none or it isn't registered yet.
	virtual int get_owner(const TickSyncObject *p_object) const override;
	virtual Error request_authority(TickSyncObject *p_object) override;
	// `p_to_peer` 0 leaves the object orphaned.
	virtual Error release_authority(TickSyncObject *p_object, int p_to_peer) override;
	virtual Error assign_authority(TickSyncObject *p_object, int p_peer) override;
	uint32_t get_version(const TickSyncObject *p_object) const;
};

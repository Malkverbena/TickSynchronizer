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
// - Both roles move while the mesh runs (ADR-073): the project changes them, or another node takes them when their
//   node leaves. A registry that takes over merges every node's view of the objects first.
// - Who takes them (ADR-074): the first of `role_candidates` in the mesh (any node, the lowest id first, without
//   candidates), once no node it's connected to still sees the role's node, and only while connected to
//   `role_quorum` nodes. A role belongs to a process of a node, not just to its id: a node with a role that restarts
//   lost what it knew, and takes the role again like any successor, from the other nodes' views and their timeline.
// - The spawns of a node that left belong to the registry.
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
		uint64_t role_changes = 0;
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

	// What a node says about the process with a role (ADR-074).
	enum RoleView {
		// It doesn't know which process has the role yet (it never met the role's node).
		ROLE_VIEW_UNKNOWN,
		// Connected to it (or it's this process).
		ROLE_VIEW_SEEN,
		// Not connected to it: it left, or another process took the node's place.
		ROLE_VIEW_LOST,
	};

	struct RoleStatus {
		// The roles the views are about.
		uint32_t term = 0;
		int registry = 0;
		int clock = 0;
		// For each role: the process (`boot_id`) the sender knows as its holder, and whether it sees it.
		int registry_view = ROLE_VIEW_UNKNOWN;
		uint32_t registry_boot = 0;
		int clock_view = ROLE_VIEW_UNKNOWN;
		uint32_t clock_boot = 0;

		bool operator==(const RoleStatus &p_other) const {
			return term == p_other.term && registry == p_other.registry && clock == p_other.clock && registry_view == p_other.registry_view && registry_boot == p_other.registry_boot && clock_view == p_other.clock_view && clock_boot == p_other.clock_boot;
		}
	};

	struct PeerState {
		bool ready = false;
		bool rejected = false;
		uint64_t reject_usec = 0;
		// The process of the node, from its hello.
		uint32_t boot = 0;
		// What it last said about the roles.
		bool status_known = false;
		RoleStatus status;
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
		// The node that spawned it.
		int origin = 0;
		// That node left: the spawn belongs to the registry (ADR-074).
		bool adopted = false;
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
	// Clock master: when its frames last advanced; the epoch is computed at that time (see `TickSyncCore`).
	uint64_t stepped_usec = 0;
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

	// The spawns of every node, in the order they were known here. Each node sends its own to the nodes that join; the
	// registry sends the ones of the nodes that left.
	HashMap<uint32_t, SpawnRecord> spawns;
	LocalVector<uint32_t> spawn_order;
	uint32_t next_spawn_counter = 1;

	LocalVector<PendingEvent> pending_events;
	uint64_t next_event_sequence = 0;

	// Roles (ADR-073). An assignment wins over another with a higher term; with the same term, the one with the lower
	// registry id, then the lower clock master id.
	uint32_t roles_term = 0;
	// This process, in the hellos: a node that restarts is another process with the same id.
	uint32_t boot_id = 0;
	// The process that holds each role (0 until this node learns it): the roles are lost while this node isn't
	// connected to that process, and then it asks nothing of the role's node and takes nothing from it.
	uint32_t registry_boot = 0;
	uint32_t clock_boot = 0;
	// The views last sent to the other nodes; sent again when they change, and to the nodes that join.
	bool status_sent = false;
	RoleStatus sent_status;
	// The mesh's timeline as another node last told it, for a clock master that doesn't know it (it restarted).
	bool reference_valid = false;
	double reference_frame = 0.0;
	uint64_t reference_usec = 0;
	// Whether this node's frames come from the mesh's timeline (not from a process that restarted as the clock).
	bool timeline_trusted = false;
	bool had_quorum = true;
	// Registry: nodes left while it couldn't act (no quorum); their objects are orphaned when it can again.
	bool registry_orphan_check = false;
	// A registry that took over merges the other nodes' views until they all reported or the settle time passed; the
	// messages for the registry wait meanwhile.
	bool registry_settling = false;
	uint64_t registry_settle_usec = 0;
	HashSet<int> registry_unreported;
	LocalVector<TickTransport::Packet> registry_deferred;
	// Objects this node asked the registry for (requests, assignments) without an answer yet.
	HashSet<uint16_t> pending_requests;
	// The highest net id this node heard of: a registry that takes over doesn't give it again.
	uint16_t highest_net_id = 0;

	bool is_registry() const { return local_id == settings.registry_peer; }
	bool is_clock_master() const { return local_id == settings.clock_master; }
	// The registry answers: it finished taking over, its role isn't in doubt and it has its quorum. Otherwise the
	// messages for it wait.
	bool registry_can_act() const { return is_registry() && !registry_settling && registry_boot == boot_id && has_role_quorum(); }
	// The process with the registry is this one or connected.
	bool is_registry_reachable() const { return get_role_view(settings.registry_peer, registry_boot) == ROLE_VIEW_SEEN; }
	int get_ready_count() const;
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

	// Roles.
	static bool roles_beat(uint32_t p_term, int p_registry, int p_clock, uint32_t p_other_term, int p_other_registry, int p_other_clock);
	// `p_resync`: this node joined a mesh that moved its roles meanwhile, so its view of the objects starts over.
	void adopt_roles(uint32_t p_term, int p_registry, int p_clock, uint32_t p_registry_boot, uint32_t p_clock_boot, bool p_resync);
	void send_roles(int p_peer, uint32_t p_term, int p_registry, int p_clock, uint32_t p_registry_boot, uint32_t p_clock_boot);
	// The process of a node (this one, or a connected one); 0 if unknown.
	uint32_t get_node_boot(int p_node) const;
	// Forgets what this node knew about the objects: it learns them again from the registry.
	void forget_objects();
	void handle_roles(int p_peer, TickDataBuffer &p_message);
	// Whether the process with a role is this one or connected to it.
	bool is_role_process_present(int p_node, uint32_t p_boot) const;
	int get_role_view(int p_node, uint32_t p_boot) const;
	bool is_registry_lost() const { return get_role_view(settings.registry_peer, registry_boot) == ROLE_VIEW_LOST; }
	bool is_clock_lost() const { return get_role_view(settings.clock_master, clock_boot) == ROLE_VIEW_LOST; }
	// The node that should take a lost role, or 0 if none of the candidates is in the mesh.
	int get_role_successor() const;
	void fill_vacant_roles();
	// Another node, with the same roles, knows other processes as their holders.
	void learn_role_boots(uint32_t p_registry_boot, uint32_t p_clock_boot);
	RoleStatus make_role_status() const;
	void send_role_status(int p_peer, const RoleStatus &p_status);
	void handle_role_status(int p_peer, TickDataBuffer &p_message);
	// After the events and the packets of a step: tells the other nodes what changed, and acts on what they said.
	void update_roles();
	void registry_defer(const TickTransport::Packet &p_packet);
	void registry_resume();
	void send_registry_report();
	void handle_registry_report(int p_peer, TickDataBuffer &p_message);
	void registry_take_over();
	void registry_finish_take_over();

	void tick(uint32_t p_frame);
	void follow_timeline(double p_target_frame);
	void send_states(uint32_t p_frame);
	void send_state_message(TickDataBuffer &r_message, int p_count);
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
	// This node's spawns, and for the registry the ones of the nodes that left.
	virtual bool owns_spawn(uint32_t p_spawn_id) const override;

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
	// Moves the registry and the clock master to connected nodes (this one included) while the mesh runs.
	virtual Error change_roles(int p_registry, int p_clock_master) override;
	virtual bool has_role_quorum() const override;
	uint32_t get_roles_term() const { return roles_term; }
};

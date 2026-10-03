// The engine of a mesh with distributed authority: `TickMeshCore`.
//
// A mesh with an owner per object (MESH + DISTRIBUTED, F5). Design in `notes/f5-design.md`.
//
// - Every object has an owner node, which simulates it and sends its state to the other nodes; the others receive it
//   (interpolated, or the latest state with `interpolate_remote` disabled).
// - The registry node (`registry_peer`) is the only one that writes the owners: it assigns the net ids, and every
//   change of owner, requested, released or assigned, goes through it and increases the object's version. State with
//   another version than the current one is discarded (ADR-041).
// - When an owner leaves, the registry marks its objects as orphaned; the project decides who adopts them.
// - The nodes follow the timeline of the clock master (`clock_master`, ADR-042).
// - Both roles move while the mesh runs (ADR-073): the project changes them, or another node takes them when their node
//   leaves. A registry that takes over merges every node's view of the objects first.
// - Who takes them (ADR-074): the first of `role_candidates` in the mesh (any node, the lowest id first, without
//   candidates), once no node it's connected to still sees the role's node, and only while connected to `role_quorum`
//   nodes. A role belongs to a process of a node, not just to its id: a node with a role that restarts lost what it
//   knew, and takes the role again like any successor, from the other nodes' views and their timeline.
// - The spawns of a node that left belong to the registry.
//
// Meant for trusted networks of servers: there's no prediction, and forwarded events carry their origin (ADR-044). What
// a node believes of the others is still bounded where a wrong value would stop the mesh (ADR-079): every release gets
// an answer, terms and versions are serial numbers, and clocks and frames no running mesh has are refused. And what a
// node told the others doesn't hold the mesh once it's gone (ADR-081): the registry's own object is the reference for
// the schema of a path, a role of a process nobody knows is vacant when its node isn't in the mesh, and an object whose
// owner isn't in the mesh goes to whoever asks for it.

#pragma once

#include "../common/tick_data_buffer.h"
#include "../tick/tick_fixed_stepper.h"
#include "tick_engine.h"
#include "tick_protocol.h"

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

class TickMeshCore : public TickEngine {
public:
	// Counters of what happened since the engine started.
	struct Stats {
		uint64_t states_sent = 0;
		uint64_t states_received = 0;
		uint64_t stale_states = 0;
		// States dropped by their frame: a newer one of the object had arrived (datagrams change places on the way), or
		// the frame is far ahead of this node's timeline. Many of them in a row mean the nodes' timelines are apart.
		uint64_t late_states = 0;
		uint64_t malformed_packets = 0;
		// Messages that are well formed but came for a role or a state this node doesn't have: from a node that isn't
		// ready, or for a registry or a clock that moved meanwhile. A few are expected whenever the roles change.
		uint64_t unexpected_packets = 0;
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
	// A state received from an object's owner, with its frame.
	struct Sample {
		uint32_t frame = TICK_FRAME_NONE;
		LocalVector<Variant> values;
	};

	// Every node's view of an object, updated only by the registry's announcements.
	struct Entry {
		String path;
		int owner = 0;
		uint32_t version = 0;
		// The registry that last announced it (see `registry_epoch`): what the current one announces wins over what
		// another one did, whatever the versions.
		uint32_t registry_epoch = 0;
		// Frame of the last change of owner.
		uint32_t frame = 0;
		uint32_t schema_hash = 0;
		TickSyncObject *object = nullptr;
		// Owner: released to the registry, waiting for its answer (an announcement with the new owner, or a denial);
		// not simulated nor sent. `release_own`: the game asked for it, so it's told when the release doesn't happen.
		bool frozen = false;
		bool release_own = false;
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
		// The version before the registry announced the object again without changing its owner (0: none): a release
		// its owner sent for that version is still good.
		uint32_t previous_version = 0;
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

	// What a node tells the others about the roles: the ones it knows, and whether it sees the processes that hold
	// them.
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

		// Whether two views say the same.
		bool operator==(const RoleStatus &p_other) const {
			return term == p_other.term && registry == p_other.registry && clock == p_other.clock && registry_view == p_other.registry_view && registry_boot == p_other.registry_boot && clock_view == p_other.clock_view && clock_boot == p_other.clock_boot;
		}
	};

	// What this node knows about another node.
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

	// An event waiting for its frame, or for its target to have an owner.
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

	// A live spawn of any node, kept for the nodes that join later.
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
		// Enters a call that may run game code.
		explicit BusyScope(TickMeshCore *p_core);


		// Leaves the call; the outermost one stops the engine if game code asked for it.
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
	// Counts the registries this node knew, one after the other: another node, or another process of the same node.
	// Their versions of an object can't be compared.
	uint32_t registry_epoch = 0;
	// The registry (see `registry_epoch`) this node last sent its view of the objects to.
	uint32_t reported_epoch = 0;
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
	// The nodes that were in the mesh and left, until they come back: the registry doesn't make one of them the first
	// owner of an object (its objects were orphaned when it left).
	HashSet<int> departed;

	// Whether this node is the registry's node.
	bool is_registry() const { return local_id == settings.registry_peer; }


	// Whether this node is the clock master's node.
	bool is_clock_master() const { return local_id == settings.clock_master; }


	// The registry answers: it finished taking over, its role isn't in doubt and it has its quorum. Otherwise the
	// messages for it wait.
	bool registry_can_act() const { return is_registry() && !registry_settling && registry_boot == boot_id && has_role_quorum(); }


	// The process with the registry is this one or connected.
	bool is_registry_reachable() const { return get_role_view(settings.registry_peer, registry_boot) == ROLE_VIEW_SEEN; }


	// How many other nodes passed the handshake.
	int get_ready_count() const;


	// Seconds a tick lasts.
	double get_tick_delta() const { return 1.0 / double(settings.ticks_per_second); }


	// Whether a node passed the handshake; this node always did.
	bool is_peer_ready(int p_peer) const;


	// Running, and not asked to stop.
	bool is_active() const { return running && !stop_requested; }


	// Frees the session's state: nodes, entries, the registry's records, spawns, pending events and messages. The
	// registered objects stay for the next start.
	void stop_now();


	// Sends a message to a node. One for this node itself is queued, and handled in the same step.
	void send(int p_peer, TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message);


	// Sends a message to every node that passed the handshake and, with `p_include_self`, to this node.
	void send_to_ready_peers(TickChannel p_channel, TickTransport::TransferMode p_mode, TickDataBuffer &p_message, bool p_include_self);


	// Reads every synchronized variable of an object into `r_values`.
	void read_values(TickSyncObject *p_object, LocalVector<Variant> &r_values) const;


	// Writes an object's values with their codecs.
	void write_values(TickDataBuffer &r_payload, TickSyncObject *p_object, const LocalVector<Variant> &p_values) const;


	// Reads an object's values with its codecs; `false` if the payload is malformed.
	bool read_payload_values(TickDataBuffer &p_payload, TickSyncObject *p_object, LocalVector<Variant> &r_values) const;


	// Sets every variable of an object to the value its codec delivers, so the owner simulates from what the others
	// receive.
	void quantize_object(TickSyncObject *p_object) const;


	// The entry bound to a local object, with its id in `r_id`; null when it has none.
	Entry *find_entry(const TickSyncObject *p_object, uint16_t *r_id = nullptr);


	// The entry bound to a local object, with its id in `r_id`; null when it has none.
	const Entry *find_entry(const TickSyncObject *p_object, uint16_t *r_id = nullptr) const;


	// Binds an entry to the local object with its path, if there is one and their schemas match.
	void bind_entry(uint16_t p_id, Entry &r_entry);


	// Asks the registry to register an object, once the registry's process is in reach.
	void claim(TickSyncObject *p_object);


	// Claims every local object that has no entry bound to it.
	void claim_unbound_objects();


	// Handles what the transport reported: a new node gets this node's hello; when a node leaves, its spawns go to the
	// registry and, on the registry, its objects are orphaned.
	void handle_events();


	// Hands a received message to its handler, checking who may send it: the registry's messages come only from the
	// registry's process, and the ones for the registry are taken only when this node has the role and can act.
	void handle_packet(const TickTransport::Packet &p_packet);


	// Another node's handshake: checks the version, the precision, the roles and the candidates, adopts newer roles,
	// and marks the node as ready.
	void handle_hello(int p_peer, TickDataBuffer &p_message);


	// Sends this node's handshake: the protocol version, the precision, the roles as it knows them, its process and the
	// hash of its candidates.
	void send_hello(int p_peer);


	// A node passed the handshake: it gets the registry's announcements and the live spawns from the nodes that own
	// them, and this node's view of the roles.
	void on_peer_ready(int p_peer);


	// Clock master: answers a ping with its time and the epoch of the timeline.
	void handle_ping(int p_peer, TickDataBuffer &p_message);


	// The clock master's answer to a ping: a sample for the clock and the epoch, when they're plausible.
	void handle_pong(int p_peer, TickDataBuffer &p_message);


	// The registry's word about an object: its owner, its version and, when the owner changes, the state to continue
	// from. Updates this node's entry and tells the game about a change of owner.
	void handle_announce(int p_peer, TickDataBuffer &p_message);


	// The registry removed an object.
	void handle_unregister(int p_peer, TickDataBuffer &p_message);


	// States from an owner: kept for interpolation when they come from the registered owner, with the current version,
	// newer than the last one and not far ahead of this node's timeline.
	void handle_state(int p_peer, TickDataBuffer &p_message);


	// The registry asks this owner to hand an object to another node. The object may refuse a request, never an
	// assignment; otherwise it's released.
	void handle_transfer(int p_peer, TickDataBuffer &p_message);


	// The registry didn't do what this node asked: an object frozen for a release is taken back, and the game is told
	// when the request was its own.
	void handle_denied(int p_peer, TickDataBuffer &p_message);


	// Another node spawned something, or the registry sends the spawn of a node that left.
	void handle_spawn(int p_peer, TickDataBuffer &p_message);


	// A spawn was removed by its node, or by the registry.
	void handle_despawn(int p_peer, TickDataBuffer &p_message);


	// An event from another node, sent or forwarded: validated and queued for its frame.
	void handle_mesh_event(int p_peer, TickDataBuffer &p_message);


	// Tells a node about a live spawn.
	void send_spawn(int p_peer, uint32_t p_spawn_id);


	// Whether this node knows a spawn whose origin left, now the registry's (ADR-082): it reports these to a registry,
	// so one that takes over as a fresh process learns them.
	bool has_adopted_spawns() const;


	// Clock master: the local time of frame 0 of its timeline, from the frame it's at now.
	int64_t compute_epoch() const;


	// Registry: a node claims an object. A new path gets an id, an owner (the controller, or the claimant) and an
	// announcement to every node; a known one is announced again to the claimant (to every node, when its schema hash
	// had to be corrected).
	void registry_handle_claim(int p_peer, const String &p_path, int p_owner, uint32_t p_schema_hash);


	// Registry: an owner's object is gone: forgets it, keeps its id in quarantine and tells every node.
	void registry_handle_drop(int p_peer, uint16_t p_id);


	// Registry: a node asks for an object. An orphan goes to it at once; otherwise the owner is asked to hand it over.
	void registry_handle_request(int p_peer, uint16_t p_id);


	// Registry: a node assigns an object to another one: the owner is told to hand it over, and can't refuse.
	void registry_handle_assign(int p_peer, uint16_t p_id, int p_target);


	// Registry: an owner releases an object, or refuses a request for it. Every release gets an answer: an announcement
	// with the new owner, or a denial.
	void registry_handle_release(int p_peer, TickDataBuffer &p_message);


	// Registry: writes the new owner of an object, with a new version, and announces it to every node. A pending
	// transfer to someone else is denied.
	void registry_change_owner(uint16_t p_id, int p_new_owner, uint32_t p_frame, const TickDataBuffer *p_state);


	// Registry: announces an object to a node, or to every node with 0, with the state to continue from when there is
	// one.
	void registry_send_announce(int p_peer, uint16_t p_id, const TickDataBuffer *p_state);


	// Registry: tells a node that what it asked for an object won't happen.
	void registry_deny(int p_peer, uint16_t p_id);


	// A release this registry can't take now: its owner is told, so the object doesn't stay frozen.
	void registry_refuse_release(const TickTransport::Packet &p_packet);


	// Registry: a node left: its objects are orphaned, and the transfers it took part in are cancelled.
	void registry_on_peer_left(int p_peer);


	// Registry: cancels the transfers their owner didn't answer in time.
	void registry_check_timeouts();


	// Registry: writes the state this node has of an object, to send with an announcement; `false` when it has none.
	bool registry_local_state(uint16_t p_id, TickDataBuffer &r_state) const;


	// Registry: the schema hash to record for a path: the one of this node's own object there, when it has one (every
	// node of a mesh declares the same variables for a path, so the registry's object is the reference); else the one
	// a node told it.
	uint32_t registry_schema_hash(const String &p_path, uint32_t p_told) const;


	// Whether an assignment of the roles wins over another: the newer term; with the same term, the lower registry id,
	// then the lower clock master id.
	static bool roles_beat(uint32_t p_term, int p_registry, int p_clock, uint32_t p_other_term, int p_other_registry, int p_other_clock);


	// Takes an assignment of the roles: this node becomes, or stops being, the registry and the clock master, follows
	// the new clock, and reports its objects to the new registry. `p_resync`: this node joined a mesh that moved its
	// roles meanwhile, so its view of the objects starts over.
	void adopt_roles(uint32_t p_term, int p_registry, int p_clock, uint32_t p_registry_boot, uint32_t p_clock_boot, bool p_resync);


	// Sends an assignment of the roles to a node, or to every ready node with 0.
	void send_roles(int p_peer, uint32_t p_term, int p_registry, int p_clock, uint32_t p_registry_boot, uint32_t p_clock_boot);


	// The process of a node (this one, or a connected one); 0 if unknown.
	uint32_t get_node_boot(int p_node) const;


	// Forgets what this node knew about the objects: it learns them again from the registry.
	void forget_objects();


	// An assignment of the roles from another node: adopted and passed on when it wins over the one known here;
	// answered with the current one when it's older.
	void handle_roles(int p_peer, TickDataBuffer &p_message);


	// Whether the process with a role is this one or connected to it.
	bool is_role_process_present(int p_node, uint32_t p_boot) const;


	// What this node sees of the process with a role: unknown, seen or lost.
	int get_role_view(int p_node, uint32_t p_boot) const;


	// Whether this node knows the registry's process and isn't connected to it.
	bool is_registry_lost() const { return get_role_view(settings.registry_peer, registry_boot) == ROLE_VIEW_LOST; }


	// Whether this node knows the clock master's process and isn't connected to it.
	bool is_clock_lost() const { return get_role_view(settings.clock_master, clock_boot) == ROLE_VIEW_LOST; }


	// Whether a role has to be taken by a successor: its process is lost, or nobody can tell which process it is (the
	// roles moved, this node never met that process, and the role's node isn't in the mesh).
	bool is_role_vacant(int p_node, uint32_t p_boot) const;


	// The node that should take a lost role, or 0 if none of the candidates is in the mesh.
	int get_role_successor() const;


	// Takes the vacant roles when this node is their successor, has its quorum, and no node it's connected to still
	// sees the process that had them.
	void fill_vacant_roles();


	// Another node, with the same roles, knows other processes as their holders.
	void learn_role_boots(uint32_t p_registry_boot, uint32_t p_clock_boot);


	// This node's view of the roles, to tell the others.
	RoleStatus make_role_status() const;


	// Sends a view of the roles to a node, or to every ready node with 0, with the timeline's frame when this node
	// follows it.
	void send_role_status(int p_peer, const RoleStatus &p_status);


	// Another node's view of the roles: kept to decide who takes a lost role. Its frame is the reference for a clock
	// master that takes over.
	void handle_role_status(int p_peer, TickDataBuffer &p_message);


	// After the events and the packets of a step: tells the other nodes what changed, and acts on what they said.
	void update_roles();


	// Registry: keeps a message it can't answer yet. When too many wait, new ones are dropped, and a release is refused
	// so its object isn't left frozen.
	void registry_defer(const TickTransport::Packet &p_packet);


	// Registry: it can act again: orphans the objects of the nodes that left meanwhile, and handles the messages that
	// waited.
	void registry_resume();


	// Sends this node's view of the objects to the registry, in as many messages as it takes.
	void send_registry_report();


	// What this node owes a registry it reaches: its view of the objects, once for each registry, and the claims of its
	// objects that have no id.
	void sync_with_registry();


	// Registry: a node's view of the objects, where the newest version of each object wins. While taking over it's
	// merged; later, each newer record is announced as it comes, and the node gets the registry's view back.
	void handle_registry_report(int p_peer, TickDataBuffer &p_message);


	// This node becomes the registry: it starts from its own view of the objects, and waits for the other nodes' views.
	void registry_take_over();


	// Registry: the views are in, or the time is up: announces every object again with a new version, orphans the ones
	// whose owner left, takes the spawns of the nodes that left, and answers the messages that waited.
	void registry_finish_take_over();


	// Simulates one frame: runs the events due, ticks the objects this node owns with their own input, and sends their
	// states.
	void tick(uint32_t p_frame);


	// Simulates the frames up to the timeline's current one; jumps when it's too far off.
	void follow_timeline(double p_target_frame);


	// Sends the state of the objects this node owns that changed, or of all of them on a keyframe, in as many messages
	// as it takes.
	void send_states(uint32_t p_frame);


	// Writes the count of a state message and sends it to every ready node.
	void send_state_message(TickDataBuffer &r_message, int p_count);


	// Owner: stops simulating an object and sends the registry its release, with the last simulated frame and the state
	// at it.
	void release_frozen(uint16_t p_id, Entry &r_entry, int p_to);


	// The releases in progress won't be answered (the registry moved, or it's gone): the objects stay with this node.
	// The paths of the ones the game asked to release are added to `r_denied_paths`, to tell it.
	void cancel_releases(LocalVector<String> &r_denied_paths);


	// Tells the game about the requests and the releases that won't happen. Game code: it may stop the engine.
	void notify_denied(const LocalVector<String> &p_paths);


	// The frames of the timeline jumped: the states received until now are of another stretch of it.
	void reset_received_states();


	// Keeps an event until its frame, in order.
	void queue_event(const PendingEvent &p_event);


	// Runs the events due at a frame; the ones whose target has no owner yet stay.
	void run_events(uint32_t p_frame);


	// Runs, forwards or keeps an event; returns `false` to keep it.
	bool dispatch_event(PendingEvent &r_event);


	// Sends an event to a node: the owner of its target, or the destination of an event without target.
	void forward_event(int p_peer, const PendingEvent &p_event);


public:
	// `TickEngine`: takes the settings if they're valid; not while running.
	virtual void set_settings(const Settings &p_settings) override;


	// `TickEngine`: the settings in use; the roles in them follow the mesh's.
	virtual const Settings &get_settings() const override { return settings; }


	// The counters.
	const Stats &get_stats() const { return stats; }


	// `TickEngine`: the counters, the term of the roles, the quorum and the timeline's frame, by name.
	virtual Dictionary get_stats_dictionary() const override;


	// `TickEngine`: sets who hears what happens.
	virtual void set_listener(Listener *p_listener) override { listener = p_listener; }


	// `TickEngine`: starts this node of the mesh: a new process id, the roles as configured, a hello to every connected
	// node, and the claims of its objects.
	virtual Error start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec) override;


	// `TickEngine`: stops the engine. Called from game code the engine is running (a tick, an event, a signal), it
	// stops once that call returns; it isn't running anymore from now on.
	virtual void stop() override;


	// `TickEngine`: whether the engine is running and wasn't asked to stop.
	virtual bool is_running() const override { return is_active(); }


	// `TickEngine`: starts synchronizing an object: binds it to its entry if the mesh knows its path, and claims it
	// from the registry.
	virtual void register_object(TickSyncObject *p_object) override;


	// `TickEngine`: stops synchronizing an object. Its owner tells the registry, which forgets it.
	virtual void unregister_object(TickSyncObject *p_object) override;


	// `TickEngine`: the id the registry gave an object, or 0.
	virtual uint16_t get_net_id(const TickSyncObject *p_object) const override;


	// `TickEngine`: receives, settles the roles, simulates the frames due on the mesh's timeline, sends, and updates
	// the remote objects.
	virtual void process(double p_delta, uint64_t p_now_usec) override;


	// `TickEngine`: shows the objects of other owners between the states received, or at the latest one.
	virtual void update_interpolation(uint64_t p_now_usec) override;


	// `TickEngine`: the next frame to simulate.
	virtual uint32_t get_frame() const override { return stepper.get_next_frame_index(); }


	// `TickEngine`: the frame of the mesh's timeline at the time given, with its fraction; negative while this node
	// doesn't know it.
	virtual double get_timeline_frame(uint64_t p_now_usec) const override;


	// `TickEngine`: makes a clock master follow another engine's timeline instead of the local delta.
	virtual void set_clock_source(const TickEngine *p_source) override { clock_source = p_source; }


	// `TickEngine`: the clock.
	virtual const TickClock &get_clock() const override { return clock; }


	// `TickEngine`: any running node spawns.
	virtual bool can_spawn() const override { return running; }


	// `TickEngine`: records a spawn and sends it to every node. Spawn ids carry the id of the node that spawned (below
	// 4096) in their high bits.
	virtual uint32_t spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override;


	// `TickEngine`: removes a spawn this node owns, and tells every node.
	virtual void despawn(uint32_t p_spawn_id) override;


	// `TickEngine`: the id the next spawn gets.
	virtual uint32_t get_next_spawn_id() const override;


	// This node's spawns, and for the registry the ones of the nodes that left.
	virtual bool owns_spawn(uint32_t p_spawn_id) const override;


	// Object events go to the target's current owner (forwarded if it changed on the way); events without target
	// go to `p_peer`, or every node with 0.
	virtual Error send_event(TickSyncObject *p_target, const StringName &p_name, const Variant &p_payload, uint32_t p_frame, int p_peer) override;


	// `TickEngine`: a frame `p_seconds` after the current one, to schedule events every node runs at the same frame.
	virtual uint32_t get_event_frame(double p_seconds) const override;


	// Owner of an object, 0 when it has none or it isn't registered yet.
	virtual int get_owner(const TickSyncObject *p_object) const override;


	// `TickEngine`: asks the registry for an object another node owns. The answer comes as a change of owner, or as a
	// denial.
	virtual Error request_authority(TickSyncObject *p_object) override;


	// `TickEngine`: the owner hands an object to `p_to_peer`; 0 leaves it orphaned. The object isn't simulated here
	// until the registry answers.
	virtual Error release_authority(TickSyncObject *p_object, int p_to_peer) override;


	// `TickEngine`: asks the registry to give an object to `p_peer`, whoever owns it.
	virtual Error assign_authority(TickSyncObject *p_object, int p_peer) override;


	// The version of an object's ownership as this node knows it; 0 when the object isn't registered.
	uint32_t get_version(const TickSyncObject *p_object) const;


	// Moves the registry and the clock master to connected nodes (this one included) while the mesh runs.
	virtual Error change_roles(int p_registry, int p_clock_master) override;


	// `TickEngine`: whether this node is connected to enough nodes to take or keep the roles.
	virtual bool has_role_quorum() const override;


	// The term of the roles this node knows.
	uint32_t get_roles_term() const { return roles_term; }
};

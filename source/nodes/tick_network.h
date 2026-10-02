// The node that runs the synchronization in a scene: `TickNetwork`.
//
// It runs a synchronization engine in the scene (ADR-040): a single authority (`TickSyncCore`: a star, or a mesh with
// one authority) or an owner per object (`TickMeshCore`: a mesh with distributed authority). The game configures it
// with properties, starts it on a transport and hears what happens through signals; the `TickObject` and `TickSpawner`
// nodes of the scene register with it. It advances its engine once per physics frame, and updates the interpolated
// objects once per rendered frame.

#pragma once

#include "../sync/tick_mesh_core.h"
#include "../sync/tick_sync_core.h"

#include "scene/main/node.h"

class TickNetwork : public Node, public TickEngine::Listener {
	GDCLASS(TickNetwork, Node);

public:
	enum Trust {
		TRUST_UNTRUSTED,
		TRUST_TRUSTED,
	};

	enum AuthorityMode {
		AUTHORITY_SINGLE,
		AUTHORITY_DISTRIBUTED,
	};

private:
	TickSyncCore single_core;
	TickMeshCore mesh_core;
	TickEngine *engine = &single_core;
	AuthorityMode authority_mode = AUTHORITY_SINGLE;
	TickEngine::Settings settings;
	// Objects registered, kept to move them when the authority mode changes.
	LocalVector<TickSyncObject *> registered_objects;
	NodePath root_path = NodePath("..");
	NodePath clock_network;
	Ref<TickTransport> transport;
	bool running = false;
	Callable event_validator;
	Callable interest_filter;
	// Time of the last update of the interpolated objects: what they show now.
	uint64_t last_update_usec = 0;

	// Turns the node's internal processing on while the network runs and off when it stops; never in the editor.
	void _update_processing();


	// Script wrapper of `set_object_relevant()`: the object must be a `TickObject`.
	Error _set_object_relevant(Object *p_object, int p_peer, bool p_relevant);


	// Script wrapper of `is_object_relevant()`: the object must be a `TickObject`.
	bool _is_object_relevant(Object *p_object, int p_peer) const;


	// Script wrapper of `get_state_at()`: the object must be a `TickObject`.
	Dictionary _get_state_at(Object *p_object, double p_frame) const;


protected:
	// Joins the group the objects find a network by, stops the network when the node leaves the tree, and drives the
	// engine: its ticks every physics frame, the interpolation every rendered frame.
	void _notification(int p_what);


	// Exposes the class to scripts.
	static void _bind_methods();


public:
	// `TickEngine::Listener`: emits `peer_ready`.
	virtual void on_peer_ready(int p_peer) override;


	// `TickEngine::Listener`: emits `peer_left`.
	virtual void on_peer_left(int p_peer) override;


	// `TickEngine::Listener`: emits `rejected`.
	virtual void on_rejected(const String &p_reason) override;


	// `TickEngine::Listener`: emits `prediction_started`.
	virtual void on_prediction_started(uint32_t p_frame) override;


	// `TickEngine::Listener`: emits `rewound`.
	virtual void on_rewound(uint32_t p_frame, int p_frame_count) override;


	// `TickEngine::Listener`: asks `event_validator` about an event without target object; -1 when there's no
	// validator.
	virtual int validate_network_event(int p_sender, const StringName &p_event, const Variant &p_payload) override;


	// `TickEngine::Listener`: emits `event_received`.
	virtual void on_network_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override;


	// `TickEngine::Listener`: hands a spawn of another peer to the `TickSpawner` at that path.
	virtual void on_spawn(const String &p_spawner, uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override;


	// `TickEngine::Listener`: hands a despawn of another peer to the `TickSpawner` at that path.
	virtual void on_despawn(const String &p_spawner, uint32_t p_spawn_id) override;


	// `TickEngine::Listener`: emits `authority_changed`.
	virtual void on_authority_changed(TickSyncObject *p_object, int p_old_owner, int p_new_owner) override;


	// `TickEngine::Listener`: emits `authority_orphaned`.
	virtual void on_authority_orphaned(TickSyncObject *p_object, int p_last_owner, uint32_t p_last_frame) override;


	// `TickEngine::Listener`: emits `authority_request_denied`.
	virtual void on_authority_request_denied(TickSyncObject *p_object) override;


	// `TickEngine::Listener`: asks `interest_filter` whether an object is relevant to a peer; -1 when there's no
	// filter, or it doesn't answer with a boolean.
	virtual int filter_relevance(int p_peer, TickSyncObject *p_object) override;


	// `TickEngine::Listener`: tells the object, then emits `relevance_changed`.
	virtual void on_relevance_changed(TickSyncObject *p_object, bool p_relevant) override;


	// `TickEngine::Listener`: emits `host_migrated`.
	virtual void on_host_migrated(int p_old_host, int p_new_host) override;


	// `TickEngine::Listener`: emits `roles_changed`.
	virtual void on_roles_changed(int p_registry, int p_clock_master) override;


	// `TickEngine::Listener`: emits `role_quorum_changed`.
	virtual void on_role_quorum_changed(bool p_has_quorum) override;


	// Sets how many ticks a second has (1 to 1000). Like the other settings, it can't change while the network runs.
	void set_ticks_per_second(int p_ticks_per_second);


	// How many ticks a second has; while the network runs, the rate in use.
	int get_ticks_per_second() const;


	// Sets how many past inputs each input packet repeats, against packet loss.
	void set_input_redundancy(int p_redundancy);


	// See `set_input_redundancy()`.
	int get_input_redundancy() const;


	// Sets how many frames of history are kept for snapshots, predictions and interpolation (at least 8).
	void set_history_size(int p_frames);


	// See `set_history_size()`.
	int get_history_size() const;


	// Sets how many ticks pass between two snapshots.
	void set_snapshot_interval(int p_ticks);


	// See `set_snapshot_interval()`.
	int get_snapshot_interval() const;


	// Sets how far behind the authority the interpolated objects are shown, in seconds.
	void set_interpolation_delay(double p_seconds);


	// See `set_interpolation_delay()`.
	double get_interpolation_delay() const;


	// Sets the fewest inputs the server should have buffered ahead of its frame; raises the maximum if needed.
	void set_min_input_buffer(int p_frames);


	// See `set_min_input_buffer()`.
	int get_min_input_buffer() const;


	// Sets the most inputs the server should have buffered ahead of its frame; lowers the minimum if needed.
	void set_max_input_buffer(int p_frames);


	// See `set_max_input_buffer()`.
	int get_max_input_buffer() const;


	// Sets whether the other peers are trusted. What untrusted clients send is limited and checked.
	void set_trust(Trust p_trust);


	// See `set_trust()`.
	Trust get_trust() const;


	// Untrusted: sets how many events per second are accepted from each client.
	void set_max_events_per_second(int p_events);


	// See `set_max_events_per_second()`.
	int get_max_events_per_second() const;


	// Untrusted: sets the largest event payload accepted, in bytes (1 to 65535).
	void set_max_event_size(int p_bytes);


	// See `set_max_event_size()`.
	int get_max_event_size() const;


	// Sets the function that accepts or refuses the events without target object, called as `validator(sender, event,
	// payload)`.
	void set_event_validator(const Callable &p_validator) { event_validator = p_validator; }


	// See `set_event_validator()`.
	Callable get_event_validator() const { return event_validator; }


	// Single authority: sets the peer that simulates every object and is the clock master.
	void set_authority_peer(int p_peer);


	// The authority peer; while the network runs, the current one (it changes with a host migration).
	int get_authority_peer() const;


	// Sets whether the remote objects are interpolated, or just get the latest state (proxies on servers that relay
	// them).
	void set_interpolate_remote(bool p_enabled);


	// See `set_interpolate_remote()`.
	bool is_interpolating_remote() const;


	// Sets another `TickNetwork` whose timeline this one follows instead of the local delta (ADR-038). Only the peer
	// that owns this network's timeline can follow another.
	void set_clock_network(const NodePath &p_path);


	// See `set_clock_network()`.
	NodePath get_clock_network() const { return clock_network; }


	// The engine in use: another network that follows this one's clock reads it.
	const TickEngine &get_engine() const { return *engine; }


	// Chooses the engine: a single authority, or an owner per object. The registered objects move to the chosen engine.
	// Not while the network runs.
	void set_authority_mode(AuthorityMode p_mode);


	// See `set_authority_mode()`.
	AuthorityMode get_authority_mode() const { return authority_mode; }


	// Distributed authority: sets the node that keeps the registry of owners. While the network runs, moves the role to
	// that node.
	void set_registry_peer(int p_peer);


	// The registry's node; while the network runs, the current one.
	int get_registry_peer() const;


	// Distributed authority: sets the node whose clock the mesh follows. While the network runs, moves the role to that
	// node.
	void set_clock_master(int p_peer);


	// The clock master; while the network runs, the current one.
	int get_clock_master() const;


	// Distributed authority: sets the registry and the clock master at once; while the network runs, moves both roles
	// together (ADR-074).
	Error set_roles(int p_registry_peer, int p_clock_master);


	// Distributed authority: sets the nodes that take the roles when their node leaves, by order of preference (empty:
	// any node, the lowest id first).
	void set_role_candidates(const PackedInt32Array &p_candidates);


	// See `set_role_candidates()`.
	PackedInt32Array get_role_candidates() const;


	// Distributed authority: sets how many nodes, this one included, a node must be connected to in order to take or
	// keep the roles (0: no check).
	void set_role_quorum(int p_nodes);


	// See `set_role_quorum()`.
	int get_role_quorum() const;


	// Whether this node is connected to enough nodes to take or keep the roles; `true` while the network isn't running.
	bool has_role_quorum() const;


	// Distributed authority: sets how often, in ticks, every object's state is sent even if it didn't change.
	void set_keyframe_interval(int p_ticks);


	// See `set_keyframe_interval()`.
	int get_keyframe_interval() const;


	// Sets the node the paths of objects and spawners are relative to; every peer needs the same nodes under it.
	void set_root_path(const NodePath &p_path);


	// See `set_root_path()`.
	NodePath get_root_path() const;


	// Interest (ADR-053): sets whether objects are relevant to every client until told otherwise.
	void set_default_relevance(bool p_relevant);


	// See `set_default_relevance()`.
	bool get_default_relevance() const;


	// Interest: sets how often, in ticks, `interest_filter` is asked.
	void set_interest_interval(int p_ticks);


	// See `set_interest_interval()`.
	int get_interest_interval() const;


	// Interest: sets the function asked whether an object is relevant to a client, called as `filter(peer, object)`. A
	// boolean decides; anything else leaves the object as it is.
	void set_interest_filter(const Callable &p_filter) { interest_filter = p_filter; }


	// See `set_interest_filter()`.
	Callable get_interest_filter() const { return interest_filter; }


	// Interest, on the server: starts or stops sending an object's state to `p_peer` (0: every client).
	Error set_object_relevant(TickSyncObject *p_object, int p_peer, bool p_relevant);


	// Interest: whether an object's state is sent to `p_peer`; on a client, to this client.
	bool is_object_relevant(const TickSyncObject *p_object, int p_peer) const;


	// History (ADR-054): the state of an object at a frame of the authority's timeline, by variable name; empty when
	// the frame isn't in the history.
	Dictionary get_state_at(const TickSyncObject *p_object, double p_frame) const;


	// The frame the interpolated objects show now; negative while it isn't known.
	double get_view_frame() const;


	// Node the object paths are relative to.
	Node *get_root_node() const;


	// Starts the network on a transport, with the settings and the authority mode chosen. Warns when the tick rate
	// isn't the physics', and when a distributed mesh isn't marked as trusted.
	Error start(const Ref<TickTransport> &p_transport);


	// Stops the network and lets go of the transport.
	void stop();


	// Whether the network was started and not stopped.
	bool is_running() const { return running; }


	// The transport the network runs on; null while it's stopped.
	Ref<TickTransport> get_transport() const { return transport; }


	// Whether this peer is the authority of a single authority network.
	bool is_server() const;


	// Whether past frames are being simulated again right now.
	bool is_rewinding() const;


	// Client: whether the objects it controls are being predicted.
	bool is_predicting() const;


	// This peer's id in the transport; 0 while the network is stopped.
	int get_local_peer_id() const;


	// The next frame to simulate.
	int64_t get_frame() const;


	// The lowest round trip time to the clock master among the recent pings, in seconds.
	double get_rtt() const;


	// The engine's counters.
	Dictionary get_stats() const;


	// Sends an event without target object; `p_frame` < 0 uses the default (see `TickSyncCore::send_event()`).
	Error send_event(const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer);


	// Sends an event to an object on other peers; the rest is as in `send_event()`.
	Error send_object_event(TickSyncObject *p_object, const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer);


	// The frame that is `p_seconds` from now, to schedule an event every peer runs together.
	int64_t get_event_frame(double p_seconds) const;


	// Called by `TickSpawner`: tells the other peers about a spawn. Returns its id; 0 when the network refused it.
	uint32_t spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data);


	// Called by `TickSpawner`: tells the other peers a spawn was removed.
	void despawn(uint32_t p_spawn_id);


	// The id the next spawn gets.
	uint32_t get_next_spawn_id() const { return engine->get_next_spawn_id(); }


	// Whether this peer may spawn now: the server of a running single authority network, or any node of a running mesh.
	bool can_spawn() const { return running && engine->can_spawn(); }


	// Whether removing the node of a spawn despawns it on the other peers: this peer made the spawn, or inherited it.
	bool owns_spawn(uint32_t p_spawn_id) const { return running && engine->owns_spawn(p_spawn_id); }


	// The peer with the authority over an object (ADR-041): the authority peer, or the object's owner in a distributed
	// mesh.
	int get_object_owner(const TickSyncObject *p_object) const;


	// Distributed authority: asks for the authority over an object.
	Error request_authority(TickSyncObject *p_object);


	// Distributed authority: gives up the authority over an object, to `p_to_peer` or to nobody (0).
	Error release_authority(TickSyncObject *p_object, int p_to_peer);


	// Distributed authority: as the registry, gives the authority over an object to `p_peer`.
	Error assign_authority(TickSyncObject *p_object, int p_peer);


	// Starts synchronizing an object; a `TickObject` calls it when it enters the scene.
	void register_object(TickSyncObject *p_object);


	// Stops synchronizing an object.
	void unregister_object(TickSyncObject *p_object);


	// The id the engine gave an object; 0 when it has none.
	int get_net_id(const TickSyncObject *p_object) const;


	// Advances the network with an explicit time; the node calls it from the physics process. Public for tests.
	void advance(double p_delta, uint64_t p_now_usec);


	// Updates the interpolated objects for the time given; the node calls it every rendered frame.
	void update_interpolation(uint64_t p_now_usec);


	// Makes the node listen to its two engines.
	TickNetwork();


	// Stops the engine, and stops listening to both.
	~TickNetwork();
};

VARIANT_ENUM_CAST(TickNetwork::Trust);
VARIANT_ENUM_CAST(TickNetwork::AuthorityMode);

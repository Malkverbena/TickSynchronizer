#pragma once

#include "../sync/tick_mesh_core.h"
#include "../sync/tick_sync_core.h"

#include "scene/main/node.h"

// Runs a synchronization engine in the scene (ADR-040): a single authority (`TickSyncCore`: a star, or a mesh with
// one authority) or an owner per object (`TickMeshCore`: a mesh with distributed authority).
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

	void _update_processing();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// TickSyncCore::Listener.
	virtual void on_peer_ready(int p_peer) override;
	virtual void on_peer_left(int p_peer) override;
	virtual void on_rejected(const String &p_reason) override;
	virtual void on_prediction_started(uint32_t p_frame) override;
	virtual void on_rewound(uint32_t p_frame, int p_frame_count) override;
	virtual int validate_network_event(int p_sender, const StringName &p_event, const Variant &p_payload) override;
	virtual void on_network_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override;
	virtual void on_spawn(const String &p_spawner, uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) override;
	virtual void on_despawn(const String &p_spawner, uint32_t p_spawn_id) override;
	virtual void on_authority_changed(TickSyncObject *p_object, int p_old_owner, int p_new_owner) override;
	virtual void on_authority_orphaned(TickSyncObject *p_object, int p_last_owner, uint32_t p_last_frame) override;
	virtual void on_authority_request_denied(TickSyncObject *p_object) override;

	void set_ticks_per_second(int p_ticks_per_second);
	int get_ticks_per_second() const;
	void set_input_redundancy(int p_redundancy);
	int get_input_redundancy() const;
	void set_history_size(int p_frames);
	int get_history_size() const;
	void set_snapshot_interval(int p_ticks);
	int get_snapshot_interval() const;
	void set_interpolation_delay(double p_seconds);
	double get_interpolation_delay() const;
	void set_min_input_buffer(int p_frames);
	int get_min_input_buffer() const;
	void set_max_input_buffer(int p_frames);
	int get_max_input_buffer() const;
	void set_trust(Trust p_trust);
	Trust get_trust() const;
	void set_max_events_per_second(int p_events);
	int get_max_events_per_second() const;
	void set_max_event_size(int p_bytes);
	int get_max_event_size() const;
	void set_event_validator(const Callable &p_validator) { event_validator = p_validator; }
	Callable get_event_validator() const { return event_validator; }
	void set_authority_peer(int p_peer);
	int get_authority_peer() const;
	void set_interpolate_remote(bool p_enabled);
	bool is_interpolating_remote() const;
	void set_clock_network(const NodePath &p_path);
	NodePath get_clock_network() const { return clock_network; }
	const TickEngine &get_engine() const { return *engine; }
	void set_authority_mode(AuthorityMode p_mode);
	AuthorityMode get_authority_mode() const { return authority_mode; }
	void set_registry_peer(int p_peer);
	int get_registry_peer() const;
	void set_clock_master(int p_peer);
	int get_clock_master() const;
	void set_keyframe_interval(int p_ticks);
	int get_keyframe_interval() const;
	void set_root_path(const NodePath &p_path);
	NodePath get_root_path() const;
	// Node the object paths are relative to.
	Node *get_root_node() const;

	Error start(const Ref<TickTransport> &p_transport);
	void stop();
	bool is_running() const { return running; }
	Ref<TickTransport> get_transport() const { return transport; }

	bool is_server() const;
	bool is_rewinding() const;
	bool is_predicting() const;
	int get_local_peer_id() const;
	int64_t get_frame() const;
	double get_rtt() const;
	Dictionary get_stats() const;

	// Events without target object; `p_frame` < 0 uses the default (see `TickSyncCore::send_event`).
	Error send_event(const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer);
	Error send_object_event(TickSyncObject *p_object, const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer);
	int64_t get_event_frame(double p_seconds) const;

	uint32_t spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data);
	void despawn(uint32_t p_spawn_id);
	uint32_t get_next_spawn_id() const { return engine->get_next_spawn_id(); }
	bool can_spawn() const { return running && engine->can_spawn(); }

	// Distributed authority (ADR-041).
	int get_object_owner(const TickSyncObject *p_object) const;
	Error request_authority(TickSyncObject *p_object);
	Error release_authority(TickSyncObject *p_object, int p_to_peer);
	Error assign_authority(TickSyncObject *p_object, int p_peer);

	void register_object(TickSyncObject *p_object);
	void unregister_object(TickSyncObject *p_object);
	int get_net_id(const TickSyncObject *p_object) const;

	// Advances the network with an explicit time; the node calls it from the physics process. Public for tests.
	void advance(double p_delta, uint64_t p_now_usec);
	void update_interpolation(uint64_t p_now_usec);

	TickNetwork();
	~TickNetwork();
};

VARIANT_ENUM_CAST(TickNetwork::Trust);
VARIANT_ENUM_CAST(TickNetwork::AuthorityMode);

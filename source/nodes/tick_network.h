#pragma once

#include "../sync/tick_sync_core.h"

#include "scene/main/node.h"

// Runs a `TickSyncCore` in the scene: STAR network with a single authority (the server).
class TickNetwork : public Node, public TickSyncCore::Listener {
	GDCLASS(TickNetwork, Node);

public:
	enum Trust {
		TRUST_UNTRUSTED,
		TRUST_TRUSTED,
	};

private:
	TickSyncCore core;
	TickSyncCore::Settings settings;
	NodePath root_path = NodePath("..");
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
	uint32_t get_next_spawn_id() const { return core.get_next_spawn_id(); }

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

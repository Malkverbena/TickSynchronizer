#pragma once

#include "../sync/tick_sync_core.h"

#include "scene/main/node.h"

// Runs a `TickSyncCore` in the scene: STAR network with a single authority (the server).
class TickNetwork : public Node, public TickSyncCore::Listener {
	GDCLASS(TickNetwork, Node);

	TickSyncCore core;
	TickSyncCore::Settings settings;
	NodePath root_path = NodePath("..");
	Ref<TickTransport> transport;
	bool running = false;

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

	void register_object(TickSyncObject *p_object);
	void unregister_object(TickSyncObject *p_object);
	int get_net_id(const TickSyncObject *p_object) const;

	// Advances the network with an explicit time; the node calls it from the physics process. Public for tests.
	void advance(double p_delta, uint64_t p_now_usec);
	void update_interpolation(uint64_t p_now_usec);

	TickNetwork();
	~TickNetwork();
};

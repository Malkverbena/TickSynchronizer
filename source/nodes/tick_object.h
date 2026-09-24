#pragma once

#include "../sync/tick_sync_object.h"
#include "data_buffer.h"

#include "scene/main/node.h"

class TickNetwork;

// Synchronizes properties of its root node (the parent, by default) through a `TickNetwork`.
//
// Extend it with a script: declare the variables in `_setup_sync()`, write the controller's input in
// `_collect_input()` and advance the object in `_process_tick()`. The server simulates it; the controlling client
// predicts it; every other peer interpolates it.
class TickObject : public Node, public TickSyncObject {
	GDCLASS(TickObject, Node);

	int controller_peer = 1;
	NodePath root_path = NodePath("..");
	NodePath network_path;

	TickSchema schema;
	LocalVector<NodePath> property_paths;
	bool setup_done = false;
	bool declaring = false;
	TickNetwork *network = nullptr;
	String sync_path;
	Ref<DataBuffer> input_wrapper;

	TickNetwork *find_network() const;
	void register_to_network();
	void unregister_from_network();

protected:
	void _notification(int p_what);
	static void _bind_methods();

	GDVIRTUAL0(_setup_sync)
	GDVIRTUAL1(_collect_input, Ref<DataBuffer>)
	GDVIRTUAL2(_process_tick, double, Ref<DataBuffer>)
	GDVIRTUAL1(_apply_interpolated_state, Dictionary)
	GDVIRTUAL3R(bool, _validate_event, int, StringName, Variant)
	GDVIRTUAL4(_on_event, int, StringName, Variant, int64_t)
	GDVIRTUAL2(_on_authority_changed, int, int)
	GDVIRTUAL1R(bool, _approve_authority_request, int)

public:
	void set_controller_peer(int p_peer);
	int get_controller_peer_id() const { return controller_peer; }
	void set_root_path(const NodePath &p_path);
	NodePath get_root_path() const { return root_path; }
	void set_network_path(const NodePath &p_path);
	NodePath get_network_path() const { return network_path; }
	Node *get_root_node() const;
	TickNetwork *get_network() const { return network; }

	// Declares a synchronized property of the root node; only valid inside `_setup_sync()`.
	void declare_var(const StringName &p_property, const Ref<TickCodec> &p_codec);
	PackedStringArray get_declared_vars() const;

	int get_net_id() const;

	// Distributed authority (see `TickNetwork.authority_mode`).
	int get_owner_peer() const;
	bool is_owner() const;
	Error request_authority();
	Error release_authority(int p_to_peer);
	Error assign_authority(int p_peer);
	// Client: to the server. Server: to `p_peer`, or every client with 0. `p_frame` < 0 uses the default.
	Error send_event(const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer);
	bool is_rewinding() const;

	// TickSyncObject.
	virtual String get_sync_path() const override { return sync_path; }
	virtual int get_controller_peer() const override { return controller_peer; }
	virtual const TickSchema &get_sync_schema() const override { return schema; }
	virtual Variant get_sync_var(int p_index) const override;
	virtual void set_sync_var(int p_index, const Variant &p_value) override;
	virtual void collect_input(TickDataBuffer &r_input) override;
	virtual void process_tick(double p_delta, TickDataBuffer &p_input) override;
	virtual void apply_interpolated_state(const LocalVector<Variant> &p_values) override;
	virtual int validate_event(int p_sender, const StringName &p_event, const Variant &p_payload) override;
	virtual void on_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override;
	virtual void on_authority_changed(int p_old_owner, int p_new_owner) override;
	virtual int approve_authority_request(int p_requester) override;
	virtual Object *get_sync_instance() override { return this; }

	TickObject();
};

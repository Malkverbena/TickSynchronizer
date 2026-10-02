// The node that synchronizes a game object: `TickObject`.
//
// It synchronizes properties of its root node (the parent, by default) through a `TickNetwork`.
//
// Extend it with a script: declare the variables in `_setup_sync()`, write the controller's input in `_collect_input()`
// and advance the object in `_process_tick()`. The server simulates it; the controlling client predicts it; every other
// peer interpolates it, or simulates it as a doll with the controller's inputs (`remote_mode`).
//
// For the engines it's a `TickSyncObject`: it reads and writes the root node's properties, and forwards the engines'
// callbacks to the script's virtual methods.

#pragma once

#include "../sync/tick_sync_object.h"
#include "data_buffer.h"

#include "core/object/class_db.h"
#include "scene/main/node.h"

class TickNetwork;

class TickObject : public Node, public TickSyncObject {
	GDCLASS(TickObject, Node);

public:
	enum RemoteMode {
		REMOTE_MODE_INTERPOLATE,
		REMOTE_MODE_DOLL,
	};

private:
	int controller_peer = 1;
	RemoteMode remote_mode = REMOTE_MODE_INTERPOLATE;
	NodePath root_path = NodePath("..");
	NodePath network_path;

	TickSchema schema;
	LocalVector<NodePath> property_paths;
	bool setup_done = false;
	bool declaring = false;
	// The network this object is registered with. Weak: the game may free the network before its objects.
	ObjectID network_id;
	String sync_path;
	Ref<DataBuffer> input_wrapper;

	// The network to register with: the one at `network_path`, the nearest ancestor, or the first one in the scene.
	TickNetwork *find_network() const;


	// Declares the variables (once, through `_setup_sync()`) and registers this object with its network, under the path
	// of its root node. Does nothing in the editor.
	void register_to_network();


	// Unregisters from the network, if the network still exists.
	void unregister_from_network();


protected:
	// Registers when the node is ready, or enters the tree again, and unregisters when it leaves the tree.
	void _notification(int p_what);


	// Exposes the class to scripts.
	static void _bind_methods();


	// Script: declares the synchronized variables, with `declare_var()`.
	GDVIRTUAL0(_setup_sync)


	// Script: writes the controller's input of this tick.
	GDVIRTUAL1(_collect_input, Ref<DataBuffer>)


	// Script: advances the object by one tick, with the input to read (empty when there is none).
	GDVIRTUAL2(_process_tick, double, Ref<DataBuffer>)


	// Script: applies an interpolated state, given by variable name, instead of having the properties set.
	GDVIRTUAL1(_apply_interpolated_state, Dictionary)


	// Script: accepts or refuses an event a peer sent.
	GDVIRTUAL3R(bool, _validate_event, int, StringName, Variant)


	// Script: runs an event, with the frame it was scheduled for.
	GDVIRTUAL4(_on_event, int, StringName, Variant, int64_t)


	// Script: the owner of the object changed (distributed authority).
	GDVIRTUAL2(_on_authority_changed, int, int)


	// Script: accepts or refuses a peer's request for the authority over this object.
	GDVIRTUAL1R(bool, _approve_authority_request, int)


	// Script: the server started or stopped sending this object's state to this client.
	GDVIRTUAL1(_on_relevance_changed, bool)


public:
	// Sets the peer whose input drives the object (1 is the server); not while the object is registered.
	void set_controller_peer(int p_peer);


	// The peer whose input drives the object.
	int get_controller_peer_id() const { return controller_peer; }


	// Sets how the peers that don't control the object show it: interpolated, or simulated as a doll. Not while the
	// object is registered.
	void set_remote_mode(RemoteMode p_mode);


	// See `set_remote_mode()`.
	RemoteMode get_remote_mode() const { return remote_mode; }


	// Sets the node whose properties are synchronized, relative to this one; not while the object is registered.
	void set_root_path(const NodePath &p_path);


	// See `set_root_path()`.
	NodePath get_root_path() const { return root_path; }


	// Sets the `TickNetwork` to register with; empty looks for one among the ancestors, then in the scene. Not while
	// the object is registered.
	void set_network_path(const NodePath &p_path);


	// See `set_network_path()`.
	NodePath get_network_path() const { return network_path; }


	// The node whose properties are synchronized; null outside the tree, or when `root_path` leads nowhere.
	Node *get_root_node() const;


	// The network this object is registered with, or null (also once that network was freed).
	TickNetwork *get_network() const;


	// Declares a synchronized property of the root node; only valid inside `_setup_sync()`.
	void declare_var(const StringName &p_property, const Ref<TickCodec> &p_codec);


	// The names of the declared variables, in declaration order.
	PackedStringArray get_declared_vars() const;


	// The id the network gave the object, or 0 while it has none.
	int get_net_id() const;


	// The peer with the authority over the object: the server, or its owner in a network with distributed authority
	// (see `TickNetwork.authority_mode`). 0 while the network isn't running, or the owner isn't known.
	int get_owner_peer() const;


	// Whether this peer has the authority over the object.
	bool is_owner() const;


	// Distributed authority: asks for the authority over the object.
	Error request_authority();


	// Distributed authority: gives up the authority over the object, to `p_to_peer` or to nobody (0).
	Error release_authority(int p_to_peer);


	// Distributed authority: as the registry, gives the authority over the object to `p_peer`.
	Error assign_authority(int p_peer);


	// Sends an event to this object on other peers. Client: to the server. Server: to `p_peer`, or every client with 0.
	// `p_frame` < 0 uses the default.
	Error send_event(const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer);


	// Whether the network is simulating past frames again: what shouldn't repeat (a sound, a particle effect) checks
	// it.
	bool is_rewinding() const;


	// Interest: whether this peer gets the object's state (always on the server).
	bool is_relevant() const;


	// The object's state at a frame of the authority's timeline, while it's in the history.
	Dictionary get_state_at(double p_frame) const;


	// `TickSyncObject`: the path of the root node from the network's root: the identity every peer shares.
	virtual String get_sync_path() const override { return sync_path; }


	// `TickSyncObject`: the peer whose input drives the object.
	virtual int get_controller_peer() const override { return controller_peer; }


	// `TickSyncObject`: the variables declared in `_setup_sync()`.
	virtual const TickSchema &get_sync_schema() const override { return schema; }


	// `TickSyncObject`: reads a declared property of the root node.
	virtual Variant get_sync_var(int p_index) const override;


	// `TickSyncObject`: sets a declared property of the root node.
	virtual void set_sync_var(int p_index, const Variant &p_value) override;


	// `TickSyncObject`: lets the script write the input, through `_collect_input()`.
	virtual void collect_input(TickDataBuffer &r_input) override;


	// `TickSyncObject`: lets the script advance the object, through `_process_tick()`.
	virtual void process_tick(double p_delta, TickDataBuffer &p_input) override;


	// `TickSyncObject`: hands the interpolated state to `_apply_interpolated_state()` when the script has it; sets the
	// properties otherwise.
	virtual void apply_interpolated_state(const LocalVector<Variant> &p_values) override;


	// `TickSyncObject`: whether the remote mode is the doll.
	virtual bool is_doll_enabled() const override { return remote_mode == REMOTE_MODE_DOLL; }


	// `TickSyncObject`: asks `_validate_event()`: 1 accepts, 0 refuses, -1 when the script doesn't have it.
	virtual int validate_event(int p_sender, const StringName &p_event, const Variant &p_payload) override;


	// `TickSyncObject`: runs `_on_event()`.
	virtual void on_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) override;


	// `TickSyncObject`: runs `_on_authority_changed()`.
	virtual void on_authority_changed(int p_old_owner, int p_new_owner) override;


	// `TickSyncObject`: asks `_approve_authority_request()`: 1 approves, 0 refuses, -1 when the script doesn't have it.
	virtual int approve_authority_request(int p_requester) override;


	// `TickSyncObject`: runs `_on_relevance_changed()`.
	virtual void on_relevance_changed(bool p_relevant) override;


	// `TickSyncObject`: this node, to report it in signals.
	virtual Object *get_sync_instance() override { return this; }


	// Makes the `DataBuffer` the script's input methods receive.
	TickObject();
};

VARIANT_ENUM_CAST(TickObject::RemoteMode);

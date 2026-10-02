// Implementation of `TickObject`: registering with its network, reading and writing the root node's properties for the
// engines, and forwarding the engines' callbacks to the script's virtual methods.

#include "tick_object.h"

#include "tick_network.h"

#include "core/config/engine.h"
#include "scene/main/scene_tree.h"

// Makes the `DataBuffer` the script's input methods receive.
TickObject::TickObject() {
	input_wrapper.instantiate();
}


// Registers when the node is ready, or enters the tree again, and unregisters when it leaves the tree.
void TickObject::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			register_to_network();
		} break;
		case NOTIFICATION_ENTER_TREE: {
			// Nodes moved in the tree register again; the first time waits for `READY`, when the whole scene
			// (including a network placed after this node) is in the tree.
			if (is_ready()) {
				register_to_network();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			unregister_from_network();
		} break;
	}
}


// The network to register with: the one at `network_path`, the nearest ancestor, or the first one in the scene.
TickNetwork *TickObject::find_network() const {
	if (!network_path.is_empty()) {
		return Object::cast_to<TickNetwork>(get_node_or_null(network_path));
	}
	for (Node *node = get_parent(); node; node = node->get_parent()) {
		TickNetwork *found = Object::cast_to<TickNetwork>(node);
		if (found) {
			return found;
		}
	}
	// A network elsewhere in the scene.
	return Object::cast_to<TickNetwork>(get_tree()->get_first_node_in_group(SNAME("_tick_networks")));
}


// The network this object is registered with, or null (also once that network was freed).
TickNetwork *TickObject::get_network() const {
	return ObjectDB::get_instance<TickNetwork>(network_id);
}


// Declares the variables (once, through `_setup_sync()`) and registers this object with its network, under the path
// of its root node. Does nothing in the editor.
void TickObject::register_to_network() {
	if (Engine::get_singleton()->is_editor_hint() || get_network()) {
		return;
	}
	TickNetwork *found = find_network();
	ERR_FAIL_NULL_MSG(found, "TickObject can't find a TickNetwork: set `network_path`, or add a TickNetwork to the scene.");
	Node *root = get_root_node();
	ERR_FAIL_NULL_MSG(root, "TickObject can't find its root node; check `root_path`.");
	Node *network_root = found->get_root_node();
	ERR_FAIL_NULL_MSG(network_root, "The TickNetwork can't find its root node; check its `root_path`.");

	if (!setup_done) {
		setup_done = true;
		declaring = true;
		GDVIRTUAL_CALL(_setup_sync);
		declaring = false;
	}

	sync_path = String(network_root->get_path_to(root));
	network_id = found->get_instance_id();
	found->register_object(this);
}


// Unregisters from the network, if the network still exists.
void TickObject::unregister_from_network() {
	TickNetwork *network = get_network();
	if (network) {
		network->unregister_object(this);
	}
	network_id = ObjectID();
}


// Sets the peer whose input drives the object (1 is the server); not while the object is registered.
void TickObject::set_controller_peer(int p_peer) {
	ERR_FAIL_COND_MSG(get_network(), "The controller can't change while the object is registered.");
	ERR_FAIL_COND_MSG(p_peer <= 0, "The controller peer must be positive (1 is the server).");
	controller_peer = p_peer;
}


// Sets how the peers that don't control the object show it: interpolated, or simulated as a doll. Not while the
// object is registered.
void TickObject::set_remote_mode(RemoteMode p_mode) {
	ERR_FAIL_COND_MSG(get_network(), "The remote mode can't change while the object is registered.");
	remote_mode = p_mode;
}


// Sets the node whose properties are synchronized, relative to this one; not while the object is registered.
void TickObject::set_root_path(const NodePath &p_path) {
	ERR_FAIL_COND_MSG(get_network(), "The root path can't change while the object is registered.");
	root_path = p_path;
}


// Sets the `TickNetwork` to register with; empty looks for one among the ancestors, then in the scene. Not while
// the object is registered.
void TickObject::set_network_path(const NodePath &p_path) {
	ERR_FAIL_COND_MSG(get_network(), "The network path can't change while the object is registered.");
	network_path = p_path;
}


// The node whose properties are synchronized; null outside the tree, or when `root_path` leads nowhere.
Node *TickObject::get_root_node() const {
	return is_inside_tree() ? get_node_or_null(root_path) : nullptr;
}


// Declares a synchronized property of the root node; only valid inside `_setup_sync()`.
void TickObject::declare_var(const StringName &p_property, const Ref<TickCodec> &p_codec) {
	ERR_FAIL_COND_MSG(!declaring, "Variables can only be declared inside `_setup_sync()`.");
	ERR_FAIL_COND_MSG(p_codec.is_null(), vformat("The codec of \"%s\" is null.", p_property));
	const int previous = schema.size();
	schema.add(p_property, p_codec);
	if (schema.size() > previous) {
		property_paths.push_back(NodePath(String(p_property)).get_as_property_path());
	}
}


// The names of the declared variables, in declaration order.
PackedStringArray TickObject::get_declared_vars() const {
	PackedStringArray names;
	for (const StringName &name : schema.names) {
		names.push_back(name);
	}
	return names;
}


// The id the network gave the object, or 0 while it has none.
int TickObject::get_net_id() const {
	const TickNetwork *network = get_network();
	return network ? network->get_net_id(this) : 0;
}


// Sends an event to this object on other peers. Client: to the server. Server: to `p_peer`, or every client with 0.
// `p_frame` < 0 uses the default.
Error TickObject::send_event(const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer) {
	TickNetwork *network = get_network();
	ERR_FAIL_NULL_V_MSG(network, ERR_UNCONFIGURED, "The object isn't registered in a TickNetwork.");
	return network->send_object_event(this, p_event, p_payload, p_frame, p_peer);
}


// `TickSyncObject`: asks `_validate_event()`: 1 accepts, 0 refuses, -1 when the script doesn't have it.
int TickObject::validate_event(int p_sender, const StringName &p_event, const Variant &p_payload) {
	bool accepted = false;
	if (GDVIRTUAL_CALL(_validate_event, p_sender, p_event, p_payload, accepted)) {
		return accepted ? 1 : 0;
	}
	return -1;
}


// `TickSyncObject`: runs `_on_event()`.
void TickObject::on_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) {
	GDVIRTUAL_CALL(_on_event, p_sender, p_event, p_payload, int64_t(p_frame));
}


// The peer with the authority over the object: the server, or its owner in a network with distributed authority
// (see `TickNetwork.authority_mode`). 0 while the network isn't running, or the owner isn't known.
int TickObject::get_owner_peer() const {
	const TickNetwork *network = get_network();
	return network && network->is_running() ? network->get_object_owner(this) : 0;
}


// Whether this peer has the authority over the object.
bool TickObject::is_owner() const {
	const TickNetwork *network = get_network();
	return network && network->is_running() && network->get_object_owner(this) == network->get_local_peer_id();
}


// Distributed authority: asks for the authority over the object.
Error TickObject::request_authority() {
	TickNetwork *network = get_network();
	ERR_FAIL_NULL_V_MSG(network, ERR_UNCONFIGURED, "The object isn't registered in a TickNetwork.");
	return network->request_authority(this);
}


// Distributed authority: gives up the authority over the object, to `p_to_peer` or to nobody (0).
Error TickObject::release_authority(int p_to_peer) {
	TickNetwork *network = get_network();
	ERR_FAIL_NULL_V_MSG(network, ERR_UNCONFIGURED, "The object isn't registered in a TickNetwork.");
	return network->release_authority(this, p_to_peer);
}


// Distributed authority: as the registry, gives the authority over the object to `p_peer`.
Error TickObject::assign_authority(int p_peer) {
	TickNetwork *network = get_network();
	ERR_FAIL_NULL_V_MSG(network, ERR_UNCONFIGURED, "The object isn't registered in a TickNetwork.");
	return network->assign_authority(this, p_peer);
}


// `TickSyncObject`: runs `_on_authority_changed()`.
void TickObject::on_authority_changed(int p_old_owner, int p_new_owner) {
	GDVIRTUAL_CALL(_on_authority_changed, p_old_owner, p_new_owner);
}


// `TickSyncObject`: asks `_approve_authority_request()`: 1 approves, 0 refuses, -1 when the script doesn't have it.
int TickObject::approve_authority_request(int p_requester) {
	bool approved = true;
	if (GDVIRTUAL_CALL(_approve_authority_request, p_requester, approved)) {
		return approved ? 1 : 0;
	}
	return -1;
}


// `TickSyncObject`: runs `_on_relevance_changed()`.
void TickObject::on_relevance_changed(bool p_relevant) {
	GDVIRTUAL_CALL(_on_relevance_changed, p_relevant);
}


// Interest: whether this peer gets the object's state (always on the server).
bool TickObject::is_relevant() const {
	const TickNetwork *network = get_network();
	if (network == nullptr || !network->is_running() || network->is_server()) {
		return true;
	}
	return network->is_object_relevant(this, network->get_local_peer_id());
}


// The object's state at a frame of the authority's timeline, while it's in the history.
Dictionary TickObject::get_state_at(double p_frame) const {
	const TickNetwork *network = get_network();
	ERR_FAIL_NULL_V_MSG(network, Dictionary(), "The object isn't registered in a TickNetwork.");
	return network->get_state_at(this, p_frame);
}


// Whether the network is simulating past frames again: what shouldn't repeat (a sound, a particle effect) checks
// it.
bool TickObject::is_rewinding() const {
	const TickNetwork *network = get_network();
	return network && network->is_rewinding();
}


// `TickSyncObject`: reads a declared property of the root node.
Variant TickObject::get_sync_var(int p_index) const {
	Node *root = get_root_node();
	ERR_FAIL_NULL_V(root, Variant());
	ERR_FAIL_INDEX_V(p_index, int(property_paths.size()), Variant());
	return root->get_indexed(property_paths[p_index].get_subnames());
}


// `TickSyncObject`: sets a declared property of the root node.
void TickObject::set_sync_var(int p_index, const Variant &p_value) {
	Node *root = get_root_node();
	ERR_FAIL_NULL(root);
	ERR_FAIL_INDEX(p_index, int(property_paths.size()));
	root->set_indexed(property_paths[p_index].get_subnames(), p_value);
}


// `TickSyncObject`: lets the script write the input, through `_collect_input()`.
void TickObject::collect_input(TickDataBuffer &r_input) {
	input_wrapper->wrap(&r_input);
	GDVIRTUAL_CALL(_collect_input, input_wrapper);
	input_wrapper->wrap(nullptr);
}


// `TickSyncObject`: lets the script advance the object, through `_process_tick()`.
void TickObject::process_tick(double p_delta, TickDataBuffer &p_input) {
	input_wrapper->wrap(&p_input);
	GDVIRTUAL_CALL(_process_tick, p_delta, input_wrapper);
	input_wrapper->wrap(nullptr);
}


// `TickSyncObject`: hands the interpolated state to `_apply_interpolated_state()` when the script has it; sets the
// properties otherwise.
void TickObject::apply_interpolated_state(const LocalVector<Variant> &p_values) {
	if (GDVIRTUAL_IS_OVERRIDDEN(_apply_interpolated_state)) {
		Dictionary state;
		for (uint32_t i = 0; i < p_values.size() && i < schema.names.size(); i++) {
			state[schema.names[i]] = p_values[i];
		}
		GDVIRTUAL_CALL(_apply_interpolated_state, state);
		return;
	}
	TickSyncObject::apply_interpolated_state(p_values);
}


// Exposes the class to scripts.
void TickObject::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_controller_peer", "peer"), &TickObject::set_controller_peer);
	ClassDB::bind_method(D_METHOD("get_controller_peer"), &TickObject::get_controller_peer_id);
	ClassDB::bind_method(D_METHOD("set_remote_mode", "mode"), &TickObject::set_remote_mode);
	ClassDB::bind_method(D_METHOD("get_remote_mode"), &TickObject::get_remote_mode);
	ClassDB::bind_method(D_METHOD("set_root_path", "path"), &TickObject::set_root_path);
	ClassDB::bind_method(D_METHOD("get_root_path"), &TickObject::get_root_path);
	ClassDB::bind_method(D_METHOD("set_network_path", "path"), &TickObject::set_network_path);
	ClassDB::bind_method(D_METHOD("get_network_path"), &TickObject::get_network_path);
	ClassDB::bind_method(D_METHOD("get_root_node"), &TickObject::get_root_node);
	ClassDB::bind_method(D_METHOD("get_network"), &TickObject::get_network);
	ClassDB::bind_method(D_METHOD("declare_var", "property", "codec"), &TickObject::declare_var);
	ClassDB::bind_method(D_METHOD("get_declared_vars"), &TickObject::get_declared_vars);
	ClassDB::bind_method(D_METHOD("get_net_id"), &TickObject::get_net_id);
	ClassDB::bind_method(D_METHOD("is_rewinding"), &TickObject::is_rewinding);
	ClassDB::bind_method(D_METHOD("is_relevant"), &TickObject::is_relevant);
	ClassDB::bind_method(D_METHOD("get_state_at", "frame"), &TickObject::get_state_at);
	ClassDB::bind_method(D_METHOD("send_event", "event", "payload", "frame", "peer"), &TickObject::send_event, DEFVAL(Variant()), DEFVAL(-1), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_owner_peer"), &TickObject::get_owner_peer);
	ClassDB::bind_method(D_METHOD("is_owner"), &TickObject::is_owner);
	ClassDB::bind_method(D_METHOD("request_authority"), &TickObject::request_authority);
	ClassDB::bind_method(D_METHOD("release_authority", "to_peer"), &TickObject::release_authority, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("assign_authority", "peer"), &TickObject::assign_authority);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "controller_peer", PROPERTY_HINT_RANGE, "1,2147483647,1"), "set_controller_peer", "get_controller_peer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "remote_mode", PROPERTY_HINT_ENUM, "Interpolate,Doll"), "set_remote_mode", "get_remote_mode");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "root_path"), "set_root_path", "get_root_path");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "network_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TickNetwork"), "set_network_path", "get_network_path");

	BIND_ENUM_CONSTANT(REMOTE_MODE_INTERPOLATE);
	BIND_ENUM_CONSTANT(REMOTE_MODE_DOLL);

	GDVIRTUAL_BIND(_setup_sync);
	GDVIRTUAL_BIND(_collect_input, "input");
	GDVIRTUAL_BIND(_process_tick, "delta", "input");
	GDVIRTUAL_BIND(_apply_interpolated_state, "state");
	GDVIRTUAL_BIND(_validate_event, "sender", "event", "payload");
	GDVIRTUAL_BIND(_on_event, "sender", "event", "payload", "frame");
	GDVIRTUAL_BIND(_on_authority_changed, "old_owner", "new_owner");
	GDVIRTUAL_BIND(_approve_authority_request, "requester");
	GDVIRTUAL_BIND(_on_relevance_changed, "relevant");
}

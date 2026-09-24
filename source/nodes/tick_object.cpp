#include "tick_object.h"

#include "tick_network.h"

#include "core/config/engine.h"
#include "scene/main/scene_tree.h"

TickObject::TickObject() {
	input_wrapper.instantiate();
}

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

void TickObject::register_to_network() {
	if (Engine::get_singleton()->is_editor_hint() || network) {
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
	network = found;
	network->register_object(this);
}

void TickObject::unregister_from_network() {
	if (network) {
		network->unregister_object(this);
		network = nullptr;
	}
}

void TickObject::set_controller_peer(int p_peer) {
	ERR_FAIL_COND_MSG(network, "The controller can't change while the object is registered.");
	ERR_FAIL_COND_MSG(p_peer <= 0, "The controller peer must be positive (1 is the server).");
	controller_peer = p_peer;
}

void TickObject::set_root_path(const NodePath &p_path) {
	ERR_FAIL_COND_MSG(network, "The root path can't change while the object is registered.");
	root_path = p_path;
}

void TickObject::set_network_path(const NodePath &p_path) {
	ERR_FAIL_COND_MSG(network, "The network path can't change while the object is registered.");
	network_path = p_path;
}

Node *TickObject::get_root_node() const {
	return is_inside_tree() ? get_node_or_null(root_path) : nullptr;
}

void TickObject::declare_var(const StringName &p_property, const Ref<TickCodec> &p_codec) {
	ERR_FAIL_COND_MSG(!declaring, "Variables can only be declared inside `_setup_sync()`.");
	ERR_FAIL_COND_MSG(p_codec.is_null(), vformat("The codec of \"%s\" is null.", p_property));
	const int previous = schema.size();
	schema.add(p_property, p_codec);
	if (schema.size() > previous) {
		property_paths.push_back(NodePath(String(p_property)).get_as_property_path());
	}
}

PackedStringArray TickObject::get_declared_vars() const {
	PackedStringArray names;
	for (const StringName &name : schema.names) {
		names.push_back(name);
	}
	return names;
}

int TickObject::get_net_id() const {
	return network ? network->get_net_id(this) : 0;
}

Error TickObject::send_event(const StringName &p_event, const Variant &p_payload, int64_t p_frame, int p_peer) {
	ERR_FAIL_NULL_V_MSG(network, ERR_UNCONFIGURED, "The object isn't registered in a TickNetwork.");
	return network->send_object_event(this, p_event, p_payload, p_frame, p_peer);
}

int TickObject::validate_event(int p_sender, const StringName &p_event, const Variant &p_payload) {
	bool accepted = false;
	if (GDVIRTUAL_CALL(_validate_event, p_sender, p_event, p_payload, accepted)) {
		return accepted ? 1 : 0;
	}
	return -1;
}

void TickObject::on_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) {
	GDVIRTUAL_CALL(_on_event, p_sender, p_event, p_payload, int64_t(p_frame));
}

bool TickObject::is_rewinding() const {
	return network && network->is_rewinding();
}

Variant TickObject::get_sync_var(int p_index) const {
	Node *root = get_root_node();
	ERR_FAIL_NULL_V(root, Variant());
	ERR_FAIL_INDEX_V(p_index, int(property_paths.size()), Variant());
	return root->get_indexed(property_paths[p_index].get_subnames());
}

void TickObject::set_sync_var(int p_index, const Variant &p_value) {
	Node *root = get_root_node();
	ERR_FAIL_NULL(root);
	ERR_FAIL_INDEX(p_index, int(property_paths.size()));
	root->set_indexed(property_paths[p_index].get_subnames(), p_value);
}

void TickObject::collect_input(TickDataBuffer &r_input) {
	input_wrapper->wrap(&r_input);
	GDVIRTUAL_CALL(_collect_input, input_wrapper);
	input_wrapper->wrap(nullptr);
}

void TickObject::process_tick(double p_delta, TickDataBuffer &p_input) {
	input_wrapper->wrap(&p_input);
	GDVIRTUAL_CALL(_process_tick, p_delta, input_wrapper);
	input_wrapper->wrap(nullptr);
}

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

void TickObject::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_controller_peer", "peer"), &TickObject::set_controller_peer);
	ClassDB::bind_method(D_METHOD("get_controller_peer"), &TickObject::get_controller_peer_id);
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
	ClassDB::bind_method(D_METHOD("send_event", "event", "payload", "frame", "peer"), &TickObject::send_event, DEFVAL(Variant()), DEFVAL(-1), DEFVAL(0));

	ADD_PROPERTY(PropertyInfo(Variant::INT, "controller_peer", PROPERTY_HINT_RANGE, "1,2147483647,1"), "set_controller_peer", "get_controller_peer");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "root_path"), "set_root_path", "get_root_path");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "network_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TickNetwork"), "set_network_path", "get_network_path");

	GDVIRTUAL_BIND(_setup_sync);
	GDVIRTUAL_BIND(_collect_input, "input");
	GDVIRTUAL_BIND(_process_tick, "delta", "input");
	GDVIRTUAL_BIND(_apply_interpolated_state, "state");
	GDVIRTUAL_BIND(_validate_event, "sender", "event", "payload");
	GDVIRTUAL_BIND(_on_event, "sender", "event", "payload", "frame");
}

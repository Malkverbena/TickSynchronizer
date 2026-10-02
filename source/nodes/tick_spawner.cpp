// Implementation of `TickSpawner`: making the node of a spawn (a listed scene, or what the game's function returns),
// announcing it through the network, and following the spawns and despawns of the other peers.

#include "tick_spawner.h"

#include "../common/tick_engine_compat.h"
#include "tick_network.h"
#include "tick_object.h"

#include "core/io/resource_loader.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/packed_scene.h"

// The network this spawner works with: the one at `network_path`, the nearest ancestor, or the first one in the
// scene.
TickNetwork *TickSpawner::find_network() const {
	if (!network_path.is_empty()) {
		return Object::cast_to<TickNetwork>(get_node_or_null(network_path));
	}
	for (Node *node = get_parent(); node; node = node->get_parent()) {
		TickNetwork *found = Object::cast_to<TickNetwork>(node);
		if (found) {
			return found;
		}
	}
	return is_inside_tree() ? Object::cast_to<TickNetwork>(get_tree()->get_first_node_in_group(SNAME("_tick_networks"))) : nullptr;
}


// Adds a scene to the list, if it isn't there.
void TickSpawner::add_spawnable_scene(const String &p_path) {
	if (!spawnable_scenes.has(p_path)) {
		spawnable_scenes.push_back(p_path);
	}
}


// Makes the node of a spawn: scene `p_scene` of `spawnable_scenes`, or, when it's negative, what `spawn_function`
// returns for `p_data`. Null on failure.
Node *TickSpawner::instantiate(int p_scene, const Variant &p_data) const {
	if (p_scene < 0) {
		ERR_FAIL_COND_V_MSG(!spawn_function.is_valid(), nullptr, "A custom spawn needs a valid `spawn_function`.");
		const Variant result = spawn_function.call(p_data);
		Node *node = Object::cast_to<Node>(result.get_validated_object());
		ERR_FAIL_NULL_V_MSG(node, nullptr, "The `spawn_function` must return a new Node.");
		ERR_FAIL_COND_V_MSG(node->is_inside_tree(), nullptr, "The `spawn_function` must return a Node that isn't in the tree.");
		return node;
	}
	// Only the scenes in the list can be instantiated: the index comes from the network.
	ERR_FAIL_INDEX_V_MSG(p_scene, spawnable_scenes.size(), nullptr, "The spawned scene isn't in `spawnable_scenes`.");
	Ref<PackedScene> scene = ResourceLoader::load(spawnable_scenes[p_scene]);
	ERR_FAIL_COND_V_MSG(scene.is_null(), nullptr, vformat("Can't load the scene \"%s\".", spawnable_scenes[p_scene]));
	return scene->instantiate();
}


// Sets the controller of every `TickObject` in a node and in its children.
void TickSpawner::apply_controller(Node *p_node, int p_controller) {
	TickObject *object = Object::cast_to<TickObject>(p_node);
	if (object) {
		object->set_controller_peer(p_controller);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		apply_controller(p_node->get_child(i), p_controller);
	}
}


// Names a spawned node, adds it under `spawn_path`, keeps it by its spawn id and emits `spawned`. Frees the node
// when the spawn path leads nowhere.
Node *TickSpawner::add_spawned(Node *p_node, uint32_t p_spawn_id, const String &p_name) {
	Node *parent = get_node_or_null(spawn_path);
	if (parent == nullptr) {
		memdelete(p_node);
		ERR_FAIL_V_MSG(nullptr, "TickSpawner can't find the node at `spawn_path`.");
	}
	p_node->set_name(p_name);
	nodes_by_spawn.insert(p_spawn_id, p_node->get_instance_id());
	p_node->connect(SceneStringName(tree_exiting), callable_mp(this, &TickSpawner::_on_spawned_exiting).bind(p_spawn_id), CONNECT_ONE_SHOT);
	parent->add_child(p_node);
	emit_signal(SNAME("spawned"), p_node);
	return p_node;
}


// Spawns on this peer and tells the others: checks that this peer may spawn, makes the node, announces the spawn
// and adds the node. Null when it can't.
Node *TickSpawner::server_spawn(int p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	TickNetwork *network = find_network();
	ERR_FAIL_NULL_V_MSG(network, nullptr, "TickSpawner can't find a TickNetwork.");
	ERR_FAIL_COND_V_MSG(!network->can_spawn(), nullptr, "Only the server of a running network (or any node of a distributed one) can spawn.");
	ERR_FAIL_COND_V_MSG(p_controller < 0, nullptr, "The controller peer can't be negative (0 is this peer).");
	const int controller = p_controller == 0 ? network->get_local_peer_id() : p_controller;
	Node *network_root = network->get_root_node();
	ERR_FAIL_NULL_V(network_root, nullptr);
	Node *parent = get_node_or_null(spawn_path);
	ERR_FAIL_NULL_V_MSG(parent, nullptr, "TickSpawner can't find the node at `spawn_path`.");

	Node *node = instantiate(p_scene, p_data);
	ERR_FAIL_NULL_V(node, nullptr);
	String name = p_name;
	if (name.is_empty()) {
		const String base = p_scene >= 0 ? spawnable_scenes[p_scene].get_file().get_basename() : String("Spawn");
		name = vformat("%s_%d", base, network->get_next_spawn_id());
	}
	if (parent->has_node(NodePath(name))) {
		memdelete(node);
		ERR_FAIL_V_MSG(nullptr, vformat("A node named \"%s\" already exists under the spawn path.", name));
	}
	apply_controller(node, controller);

	// The spawn message goes before the objects of the node register.
	const uint32_t spawn_id = network->spawn(String(network_root->get_path_to(this)), p_scene, name, controller, p_data);
	if (spawn_id == 0) {
		// The network refused it (and said why): the node would exist only here.
		memdelete(node);
		return nullptr;
	}
	return add_spawned(node, spawn_id, name);
}


// Spawns one of `spawnable_scenes` here and on every other peer. Only the server of a single authority network, or
// any node of a distributed one, can. `p_controller` becomes the controller of every `TickObject` of the new node
// (0: this peer). An empty name gets one from the scene and the spawn id.
Node *TickSpawner::spawn(const String &p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	const int index = spawnable_scenes.find(p_scene);
	ERR_FAIL_COND_V_MSG(index < 0, nullptr, vformat("\"%s\" isn't in `spawnable_scenes`.", p_scene));
	return server_spawn(index, p_name, p_controller, p_data);
}


// Spawns the node `spawn_function` makes from `p_data`, here and on every other peer; the rest is as in `spawn()`.
Node *TickSpawner::spawn_custom(const Variant &p_data, const String &p_name, int p_controller) {
	return server_spawn(-1, p_name, p_controller, p_data);
}


// The nodes spawned through this spawner that are still alive.
TypedArray<Node> TickSpawner::get_spawned_nodes() const {
	TypedArray<Node> nodes;
	for (const KeyValue<uint32_t, ObjectID> &E : nodes_by_spawn) {
		Node *node = ObjectDB::get_instance<Node>(E.value);
		if (node) {
			nodes.push_back(node);
		}
	}
	return nodes;
}


// A spawned node is leaving the tree: forgets it, despawns it on the other peers when this peer owns the spawn, and
// emits `despawned`.
void TickSpawner::_on_spawned_exiting(uint32_t p_spawn_id) {
	const ObjectID *id = nodes_by_spawn.getptr(p_spawn_id);
	Node *node = id ? ObjectDB::get_instance<Node>(*id) : nullptr;
	nodes_by_spawn.erase(p_spawn_id);
	TickNetwork *network = find_network();
	if (network && network->owns_spawn(p_spawn_id)) {
		network->despawn(p_spawn_id);
	}
	if (node) {
		emit_signal(SNAME("despawned"), node);
	}
}


// Called by the network: another peer spawned this; makes the same node here. A spawn already known is ignored (a
// new host sends them again after a migration).
void TickSpawner::client_spawn(uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) {
	if (nodes_by_spawn.has(p_spawn_id)) {
		// Sent again by a new host after a migration.
		return;
	}
	Node *parent = get_node_or_null(spawn_path);
	ERR_FAIL_NULL_MSG(parent, "TickSpawner can't find the node at `spawn_path`.");
	ERR_FAIL_COND_MSG(p_name.validate_node_name() != p_name || parent->has_node(NodePath(p_name)), vformat("Can't spawn a node named \"%s\".", p_name));
	Node *node = instantiate(p_scene, p_data);
	ERR_FAIL_NULL(node);
	apply_controller(node, p_controller);
	add_spawned(node, p_spawn_id, p_name);
}


// Called by the network: another peer removed a spawn; frees its node here.
void TickSpawner::client_despawn(uint32_t p_spawn_id) {
	const ObjectID *id = nodes_by_spawn.getptr(p_spawn_id);
	ERR_FAIL_NULL_MSG(id, vformat("Spawn %d doesn't exist.", p_spawn_id));
	Node *node = ObjectDB::get_instance<Node>(*id);
	if (node) {
		node->queue_free();
	}
}


// Exposes the class to scripts.
void TickSpawner::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_spawn_path", "path"), &TickSpawner::set_spawn_path);
	ClassDB::bind_method(D_METHOD("get_spawn_path"), &TickSpawner::get_spawn_path);
	ClassDB::bind_method(D_METHOD("set_network_path", "path"), &TickSpawner::set_network_path);
	ClassDB::bind_method(D_METHOD("get_network_path"), &TickSpawner::get_network_path);
	ClassDB::bind_method(D_METHOD("set_spawnable_scenes", "scenes"), &TickSpawner::set_spawnable_scenes);
	ClassDB::bind_method(D_METHOD("get_spawnable_scenes"), &TickSpawner::get_spawnable_scenes);
	ClassDB::bind_method(D_METHOD("add_spawnable_scene", "path"), &TickSpawner::add_spawnable_scene);
	ClassDB::bind_method(D_METHOD("set_spawn_function", "function"), &TickSpawner::set_spawn_function);
	ClassDB::bind_method(D_METHOD("get_spawn_function"), &TickSpawner::get_spawn_function);
	ClassDB::bind_method(D_METHOD("spawn", "scene", "name", "controller_peer", "data"), &TickSpawner::spawn, DEFVAL(String()), DEFVAL(0), DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("spawn_custom", "data", "name", "controller_peer"), &TickSpawner::spawn_custom, DEFVAL(String()), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_spawned_nodes"), &TickSpawner::get_spawned_nodes);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "spawn_path"), "set_spawn_path", "get_spawn_path");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "network_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TickNetwork"), "set_network_path", "get_network_path");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "spawnable_scenes", PROPERTY_HINT_TYPE_STRING, vformat("%s/%s:*.tscn,*.scn", Variant::STRING, PROPERTY_HINT_FILE)), "set_spawnable_scenes", "get_spawnable_scenes");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "spawn_function"), "set_spawn_function", "get_spawn_function");

	ADD_SIGNAL(MethodInfo("spawned", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_RESOURCE_TYPE, "Node")));
	ADD_SIGNAL(MethodInfo("despawned", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_RESOURCE_TYPE, "Node")));
}

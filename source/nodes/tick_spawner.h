#pragma once

#include "core/templates/hash_set.h"
#include "core/variant/typed_array.h"
#include "scene/main/node.h"

class TickNetwork;

// Replicates the creation and removal of nodes from the server to the clients (ADR-031).
//
// The server calls `spawn()` with one of `spawnable_scenes`, or `spawn_custom()` with data for `spawn_function`;
// every client creates the same node under `spawn_path`. A spawned node removed from the tree on the server is
// removed on the clients. Clients joining later receive the live spawns.
class TickSpawner : public Node {
	GDCLASS(TickSpawner, Node);

	NodePath spawn_path = NodePath("..");
	NodePath network_path;
	PackedStringArray spawnable_scenes;
	Callable spawn_function;

	HashMap<uint32_t, ObjectID> nodes_by_spawn;
	// Spawns made by this peer: removing their node despawns them on the others.
	HashSet<uint32_t> local_spawns;

	TickNetwork *find_network() const;
	Node *instantiate(int p_scene, const Variant &p_data) const;
	static void apply_controller(Node *p_node, int p_controller);
	Node *add_spawned(Node *p_node, uint32_t p_spawn_id, const String &p_name);
	Node *server_spawn(int p_scene, const String &p_name, int p_controller, const Variant &p_data);
	void _on_spawned_exiting(uint32_t p_spawn_id);

protected:
	static void _bind_methods();

public:
	void set_spawn_path(const NodePath &p_path) { spawn_path = p_path; }
	NodePath get_spawn_path() const { return spawn_path; }
	void set_network_path(const NodePath &p_path) { network_path = p_path; }
	NodePath get_network_path() const { return network_path; }
	void set_spawnable_scenes(const PackedStringArray &p_scenes) { spawnable_scenes = p_scenes; }
	PackedStringArray get_spawnable_scenes() const { return spawnable_scenes; }
	void add_spawnable_scene(const String &p_path);
	void set_spawn_function(const Callable &p_function) { spawn_function = p_function; }
	Callable get_spawn_function() const { return spawn_function; }

	// The server of a single authority network, or any node of a distributed one. `p_controller` becomes the
	// controller of every `TickObject` of the new node (0: this peer).
	Node *spawn(const String &p_scene, const String &p_name, int p_controller, const Variant &p_data);
	Node *spawn_custom(const Variant &p_data, const String &p_name, int p_controller);
	TypedArray<Node> get_spawned_nodes() const;

	// Client side, called by the network.
	void client_spawn(uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data);
	void client_despawn(uint32_t p_spawn_id);
};

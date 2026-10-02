// The node that replicates the creation and removal of nodes: `TickSpawner`.
//
// It replicates spawns from the server to the clients (ADR-031); in a network with distributed authority, from any node
// to the others. The spawning peer calls `spawn()` with one of `spawnable_scenes`, or `spawn_custom()` with data for
// `spawn_function`; every other peer creates the same node under `spawn_path`. A spawned node removed from the tree on
// the peer that owns the spawn is removed on the others. Peers joining later receive the live spawns. After a host
// migration, the new server owns the old one's spawns too.

#pragma once

#include "core/templates/hash_map.h"
#include "core/variant/typed_array.h"
#include "scene/main/node.h"

class TickNetwork;

class TickSpawner : public Node {
	GDCLASS(TickSpawner, Node);

	NodePath spawn_path = NodePath("..");
	NodePath network_path;
	PackedStringArray spawnable_scenes;
	Callable spawn_function;

	// Removing the node of a spawn this peer owns (see `TickNetwork::owns_spawn()`) despawns it on the others.
	HashMap<uint32_t, ObjectID> nodes_by_spawn;

	// The network this spawner works with: the one at `network_path`, the nearest ancestor, or the first one in the
	// scene.
	TickNetwork *find_network() const;


	// Makes the node of a spawn: scene `p_scene` of `spawnable_scenes`, or, when it's negative, what `spawn_function`
	// returns for `p_data`. Null on failure.
	Node *instantiate(int p_scene, const Variant &p_data) const;


	// Sets the controller of every `TickObject` in a node and in its children.
	static void apply_controller(Node *p_node, int p_controller);


	// Names a spawned node, adds it under `spawn_path`, keeps it by its spawn id and emits `spawned`. Frees the node
	// when the spawn path leads nowhere.
	Node *add_spawned(Node *p_node, uint32_t p_spawn_id, const String &p_name);


	// Spawns on this peer and tells the others: checks that this peer may spawn, makes the node, announces the spawn
	// and adds the node. Null when it can't.
	Node *server_spawn(int p_scene, const String &p_name, int p_controller, const Variant &p_data);


	// A spawned node is leaving the tree: forgets it, despawns it on the other peers when this peer owns the spawn, and
	// emits `despawned`.
	void _on_spawned_exiting(uint32_t p_spawn_id);


protected:
	// Exposes the class to scripts.
	static void _bind_methods();


public:
	// Sets the node the spawned nodes are added under, relative to this one.
	void set_spawn_path(const NodePath &p_path) { spawn_path = p_path; }


	// See `set_spawn_path()`.
	NodePath get_spawn_path() const { return spawn_path; }


	// Sets the `TickNetwork` to use; empty looks for one among the ancestors, then in the scene.
	void set_network_path(const NodePath &p_path) { network_path = p_path; }


	// See `set_network_path()`.
	NodePath get_network_path() const { return network_path; }


	// Sets the scenes `spawn()` may instantiate. Every peer needs the same list, in the same order: the index goes on
	// the wire.
	void set_spawnable_scenes(const PackedStringArray &p_scenes) { spawnable_scenes = p_scenes; }


	// See `set_spawnable_scenes()`.
	PackedStringArray get_spawnable_scenes() const { return spawnable_scenes; }


	// Adds a scene to the list, if it isn't there.
	void add_spawnable_scene(const String &p_path);


	// Sets the function that makes the node of a custom spawn from its data. It must return a new node, outside the
	// tree.
	void set_spawn_function(const Callable &p_function) { spawn_function = p_function; }


	// See `set_spawn_function()`.
	Callable get_spawn_function() const { return spawn_function; }


	// Spawns one of `spawnable_scenes` here and on every other peer. Only the server of a single authority network, or
	// any node of a distributed one, can. `p_controller` becomes the controller of every `TickObject` of the new node
	// (0: this peer). An empty name gets one from the scene and the spawn id.
	Node *spawn(const String &p_scene, const String &p_name, int p_controller, const Variant &p_data);


	// Spawns the node `spawn_function` makes from `p_data`, here and on every other peer; the rest is as in `spawn()`.
	Node *spawn_custom(const Variant &p_data, const String &p_name, int p_controller);


	// The nodes spawned through this spawner that are still alive.
	TypedArray<Node> get_spawned_nodes() const;


	// Called by the network: another peer spawned this; makes the same node here. A spawn already known is ignored (a
	// new host sends them again after a migration).
	void client_spawn(uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data);


	// Called by the network: another peer removed a spawn; frees its node here.
	void client_despawn(uint32_t p_spawn_id);
};

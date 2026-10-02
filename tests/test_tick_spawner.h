// Tests of `TickSpawner` in a scene tree: a listed scene and a custom spawn are created on the client with their
// controller, their objects are bound and exchange events, data that can't be sent spawns nothing, and removing a
// spawned node on the server removes it on the client.
//
// `SpawnWorld` is a world whose root is its network, with a spawner under it.

#pragma once

#include "../source/nodes/tick_network.h"
#include "../source/nodes/tick_object.h"
#include "../source/nodes/tick_spawner.h"
#include "../source/transport/tick_local_transport.h"

#include "core/io/resource_saver.h"
#include "scene/2d/node_2d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h" // For MODULE_GDSCRIPT_ENABLED.

#ifdef MODULE_GDSCRIPT_ENABLED
#include "modules/gdscript/gdscript.h"
#endif

namespace TestTickSpawner {

#ifdef MODULE_GDSCRIPT_ENABLED

// Compiles GDScript source into a script; the test fails if it doesn't compile.
inline Ref<GDScript> make_spawner_script(const String &p_source) {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code(p_source);
	ERR_PRINT_OFF;
	const Error err = script->reload();
	ERR_PRINT_ON;
	CHECK(err == OK);
	return script;
}

// The network is the root of each world, so objects find it as an ancestor.
struct SpawnWorld {
	TickNetwork *network = nullptr;
	TickSpawner *spawner = nullptr;
	Ref<RefCounted> factory;

	// Builds a network named `p_name` with a spawner that knows the scene at `p_scene_path` and makes custom spawns
	// with the factory script, and adds it to the scene tree.
	SpawnWorld(const String &p_name, const String &p_scene_path, const Ref<GDScript> &p_factory_script) {
		network = memnew(TickNetwork);
		network->set_name(p_name);
		network->set_root_path(NodePath("."));
		spawner = memnew(TickSpawner);
		spawner->set_name("Spawner");
		spawner->add_spawnable_scene(p_scene_path);
		factory.instantiate();
		factory->set_script(p_factory_script);
		spawner->set_spawn_function(Callable(factory.ptr(), "make"));
		network->add_child(spawner);
		SceneTree::get_singleton()->get_root()->add_child(network);
	}


	// Stops the network and frees the world.
	~SpawnWorld() {
		network->stop();
		memdelete(network);
	}
};

// Runs the simulated network and both worlds for `p_seconds`.
inline void run_worlds(TickLocalNetwork &r_network, SpawnWorld &r_a, SpawnWorld &r_b, double p_seconds) {
	for (int i = 0; i < int(p_seconds * 60.0); i++) {
		r_network.process(1.0 / 60.0);
		r_a.network->advance(1.0 / 60.0, r_network.get_time_usec());
		r_b.network->advance(1.0 / 60.0, r_network.get_time_usec());
	}
}


// The server spawns a scene and a custom node: the client gets both, with the controller set and the objects bound; the
// controller's events arrive, filtered by the object's validator; a spawn with data that can't be sent doesn't happen;
// and freeing a spawned node on the server frees it on the client.
TEST_CASE("[SceneTree][Modules][TickSynchronizer][TickSpawner] Scenes and custom spawns replicate with their controller and events") {
	// Objects created by `spawn_function`: a body with a scripted TickObject that records its events.
	const Ref<GDScript> object_script = make_spawner_script(R"(
extends TickObject

var received := []

func _setup_sync():
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_SINGLE))

func _validate_event(sender: int, event: StringName, payload: Variant) -> bool:
	return event != &"forbidden"

func _on_event(sender: int, event: StringName, payload: Variant, frame: int):
	received.push_back([sender, event, payload])
)");
	const Ref<GDScript> factory_script = make_spawner_script(R"(
extends RefCounted

var object_script: Script

func make(data):
	var body := Node2D.new()
	body.position = data
	var sync := TickObject.new()
	sync.name = "Sync"
	sync.set_script(object_script)
	body.add_child(sync)
	return body
)");

	// A scene with a plain TickObject, saved to a temporary file.
	Node2D *scene_root = memnew(Node2D);
	scene_root->set_name("Crate");
	TickObject *scene_object = memnew(TickObject);
	scene_object->set_name("Sync");
	scene_root->add_child(scene_object);
	scene_object->set_owner(scene_root);
	Ref<PackedScene> packed;
	packed.instantiate();
	REQUIRE(packed->pack(scene_root) == OK);
	memdelete(scene_root);
	const String scene_path = TestUtils::get_temp_path("tick_spawner_crate.tscn");
	REQUIRE(ResourceSaver::save(packed, scene_path) == OK);

	TickLocalNetwork local_network;
	local_network.set_latency_usec(20000);
	Ref<TickLocalTransport> server_transport = local_network.add_peer();
	Ref<TickLocalTransport> client_transport = local_network.add_peer();

	SpawnWorld server("ServerNet", scene_path, factory_script);
	SpawnWorld client("ClientNet", scene_path, factory_script);
	server.factory->set("object_script", object_script);
	client.factory->set("object_script", object_script);

	REQUIRE(server.network->start(server_transport) == OK);
	REQUIRE(client.network->start(client_transport) == OK);
	local_network.connect_peers(1, 2);
	run_worlds(local_network, server, client, 1.0);

	// Only the server spawns.
	ERR_PRINT_OFF;
	CHECK(client.spawner->spawn(scene_path, "Nope", 1, Variant()) == nullptr);
	ERR_PRINT_ON;

	Node *crate = server.spawner->spawn(scene_path, "", 1, Variant());
	REQUIRE(crate != nullptr);
	Node *player = server.spawner->spawn_custom(Vector2(5, 6), "Player_2", 2);
	REQUIRE(player != nullptr);
	// Data the network can't send: nothing is spawned, not even here.
	Ref<RefCounted> not_sendable;
	not_sendable.instantiate();
	ERR_PRINT_OFF;
	CHECK(server.spawner->spawn(scene_path, "Unsent", 1, not_sendable) == nullptr);
	ERR_PRINT_ON;
	CHECK(server.network->get_node_or_null(NodePath("Unsent")) == nullptr);
	CHECK(server.spawner->get_spawned_nodes().size() == 2);
	run_worlds(local_network, server, client, 1.0);

	Node *client_crate = client.network->get_node_or_null(NodePath(String(crate->get_name())));
	Node2D *client_player = Object::cast_to<Node2D>(client.network->get_node_or_null(NodePath("Player_2")));
	REQUIRE(client_crate != nullptr);
	REQUIRE(client_player != nullptr);
	CHECK(client.spawner->get_spawned_nodes().size() == 2);
	CHECK(client_player->get_position() == Vector2(5, 6));

	TickObject *server_sync = Object::cast_to<TickObject>(player->get_node(NodePath("Sync")));
	TickObject *client_sync = Object::cast_to<TickObject>(client_player->get_node(NodePath("Sync")));
	REQUIRE(server_sync != nullptr);
	REQUIRE(client_sync != nullptr);
	// The spawn's controller reached the objects on both sides, and they're bound.
	CHECK(client_sync->get_controller_peer() == 2);
	CHECK(client_sync->get_net_id() == server_sync->get_net_id());
	CHECK(client_sync->get_net_id() != 0);

	// Events from the controller, filtered by the object's `_validate_event`.
	CHECK(client_sync->send_event("wave", 3, -1, 0) == OK);
	CHECK(client_sync->send_event("forbidden", 4, -1, 0) == OK);
	run_worlds(local_network, server, client, 0.5);
	const Array received = server_sync->get("received");
	REQUIRE(received.size() == 1);
	CHECK(int(Array(received[0])[0]) == 2);
	CHECK(StringName(Array(received[0])[1]) == StringName("wave"));

	// Despawn: removing the node on the server removes it on the client.
	const NodePath crate_path = NodePath(String(crate->get_name()));
	crate->queue_free();
	// Deletions happen when the scene tree flushes its queue.
	SceneTree::get_singleton()->process(0.0);
	run_worlds(local_network, server, client, 0.5);
	SceneTree::get_singleton()->process(0.0);
	CHECK(server.network->get_node_or_null(crate_path) == nullptr);
	CHECK(client.network->get_node_or_null(crate_path) == nullptr);
	CHECK(client.spawner->get_spawned_nodes().size() == 1);
}

#endif // MODULE_GDSCRIPT_ENABLED

} // namespace TestTickSpawner

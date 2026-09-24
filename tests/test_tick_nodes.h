#pragma once

#include "../source/nodes/tick_network.h"
#include "../source/nodes/tick_object.h"
#include "../source/transport/tick_local_transport.h"

#include "scene/2d/node_2d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "tests/test_macros.h"

#include "modules/modules_enabled.gen.h" // For MODULE_GDSCRIPT_ENABLED.

#ifdef MODULE_GDSCRIPT_ENABLED
#include "modules/gdscript/gdscript.h"
#endif

namespace TestTickNodes {

#ifdef MODULE_GDSCRIPT_ENABLED

inline Ref<GDScript> make_script(const String &p_source) {
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

// A world: a root with a network and two bodies, the player (controlled by peer 2) and an NPC (the server's).
struct TestNodeWorld {
	Node *root = nullptr;
	TickNetwork *network = nullptr;
	Node2D *player = nullptr;
	Node2D *npc = nullptr;
	TickObject *player_sync = nullptr;
	TickObject *npc_sync = nullptr;

	TestNodeWorld(const String &p_name, const Ref<GDScript> &p_player_script, const Ref<GDScript> &p_npc_script) {
		root = memnew(Node);
		root->set_name(p_name);
		network = memnew(TickNetwork);
		network->set_name("Net");
		root->add_child(network);

		player = memnew(Node2D);
		player->set_name("Player");
		root->add_child(player);
		player_sync = memnew(TickObject);
		player_sync->set_controller_peer(2);
		player_sync->set_network_path(NodePath("../../Net"));
		player_sync->set_script(p_player_script);
		player->add_child(player_sync);

		npc = memnew(Node2D);
		npc->set_name("Npc");
		root->add_child(npc);
		npc_sync = memnew(TickObject);
		npc_sync->set_network_path(NodePath("../../Net"));
		npc_sync->set_script(p_npc_script);
		npc->add_child(npc_sync);

		SceneTree::get_singleton()->get_root()->add_child(root);
	}

	~TestNodeWorld() {
		memdelete(root);
	}
};

TEST_CASE("[SceneTree][Modules][TickSynchronizer][TickNetwork] Nodes predict and interpolate over a simulated network") {
	const Ref<GDScript> player_script = make_script(R"(
extends TickObject

func _setup_sync():
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_HALF))

func _collect_input(input: DataBuffer):
	input.add_int(1, 2)

func _process_tick(delta: float, input: DataBuffer):
	var direction := 0
	if input.get_size() > 0:
		direction = input.read_int(2)
	get_root_node().position.x += direction * 5.0 * delta
)");
	const Ref<GDScript> npc_script = make_script(R"(
extends TickObject

func _setup_sync():
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_SINGLE))

func _process_tick(delta: float, _input: DataBuffer):
	get_root_node().position.y += 2.0 * delta
)");

	TickLocalNetwork local_network;
	local_network.set_latency_usec(30000);
	Ref<TickLocalTransport> server_transport = local_network.add_peer();
	Ref<TickLocalTransport> client_transport = local_network.add_peer();

	TestNodeWorld server("ServerWorld", player_script, npc_script);
	TestNodeWorld client("ClientWorld", player_script, npc_script);

	CHECK(server.player_sync->get_declared_vars().size() == 1);
	CHECK(server.network->start(server_transport) == OK);
	CHECK(client.network->start(client_transport) == OK);
	local_network.connect_peers(1, 2);

	for (int i = 0; i < 60 * 4; i++) {
		local_network.process(1.0 / 60.0);
		const uint64_t now = local_network.get_time_usec();
		server.network->advance(1.0 / 60.0, now);
		client.network->advance(1.0 / 60.0, now);
	}

	CHECK(server.network->is_server());
	CHECK(client.network->is_predicting());
	CHECK(client.player_sync->get_net_id() == server.player_sync->get_net_id());
	CHECK(client.npc_sync->get_net_id() != 0);

	// The server moved the player with the client's input, and the client predicted it ahead.
	CHECK(server.player->get_position().x > 5.0);
	CHECK(client.player->get_position().x >= server.player->get_position().x);
	// The NPC is interpolated on the client, a little behind the server.
	CHECK(client.npc->get_position().y > 1.0);
	CHECK(client.npc->get_position().y <= server.npc->get_position().y);
	CHECK(server.npc->get_position().y - client.npc->get_position().y < 1.0);

	const Dictionary stats = client.network->get_stats();
	CHECK(int(stats["malformed_packets"]) == 0);

	server.network->stop();
	client.network->stop();
}

#endif // MODULE_GDSCRIPT_ENABLED

TEST_CASE("[Modules][TickSynchronizer][TickObject] Variables can only be declared during the setup") {
	TickObject *object = memnew(TickObject);
	ERR_PRINT_OFF;
	object->declare_var("position", TickCodec::vector2());
	ERR_PRINT_ON;
	CHECK(object->get_declared_vars().is_empty());
	memdelete(object);
}

} // namespace TestTickNodes

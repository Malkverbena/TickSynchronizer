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

namespace TestTickMeshNodes {

#ifdef MODULE_GDSCRIPT_ENABLED

inline Ref<GDScript> make_mesh_script(const String &p_source) {
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

// A node of the mesh: a distributed network (its root) with one body.
struct MeshNodeWorld {
	TickNetwork *network = nullptr;
	Node2D *body = nullptr;
	TickObject *sync = nullptr;
	Ref<RefCounted> recorder;

	MeshNodeWorld(const String &p_name, const Ref<GDScript> &p_script, const Ref<GDScript> &p_recorder_script) {
		network = memnew(TickNetwork);
		network->set_name(p_name);
		network->set_root_path(NodePath("."));
		network->set_authority_mode(TickNetwork::AUTHORITY_DISTRIBUTED);
		network->set_trust(TickNetwork::TRUST_TRUSTED);
		network->set_interpolate_remote(false);
		body = memnew(Node2D);
		body->set_name("Body");
		network->add_child(body);
		sync = memnew(TickObject);
		sync->set_script(p_script);
		body->add_child(sync);
		recorder.instantiate();
		recorder->set_script(p_recorder_script);
		network->connect(SNAME("authority_changed"), Callable(recorder.ptr(), "on_changed"));
		network->connect(SNAME("roles_changed"), Callable(recorder.ptr(), "on_roles"));
		SceneTree::get_singleton()->get_root()->add_child(network);
	}

	~MeshNodeWorld() {
		network->stop();
		memdelete(network);
	}
};

TEST_CASE("[SceneTree][Modules][TickSynchronizer][TickNetwork] Distributed authority through the nodes") {
	const Ref<GDScript> body_script = make_mesh_script(R"(
extends TickObject

var changes := []
var refuse := false

func _setup_sync():
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_SINGLE))

func _process_tick(delta: float, _input: DataBuffer):
	get_root_node().position.x += 5.0 * delta

func _on_authority_changed(old_owner: int, new_owner: int):
	changes.push_back([old_owner, new_owner])

func _approve_authority_request(requester: int) -> bool:
	return not refuse
)");
	const Ref<GDScript> recorder_script = make_mesh_script(R"(
extends RefCounted

var signals := []
var roles := []

func on_changed(object, old_owner, new_owner):
	signals.push_back([object, old_owner, new_owner])

func on_roles(registry_peer, clock_master):
	roles.push_back([registry_peer, clock_master])
)");

	TickLocalNetwork local_network;
	local_network.set_latency_usec(10000);
	Ref<TickLocalTransport> transport_a = local_network.add_peer();
	Ref<TickLocalTransport> transport_b = local_network.add_peer();

	MeshNodeWorld a("MeshA", body_script, recorder_script);
	MeshNodeWorld b("MeshB", body_script, recorder_script);
	REQUIRE(a.network->start(transport_a) == OK);
	REQUIRE(b.network->start(transport_b) == OK);
	local_network.connect_all();

	for (int i = 0; i < 90; i++) {
		local_network.process(1.0 / 60.0);
		a.network->advance(1.0 / 60.0, local_network.get_time_usec());
		b.network->advance(1.0 / 60.0, local_network.get_time_usec());
	}
	// The default controller (1) owns the body; node 2 follows it.
	CHECK(a.sync->get_owner_peer() == 1);
	CHECK(a.sync->is_owner());
	CHECK_FALSE(b.sync->is_owner());
	CHECK(a.body->get_position().x > 1.0);
	CHECK(Math::abs(a.body->get_position().x - b.body->get_position().x) < 0.5);

	// Node 2 asks for it through the TickObject.
	CHECK(b.sync->request_authority() == OK);
	for (int i = 0; i < 30; i++) {
		local_network.process(1.0 / 60.0);
		a.network->advance(1.0 / 60.0, local_network.get_time_usec());
		b.network->advance(1.0 / 60.0, local_network.get_time_usec());
	}
	CHECK(b.sync->is_owner());
	CHECK(a.sync->get_owner_peer() == 2);
	const Array changes = a.sync->get("changes");
	REQUIRE(changes.size() == 1);
	CHECK(int(Array(changes[0])[1]) == 2);
	// The network's signal reports the TickObject.
	const Array signals = b.recorder->get("signals");
	REQUIRE(signals.size() == 1);
	CHECK(Object::cast_to<TickObject>(Array(signals[0])[0]) == b.sync);

	// The owner refuses the next request.
	b.sync->set("refuse", true);
	CHECK(a.sync->request_authority() == OK);
	for (int i = 0; i < 30; i++) {
		local_network.process(1.0 / 60.0);
		a.network->advance(1.0 / 60.0, local_network.get_time_usec());
		b.network->advance(1.0 / 60.0, local_network.get_time_usec());
	}
	CHECK(b.sync->is_owner());
	CHECK(int(a.network->get_stats()["denied_requests"]) == 1);

	// The registry and the clock move while the mesh runs, through the network's properties (ADR-073).
	b.network->set_registry_peer(2);
	b.network->set_clock_master(2);
	b.sync->set("refuse", false);
	for (int i = 0; i < 60; i++) {
		local_network.process(1.0 / 60.0);
		a.network->advance(1.0 / 60.0, local_network.get_time_usec());
		b.network->advance(1.0 / 60.0, local_network.get_time_usec());
	}
	CHECK(a.network->get_registry_peer() == 2);
	CHECK(a.network->get_clock_master() == 2);
	CHECK(b.network->get_registry_peer() == 2);
	const Array roles = a.recorder->get("roles");
	REQUIRE(roles.size() == 2);
	CHECK(int(Array(roles[1])[0]) == 2);
	CHECK(int(Array(roles[1])[1]) == 2);
	// Ownership changes through the new registry.
	CHECK(a.sync->request_authority() == OK);
	for (int i = 0; i < 30; i++) {
		local_network.process(1.0 / 60.0);
		a.network->advance(1.0 / 60.0, local_network.get_time_usec());
		b.network->advance(1.0 / 60.0, local_network.get_time_usec());
	}
	CHECK(a.sync->is_owner());
}

#endif // MODULE_GDSCRIPT_ENABLED

} // namespace TestTickMeshNodes

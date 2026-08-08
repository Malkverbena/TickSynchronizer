// Declares the placeholder synchronized-object Godot node.
// Establishes the future public type without premature runtime behavior.

#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "scene/main/node.h"

class TickSynchronizerObject : public Node {
	GDCLASS(TickSynchronizerObject, Node);

protected:
	// Binds the class API and constants to Godot ClassDB.
	static void _bind_methods();
};

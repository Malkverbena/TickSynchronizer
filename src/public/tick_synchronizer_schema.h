// Declares the placeholder synchronization schema resource.
// Establishes the future schema API without freezing field semantics.

#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "core/io/resource.h"

class TickSynchronizerSchema : public Resource {
	GDCLASS(TickSynchronizerSchema, Resource);

protected:
	// Binds the class API and constants to Godot ClassDB.
	static void _bind_methods();
};

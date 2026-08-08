// Declares the placeholder synchronization settings resource.
// Establishes the future settings API without premature configuration fields.

#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "core/io/resource.h"

class TickSynchronizerSettings : public Resource {
	GDCLASS(TickSynchronizerSettings, Resource);

protected:
	// Binds the class API and constants to Godot ClassDB.
	static void _bind_methods();
};

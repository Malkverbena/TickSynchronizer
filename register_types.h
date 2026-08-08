// Declares TickSynchronizer module lifecycle entry points.
// Provides the conventional interface consumed by Godot module registration.

#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "modules/register_module_types.h"

// Registers public classes at the Godot scene initialization level.
void initialize_tick_synchronizer_module(ModuleInitializationLevel p_level);

// Releases module-level state during Godot shutdown.
void uninitialize_tick_synchronizer_module(ModuleInitializationLevel p_level);

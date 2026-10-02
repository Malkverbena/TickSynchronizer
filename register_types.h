// Entry points of the TickSynchronizer module. The engine calls them once per initialization level, when it starts and
// when it quits. No class is declared here.

#pragma once

#include "modules/register_module_types.h"

// Registers the module's classes with the engine, at the scene level; does nothing at the other levels.
void initialize_tick_synchronizer_module(ModuleInitializationLevel p_level);


// The counterpart of `initialize_tick_synchronizer_module()`. The module keeps nothing that has to be freed.
void uninitialize_tick_synchronizer_module(ModuleInitializationLevel p_level);

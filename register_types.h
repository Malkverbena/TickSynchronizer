#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "modules/register_module_types.h"

void initialize_tick_synchronizer_module(ModuleInitializationLevel p_level);
void uninitialize_tick_synchronizer_module(ModuleInitializationLevel p_level);


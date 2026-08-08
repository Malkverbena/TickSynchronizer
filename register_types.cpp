// Registers and unregisters TickSynchronizer classes with Godot.
// Connects project-owned runtime classes to the engine module lifecycle.

#include "register_types.h"

#include "core/object/class_db.h"
#include "src/public/tick_synchronizer.h"
#include "src/public/tick_synchronizer_buffer.h"
#include "src/public/tick_synchronizer_object.h"
#include "src/public/tick_synchronizer_schema.h"
#include "src/public/tick_synchronizer_settings.h"

void initialize_tick_synchronizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(TickSynchronizer);
	GDREGISTER_CLASS(TickSynchronizerSettings);
	GDREGISTER_CLASS(TickSynchronizerBuffer);
	GDREGISTER_CLASS(TickSynchronizerObject);
	GDREGISTER_CLASS(TickSynchronizerSchema);
}


void uninitialize_tick_synchronizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

#include "register_types.h"

#include "source/codec/tick_codec.h"
#include "source/transport/tick_transport.h"

#include "core/object/class_db.h"

void initialize_tick_synchronizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(TickCodec);
	GDREGISTER_ABSTRACT_CLASS(TickTransport);
}

void uninitialize_tick_synchronizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

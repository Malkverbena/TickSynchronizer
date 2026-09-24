#include "register_types.h"

#include "source/codec/tick_codec.h"
#include "source/nodes/data_buffer.h"
#include "source/nodes/tick_network.h"
#include "source/nodes/tick_object.h"
#include "source/nodes/tick_spawner.h"
#include "source/transport/enet_star_transport.h"
#include "source/transport/tick_transport.h"

#include "core/object/class_db.h"

void initialize_tick_synchronizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(TickCodec);
	GDREGISTER_CLASS(DataBuffer);
	GDREGISTER_ABSTRACT_CLASS(TickTransport);
	GDREGISTER_CLASS(EnetStarTransport);
	GDREGISTER_CLASS(TickNetwork);
	GDREGISTER_CLASS(TickObject);
	GDREGISTER_CLASS(TickSpawner);
}

void uninitialize_tick_synchronizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

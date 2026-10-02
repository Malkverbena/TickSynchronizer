// Registers the classes the module exposes to scripts and to the editor: the codec of the synchronized variables, the
// buffer of bits, the transports and the nodes. The engines behind the nodes (`TickSyncCore`, `TickMeshCore`) are plain
// C++ and aren't registered.

#include "register_types.h"

#include "source/codec/tick_codec.h"
#include "source/nodes/data_buffer.h"
#include "source/nodes/tick_network.h"
#include "source/nodes/tick_object.h"
#include "source/nodes/tick_spawner.h"
#include "source/transport/enet_hosted_mesh_transport.h"
#include "source/transport/enet_mesh_transport.h"
#include "source/transport/enet_star_transport.h"
#include "source/transport/tick_multiplayer_peer.h"
#include "source/transport/tick_transport.h"

#include "core/object/class_db.h"

// Registers the module's classes with the engine, at the scene level; does nothing at the other levels.
void initialize_tick_synchronizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(TickCodec);
	GDREGISTER_CLASS(DataBuffer);
	GDREGISTER_ABSTRACT_CLASS(TickTransport);
	GDREGISTER_CLASS(EnetStarTransport);
	GDREGISTER_CLASS(EnetMeshTransport);
	GDREGISTER_CLASS(EnetHostedMeshTransport);
	GDREGISTER_ABSTRACT_CLASS(TickMultiplayerPeer);
	GDREGISTER_CLASS(TickNetwork);
	GDREGISTER_CLASS(TickObject);
	GDREGISTER_CLASS(TickSpawner);
}


// The counterpart of `initialize_tick_synchronizer_module()`. The module keeps nothing that has to be freed.
void uninitialize_tick_synchronizer_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

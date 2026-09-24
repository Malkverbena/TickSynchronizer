#include "tick_transport.h"

void TickTransport::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_local_peer_id"), &TickTransport::get_local_peer_id);
	ClassDB::bind_method(D_METHOD("is_peer_connected", "peer"), &TickTransport::is_peer_connected);
	ClassDB::bind_method(D_METHOD("get_channel_count"), &TickTransport::get_channel_count);
	ClassDB::bind_method(D_METHOD("get_max_payload_size"), &TickTransport::get_max_payload_size);
	ClassDB::bind_method(D_METHOD("disconnect_peer", "peer"), &TickTransport::disconnect_peer);
	ClassDB::bind_method(D_METHOD("poll"), &TickTransport::poll);

	BIND_ENUM_CONSTANT(TRANSFER_MODE_UNRELIABLE);
	BIND_ENUM_CONSTANT(TRANSFER_MODE_UNRELIABLE_ORDERED);
	BIND_ENUM_CONSTANT(TRANSFER_MODE_RELIABLE);

	BIND_CONSTANT(PEER_BROADCAST);
	BIND_CONSTANT(PEER_SERVER);
}

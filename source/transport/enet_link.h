// How the ENet transports close a link of a socket that keeps being serviced (ADR-075). No class is declared here: only
// the helper every ENet transport of the module uses for it, and the time it gives the other side to answer.

#pragma once

#include "modules/enet/enet_packet_peer.h"

// How long a link closed by this side waits for the other side to acknowledge it, in milliseconds.
static constexpr int ENET_CLOSED_LINK_TIMEOUT_MS = 1000;

// Closes a link of a socket that keeps being serviced. Never with `peer_disconnect_now()` or `reset()`: they leave the
// peer in the socket's list, and `ENetConnection::service()` reads a freed element of that list when it drops the
// peer, which crashes release builds (ADR-075). Closed this way, ENet reports the disconnection once the other side
// acknowledges it, or after the timeout, and the engine takes the peer off the list then. Nothing the other side
// sends meanwhile is delivered. A link whose socket is destroyed right after can still be closed at once.
static inline void enet_close_link(ENetPacketPeer *p_link, int p_data = 0) {
	p_link->set_timeout(32, ENET_CLOSED_LINK_TIMEOUT_MS, ENET_CLOSED_LINK_TIMEOUT_MS);
	p_link->peer_disconnect(p_data);
}

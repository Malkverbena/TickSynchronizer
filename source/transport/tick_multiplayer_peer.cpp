#include "tick_multiplayer_peer.h"

#include "core/string/string_name.h"

void TickMultiplayerPeer::_bind_methods() {
	ADD_SIGNAL(MethodInfo("host_migrated", PropertyInfo(Variant::INT, "old_host"), PropertyInfo(Variant::INT, "new_host")));
}

TickMultiplayerPeer::~TickMultiplayerPeer() {
	if (transport.is_valid() && transport->multiplayer_peer == this) {
		transport->attach_multiplayer_peer(nullptr);
	}
}

const EnetHostedMeshTransport::MultiplayerPacket *TickMultiplayerPeer::get_next_packet() const {
	if (transport.is_null() || transport->next_multiplayer_packet >= transport->multiplayer_packets.size()) {
		return nullptr;
	}
	return &transport->multiplayer_packets[transport->next_multiplayer_packet];
}

int TickMultiplayerPeer::get_available_packet_count() const {
	if (transport.is_null()) {
		return 0;
	}
	return int(transport->multiplayer_packets.size() - transport->next_multiplayer_packet);
}

Error TickMultiplayerPeer::get_packet(const uint8_t **r_buffer, int &r_buffer_size) {
	const EnetHostedMeshTransport::MultiplayerPacket *packet = get_next_packet();
	ERR_FAIL_NULL_V_MSG(packet, ERR_UNAVAILABLE, "No packet available.");
	current_packet = packet->data;
	transport->next_multiplayer_packet++;
	transport->multiplayer_queued_bytes -= current_packet.size();
	if (transport->next_multiplayer_packet >= transport->multiplayer_packets.size()) {
		transport->multiplayer_packets.clear();
		transport->next_multiplayer_packet = 0;
		transport->multiplayer_queued_bytes = 0;
	}
	*r_buffer = current_packet.ptr();
	r_buffer_size = int(current_packet.size());
	return OK;
}

Error TickMultiplayerPeer::put_packet(const uint8_t *p_buffer, int p_buffer_size) {
	ERR_FAIL_COND_V_MSG(transport.is_null(), ERR_UNCONFIGURED, "The multiplayer peer has no transport.");
	ERR_FAIL_COND_V_MSG(transport->get_status() != EnetHostedMeshTransport::STATUS_CONNECTED, ERR_UNCONFIGURED, "The mesh isn't connected.");
	ERR_FAIL_COND_V(p_buffer_size <= 0 || p_buffer == nullptr, ERR_INVALID_PARAMETER);
	return transport->send_multiplayer(target_peer, get_transfer_channel(), TickTransport::TransferMode(get_transfer_mode()), p_buffer, p_buffer_size);
}

int TickMultiplayerPeer::get_max_packet_size() const {
	// ENet fragments bigger packets, as `ENetMultiplayerPeer` does.
	return 1 << 24;
}

int TickMultiplayerPeer::get_packet_peer() const {
	const EnetHostedMeshTransport::MultiplayerPacket *packet = get_next_packet();
	ERR_FAIL_NULL_V_MSG(packet, 0, "No packet available.");
	return packet->from_peer;
}

MultiplayerPeer::TransferMode TickMultiplayerPeer::get_packet_mode() const {
	const EnetHostedMeshTransport::MultiplayerPacket *packet = get_next_packet();
	ERR_FAIL_NULL_V_MSG(packet, TRANSFER_MODE_RELIABLE, "No packet available.");
	return TransferMode(packet->mode);
}

int TickMultiplayerPeer::get_packet_channel() const {
	const EnetHostedMeshTransport::MultiplayerPacket *packet = get_next_packet();
	ERR_FAIL_NULL_V_MSG(packet, 0, "No packet available.");
	return packet->channel;
}

void TickMultiplayerPeer::disconnect_peer(int p_peer, bool p_force) {
	ERR_FAIL_COND(transport.is_null());
	transport->disconnect_peer(p_peer);
}

bool TickMultiplayerPeer::is_server() const {
	return transport.is_valid() && transport->is_hosting();
}

void TickMultiplayerPeer::poll() {
	if (transport.is_null()) {
		return;
	}
	transport->poll();
	// Signals are emitted after polling: a handler may close the peer.
	LocalVector<TickTransport::Event> pending;
	pending = transport->multiplayer_events;
	transport->multiplayer_events.clear();
	for (const TickTransport::Event &event : pending) {
		if (event.type == TickTransport::EVENT_HOST_MIGRATED) {
			// Before the old host's `peer_disconnected`.
			const int old_host = host_peer;
			host_peer = event.peer;
			emit_signal(SNAME("host_migrated"), old_host, event.peer);
			continue;
		}
		emit_signal(event.type == TickTransport::EVENT_PEER_CONNECTED ? SNAME("peer_connected") : SNAME("peer_disconnected"), event.peer);
	}
}

void TickMultiplayerPeer::close() {
	if (transport.is_valid()) {
		transport->close();
	}
}

int TickMultiplayerPeer::get_unique_id() const {
	return transport.is_valid() ? transport->get_local_peer_id() : 0;
}

MultiplayerPeer::ConnectionStatus TickMultiplayerPeer::get_connection_status() const {
	if (transport.is_null()) {
		return CONNECTION_DISCONNECTED;
	}
	switch (transport->get_status()) {
		case EnetHostedMeshTransport::STATUS_CONNECTED:
			return CONNECTION_CONNECTED;
		case EnetHostedMeshTransport::STATUS_CONNECTING:
			return CONNECTION_CONNECTING;
		default:
			return CONNECTION_DISCONNECTED;
	}
}

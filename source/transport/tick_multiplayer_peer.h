// The mesh of players as `SceneMultiplayer` sees it: `TickMultiplayerPeer`.
//
// A `MultiplayerPeer` over an `EnetHostedMeshTransport` (ADR-050): `SceneMultiplayer` (RPCs, spawners, synchronizers)
// reaches every player of the mesh, directly or relayed by the host, next to the `TickNetwork` that uses the same
// transport. Get it with `EnetHostedMeshTransport.get_multiplayer_peer()`.
//
// `SceneMultiplayer` takes peer 1 for the server. After a host migration nobody is peer 1: the `host_migrated` signal
// lets the game move to the new host the authority it gave to peer 1 (ADR-067).

#pragma once

#include "enet_hosted_mesh_transport.h"

#include "scene/main/multiplayer_peer.h"

class TickMultiplayerPeer : public MultiplayerPeer {
	GDCLASS(TickMultiplayerPeer, MultiplayerPeer);
	friend class EnetHostedMeshTransport;

	Ref<EnetHostedMeshTransport> transport;
	int target_peer = TARGET_PEER_BROADCAST;
	// The host as of the events emitted so far: the old host of the next migration.
	int host_peer = 1;
	// The packet returned by the last `get_packet()`, valid until the next call.
	LocalVector<uint8_t> current_packet;

	// The oldest packet the transport queued for `SceneMultiplayer`; null when there is none.
	const EnetHostedMeshTransport::MultiplayerPacket *get_next_packet() const;


protected:
	// Declares the `host_migrated` signal.
	static void _bind_methods();


public:
	// `PacketPeer`: how many packets wait to be taken.
	virtual int get_available_packet_count() const override;


	// `PacketPeer`: takes the oldest packet; its bytes stay valid until the next call.
	virtual Error get_packet(const uint8_t **r_buffer, int &r_buffer_size) override;


	// `PacketPeer`: sends a packet to the target peer, on the transfer channel and with the transfer mode set on this
	// peer.
	virtual Error put_packet(const uint8_t *p_buffer, int p_buffer_size) override;


	// `PacketPeer`: the largest packet; ENet fragments what doesn't fit a datagram.
	virtual int get_max_packet_size() const override;


	// `MultiplayerPeer`: sets who `put_packet()` sends to: a peer, everyone (0), or everyone but one (a negative id).
	virtual void set_target_peer(int p_peer_id) override { target_peer = p_peer_id; }


	// `MultiplayerPeer`: who sent the packet `get_packet()` returns next.
	virtual int get_packet_peer() const override;


	// `MultiplayerPeer`: the transfer mode of the packet `get_packet()` returns next.
	virtual TransferMode get_packet_mode() const override;


	// `MultiplayerPeer`: the transfer channel of the packet `get_packet()` returns next.
	virtual int get_packet_channel() const override;


	// `MultiplayerPeer`: on the host, removes a player from the mesh; on a player, leaves the host. `p_force` is
	// ignored.
	virtual void disconnect_peer(int p_peer, bool p_force = false) override;


	// `MultiplayerPeer`: whether this node hosts the mesh.
	virtual bool is_server() const override;


	// `MultiplayerPeer`: `false`: the transport reaches every player by itself, so `SceneMultiplayer` relays nothing.
	virtual bool is_server_relay_supported() const override { return false; }


	// `MultiplayerPeer`: polls the transport, then emits `peer_connected`, `peer_disconnected` and `host_migrated` for
	// what happened.
	virtual void poll() override;


	// `MultiplayerPeer`: closes the transport: this node leaves the mesh, which ends if it was the host.
	virtual void close() override;


	// `MultiplayerPeer`: this node's id in the mesh; 0 without a transport.
	virtual int get_unique_id() const override;


	// `MultiplayerPeer`: the transport's status, in the terms of `MultiplayerPeer`.
	virtual ConnectionStatus get_connection_status() const override;


	// Detaches from the transport, which stops keeping packets and events for `SceneMultiplayer`.
	~TickMultiplayerPeer();
};

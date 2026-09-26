#pragma once

#include "enet_hosted_mesh_transport.h"

#include "scene/main/multiplayer_peer.h"

// A `MultiplayerPeer` over an `EnetHostedMeshTransport` (ADR-050): `SceneMultiplayer` (RPCs, spawners, synchronizers)
// reaches every player of the mesh, directly or relayed by the host, next to the `TickNetwork` that uses the same
// transport. Get it with `EnetHostedMeshTransport.get_multiplayer_peer()`.
//
// `SceneMultiplayer` takes peer 1 for the server. After a host migration nobody is peer 1: the `host_migrated` signal
// lets the game move to the new host the authority it gave to peer 1 (ADR-067).
class TickMultiplayerPeer : public MultiplayerPeer {
	GDCLASS(TickMultiplayerPeer, MultiplayerPeer);
	friend class EnetHostedMeshTransport;

	Ref<EnetHostedMeshTransport> transport;
	int target_peer = TARGET_PEER_BROADCAST;
	// The host as of the events emitted so far: the old host of the next migration.
	int host_peer = 1;
	// The packet returned by the last `get_packet()`, valid until the next call.
	LocalVector<uint8_t> current_packet;

	const EnetHostedMeshTransport::MultiplayerPacket *get_next_packet() const;

protected:
	static void _bind_methods();

public:
	// PacketPeer.
	virtual int get_available_packet_count() const override;
	virtual Error get_packet(const uint8_t **r_buffer, int &r_buffer_size) override;
	virtual Error put_packet(const uint8_t *p_buffer, int p_buffer_size) override;
	virtual int get_max_packet_size() const override;

	// MultiplayerPeer.
	virtual void set_target_peer(int p_peer_id) override { target_peer = p_peer_id; }
	virtual int get_packet_peer() const override;
	virtual TransferMode get_packet_mode() const override;
	virtual int get_packet_channel() const override;
	virtual void disconnect_peer(int p_peer, bool p_force = false) override;
	virtual bool is_server() const override;
	virtual bool is_server_relay_supported() const override { return false; }
	virtual void poll() override;
	virtual void close() override;
	virtual int get_unique_id() const override;
	virtual ConnectionStatus get_connection_status() const override;

	~TickMultiplayerPeer();
};

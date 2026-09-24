#pragma once

#include "core/error/error_list.h"
#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"

// Moves bytes between peers; the sync engines only talk to the network through this interface.
//
// The transport identifies the sender of every packet: `Packet::from_peer` comes from the connection, never from
// the payload, so a peer can't impersonate another one.
//
// Events and packets are queued by `poll()` and consumed with `pop_event()` and `pop_packet()`.
class TickTransport : public RefCounted {
	GDCLASS(TickTransport, RefCounted);

public:
	enum TransferMode {
		// May be lost, duplicated never, and arrive in any order.
		TRANSFER_MODE_UNRELIABLE,
		// May be lost; a packet older than the last one received on the same channel is dropped.
		TRANSFER_MODE_UNRELIABLE_ORDERED,
		// Always arrives, in order with the other reliable packets of the same channel.
		TRANSFER_MODE_RELIABLE,
	};

	enum EventType {
		EVENT_PEER_CONNECTED,
		EVENT_PEER_DISCONNECTED,
	};

	struct Event {
		EventType type = EVENT_PEER_CONNECTED;
		int peer = 0;
	};

	struct Packet {
		int from_peer = 0;
		int channel = 0;
		TransferMode mode = TRANSFER_MODE_RELIABLE;
		LocalVector<uint8_t> data;
	};

	// Target of `send()` that reaches every connected peer.
	static constexpr int PEER_BROADCAST = 0;
	// Id of the server (the hub of a star network).
	static constexpr int PEER_SERVER = 1;

protected:
	static void _bind_methods();

public:
	virtual int get_local_peer_id() const = 0;
	virtual bool is_peer_connected(int p_peer) const = 0;
	virtual void get_connected_peers(LocalVector<int> &r_peers) const = 0;
	virtual int get_channel_count() const = 0;

	// Largest payload that fits in a single datagram (no fragmentation).
	virtual int get_max_payload_size() const = 0;

	// Queues `p_size` bytes for `p_peer`, or for every connected peer with `PEER_BROADCAST`.
	virtual Error send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) = 0;

	// Closes the connection with a peer; used to drop peers that fail the handshake.
	virtual void disconnect_peer(int p_peer) = 0;

	// Sends the queued packets and receives the incoming ones.
	virtual void poll() = 0;

	virtual bool pop_event(Event &r_event) = 0;
	virtual bool pop_packet(Packet &r_packet) = 0;
};

VARIANT_ENUM_CAST(TickTransport::TransferMode);

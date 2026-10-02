// The transport of a network with a server and clients: `EnetStarTransport`.
//
// A star transport over a `SceneMultiplayer` (ADR-027): the module's messages travel with `send_bytes()`, so the game
// keeps using RPCs and `auth_callback` on the same connection. `create_server()` and `create_client()` build an ENet
// peer with the compression mode and DTLS configured (ADR-024).
//
// For debugging, latency, jitter and packet loss can be simulated on the sending side (ADR-029).

#pragma once

#include "tick_transport.h"

#include "core/crypto/crypto.h"
#include "core/math/random_pcg.h"

#include "modules/multiplayer/scene_multiplayer.h"

class EnetStarTransport : public TickTransport {
	GDCLASS(EnetStarTransport, TickTransport);

public:
	// Same values as `ENetConnection::CompressionMode`; ENet's headers stay out of this header.
	enum Compression {
		COMPRESSION_NONE,
		COMPRESSION_RANGE_CODER,
		COMPRESSION_FASTLZ,
		COMPRESSION_ZLIB,
		COMPRESSION_ZSTD,
	};

private:
	// A packet held back by the simulated latency.
	struct Outgoing {
		uint64_t send_at_usec = 0;
		uint64_t sequence = 0;
		int peer = 0;
		int channel = 0;
		TransferMode mode = TRANSFER_MODE_RELIABLE;
		Vector<uint8_t> data;

		// Orders the delayed packets by the time they're due, then by the order they were sent in.
		bool operator<(const Outgoing &p_other) const {
			return send_at_usec != p_other.send_at_usec ? send_at_usec < p_other.send_at_usec : sequence < p_other.sequence;
		}
	};

	Ref<SceneMultiplayer> multiplayer;
	bool poll_multiplayer = true;
	bool encrypted = false;

	LocalVector<Event> events;
	LocalVector<Packet> packets;
	uint32_t next_event = 0;
	uint32_t next_packet = 0;
	uint64_t queued_bytes = 0;

	uint64_t simulated_latency_usec = 0;
	uint64_t simulated_jitter_usec = 0;
	double simulated_packet_loss = 0.0;
	RandomPCG rng;
	LocalVector<Outgoing> outgoing;
	uint64_t next_sequence = 0;
	uint64_t last_reliable_send_usec = 0;

	// `SceneMultiplayer` reported a peer: queues its connection for the engines.
	void _on_peer_connected(int p_peer);


	// `SceneMultiplayer` reported that a peer left: queues its disconnection for the engines.
	void _on_peer_disconnected(int p_peer);


	// `SceneMultiplayer` delivered bytes from a peer: queues them as a packet, unless the queue is full.
	void _on_peer_packet(int p_peer, const PackedByteArray &p_packet);


	// Sends a packet through `SceneMultiplayer` right away.
	Error send_now(int p_peer, int p_channel, TransferMode p_mode, const Vector<uint8_t> &p_data);


	// Sends the delayed packets that are due, or all of them with `p_all`.
	void flush_simulated(bool p_all);


protected:
	// Exposes the class to scripts.
	static void _bind_methods();


public:
	// Makes the transport of a server: an ENet host on `p_port` for `p_max_clients` clients, with the given compression
	// and, with TLS options, DTLS.
	static Ref<EnetStarTransport> create_server(int p_port, int p_max_clients = 32, Compression p_compression = COMPRESSION_RANGE_CODER, const Ref<TLSOptions> &p_tls_options = Ref<TLSOptions>());


	// Makes the transport of a client that connects to `p_address` and `p_port`, with the server's compression and,
	// with TLS options, DTLS. The certificate is checked against `p_tls_hostname`, or against the address when it's
	// empty.
	static Ref<EnetStarTransport> create_client(const String &p_address, int p_port, Compression p_compression = COMPRESSION_RANGE_CODER, const Ref<TLSOptions> &p_tls_options = Ref<TLSOptions>(), const String &p_tls_hostname = String());


	// Uses a `SceneMultiplayer` configured by the game. Its peer must have at least `TICK_CHANNEL_COUNT` channels.
	Error setup(const Ref<SceneMultiplayer> &p_multiplayer, bool p_encrypted = false);


	// The `SceneMultiplayer` the transport runs over, for the game's RPCs and authentication.
	Ref<SceneMultiplayer> get_multiplayer() const { return multiplayer; }


	// When the game also polls the `SceneMultiplayer` (for example, set as the scene tree's multiplayer), this
	// can be disabled.
	void set_poll_multiplayer(bool p_enabled) { poll_multiplayer = p_enabled; }


	// See `set_poll_multiplayer()`.
	bool is_polling_multiplayer() const { return poll_multiplayer; }


	// Delays what this side sends by `p_seconds` (one way), for debugging; 0 disables it.
	void set_simulated_latency(double p_seconds);


	// The simulated latency, in seconds.
	double get_simulated_latency() const;


	// Adds a random delay of up to `p_seconds` to each packet this side sends.
	void set_simulated_jitter(double p_seconds);


	// The simulated jitter, in seconds.
	double get_simulated_jitter() const;


	// Drops this share (0 to 1) of the unreliable packets this side sends.
	void set_simulated_packet_loss(double p_ratio);


	// The simulated packet loss, from 0 to 1.
	double get_simulated_packet_loss() const;


	// `TickTransport`: this peer's id in `SceneMultiplayer` (1 for the server).
	virtual int get_local_peer_id() const override;


	// `TickTransport`: whether `SceneMultiplayer` has the peer.
	virtual bool is_peer_connected(int p_peer) const override;


	// `TickTransport`: the peers of `SceneMultiplayer`.
	virtual void get_connected_peers(LocalVector<int> &r_peers) const override;


	// `TickTransport`: the engines' channels.
	virtual int get_channel_count() const override;


	// `TickTransport`: the largest payload that fits a datagram, which is smaller with DTLS.
	virtual int get_max_payload_size() const override;


	// `TickTransport`: sends bytes to a peer or to all of them, now or after the simulated delay.
	virtual Error send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) override;


	// `TickTransport`: closes the connection with a peer.
	virtual void disconnect_peer(int p_peer) override;


	// `TickTransport`: sends the delayed packets that are due and, unless the game does it, polls `SceneMultiplayer`.
	virtual void poll() override;


	// `TickTransport`: the next connection or disconnection, if any.
	virtual bool pop_event(Event &r_event) override;


	// `TickTransport`: the next packet received, if any.
	virtual bool pop_packet(Packet &r_packet) override;


	// Seeds the generator of the simulated jitter and loss.
	EnetStarTransport();


	// Stops listening to the `SceneMultiplayer`.
	~EnetStarTransport();
};

VARIANT_ENUM_CAST(EnetStarTransport::Compression);

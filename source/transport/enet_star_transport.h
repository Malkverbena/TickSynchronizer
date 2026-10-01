#pragma once

#include "tick_transport.h"

#include "core/crypto/crypto.h"
#include "core/math/random_pcg.h"

#include "modules/multiplayer/scene_multiplayer.h"

// Star transport over a `SceneMultiplayer` (ADR-027): the module's messages travel with `send_bytes()`, so the
// game keeps using RPCs and `auth_callback` on the same connection. `create_server()` and `create_client()` build
// an ENet peer with the compression mode and DTLS configured (ADR-024).
//
// For debugging, latency, jitter and packet loss can be simulated on the sending side (ADR-029).
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
	struct Outgoing {
		uint64_t send_at_usec = 0;
		uint64_t sequence = 0;
		int peer = 0;
		int channel = 0;
		TransferMode mode = TRANSFER_MODE_RELIABLE;
		Vector<uint8_t> data;

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

	void _on_peer_connected(int p_peer);
	void _on_peer_disconnected(int p_peer);
	void _on_peer_packet(int p_peer, const PackedByteArray &p_packet);
	Error send_now(int p_peer, int p_channel, TransferMode p_mode, const Vector<uint8_t> &p_data);
	void flush_simulated(bool p_all);

protected:
	static void _bind_methods();

public:
	static Ref<EnetStarTransport> create_server(int p_port, int p_max_clients = 32, Compression p_compression = COMPRESSION_RANGE_CODER, const Ref<TLSOptions> &p_tls_options = Ref<TLSOptions>());
	static Ref<EnetStarTransport> create_client(const String &p_address, int p_port, Compression p_compression = COMPRESSION_RANGE_CODER, const Ref<TLSOptions> &p_tls_options = Ref<TLSOptions>(), const String &p_tls_hostname = String());

	// Uses a `SceneMultiplayer` configured by the game. Its peer must have at least `TICK_CHANNEL_COUNT` channels.
	Error setup(const Ref<SceneMultiplayer> &p_multiplayer, bool p_encrypted = false);
	Ref<SceneMultiplayer> get_multiplayer() const { return multiplayer; }

	// When the game also polls the `SceneMultiplayer` (for example, set as the scene tree's multiplayer), this
	// can be disabled.
	void set_poll_multiplayer(bool p_enabled) { poll_multiplayer = p_enabled; }
	bool is_polling_multiplayer() const { return poll_multiplayer; }

	void set_simulated_latency(double p_seconds);
	double get_simulated_latency() const;
	void set_simulated_jitter(double p_seconds);
	double get_simulated_jitter() const;
	void set_simulated_packet_loss(double p_ratio);
	double get_simulated_packet_loss() const;

	// TickTransport.
	virtual int get_local_peer_id() const override;
	virtual bool is_peer_connected(int p_peer) const override;
	virtual void get_connected_peers(LocalVector<int> &r_peers) const override;
	virtual int get_channel_count() const override;
	virtual int get_max_payload_size() const override;
	virtual Error send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) override;
	virtual void disconnect_peer(int p_peer) override;
	virtual void poll() override;
	virtual bool pop_event(Event &r_event) override;
	virtual bool pop_packet(Packet &r_packet) override;

	EnetStarTransport();
	~EnetStarTransport();
};

VARIANT_ENUM_CAST(EnetStarTransport::Compression);

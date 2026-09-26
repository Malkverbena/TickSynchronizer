#pragma once

#include "tick_transport.h"

#include "core/crypto/crypto.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

class TickMultiplayerPeer;

// Mesh transport between players over the internet (F7, `notes/f7-design.md`). One node hosts: it must be reachable
// on its port (a public address, or a port forwarded by the project). Players join through it, get their id from it,
// and reach each other directly through NAT hole punching; a pair that can't is relayed by the host. The engines see
// every player as connected either way.
//
// Each player has one socket for the host and one per other player; the host introduces a pair by learning the public
// endpoint of both sockets and a token the pair uses to connect. The same mesh can also carry `SceneMultiplayer`
// through `get_multiplayer_peer()`.
//
// With DTLS (ADR-061), the host link uses the game's `TLSOptions`, and each direct link the certificate its accepting
// player generates, pinned by the other player through the host. The endpoints are then registered on a second port
// of the host (the rendezvous port), because a DTLS socket only takes DTLS.
class EnetHostedMeshTransport : public TickTransport {
	GDCLASS(EnetHostedMeshTransport, TickTransport);
	friend class TickMultiplayerPeer;

public:
	// Same values as `EnetStarTransport::Compression`.
	enum Compression {
		COMPRESSION_NONE,
		COMPRESSION_RANGE_CODER,
		COMPRESSION_FASTLZ,
		COMPRESSION_ZLIB,
		COMPRESSION_ZSTD,
	};

	enum Status {
		STATUS_DISCONNECTED,
		STATUS_CONNECTING,
		STATUS_CONNECTED,
	};

	// How this node reaches another one.
	enum PeerPath {
		PATH_NONE,
		// The host, or a player seen from the host.
		PATH_HOST,
		// Being introduced: not connected yet.
		PATH_CONNECTING,
		PATH_DIRECT,
		PATH_RELAYED,
	};

	// Transfer channels available to the multiplayer peer.
	static constexpr int MULTIPLAYER_CHANNEL_COUNT = 4;

private:
	enum PairState {
		PAIR_REGISTERING,
		PAIR_PUNCHING,
		PAIR_DIRECT,
		// Waiting for the host to confirm the relay.
		PAIR_FAILED,
		PAIR_RELAYED,
	};

	// Player: the link to another player.
	struct Pair {
		PairState state = PAIR_REGISTERING;
		// `ENetConnection` of this pair, and its `ENetPacketPeer`s (kept as `RefCounted`, so this header doesn't need
		// ENet's): the registration with the host, then the direct link.
		Ref<RefCounted> socket;
		Ref<RefCounted> registration;
		Ref<RefCounted> link;
		bool has_endpoint = false;
		String address;
		int port = 0;
		uint32_t connect_token = 0;
		// DTLS: the accepting player's certificate, for the connecting player to pin.
		String peer_certificate;
		uint64_t deadline_usec = 0;
		uint64_t next_punch_usec = 0;
		// DTLS: when the connecting side switches to DTLS and connects (after the other side's punches arrived).
		uint64_t connect_at_usec = 0;
		// DTLS: when the punching starts, once the registration's disconnection is over on both sides.
		uint64_t punch_at_usec = 0;
		// The engines were told this peer is connected.
		bool reported = false;
	};

	// Host: a pair of players being introduced.
	struct Introduction {
		int first = 0;
		int second = 0;
		uint32_t registration_tokens[2] = {};
		bool registered[2] = {};
		String addresses[2];
		int ports[2] = {};
		uint32_t connect_token = 0;
		bool punching = false;
		bool relayed = false;
		uint64_t deadline_usec = 0;
	};

	struct MultiplayerPacket {
		int from_peer = 0;
		int channel = 0;
		TransferMode mode = TRANSFER_MODE_RELIABLE;
		LocalVector<uint8_t> data;
	};

	bool is_host = false;
	int local_id = 0;
	// The node that hosts the mesh now: 1, or the successor after a migration.
	int host_id = PEER_SERVER;
	// Host migration (ADR-062): the order in which players take over when the host leaves, sent by the host.
	bool host_migration = true;
	double host_timeout = 5.0;
	LocalVector<int> succession;
	// This player asked to leave the host: its disconnection isn't the host leaving.
	bool leaving = false;
	Status status = STATUS_DISCONNECTED;
	Compression compression = COMPRESSION_RANGE_CODER;
	double punch_timeout = 3.0;
	bool direct_connections = true;
	Ref<TLSOptions> tls_options;
	bool encrypted = false;

	// Host: the listening socket, its players and the introductions in progress.
	Ref<RefCounted> listener;
	// With DTLS, the plain socket where the pairs register their endpoints.
	Ref<RefCounted> rendezvous;
	int max_players = 32;
	HashMap<int, String> member_certificates;
	int next_player_id = 2;
	HashMap<int, Ref<RefCounted>> members;
	HashMap<ObjectID, int> members_by_link;
	// After a migration: each member's link is on the socket of the former direct link.
	HashMap<int, Ref<RefCounted>> member_sockets;
	HashMap<uint64_t, Introduction> introductions;
	HashMap<uint32_t, uint64_t> introductions_by_token;

	// Player: the host's socket and link, and the other players.
	String host_address;
	int host_port = 0;
	int host_rendezvous_port = 0;
	// DTLS: this player's key and self-signed certificate, for the direct links it accepts.
	Ref<CryptoKey> player_key;
	Ref<X509Certificate> player_certificate;
	Ref<RefCounted> host_socket;
	Ref<RefCounted> host_link;
	HashMap<int, Pair> pairs;

	LocalVector<Event> events;
	LocalVector<Packet> packets;
	uint32_t next_event = 0;
	uint32_t next_packet = 0;
	uint64_t queued_bytes = 0;

	TickMultiplayerPeer *multiplayer_peer = nullptr;
	LocalVector<Event> multiplayer_events;
	LocalVector<MultiplayerPacket> multiplayer_packets;
	uint32_t next_multiplayer_packet = 0;
	uint64_t multiplayer_queued_bytes = 0;

	uint64_t relayed_packets = 0;
	uint64_t rejected_connections = 0;
	uint64_t failed_punches = 0;
	uint64_t dropped_packets = 0;

	static uint64_t make_pair_key(int p_a, int p_b);
	static uint32_t make_token();

	void push_event(EventType p_type, int p_peer);
	// Hands a packet of logical channel `p_logical` to the engines or to the multiplayer peer.
	void deliver(int p_from, int p_logical, int p_flags, const uint8_t *p_data, int p_size);
	Error send_logical(int p_peer, int p_logical, int p_flags, const uint8_t *p_data, int p_size);
	Error send_on_link(const Ref<RefCounted> &p_link, int p_channel, int p_flags, const uint8_t *p_data, int p_size);
	void send_control(const Ref<RefCounted> &p_link, const LocalVector<uint8_t> &p_message);

	// Host.
	void host_poll();
	// `p_kind`: 0 the main socket, 1 the rendezvous one, 2 a member's own socket (after a migration).
	void host_service(const Ref<RefCounted> &p_socket, int p_kind);
	void host_send_succession();
	// Sends both players their partner's endpoint once both registered (and, with DTLS, the certificate is known).
	void host_try_punch(uint64_t p_key);
	void host_on_connect(const Ref<RefCounted> &p_link, uint32_t p_data, int p_kind);
	void host_on_disconnect(const Ref<RefCounted> &p_link);
	void host_on_receive(int p_from, int p_channel, const uint8_t *p_data, int p_size, int p_flags);
	void host_introduce(int p_first, int p_second);
	void host_relay_pair(uint64_t p_key);
	void host_forget_introduction(uint64_t p_key);

	// Player.
	void player_poll();
	void player_service_host();
	void player_on_control(const uint8_t *p_data, int p_size);
	void player_open_pair(int p_peer, uint32_t p_registration_token);
	// The registration is closed and the endpoint known: punching starts (with DTLS, a moment later).
	void player_endpoint_ready(int p_peer, Pair &r_pair);
	void player_start_punching(int p_peer, Pair &r_pair);
	void player_connect_pair(int p_peer, Pair &r_pair);
	void player_service_pair(int p_peer, Pair &r_pair);
	void player_fail_pair(int p_peer, Pair &r_pair);
	void player_set_relayed(int p_peer);
	void player_close_pair(Pair &r_pair);
	void player_report_connected(int p_peer, Pair &r_pair);
	void player_report_disconnected(int p_peer, Pair &r_pair);
	// `p_migrate`: `false` when the host removed this player, or the player left: then the mesh ends for it.
	void player_lost_host(bool p_migrate);
	// The host left: the first living player of the succession takes over. `false` when there's none to reach.
	bool player_migrate(int p_old_host);
	void player_become_host(int p_old_host);
	void player_follow_host(int p_old_host, int p_new_host);
	void apply_host_timeout(const Ref<RefCounted> &p_link);

	// Multiplayer peer.
	void attach_multiplayer_peer(TickMultiplayerPeer *p_peer);
	Error send_multiplayer(int p_target, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size);

protected:
	static void _bind_methods();

public:
	// Hosts a mesh on `p_port`: this node is player 1, and the others join through it.
	// With `p_tls_options` (`TLSOptions.server()`), the links are encrypted, and the pairs register on
	// `p_rendezvous_port` (the next port by default), which must be reachable too.
	static Ref<EnetHostedMeshTransport> create_host(int p_port, int p_max_players = 32, const String &p_bind_address = "*", Compression p_compression = COMPRESSION_RANGE_CODER, const Ref<TLSOptions> &p_tls_options = Ref<TLSOptions>(), int p_rendezvous_port = 0);
	// Joins the mesh hosted at `p_address`; the host gives this node its id. With `p_tls_options` (`TLSOptions.client()`
	// or `client_unsafe()`), the host must use DTLS too.
	static Ref<EnetHostedMeshTransport> create_player(const String &p_address, int p_port, Compression p_compression = COMPRESSION_RANGE_CODER, const Ref<TLSOptions> &p_tls_options = Ref<TLSOptions>(), const String &p_tls_hostname = String());

	Status get_status() const { return status; }
	bool is_hosting() const { return is_host; }
	bool is_encrypted() const { return encrypted; }
	PeerPath get_peer_path(int p_peer) const;
	int get_host_peer() const { return is_host ? local_id : host_id; }
	PackedInt32Array get_succession() const;
	PackedInt32Array get_peers() const;
	Dictionary get_stats() const;

	// Seconds to establish a direct link between two players before relaying them.
	void set_punch_timeout(double p_seconds);
	double get_punch_timeout() const { return punch_timeout; }
	// When `false`, this player never tries direct links: every other player is relayed by the host.
	void set_direct_connections(bool p_enabled);
	bool is_direct_connections_enabled() const { return direct_connections; }
	// When the host leaves, a player takes its place (ADR-062) instead of the mesh ending.
	void set_host_migration(bool p_enabled) { host_migration = p_enabled; }
	bool is_host_migration_enabled() const { return host_migration; }
	// Seconds without an answer from the host before a player considers it gone.
	void set_host_timeout(double p_seconds);
	double get_host_timeout() const { return host_timeout; }

	// A `MultiplayerPeer` on this mesh, for `SceneMultiplayer` (RPCs, spawners, synchronizers).
	Ref<TickMultiplayerPeer> get_multiplayer_peer();

	// Closes every connection.
	void close();

	// TickTransport.
	virtual int get_local_peer_id() const override { return local_id; }
	virtual bool is_peer_connected(int p_peer) const override;
	virtual void get_connected_peers(LocalVector<int> &r_peers) const override;
	virtual int get_channel_count() const override;
	virtual int get_max_payload_size() const override;
	virtual Error send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) override;
	virtual void disconnect_peer(int p_peer) override;
	virtual void poll() override;
	virtual bool pop_event(Event &r_event) override;
	virtual bool pop_packet(Packet &r_packet) override;

	~EnetHostedMeshTransport();
};

VARIANT_ENUM_CAST(EnetHostedMeshTransport::Compression);
VARIANT_ENUM_CAST(EnetHostedMeshTransport::Status);
VARIANT_ENUM_CAST(EnetHostedMeshTransport::PeerPath);

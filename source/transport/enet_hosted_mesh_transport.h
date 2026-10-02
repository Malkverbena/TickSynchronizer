// The transport of a mesh between players, with a host: `EnetHostedMeshTransport`.
//
// Mesh transport between players over the internet (F7, `notes/f7-design.md`). One node hosts: it must be reachable on
// its port (a public address, or a port forwarded by the project). Players join through it, get their id from it, and
// reach each other directly through NAT hole punching; a pair that can't is relayed by the host. The engines see every
// player as connected either way.
//
// Each player has one socket for the host and one per other player; the host introduces a pair by learning the public
// endpoint of both sockets and a token the pair uses to connect. The same mesh can also carry `SceneMultiplayer`
// through `get_multiplayer_peer()`.
//
// With DTLS (ADR-061), the host link uses the game's `TLSOptions`, and each direct link the certificate its accepting
// player generates, pinned by the other player through the host. The endpoints are then registered on a second port of
// the host (the rendezvous port), because a DTLS socket only takes DTLS.
//
// A joining player first sends its join data; the host admits it (the game decides with `join_validator`) before
// anybody learns about it (ADR-066). The host is trusted: it assigns the ids, introduces the players and relays their
// packets. When it disappears, a player takes over only if other players it reaches directly lost the host too.
//
// The protocol, with its version (a player of another version is refused), is specified in
// `notes/hosted-mesh-protocol.md` (ADR-069). A player with a `takeover_port` takes new players there if it becomes the
// host (ADR-072).
//
// What a player and the host can cost each other is bounded (ADR-077): the places a connection holds before its join
// data, the bytes the relay carries and holds, and what a player believes of what the host tells it.

#pragma once

#include "tick_transport.h"

#include "core/crypto/crypto.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

class TickMultiplayerPeer;

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

	// Why this node left the mesh.
	enum DisconnectReason {
		DISCONNECT_REASON_NONE,
		// This node closed its connections, or left the host.
		DISCONNECT_REASON_CLOSED,
		// The host stopped answering (or couldn't be reached), and no other host could take its place for this node.
		DISCONNECT_REASON_LOST,
		// The host refused this player (itself or through the game), or removed it.
		DISCONNECT_REASON_REFUSED,
		DISCONNECT_REASON_FULL,
		// Too many joins from this address: it can try again a bit later.
		DISCONNECT_REASON_BUSY,
		// The host uses another version of the protocol.
		DISCONNECT_REASON_VERSION,
		// The host ended the mesh.
		DISCONNECT_REASON_ENDED,
		// The mesh takes more players than this player keeps links with (`pair_limit`).
		DISCONNECT_REASON_TOO_LARGE,
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
		// A direct link that dropped: when the relay is asked for, unless the host says first that the other player left.
		uint64_t relay_at_usec = 0;
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

	// A packet for `SceneMultiplayer`, kept until the multiplayer peer takes it.
	struct MultiplayerPacket {
		int from_peer = 0;
		int channel = 0;
		TransferMode mode = TRANSFER_MODE_RELIABLE;
		LocalVector<uint8_t> data;
	};

	// Host: a player that connected and isn't admitted yet. Its id is only told to it with the welcome.
	struct PendingJoin {
		int id = 0;
		Ref<RefCounted> link;
		String address;
		uint64_t deadline_usec = 0;
		// Its join data arrived: the game decides.
		bool received = false;
		PackedByteArray data;
	};

	// Host: the joins an address can still start (refilled over time).
	struct JoinBudget {
		double tokens = 0.0;
		uint64_t last_usec = 0;
	};

	// Host: the bytes a player can still have relayed (refilled over time).
	struct RelayBudget {
		double bytes = 0.0;
		uint64_t last_usec = 0;
	};

	// Host: the bytes relayed to a player that ENet still holds (not sent yet, or not acknowledged). ENet tells when a
	// packet is done through the packet's callback, which gets this through a pointer: allocated apart, so it stays
	// in place.
	struct RelayQueue {
		uint64_t bytes = 0;
	};

	// Player: the key and certificate of its direct links, generated on a worker thread (defined in the source).
	struct PlayerKeyJob;
	// Generates the key and the certificate of a player's direct links; runs on a worker thread.
	static void generate_player_key(void *p_job);

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
	DisconnectReason disconnect_reason = DISCONNECT_REASON_NONE;
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
	Callable join_validator;
	double join_timeout = 10.0;
	HashMap<ObjectID, PendingJoin> pending_joins;
	// Joins whose data arrived during this poll: the game's validator runs once the sockets are serviced.
	LocalVector<ObjectID> joins_to_validate;
	HashMap<String, JoinBudget> join_budgets;
	uint64_t next_budget_prune_usec = 0;
	uint64_t next_heartbeat_usec = 0;
	// The relay's limits (ADR-077): bytes per second a player may have relayed, and bytes the host holds for a player.
	int relay_rate_limit = 1024 * 1024;
	int relay_queue_limit = 8 * 1024 * 1024;
	HashMap<int, RelayBudget> relay_budgets;
	HashMap<int, RelayQueue *> relay_queues;

	// Player: the host's socket and link, and the other players.
	String host_address;
	int host_port = 0;
	int host_rendezvous_port = 0;
	// DTLS: this player's key and self-signed certificate, for the direct links it accepts.
	Ref<CryptoKey> player_key;
	Ref<X509Certificate> player_certificate;
	PlayerKeyJob *key_job = nullptr;
	int64_t key_task = -1;
	bool certificate_sent = false;
	PackedByteArray join_data;
	// The host's player limit, from its welcome: a player never keeps more pairs than the mesh can have.
	int host_max_players = 0;
	// The most other players this player keeps a link with: it leaves a mesh that takes more (the host says how many).
	int pair_limit = 64;
	// When the host was last heard from (it sends heartbeats, so a silent host is a gone one).
	uint64_t host_heard_usec = 0;
	Ref<RefCounted> host_socket;
	Ref<RefCounted> host_link;
	HashMap<int, Pair> pairs;
	// The host's link dropped without a word: this player asked the players it reaches directly whether they lost the
	// host too, and waits for their answers (`true` when the host is alive for them).
	bool confirming = false;
	int confirm_old_host = 0;
	uint64_t confirm_deadline_usec = 0;
	LocalVector<int> confirm_asked;
	HashMap<int, bool> confirm_answers;
	// Players that chose this one as the new host before it noticed the old one was gone: their rejoin requests.
	HashMap<int, LocalVector<int>> pending_rejoins;
	// DTLS: the certificates those players sent with their rejoin requests.
	HashMap<int, String> pending_certificates;
	// Where this player takes new players if it becomes the host (0: nowhere).
	int takeover_port = 0;
	int takeover_rendezvous_port = 0;
	Ref<TLSOptions> takeover_tls_options;
	// The highest player id this node heard of: a host that took over never gives another player's id to a new one.
	int highest_peer_id = 0;

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
	uint64_t relay_dropped_packets = 0;
	uint64_t rejected_connections = 0;
	uint64_t failed_punches = 0;
	uint64_t dropped_packets = 0;

	// The key of a pair of players in the maps: the same whatever the order of the two ids.
	static uint64_t make_pair_key(int p_a, int p_b);


	// A random token nobody can guess, never 0 nor the connection data of a join; 0 when the system gives no random
	// bytes.
	static uint32_t make_token();


	// Queues an event for the engines and, when there is one, for the multiplayer peer.
	void push_event(EventType p_type, int p_peer);


	// Hands a packet of logical channel `p_logical` to the engines or to the multiplayer peer.
	void deliver(int p_from, int p_logical, int p_flags, const uint8_t *p_data, int p_size);


	// Sends a packet of logical channel `p_logical` to a peer, or to every connected one: on the host link, on the
	// pair's direct link, or through the host's relay.
	Error send_logical(int p_peer, int p_logical, int p_flags, const uint8_t *p_data, int p_size);


	// Sends bytes on an ENet link and channel, with ENet's flags; fails when the link isn't active.
	Error send_on_link(const Ref<RefCounted> &p_link, int p_channel, int p_flags, const uint8_t *p_data, int p_size);


	// Sends a control message on a link: reliable, on the control channel.
	void send_control(const Ref<RefCounted> &p_link, const LocalVector<uint8_t> &p_message);


	// Links drop after `host_timeout` without an answer, on both ends.
	void apply_link_timeout(const Ref<RefCounted> &p_link);


	// Closes every link: the members get `p_member_reason` as the reason.
	void close_links(int p_member_reason);


	// Host: services its sockets, lets the game decide on the joins, refuses the late ones, sends the heartbeat and
	// relays the pairs that didn't register in time.
	void host_poll();


	// Host: takes one join from the budget of an address; `false` when the address started too many.
	bool host_take_join_budget(const String &p_address);


	// Host: the join data of a connection arrived: from now on it holds a place, and the game decides on it.
	void host_on_join(ObjectID p_link, const uint8_t *p_data, int p_size);


	// Host: calls the game's validator for the joins whose data arrived in this poll, and admits or refuses them by its
	// answer.
	void host_validate_joins();


	// Host: makes a pending join a member: welcomes it, reports it to the engines, introduces it to every other member
	// and sends the new succession.
	void host_admit(ObjectID p_link);


	// Host: refuses a pending join and closes its link. `p_reason` is the disconnection data the player gets (why it
	// was refused).
	void host_refuse(ObjectID p_link, int p_reason);


	// Whether the mesh has a place for one more player, counting the ones the game is still deciding on.
	bool host_has_place(ObjectID p_except) const;


	// Takes `p_bytes` of the relay budget of a player; `false` when it doesn't have them.
	bool host_take_relay_budget(int p_player, int p_bytes);


	// Removes a member the relay can't serve (it sent too much, or it can't take what is sent to it).
	void host_remove_member(int p_member);


	// Frees what the relay kept about the players (once their sockets are closed).
	void host_clear_relay();


	// Host that took over: a player that followed it says which players it reaches directly; its other pairs are
	// relayed here.
	void host_handle_rejoin(int p_from, const LocalVector<int> &p_direct);


	// A player that took over opens its `takeover_port`, if it has one.
	void host_open_takeover_sockets();


	// Host: takes the events of one of its sockets. `p_kind`: 0 the main socket, 1 the rendezvous one, 2 a member's own
	// socket (after a migration).
	void host_service(const Ref<RefCounted> &p_socket, int p_kind);


	// Host: tells every member the order in which the players take over when the host leaves: the ones without relayed
	// pairs first, then by id.
	void host_send_succession();


	// Sends both players their partner's endpoint once both registered (and, with DTLS, the certificate is known).
	void host_try_punch(uint64_t p_key);


	// Host: a connection arrived: a join (checked against the version, the places left and the budget of its address),
	// or the registration of a pair's socket, by its token.
	void host_on_connect(const Ref<RefCounted> &p_link, uint32_t p_data, int p_kind);


	// Host: a link closed: a pending join is forgotten; a member is removed and reported, and the other members are
	// told.
	void host_on_disconnect(const Ref<RefCounted> &p_link);


	// Host: a member sent a packet: a control message, a packet for the host's engines, or one to relay to another
	// member, within the relay's limits.
	void host_on_receive(int p_from, int p_channel, const uint8_t *p_data, int p_size, int p_flags);


	// Host: starts introducing two members: each gets a token to register the endpoint of its socket with. Without
	// random bytes for the tokens, the pair is relayed.
	void host_introduce(int p_first, int p_second);


	// Host: gives up the direct link of a pair: both players are told to reach each other through the relay.
	void host_relay_pair(uint64_t p_key);


	// Host: forgets a pair and its tokens, when one of its players left.
	void host_forget_introduction(uint64_t p_key);


	// Player: services the host's socket and every pair's, checks the confirmation of a lost host, and flushes what was
	// sent.
	void player_poll();


	// Player: takes the events of the host's socket: the connection (then it asks to join), control messages, packets
	// from the host and packets relayed by it.
	void player_service_host();


	// Player: handles a control message of the host, checking everything it says (ids, ports, addresses, counts).
	void player_on_control(const uint8_t *p_data, int p_size);


	// Player: the host introduces another player: opens a socket for the pair and registers its endpoint with the host,
	// using the token.
	void player_open_pair(int p_peer, uint32_t p_registration_token);


	// The registration is closed and the endpoint known: punching starts (with DTLS, a moment later).
	void player_endpoint_ready(int p_peer, Pair &r_pair);


	// Player: starts making the direct link of a pair: the lower id accepts and punches its NAT, the higher id
	// connects.
	void player_start_punching(int p_peer, Pair &r_pair);


	// Player: the connecting side connects to the other player's endpoint; with DTLS, pinning its certificate.
	void player_connect_pair(int p_peer, Pair &r_pair);


	// Player: takes the events of a pair's socket and moves the pair along: the registration, the punching, the direct
	// link and its packets, and the deadlines.
	void player_service_pair(int p_peer, Pair &r_pair);


	// The direct link couldn't be made: the host relays the pair.
	void player_fail_pair(int p_peer, Pair &r_pair);


	// Player: asks the host to relay a pair.
	void player_request_relay(int p_peer);


	// Player: the host relays this pair: closes what there was of the direct link and reports the other player as
	// connected.
	void player_set_relayed(int p_peer);


	// Player: closes the socket and the links of a pair.
	void player_close_pair(Pair &r_pair);


	// Player: tells the engines, once, that the other player of a pair is connected.
	void player_report_connected(int p_peer, Pair &r_pair);


	// Player: tells the engines that the other player of a pair disconnected, if they knew it as connected.
	void player_report_disconnected(int p_peer, Pair &r_pair);


	// Player: handles a control message from the other player of a pair: the confirmation of the link, a question or an
	// answer about the host, or a request to rejoin through this player.
	void player_on_pair_control(int p_peer, Pair &r_pair, const uint8_t *p_data, int p_size);


	// Player: sends the host the certificate of the direct links it accepts, once it's generated and the host welcomed
	// the player.
	void player_send_certificate();


	// Player: takes the key and the certificate from the worker thread once they're ready.
	void player_finish_key_job();


	// The link with the host dropped, for `p_reason` (the ENet disconnection data).
	void player_on_host_disconnect(int p_reason);


	// Whether the host `p_host` is alive for this player (it was heard from recently).
	bool player_sees_host(int p_host) const;


	// Player: answers, on `p_link`, whether the host `p_host` is alive for it.
	void player_answer_host_query(const Ref<RefCounted> &p_link, int p_host);


	// Player: the host's link dropped without a word: asks the players it reaches directly whether they lost the host
	// too. With nobody to ask, the mesh ends for it.
	void player_confirm_host_loss();


	// Player: counts the answers: it migrates when more players lost the host than still hear it; on a tie, or when
	// nobody answers, it leaves.
	void player_check_confirmation();


	// The mesh ends for this player.
	void player_lost_host(DisconnectReason p_reason);


	// The host left: the first living player of the succession takes over. `false` when there's none to reach.
	bool player_migrate(int p_old_host);


	// Player: takes the host's place: its direct links become the members' links, and it takes new players if it has a
	// takeover port.
	void player_become_host(int p_old_host);


	// Player: follows the successor: its direct link with it becomes the host link, and it tells the new host which
	// players it reaches directly.
	void player_follow_host(int p_old_host, int p_new_host);


	// Sets the multiplayer peer that takes this mesh's `SceneMultiplayer` packets and events (null: none), and tells it
	// about the peers already connected.
	void attach_multiplayer_peer(TickMultiplayerPeer *p_peer);


	// Sends a `SceneMultiplayer` packet to a player, to everyone (0) or to everyone but one (a negative id), on a
	// multiplayer channel.
	Error send_multiplayer(int p_target, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size);


protected:
	// Exposes the class to scripts.
	static void _bind_methods();


public:
	// Hosts a mesh on `p_port`: this node is player 1, and the others join through it.
	// With `p_tls_options` (`TLSOptions.server()`), the links are encrypted, and the pairs register on
	// `p_rendezvous_port` (the next port by default), which must be reachable too.
	static Ref<EnetHostedMeshTransport> create_host(int p_port, int p_max_players = 32, const String &p_bind_address = "*", Compression p_compression = COMPRESSION_RANGE_CODER, const Ref<TLSOptions> &p_tls_options = Ref<TLSOptions>(), int p_rendezvous_port = 0);


	// Joins the mesh hosted at `p_address`; the host gives this node its id. With `p_tls_options` (`TLSOptions.client()`
	// or `client_unsafe()`), the host must use DTLS too.
	// `p_join_data` goes to the host's `join_validator` (a password or a token, for example; encrypted only with DTLS).
	static Ref<EnetHostedMeshTransport> create_player(const String &p_address, int p_port, Compression p_compression = COMPRESSION_RANGE_CODER, const Ref<TLSOptions> &p_tls_options = Ref<TLSOptions>(), const String &p_tls_hostname = String(), const PackedByteArray &p_join_data = PackedByteArray());


	// Whether this node is in the mesh, joining it, or out of it.
	Status get_status() const { return status; }


	// Why this node left the mesh; it means something once the status is `STATUS_DISCONNECTED`.
	DisconnectReason get_disconnect_reason() const { return disconnect_reason; }


	// Whether this node hosts the mesh: it created it, or took over after a migration.
	bool is_hosting() const { return is_host; }


	// Whether the links use DTLS.
	bool is_encrypted() const { return encrypted; }


	// How this node reaches `p_peer`: through the host link, directly, relayed, or not yet.
	PeerPath get_peer_path(int p_peer) const;


	// The id of the node that hosts the mesh now.
	int get_host_peer() const { return is_host ? local_id : host_id; }


	// The order in which the players take over when the host leaves, as the host last sent it.
	PackedInt32Array get_succession() const;


	// The ids of the connected peers, in order.
	PackedInt32Array get_peers() const;


	// Counters for debugging: direct and relayed pairs, relayed and dropped packets, refused connections, failed
	// punches.
	Dictionary get_stats() const;


	// Seconds to establish a direct link between two players before relaying them.
	void set_punch_timeout(double p_seconds);


	// See `set_punch_timeout()`.
	double get_punch_timeout() const { return punch_timeout; }


	// When `false`, this player never tries direct links: every other player is relayed by the host.
	void set_direct_connections(bool p_enabled);


	// See `set_direct_connections()`.
	bool is_direct_connections_enabled() const { return direct_connections; }


	// When the host leaves, a player takes its place (ADR-062) instead of the mesh ending.
	void set_host_migration(bool p_enabled) { host_migration = p_enabled; }


	// See `set_host_migration()`.
	bool is_host_migration_enabled() const { return host_migration; }


	// Seconds without an answer before a link (with the host, a player, or another player) is considered gone.
	void set_host_timeout(double p_seconds);


	// See `set_host_timeout()`.
	double get_host_timeout() const { return host_timeout; }


	// Host: decides who joins. Called as `validator(peer, join_data, address)`: `true` admits the player, `false`
	// refuses it, anything else waits for `admit_player()` or `refuse_player()` (until `join_timeout`). Without a
	// validator, everybody is admitted.
	void set_join_validator(const Callable &p_validator) { join_validator = p_validator; }


	// See `set_join_validator()`.
	Callable get_join_validator() const { return join_validator; }


	// Seconds the game has to admit a player whose join data arrived (the data itself must come within 2 seconds).
	void set_join_timeout(double p_seconds);


	// See `set_join_timeout()`.
	double get_join_timeout() const { return join_timeout; }


	// Host: bytes per second a player may send through the relay (twice that at once); 0 for no limit. Beyond it,
	// unreliable packets are dropped, and a player that sends reliable ones is removed.
	void set_relay_rate_limit(int p_bytes_per_second);


	// See `set_relay_rate_limit()`.
	int get_relay_rate_limit() const { return relay_rate_limit; }


	// Host: bytes the relay holds for a player that takes them slower than they come; 0 for no limit. Beyond it,
	// unreliable packets are dropped, and a player that can't take the reliable ones is removed.
	void set_relay_queue_limit(int p_bytes);


	// See `set_relay_queue_limit()`.
	int get_relay_queue_limit() const { return relay_queue_limit; }


	// Player: the most other players it keeps a link with (a socket each, when direct). It leaves a mesh whose host
	// says it takes more players than that, with `DISCONNECT_REASON_TOO_LARGE`.
	void set_pair_limit(int p_pairs);


	// See `set_pair_limit()`.
	int get_pair_limit() const { return pair_limit; }


	// Host: admits a player whose join data arrived and that the validator left waiting.
	Error admit_player(int p_peer);


	// Host: refuses a player that is waiting to join.
	Error refuse_player(int p_peer);


	// Host: leaves the mesh, and the next player of the succession takes its place (`close()` ends the mesh).
	Error hand_over();


	// Player: the port it takes new players on if it becomes the host after a migration (0: none). With DTLS, it needs
	// `takeover_tls_options` (`TLSOptions.server()`), and the pairs register on `takeover_rendezvous_port` (the next port
	// when 0). The game tells the new players where the new host is.
	void set_takeover_port(int p_port);


	// See `set_takeover_port()`.
	int get_takeover_port() const { return takeover_port; }


	// Player: with DTLS, the port the pairs register on if it becomes the host (0: the port after `takeover_port`).
	void set_takeover_rendezvous_port(int p_port);


	// See `set_takeover_rendezvous_port()`.
	int get_takeover_rendezvous_port() const { return takeover_rendezvous_port; }


	// Player: the server's `TLSOptions` it takes new players with if it becomes the host of a mesh that uses DTLS.
	void set_takeover_tls_options(const Ref<TLSOptions> &p_options) { takeover_tls_options = p_options; }


	// See `set_takeover_tls_options()`.
	Ref<TLSOptions> get_takeover_tls_options() const { return takeover_tls_options; }


	// A `MultiplayerPeer` on this mesh, for `SceneMultiplayer` (RPCs, spawners, synchronizers).
	Ref<TickMultiplayerPeer> get_multiplayer_peer();


	// Closes every connection. On the host, the mesh ends for every player.
	void close();


	// `TickTransport`: this node's id: 1 for the host that created the mesh; a player gets its id from the host (0
	// until then).
	virtual int get_local_peer_id() const override { return local_id; }


	// `TickTransport`: whether the engines were told the peer is connected, directly or relayed.
	virtual bool is_peer_connected(int p_peer) const override;


	// `TickTransport`: the connected peers, in order.
	virtual void get_connected_peers(LocalVector<int> &r_peers) const override;


	// `TickTransport`: the engines' channels.
	virtual int get_channel_count() const override;


	// `TickTransport`: the largest payload that fits a datagram, also when relayed; smaller with DTLS.
	virtual int get_max_payload_size() const override;


	// `TickTransport`: sends bytes to a peer, or to every connected peer.
	virtual Error send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) override;


	// `TickTransport`: on the host, removes a player from the mesh; on a player, leaves the host (the only peer a
	// player disconnects from).
	virtual void disconnect_peer(int p_peer) override;


	// `TickTransport`: services the sockets of the host, or of the player.
	virtual void poll() override;


	// `TickTransport`: the next connection, disconnection or host migration, if any.
	virtual bool pop_event(Event &r_event) override;


	// `TickTransport`: the next packet received, if any.
	virtual bool pop_packet(Packet &r_packet) override;


	// Closes every connection.
	~EnetHostedMeshTransport();
};

VARIANT_ENUM_CAST(EnetHostedMeshTransport::Compression);
VARIANT_ENUM_CAST(EnetHostedMeshTransport::Status);
VARIANT_ENUM_CAST(EnetHostedMeshTransport::DisconnectReason);
VARIANT_ENUM_CAST(EnetHostedMeshTransport::PeerPath);

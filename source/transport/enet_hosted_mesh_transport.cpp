#include "enet_hosted_mesh_transport.h"

#include "../common/tick_data_buffer.h"
#include "../sync/tick_protocol.h"
#include "tick_multiplayer_peer.h"

#include "core/io/marshalls.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/variant/variant.h"

#include "modules/enet/enet_connection.h"
#include "modules/enet/enet_packet_peer.h"

static_assert(int(EnetHostedMeshTransport::COMPRESSION_ZSTD) == int(ENetConnection::COMPRESS_ZSTD), "The compression modes must match ENet's.");

// Version of the protocol (`notes/hosted-mesh-protocol.md`), changed only by incompatible changes. A joining player
// sends it as the ASCII digit after "TKM" in its connection data ("TKM2"); any other value is a registration token.
// Version 1 had no admission and no confirmed migration.
static constexpr uint32_t MESH_PROTOCOL_VERSION = 2;
static constexpr uint32_t JOIN_MAGIC_PREFIX = 0x544B4D00;
static constexpr uint32_t JOIN_MAGIC = JOIN_MAGIC_PREFIX | ('0' + MESH_PROTOCOL_VERSION);
// Disconnection data. A player the host refuses (itself or through the game) or removes doesn't migrate ("TKRX"); the
// host also refuses joins when the mesh is full ("TKFL"), when their address started too many ("TKBZ"), and from
// another version of the protocol ("TKV" and the host's version digit). The players of a host that ends the mesh don't
// migrate ("TKEN"); those of a host that hands it over migrate right away ("TKHO"). Any other disconnection of the host
// is confirmed with the other players before migrating.
static constexpr int DISCONNECT_REMOVED = 0x544B5258;
static constexpr int DISCONNECT_FULL = 0x544B464C;
static constexpr int DISCONNECT_BUSY = 0x544B425A;
static constexpr int DISCONNECT_VERSION_PREFIX = 0x544B5600;
static constexpr int DISCONNECT_ENDED = 0x544B454E;
static constexpr int DISCONNECT_HANDOVER = 0x544B484F;

// ENet channels: 0 is the transport's own control channel; then every logical channel (the engines' ones, then two
// per multiplayer channel: reliable and unreliable) twice, direct and relayed.
static constexpr int LOGICAL_CHANNEL_COUNT = TICK_CHANNEL_COUNT + 2 * EnetHostedMeshTransport::MULTIPLAYER_CHANNEL_COUNT;
static constexpr int CHANNEL_CONTROL = 0;
static constexpr int FIRST_DIRECT_CHANNEL = 1;
static constexpr int FIRST_RELAY_CHANNEL = FIRST_DIRECT_CHANNEL + LOGICAL_CHANNEL_COUNT;
static constexpr int ENET_CHANNEL_COUNT = FIRST_RELAY_CHANNEL + LOGICAL_CHANNEL_COUNT;

// A relayed packet starts with the id of its target (to the host) or of its origin (from the host).
static constexpr int RELAY_HEADER_SIZE = 4;
// ENet's default MTU minus the ENet headers (protocol and send command) and the relay header.
static constexpr int HOSTED_MESH_MAX_PAYLOAD = 1400 - 6 - 12 - RELAY_HEADER_SIZE;
// Record header and authentication tag of a DTLS 1.2 datagram with AES-GCM (as in `EnetStarTransport`).
static constexpr int DTLS_OVERHEAD = 37;
// Common name of the players' self-signed certificates; the connecting player checks it with the pinned certificate.
static const char *PLAYER_COMMON_NAME = "tick-mesh-player";
// With DTLS, the accepting player punches its NAT with this many datagrams before its socket switches to DTLS.
static constexpr int DTLS_PUNCHES = 3;
// With DTLS, the connecting player waits this long for those datagrams before its socket switches to DTLS.
static constexpr uint64_t DTLS_CONNECT_DELAY_USEC = 500000;
// With DTLS, a socket waits this long after its registration closed before switching to DTLS: the acknowledgment of
// the disconnection must reach the host, or the host's retries would hit the DTLS socket.
static constexpr uint64_t DTLS_SETTLE_USEC = 200000;

// While punching, the accepting side sends a datagram this often, so its NAT lets the other side's packets in.
static constexpr uint64_t PUNCH_INTERVAL_USEC = 100000;
// The connecting side waits this much longer than the accepting side, which decides.
static constexpr uint64_t CONNECTOR_GRACE_USEC = 1000000;

// Largest join data a player sends.
static constexpr int MAX_JOIN_DATA_BYTES = 4096;
// Joins an address can start at once, and how often it gets one more.
static constexpr int JOIN_BURST = 5;
static constexpr uint64_t JOIN_REFILL_USEC = 2000000;
// The host sends a heartbeat this often, so its players tell a silent host from a busy one.
static constexpr uint64_t HEARTBEAT_INTERVAL_USEC = 250000;
// A host heard from within this long (at least; half the `host_timeout` if longer) is alive.
static constexpr uint64_t HOST_ALIVE_MIN_USEC = 750000;
// How long a player that lost the host waits for the other players to confirm it.
static constexpr uint64_t CONFIRM_WINDOW_USEC = 1000000;

enum HostedMeshControl {
	CONTROL_WELCOME = 1,
	CONTROL_PAIR_OPEN,
	CONTROL_PAIR_PUNCH,
	CONTROL_PAIR_RELAY,
	CONTROL_PAIR_FAILED,
	CONTROL_MEMBER_LEFT,
	CONTROL_PAIR_READY,
	CONTROL_CERTIFICATE,
	CONTROL_SUCCESSION,
	CONTROL_REJOIN,
	// Player to host, first: the join data.
	CONTROL_JOIN,
	CONTROL_HEARTBEAT,
	// Between players that reach each other directly, when one lost the host: did you lose it too?
	CONTROL_HOST_QUERY,
	CONTROL_HOST_STATUS,
};

static ENetConnection *as_socket(const Ref<RefCounted> &p_socket) {
	return Object::cast_to<ENetConnection>(p_socket.ptr());
}

static ENetPacketPeer *as_link(const Ref<RefCounted> &p_link) {
	return Object::cast_to<ENetPacketPeer>(p_link.ptr());
}

static int flags_for_mode(TickTransport::TransferMode p_mode) {
	if (p_mode == TickTransport::TRANSFER_MODE_RELIABLE) {
		return ENET_PACKET_FLAG_RELIABLE;
	}
	if (p_mode == TickTransport::TRANSFER_MODE_UNRELIABLE) {
		return ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
	}
	return ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
}

static TickTransport::TransferMode mode_for_flags(int p_flags) {
	if (p_flags & ENET_PACKET_FLAG_RELIABLE) {
		return TickTransport::TRANSFER_MODE_RELIABLE;
	}
	if (p_flags & ENET_PACKET_FLAG_UNSEQUENCED) {
		return TickTransport::TRANSFER_MODE_UNRELIABLE;
	}
	return TickTransport::TRANSFER_MODE_UNRELIABLE_ORDERED;
}

// Flags a received packet is sent again with (the relay keeps the reliability and ordering of the original).
static int resend_flags(int p_flags) {
	return p_flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
}

// Control messages: a type byte, then little-endian fields.
struct HostedMeshWriter {
	LocalVector<uint8_t> bytes;

	explicit HostedMeshWriter(HostedMeshControl p_type) { bytes.push_back(uint8_t(p_type)); }
	void put_u32(uint32_t p_value) {
		const uint32_t offset = bytes.size();
		bytes.resize(offset + 4);
		encode_uint32(p_value, bytes.ptr() + offset);
	}
	void put_string(const String &p_value) {
		const CharString utf8 = p_value.utf8();
		put_u32(uint32_t(utf8.length()));
		for (int i = 0; i < utf8.length(); i++) {
			bytes.push_back(uint8_t(utf8[i]));
		}
	}
	void put_bytes(const PackedByteArray &p_value) {
		put_u32(uint32_t(p_value.size()));
		for (int i = 0; i < p_value.size(); i++) {
			bytes.push_back(p_value[i]);
		}
	}
};

struct HostedMeshReader {
	const uint8_t *data = nullptr;
	int size = 0;
	int offset = 0;
	bool failed = false;

	HostedMeshReader(const uint8_t *p_data, int p_size) :
			data(p_data), size(p_size) {}
	uint8_t get_u8() {
		if (offset + 1 > size) {
			failed = true;
			return 0;
		}
		return data[offset++];
	}
	uint32_t get_u32() {
		if (offset + 4 > size) {
			failed = true;
			return 0;
		}
		const uint32_t value = decode_uint32(data + offset);
		offset += 4;
		return value;
	}
	String get_string() {
		const uint32_t length = get_u32();
		// Addresses, and certificates (PEM) with DTLS. Checked before decoding: the engine's decoder would print an
		// error for every invalid byte.
		if (failed || length > 16384 || offset + int(length) > size || !TickDataBuffer::is_valid_utf8(data + offset, int(length))) {
			failed = true;
			return String();
		}
		String value = String::utf8((const char *)data + offset, int(length));
		offset += int(length);
		return value;
	}
	PackedByteArray get_bytes(int p_max_length) {
		const uint32_t length = get_u32();
		if (failed || length > uint32_t(p_max_length) || offset + int(length) > size) {
			failed = true;
			return PackedByteArray();
		}
		PackedByteArray value;
		value.resize(int(length));
		if (length > 0) {
			memcpy(value.ptrw(), data + offset, length);
		}
		offset += int(length);
		return value;
	}
};

struct EnetHostedMeshTransport::PlayerKeyJob {
	Ref<CryptoKey> key;
	Ref<X509Certificate> certificate;
};

// Runs on a worker thread: an RSA key takes a while to generate (the crypto library is built thread safe).
void EnetHostedMeshTransport::generate_player_key(void *p_job) {
	PlayerKeyJob *job = static_cast<PlayerKeyJob *>(p_job);
	Ref<Crypto> crypto = Ref<Crypto>(Crypto::create());
	if (crypto.is_null()) {
		return;
	}
	job->key = crypto->generate_rsa(2048);
	if (job->key.is_valid()) {
		job->certificate = crypto->generate_self_signed_certificate(job->key, vformat("CN=%s,O=TickSynchronizer,C=US", PLAYER_COMMON_NAME), "20000101000000", "20991231235959");
	}
}

// ------------------------------------------------------------------------------------------------------ Creation

Ref<EnetHostedMeshTransport> EnetHostedMeshTransport::create_host(int p_port, int p_max_players, const String &p_bind_address, Compression p_compression, const Ref<TLSOptions> &p_tls_options, int p_rendezvous_port) {
	ERR_FAIL_COND_V_MSG(p_max_players < 2 || p_max_players > 1024, Ref<EnetHostedMeshTransport>(), "The number of players must be between 2 and 1024.");
	Ref<ENetConnection> socket;
	socket.instantiate();
	// Every player keeps one connection; each pair being introduced adds two short registrations. Bandwidth limits stay
	// at 0 (unlimited): see ADR-030.
	const int max_peers = MIN(4095, p_max_players * 3);
	Error err = socket->create_host_bound(IPAddress(p_bind_address), p_port, max_peers, ENET_CHANNEL_COUNT, 0, 0);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<EnetHostedMeshTransport>(), vformat("Can't host a mesh on port %d.", p_port));
	socket->compress(ENetConnection::CompressionMode(p_compression));

	Ref<EnetHostedMeshTransport> transport;
	transport.instantiate();
	if (p_tls_options.is_valid()) {
		err = socket->dtls_server_setup(p_tls_options);
		ERR_FAIL_COND_V_MSG(err != OK, Ref<EnetHostedMeshTransport>(), "Can't set up DTLS on the mesh host.");
		// A DTLS socket only takes DTLS: the pairs register their endpoints on a plain one.
		Ref<ENetConnection> rendezvous;
		rendezvous.instantiate();
		const int rendezvous_port = p_rendezvous_port > 0 ? p_rendezvous_port : p_port + 1;
		err = rendezvous->create_host_bound(IPAddress(p_bind_address), rendezvous_port, max_peers, ENET_CHANNEL_COUNT, 0, 0);
		ERR_FAIL_COND_V_MSG(err != OK, Ref<EnetHostedMeshTransport>(), vformat("Can't open the rendezvous port %d.", rendezvous_port));
		rendezvous->compress(ENetConnection::CompressionMode(p_compression));
		transport->rendezvous = rendezvous;
		transport->tls_options = p_tls_options;
		transport->encrypted = true;
	}
	transport->is_host = true;
	transport->local_id = 1;
	transport->status = STATUS_CONNECTED;
	transport->listener = socket;
	transport->max_players = p_max_players;
	transport->compression = p_compression;
	return transport;
}

Ref<EnetHostedMeshTransport> EnetHostedMeshTransport::create_player(const String &p_address, int p_port, Compression p_compression, const Ref<TLSOptions> &p_tls_options, const String &p_tls_hostname, const PackedByteArray &p_join_data) {
	ERR_FAIL_COND_V_MSG(p_join_data.size() > MAX_JOIN_DATA_BYTES, Ref<EnetHostedMeshTransport>(), vformat("The join data can't be bigger than %d bytes.", MAX_JOIN_DATA_BYTES));
	Ref<ENetConnection> socket;
	socket.instantiate();
	ERR_FAIL_COND_V_MSG(socket->create_host(1, ENET_CHANNEL_COUNT, 0, 0) != OK, Ref<EnetHostedMeshTransport>(), "Can't create the socket for the host.");
	socket->compress(ENetConnection::CompressionMode(p_compression));

	Ref<EnetHostedMeshTransport> transport;
	transport.instantiate();
	if (p_tls_options.is_valid()) {
		const Error err = socket->dtls_client_setup(p_tls_hostname.is_empty() ? p_address : p_tls_hostname, p_tls_options);
		ERR_FAIL_COND_V_MSG(err != OK, Ref<EnetHostedMeshTransport>(), "Can't set up DTLS for the mesh host.");
		// The certificate of the direct links this player accepts; the host hands it to the other players. Generated on
		// a worker thread, and sent to the host once it's ready.
		Ref<Crypto> crypto = Ref<Crypto>(Crypto::create());
		ERR_FAIL_COND_V_MSG(crypto.is_null(), Ref<EnetHostedMeshTransport>(), "DTLS needs the crypto module.");
		transport->key_job = memnew(PlayerKeyJob);
		transport->key_task = WorkerThreadPool::get_singleton()->add_native_task(&EnetHostedMeshTransport::generate_player_key, transport->key_job, false, "TickSynchronizer mesh player key");
		transport->tls_options = p_tls_options;
		transport->encrypted = true;
	}
	Ref<ENetPacketPeer> link = socket->connect_to_host(p_address, p_port, ENET_CHANNEL_COUNT, int(JOIN_MAGIC));
	ERR_FAIL_COND_V_MSG(link.is_null(), Ref<EnetHostedMeshTransport>(), vformat("Can't connect to the host at %s:%d.", p_address, p_port));

	transport->apply_link_timeout(link);
	transport->status = STATUS_CONNECTING;
	transport->host_address = p_address;
	transport->host_port = p_port;
	transport->host_socket = socket;
	transport->host_link = link;
	transport->compression = p_compression;
	transport->join_data = p_join_data;
	return transport;
}

EnetHostedMeshTransport::~EnetHostedMeshTransport() {
	close();
}

void EnetHostedMeshTransport::close() {
	close_links(DISCONNECT_ENDED);
}

Error EnetHostedMeshTransport::hand_over() {
	ERR_FAIL_COND_V_MSG(!is_host || status != STATUS_CONNECTED, ERR_UNCONFIGURED, "Only the host of a mesh can hand it over.");
	close_links(DISCONNECT_HANDOVER);
	return OK;
}

void EnetHostedMeshTransport::close_links(int p_member_reason) {
	if (status != STATUS_DISCONNECTED) {
		disconnect_reason = DISCONNECT_REASON_CLOSED;
	}
	if (key_job) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(key_task);
		memdelete(key_job);
		key_job = nullptr;
	}
	confirming = false;
	pending_rejoins.clear();
	for (KeyValue<int, Pair> &E : pairs) {
		player_close_pair(E.value);
	}
	pairs.clear();
	ENetPacketPeer *link = as_link(host_link);
	if (link && link->is_active()) {
		link->peer_disconnect_now();
	}
	host_link.unref();
	ENetConnection *socket = as_socket(host_socket);
	if (socket) {
		socket->flush();
		socket->destroy();
	}
	host_socket.unref();

	for (KeyValue<int, Ref<RefCounted>> &E : members) {
		ENetPacketPeer *member = as_link(E.value);
		if (member && member->is_active()) {
			member->peer_disconnect_now(p_member_reason);
		}
	}
	members.clear();
	members_by_link.clear();
	for (KeyValue<ObjectID, PendingJoin> &E : pending_joins) {
		ENetPacketPeer *pending = as_link(E.value.link);
		if (pending && pending->is_active()) {
			pending->peer_disconnect_now(DISCONNECT_REMOVED);
		}
	}
	pending_joins.clear();
	joins_to_validate.clear();
	for (KeyValue<int, Ref<RefCounted>> &E : member_sockets) {
		ENetConnection *member_socket = as_socket(E.value);
		if (member_socket) {
			member_socket->flush();
			member_socket->destroy();
		}
	}
	member_sockets.clear();
	introductions.clear();
	introductions_by_token.clear();
	ENetConnection *listening = as_socket(listener);
	if (listening) {
		listening->flush();
		listening->destroy();
	}
	listener.unref();
	ENetConnection *rendezvous_socket = as_socket(rendezvous);
	if (rendezvous_socket) {
		rendezvous_socket->destroy();
	}
	rendezvous.unref();
	member_certificates.clear();
	status = STATUS_DISCONNECTED;
}

uint64_t EnetHostedMeshTransport::make_pair_key(int p_a, int p_b) {
	return (uint64_t(uint32_t(MIN(p_a, p_b))) << 32) | uint64_t(uint32_t(MAX(p_a, p_b)));
}

uint32_t EnetHostedMeshTransport::make_token() {
	// Unpredictable: whoever knows a token can take the place of a player in a pair. Never a join, of any version.
	uint32_t token = 0;
	while (token == 0 || (token & 0xFFFFFF00) == JOIN_MAGIC_PREFIX) {
		uint8_t bytes[4] = {};
		if (OS::get_singleton()->get_entropy(bytes, 4) != OK) {
			bytes[0] = uint8_t(OS::get_singleton()->get_ticks_usec());
		}
		token = decode_uint32(bytes);
	}
	return token;
}

// ------------------------------------------------------------------------------------------------------ Common

void EnetHostedMeshTransport::push_event(EventType p_type, int p_peer) {
	Event event;
	event.type = p_type;
	event.peer = p_peer;
	events.push_back(event);
	if (multiplayer_peer) {
		multiplayer_events.push_back(event);
	}
}

void EnetHostedMeshTransport::deliver(int p_from, int p_logical, int p_flags, const uint8_t *p_data, int p_size) {
	if (p_logical < 0 || p_logical >= LOGICAL_CHANNEL_COUNT) {
		return;
	}
	if (p_logical < TICK_CHANNEL_COUNT) {
		if (!queue_has_room(packets.size() - next_packet, queued_bytes, p_size)) {
			// Nothing consumes them (no engine runs on this transport, for example).
			dropped_packets++;
			return;
		}
		Packet packet;
		packet.from_peer = p_from;
		packet.channel = p_logical;
		packet.mode = mode_for_flags(p_flags);
		packet.data.resize(p_size);
		if (p_size > 0) {
			memcpy(packet.data.ptr(), p_data, p_size);
		}
		queued_bytes += uint64_t(p_size);
		packets.push_back(packet);
		return;
	}
	if (multiplayer_peer == nullptr) {
		return;
	}
	if (!queue_has_room(multiplayer_packets.size() - next_multiplayer_packet, multiplayer_queued_bytes, p_size)) {
		// The multiplayer peer isn't polled.
		dropped_packets++;
		return;
	}
	MultiplayerPacket packet;
	packet.from_peer = p_from;
	packet.channel = (p_logical - TICK_CHANNEL_COUNT) / 2;
	packet.mode = mode_for_flags(p_flags);
	packet.data.resize(p_size);
	if (p_size > 0) {
		memcpy(packet.data.ptr(), p_data, p_size);
	}
	multiplayer_queued_bytes += uint64_t(p_size);
	multiplayer_packets.push_back(packet);
}

Error EnetHostedMeshTransport::send_on_link(const Ref<RefCounted> &p_link, int p_channel, int p_flags, const uint8_t *p_data, int p_size) {
	ENetPacketPeer *link = as_link(p_link);
	// A link ENet already reset (its disconnection is on the way, when several players leave at once) has no channels.
	if (link == nullptr || !link->is_active() || p_channel >= link->get_channels()) {
		return ERR_UNAVAILABLE;
	}
	ENetPacket *packet = enet_packet_create(p_data, p_size, p_flags);
	ERR_FAIL_NULL_V(packet, ERR_OUT_OF_MEMORY);
	if (link->send(uint8_t(p_channel), packet) < 0) {
		enet_packet_destroy(packet);
		return ERR_CANT_CONNECT;
	}
	return OK;
}

void EnetHostedMeshTransport::send_control(const Ref<RefCounted> &p_link, const LocalVector<uint8_t> &p_message) {
	send_on_link(p_link, CHANNEL_CONTROL, ENET_PACKET_FLAG_RELIABLE, p_message.ptr(), int(p_message.size()));
}

Error EnetHostedMeshTransport::send_logical(int p_peer, int p_logical, int p_flags, const uint8_t *p_data, int p_size) {
	if (p_peer == PEER_BROADCAST) {
		LocalVector<int> connected;
		get_connected_peers(connected);
		for (const int peer : connected) {
			send_logical(peer, p_logical, p_flags, p_data, p_size);
		}
		return OK;
	}
	if (is_host) {
		const Ref<RefCounted> *member = members.getptr(p_peer);
		ERR_FAIL_NULL_V_MSG(member, ERR_UNAVAILABLE, vformat("Player %d isn't connected.", p_peer));
		return send_on_link(*member, FIRST_DIRECT_CHANNEL + p_logical, p_flags, p_data, p_size);
	}
	if (p_peer == host_id) {
		ERR_FAIL_COND_V_MSG(status != STATUS_CONNECTED, ERR_UNAVAILABLE, "The host isn't connected.");
		return send_on_link(host_link, FIRST_DIRECT_CHANNEL + p_logical, p_flags, p_data, p_size);
	}
	const Pair *pair = pairs.getptr(p_peer);
	ERR_FAIL_COND_V_MSG(pair == nullptr || !pair->reported, ERR_UNAVAILABLE, vformat("Player %d isn't connected.", p_peer));
	if (pair->state == PAIR_DIRECT) {
		return send_on_link(pair->link, FIRST_DIRECT_CHANNEL + p_logical, p_flags, p_data, p_size);
	}
	// Relayed: the host forwards it, declaring this player as the origin.
	LocalVector<uint8_t> relayed;
	relayed.resize(RELAY_HEADER_SIZE + p_size);
	encode_uint32(uint32_t(p_peer), relayed.ptr());
	if (p_size > 0) {
		memcpy(relayed.ptr() + RELAY_HEADER_SIZE, p_data, p_size);
	}
	return send_on_link(host_link, FIRST_RELAY_CHANNEL + p_logical, p_flags, relayed.ptr(), int(relayed.size()));
}

Error EnetHostedMeshTransport::send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) {
	ERR_FAIL_INDEX_V(p_channel, TICK_CHANNEL_COUNT, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_size <= 0 || p_data == nullptr, ERR_INVALID_PARAMETER);
	return send_logical(p_peer, p_channel, flags_for_mode(p_mode), p_data, p_size);
}

bool EnetHostedMeshTransport::is_peer_connected(int p_peer) const {
	if (is_host) {
		return members.has(p_peer);
	}
	if (p_peer == host_id) {
		return status == STATUS_CONNECTED;
	}
	const Pair *pair = pairs.getptr(p_peer);
	return pair && pair->reported;
}

void EnetHostedMeshTransport::get_connected_peers(LocalVector<int> &r_peers) const {
	r_peers.clear();
	if (is_host) {
		for (const KeyValue<int, Ref<RefCounted>> &E : members) {
			r_peers.push_back(E.key);
		}
	} else if (status == STATUS_CONNECTED) {
		r_peers.push_back(host_id);
		for (const KeyValue<int, Pair> &E : pairs) {
			if (E.value.reported) {
				r_peers.push_back(E.key);
			}
		}
	}
	r_peers.sort();
}

PackedInt32Array EnetHostedMeshTransport::get_peers() const {
	LocalVector<int> connected;
	get_connected_peers(connected);
	PackedInt32Array result;
	for (const int peer : connected) {
		result.push_back(peer);
	}
	return result;
}

EnetHostedMeshTransport::PeerPath EnetHostedMeshTransport::get_peer_path(int p_peer) const {
	if (is_host) {
		return members.has(p_peer) ? PATH_HOST : PATH_NONE;
	}
	if (p_peer == host_id) {
		return status == STATUS_CONNECTED ? PATH_HOST : (status == STATUS_CONNECTING ? PATH_CONNECTING : PATH_NONE);
	}
	const Pair *pair = pairs.getptr(p_peer);
	if (pair == nullptr) {
		return PATH_NONE;
	}
	if (!pair->reported) {
		return PATH_CONNECTING;
	}
	return pair->state == PAIR_DIRECT ? PATH_DIRECT : PATH_RELAYED;
}

int EnetHostedMeshTransport::get_channel_count() const {
	return TICK_CHANNEL_COUNT;
}

int EnetHostedMeshTransport::get_max_payload_size() const {
	return HOSTED_MESH_MAX_PAYLOAD - (encrypted ? DTLS_OVERHEAD : 0);
}

void EnetHostedMeshTransport::disconnect_peer(int p_peer) {
	if (is_host) {
		const Ref<RefCounted> *member = members.getptr(p_peer);
		ERR_FAIL_NULL_MSG(member, vformat("Player %d isn't connected.", p_peer));
		ENetPacketPeer *link = as_link(*member);
		if (link && link->is_active()) {
			link->peer_disconnect(DISCONNECT_REMOVED);
		}
		return;
	}
	ERR_FAIL_COND_MSG(p_peer != host_id, "A player can only disconnect from the host; the host disconnects players.");
	ENetPacketPeer *link = as_link(host_link);
	if (link && link->is_active()) {
		leaving = true;
		link->peer_disconnect();
	}
}

void EnetHostedMeshTransport::set_punch_timeout(double p_seconds) {
	ERR_FAIL_COND_MSG(!(p_seconds > 0.0), "The punch timeout must be positive.");
	punch_timeout = p_seconds;
}

void EnetHostedMeshTransport::set_direct_connections(bool p_enabled) {
	direct_connections = p_enabled;
}

Dictionary EnetHostedMeshTransport::get_stats() const {
	Dictionary result;
	int direct = 0;
	int relayed = 0;
	for (const KeyValue<int, Pair> &E : pairs) {
		if (E.value.reported) {
			direct += E.value.state == PAIR_DIRECT ? 1 : 0;
			relayed += E.value.state == PAIR_RELAYED ? 1 : 0;
		}
	}
	for (const KeyValue<uint64_t, Introduction> &E : introductions) {
		relayed += E.value.relayed ? 1 : 0;
	}
	result["direct_pairs"] = direct;
	result["relayed_pairs"] = relayed;
	result["relayed_packets"] = relayed_packets;
	result["rejected_connections"] = rejected_connections;
	result["failed_punches"] = failed_punches;
	result["dropped_packets"] = dropped_packets;
	return result;
}

void EnetHostedMeshTransport::poll() {
	if (is_host) {
		host_poll();
	} else {
		player_poll();
	}
}

bool EnetHostedMeshTransport::pop_event(Event &r_event) {
	if (next_event >= events.size()) {
		events.clear();
		next_event = 0;
		return false;
	}
	r_event = events[next_event++];
	return true;
}

bool EnetHostedMeshTransport::pop_packet(Packet &r_packet) {
	if (next_packet >= packets.size()) {
		packets.clear();
		next_packet = 0;
		queued_bytes = 0;
		return false;
	}
	r_packet = packets[next_packet++];
	queued_bytes -= r_packet.data.size();
	return true;
}

// ------------------------------------------------------------------------------------------------------ Host

void EnetHostedMeshTransport::host_poll() {
	if (listener.is_valid()) {
		host_service(listener, 0);
	}
	if (rendezvous.is_valid()) {
		host_service(rendezvous, 1);
	}
	LocalVector<Ref<RefCounted>> sockets;
	for (const KeyValue<int, Ref<RefCounted>> &E : member_sockets) {
		sockets.push_back(E.value);
	}
	for (const Ref<RefCounted> &socket : sockets) {
		host_service(socket, 2);
	}
	// The game decides on the players whose join data arrived; its validator may even close the mesh.
	host_validate_joins();
	if (status == STATUS_DISCONNECTED) {
		return;
	}

	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	// Players that didn't send their join data, or that the game didn't admit, in time.
	LocalVector<ObjectID> late;
	for (const KeyValue<ObjectID, PendingJoin> &E : pending_joins) {
		if (now >= E.value.deadline_usec) {
			late.push_back(E.key);
		}
	}
	for (const ObjectID link : late) {
		rejected_connections++;
		host_refuse(link);
	}
	if (now >= next_heartbeat_usec) {
		next_heartbeat_usec = now + HEARTBEAT_INTERVAL_USEC;
		const uint8_t heartbeat = CONTROL_HEARTBEAT;
		for (const KeyValue<int, Ref<RefCounted>> &E : members) {
			send_on_link(E.value, CHANNEL_CONTROL, ENET_PACKET_FLAG_UNSEQUENCED, &heartbeat, 1);
		}
	}
	if (now >= next_budget_prune_usec) {
		// The addresses whose budget is full again are forgotten.
		next_budget_prune_usec = now + JOIN_REFILL_USEC * JOIN_BURST;
		LocalVector<String> full;
		for (const KeyValue<String, JoinBudget> &E : join_budgets) {
			if (now - E.value.last_usec >= JOIN_REFILL_USEC * JOIN_BURST) {
				full.push_back(E.key);
			}
		}
		for (const String &address : full) {
			join_budgets.erase(address);
		}
	}

	// Pairs that didn't register their endpoints in time are relayed.
	LocalVector<uint64_t> expired;
	for (const KeyValue<uint64_t, Introduction> &E : introductions) {
		if (!E.value.punching && !E.value.relayed && now >= E.value.deadline_usec) {
			expired.push_back(E.key);
		}
	}
	for (const uint64_t key : expired) {
		host_relay_pair(key);
	}
	if (listener.is_valid()) {
		as_socket(listener)->flush();
	}
	if (rendezvous.is_valid()) {
		as_socket(rendezvous)->flush();
	}
	for (const KeyValue<int, Ref<RefCounted>> &E : member_sockets) {
		as_socket(E.value)->flush();
	}
}

void EnetHostedMeshTransport::host_service(const Ref<RefCounted> &p_socket, int p_kind) {
	ENetConnection *socket = as_socket(p_socket);
	// Bounded, so a flood can't stall the frame.
	for (int i = 0; socket && i < 4096; i++) {
		ENetConnection::Event event;
		const ENetConnection::EventType type = socket->service(0, event);
		if (type == ENetConnection::EVENT_NONE || type == ENetConnection::EVENT_ERROR) {
			break;
		}
		if (type == ENetConnection::EVENT_CONNECT) {
			host_on_connect(event.peer, event.data, p_kind);
		} else if (type == ENetConnection::EVENT_DISCONNECT) {
			host_on_disconnect(event.peer);
			if (p_kind == 2) {
				// A member's own socket had a single link.
				return;
			}
		} else if (type == ENetConnection::EVENT_RECEIVE) {
			const ObjectID link = event.peer->get_instance_id();
			const int *id = members_by_link.getptr(link);
			if (id) {
				host_on_receive(*id, event.channel_id, event.packet->data, int(event.packet->dataLength), event.packet->flags);
			} else if (event.channel_id == CHANNEL_CONTROL && pending_joins.has(link)) {
				host_on_join(link, event.packet->data, int(event.packet->dataLength));
			}
			enet_packet_destroy(event.packet);
		}
	}
}

bool EnetHostedMeshTransport::host_take_join_budget(const String &p_address) {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	JoinBudget *budget = join_budgets.getptr(p_address);
	if (budget == nullptr) {
		JoinBudget fresh;
		fresh.tokens = JOIN_BURST;
		fresh.last_usec = now;
		budget = &join_budgets.insert(p_address, fresh)->value;
	}
	budget->tokens = MIN(double(JOIN_BURST), budget->tokens + double(now - budget->last_usec) / double(JOIN_REFILL_USEC));
	budget->last_usec = now;
	if (budget->tokens < 1.0) {
		return false;
	}
	budget->tokens -= 1.0;
	return true;
}

void EnetHostedMeshTransport::host_on_join(ObjectID p_link, const uint8_t *p_data, int p_size) {
	PendingJoin *pending = pending_joins.getptr(p_link);
	HostedMeshReader reader(p_data, p_size);
	if (pending == nullptr || pending->received || reader.get_u8() != CONTROL_JOIN) {
		return;
	}
	const PackedByteArray data = reader.get_bytes(MAX_JOIN_DATA_BYTES);
	if (reader.failed) {
		rejected_connections++;
		host_refuse(p_link);
		return;
	}
	pending->received = true;
	pending->data = data;
	joins_to_validate.push_back(p_link);
}

void EnetHostedMeshTransport::host_validate_joins() {
	LocalVector<ObjectID> joins;
	joins = joins_to_validate;
	joins_to_validate.clear();
	for (const ObjectID link : joins) {
		const PendingJoin *pending = pending_joins.getptr(link);
		if (status == STATUS_DISCONNECTED || pending == nullptr) {
			continue;
		}
		if (!join_validator.is_valid()) {
			host_admit(link);
			continue;
		}
		// Game code: it may admit or refuse the player itself, or close the mesh.
		const Variant verdict = join_validator.call(pending->id, pending->data, pending->address);
		if (status == STATUS_DISCONNECTED || !pending_joins.has(link) || verdict.get_type() != Variant::BOOL) {
			// Otherwise the game decides later, with `admit_player()` or `refuse_player()`.
			continue;
		}
		if (bool(verdict)) {
			host_admit(link);
		} else {
			rejected_connections++;
			host_refuse(link);
		}
	}
}

void EnetHostedMeshTransport::host_admit(ObjectID p_link) {
	PendingJoin *pending = pending_joins.getptr(p_link);
	ERR_FAIL_NULL(pending);
	const int id = pending->id;
	const Ref<RefCounted> link = pending->link;
	pending_joins.erase(p_link);
	members.insert(id, link);
	members_by_link.insert(p_link, id);
	apply_link_timeout(link);
	HostedMeshWriter welcome(CONTROL_WELCOME);
	welcome.put_u32(uint32_t(id));
	// Where the pairs register their endpoints (0: this port), and how many players the mesh can have.
	welcome.put_u32(rendezvous.is_valid() ? uint32_t(as_socket(rendezvous)->get_local_port()) : 0);
	welcome.put_u32(uint32_t(max_players));
	send_control(link, welcome.bytes);
	push_event(EVENT_PEER_CONNECTED, id);
	for (const KeyValue<int, Ref<RefCounted>> &E : members) {
		if (E.key != id) {
			host_introduce(E.key, id);
		}
	}
	host_send_succession();
}

void EnetHostedMeshTransport::host_refuse(ObjectID p_link) {
	PendingJoin *pending = pending_joins.getptr(p_link);
	ERR_FAIL_NULL(pending);
	ENetPacketPeer *link = as_link(pending->link);
	if (link && link->is_active()) {
		// Right away, so the connection doesn't hold a place of the host.
		link->peer_disconnect_now(DISCONNECT_REMOVED);
	}
	pending_joins.erase(p_link);
}

Error EnetHostedMeshTransport::admit_player(int p_peer) {
	ERR_FAIL_COND_V_MSG(!is_host, ERR_UNCONFIGURED, "Only the host admits players.");
	ObjectID found;
	for (const KeyValue<ObjectID, PendingJoin> &E : pending_joins) {
		if (E.value.id == p_peer && E.value.received) {
			found = E.key;
		}
	}
	ERR_FAIL_COND_V_MSG(found.is_null(), ERR_INVALID_PARAMETER, vformat("Player %d isn't waiting to join.", p_peer));
	host_admit(found);
	return OK;
}

Error EnetHostedMeshTransport::refuse_player(int p_peer) {
	ERR_FAIL_COND_V_MSG(!is_host, ERR_UNCONFIGURED, "Only the host refuses players.");
	ObjectID found;
	for (const KeyValue<ObjectID, PendingJoin> &E : pending_joins) {
		if (E.value.id == p_peer) {
			found = E.key;
		}
	}
	ERR_FAIL_COND_V_MSG(found.is_null(), ERR_INVALID_PARAMETER, vformat("Player %d isn't waiting to join.", p_peer));
	rejected_connections++;
	host_refuse(found);
	return OK;
}

void EnetHostedMeshTransport::host_send_succession() {
	// Players without relayed pairs first (they reach everyone directly), then by id.
	LocalVector<int> ids;
	LocalVector<int> relayed;
	for (const KeyValue<int, Ref<RefCounted>> &E : members) {
		int count = 0;
		for (const KeyValue<uint64_t, Introduction> &I : introductions) {
			if (I.value.relayed && (I.value.first == E.key || I.value.second == E.key)) {
				count++;
			}
		}
		ids.push_back(E.key);
		relayed.push_back(count);
	}
	// Insertion sort: a handful of players.
	for (uint32_t i = 1; i < ids.size(); i++) {
		for (uint32_t j = i; j > 0 && (relayed[j] < relayed[j - 1] || (relayed[j] == relayed[j - 1] && ids[j] < ids[j - 1])); j--) {
			SWAP(ids[j], ids[j - 1]);
			SWAP(relayed[j], relayed[j - 1]);
		}
	}
	HostedMeshWriter message(CONTROL_SUCCESSION);
	message.put_u32(ids.size());
	for (const int id : ids) {
		message.put_u32(uint32_t(id));
	}
	for (const KeyValue<int, Ref<RefCounted>> &E : members) {
		send_control(E.value, message.bytes);
	}
}

void EnetHostedMeshTransport::host_on_connect(const Ref<RefCounted> &p_link, uint32_t p_data, int p_kind) {
	ENetPacketPeer *link = as_link(p_link);
	ERR_FAIL_NULL(link);
	// Refused connections are closed right away, so they don't hold places of the host.
	if (p_kind == 2) {
		// A member's own socket (after a migration) takes no new connections.
		rejected_connections++;
		link->peer_disconnect_now();
		return;
	}
	if ((p_data & 0xFFFFFF00) == JOIN_MAGIC_PREFIX) {
		// Players of this version join on the main socket (DTLS when encrypted), never on the rendezvous one, within
		// the budget of their address; the players still joining count toward the limit. The refused ones learn why.
		// Nobody learns about a joining player until it's admitted: its join data comes first.
		const String address = String(link->get_remote_address());
		int refusal = 0;
		if (p_data != JOIN_MAGIC) {
			refusal = DISCONNECT_VERSION_PREFIX | int(JOIN_MAGIC & 0xFF);
		} else if (p_kind == 1) {
			refusal = DISCONNECT_REMOVED;
		} else if (int(members.size() + pending_joins.size()) >= max_players - 1) {
			refusal = DISCONNECT_FULL;
		} else if (!host_take_join_budget(address)) {
			refusal = DISCONNECT_BUSY;
		}
		if (refusal != 0) {
			rejected_connections++;
			link->peer_disconnect_now(refusal);
			return;
		}
		PendingJoin pending;
		pending.id = next_player_id++;
		pending.link = p_link;
		pending.address = address;
		pending.deadline_usec = OS::get_singleton()->get_ticks_usec() + uint64_t(join_timeout * 1000000.0);
		pending_joins.insert(p_link->get_instance_id(), pending);
		return;
	}

	// The registration of a pair's socket: its public endpoint is where the connection came from.
	const uint64_t *key = introductions_by_token.getptr(p_data);
	Introduction *introduction = key ? introductions.getptr(*key) : nullptr;
	if (introduction == nullptr || introduction->relayed) {
		rejected_connections++;
		link->peer_disconnect_now();
		return;
	}
	const int side = introduction->registration_tokens[0] == p_data ? 0 : 1;
	introduction->registered[side] = true;
	introduction->addresses[side] = String(link->get_remote_address());
	introduction->ports[side] = link->get_remote_port();
	introductions_by_token.erase(p_data);
	// Closed gracefully, so the player's socket is free to connect to the other player.
	link->peer_disconnect();

	host_try_punch(*key);
}

void EnetHostedMeshTransport::host_try_punch(uint64_t p_key) {
	Introduction *introduction = introductions.getptr(p_key);
	if (introduction == nullptr || introduction->punching || introduction->relayed || !introduction->registered[0] || !introduction->registered[1]) {
		return;
	}
	// With DTLS, the connecting player (the higher id) pins the accepting player's certificate.
	const String *certificate = member_certificates.getptr(introduction->first);
	if (encrypted && certificate == nullptr) {
		return;
	}
	introduction->punching = true;
	for (int i = 0; i < 2; i++) {
		const int member = i == 0 ? introduction->first : introduction->second;
		const int other = 1 - i;
		const Ref<RefCounted> *member_link = members.getptr(member);
		if (member_link == nullptr) {
			continue;
		}
		HostedMeshWriter punch(CONTROL_PAIR_PUNCH);
		punch.put_u32(uint32_t(other == 0 ? introduction->first : introduction->second));
		punch.put_u32(introduction->connect_token);
		punch.put_u32(uint32_t(introduction->ports[other]));
		punch.put_string(introduction->addresses[other]);
		punch.put_string(i == 1 && certificate ? *certificate : String());
		send_control(*member_link, punch.bytes);
	}
}

void EnetHostedMeshTransport::host_on_disconnect(const Ref<RefCounted> &p_link) {
	if (pending_joins.erase(p_link->get_instance_id())) {
		// Left before being admitted: nobody knew about it.
		return;
	}
	const int *found = members_by_link.getptr(p_link->get_instance_id());
	if (found == nullptr) {
		// A registration, closed after being recorded.
		return;
	}
	const int id = *found;
	members_by_link.erase(p_link->get_instance_id());
	members.erase(id);
	member_certificates.erase(id);
	Ref<RefCounted> *member_socket = member_sockets.getptr(id);
	if (member_socket) {
		// Destroyed once its events are serviced.
		Ref<RefCounted> socket = *member_socket;
		member_sockets.erase(id);
		as_socket(socket)->flush();
	}
	push_event(EVENT_PEER_DISCONNECTED, id);

	HostedMeshWriter left(CONTROL_MEMBER_LEFT);
	left.put_u32(uint32_t(id));
	for (const KeyValue<int, Ref<RefCounted>> &E : members) {
		send_control(E.value, left.bytes);
	}
	LocalVector<uint64_t> forgotten;
	for (const KeyValue<uint64_t, Introduction> &E : introductions) {
		if (E.value.first == id || E.value.second == id) {
			forgotten.push_back(E.key);
		}
	}
	for (const uint64_t key : forgotten) {
		host_forget_introduction(key);
	}
	host_send_succession();
}

void EnetHostedMeshTransport::host_on_receive(int p_from, int p_channel, const uint8_t *p_data, int p_size, int p_flags) {
	if (p_channel == CHANNEL_CONTROL) {
		HostedMeshReader reader(p_data, p_size);
		const uint8_t type = reader.get_u8();
		if (type == CONTROL_PAIR_FAILED) {
			const int peer = int(reader.get_u32());
			const uint64_t key = make_pair_key(p_from, peer);
			if (!reader.failed && introductions.has(key)) {
				host_relay_pair(key);
			}
		} else if (type == CONTROL_REJOIN) {
			const int count = int(reader.get_u32());
			LocalVector<int> direct;
			for (int i = 0; i < count && i < 1024 && !reader.failed; i++) {
				direct.push_back(int(reader.get_u32()));
			}
			if (!reader.failed) {
				host_handle_rejoin(p_from, direct);
			}
		} else if (type == CONTROL_HOST_QUERY) {
			// A player still confirming that the host before this one is gone.
			const int asked = int(reader.get_u32());
			if (!reader.failed) {
				HostedMeshWriter answer(CONTROL_HOST_STATUS);
				answer.put_u32(uint32_t(asked));
				answer.put_u32(asked == local_id ? 1 : 0);
				send_control(members[p_from], answer.bytes);
			}
		} else if (type == CONTROL_CERTIFICATE && encrypted && !member_certificates.has(p_from)) {
			// Only the first one counts: the others aren't even read.
			const String certificate = reader.get_string();
			if (!reader.failed) {
				member_certificates.insert(p_from, certificate);
				// Introductions that waited for it.
				LocalVector<uint64_t> keys;
				for (const KeyValue<uint64_t, Introduction> &E : introductions) {
					if (E.value.first == p_from || E.value.second == p_from) {
						keys.push_back(E.key);
					}
				}
				for (const uint64_t key : keys) {
					host_try_punch(key);
				}
			}
		}
		return;
	}
	if (p_channel < FIRST_RELAY_CHANNEL) {
		deliver(p_from, p_channel - FIRST_DIRECT_CHANNEL, p_flags, p_data, p_size);
		return;
	}
	if (p_channel >= ENET_CHANNEL_COUNT || p_size < RELAY_HEADER_SIZE) {
		return;
	}
	// Relay: only between players of a relayed pair. The origin comes from the connection, never from the packet.
	const int target = int(decode_uint32(p_data));
	const Introduction *introduction = introductions.getptr(make_pair_key(p_from, target));
	const Ref<RefCounted> *target_link = members.getptr(target);
	// A pair known to be direct isn't relayed; after a migration, a pair not reported yet is.
	if (target == p_from || target_link == nullptr || (introduction && !introduction->relayed)) {
		return;
	}
	LocalVector<uint8_t> forwarded;
	forwarded.resize(p_size);
	encode_uint32(uint32_t(p_from), forwarded.ptr());
	memcpy(forwarded.ptr() + RELAY_HEADER_SIZE, p_data + RELAY_HEADER_SIZE, p_size - RELAY_HEADER_SIZE);
	if (send_on_link(*target_link, p_channel, resend_flags(p_flags), forwarded.ptr(), int(forwarded.size())) == OK) {
		relayed_packets++;
	}
}

void EnetHostedMeshTransport::host_handle_rejoin(int p_from, const LocalVector<int> &p_direct) {
	// A player that followed this node after a migration: the players it reaches directly. The other pairs with it
	// are relayed here.
	const Ref<RefCounted> *rejoined = members.getptr(p_from);
	ERR_FAIL_NULL(rejoined);
	for (const KeyValue<int, Ref<RefCounted>> &E : members) {
		if (E.key == p_from) {
			continue;
		}
		const uint64_t key = make_pair_key(p_from, E.key);
		if (!introductions.has(key)) {
			Introduction introduction;
			introduction.first = MIN(p_from, E.key);
			introduction.second = MAX(p_from, E.key);
			introduction.punching = true;
			introductions.insert(key, introduction);
		}
		if (!p_direct.has(E.key)) {
			Introduction &introduction = introductions[key];
			introduction.relayed = true;
			// Both sides, every time: the other player may have followed this host before or after.
			HostedMeshWriter to_rejoined(CONTROL_PAIR_RELAY);
			to_rejoined.put_u32(uint32_t(E.key));
			send_control(*rejoined, to_rejoined.bytes);
			HostedMeshWriter to_other(CONTROL_PAIR_RELAY);
			to_other.put_u32(uint32_t(p_from));
			send_control(E.value, to_other.bytes);
		}
	}
	host_send_succession();
}

void EnetHostedMeshTransport::host_introduce(int p_first, int p_second) {
	Introduction introduction;
	introduction.first = MIN(p_first, p_second);
	introduction.second = MAX(p_first, p_second);
	for (int i = 0; i < 2; i++) {
		uint32_t token = make_token();
		while (introductions_by_token.has(token)) {
			token = make_token();
		}
		introduction.registration_tokens[i] = token;
	}
	introduction.connect_token = make_token();
	introduction.deadline_usec = OS::get_singleton()->get_ticks_usec() + uint64_t(punch_timeout * 1000000.0);
	const uint64_t key = make_pair_key(p_first, p_second);
	introductions.insert(key, introduction);
	introductions_by_token.insert(introduction.registration_tokens[0], key);
	introductions_by_token.insert(introduction.registration_tokens[1], key);

	for (int i = 0; i < 2; i++) {
		const int member = i == 0 ? introduction.first : introduction.second;
		const Ref<RefCounted> *member_link = members.getptr(member);
		if (member_link == nullptr) {
			continue;
		}
		HostedMeshWriter open(CONTROL_PAIR_OPEN);
		open.put_u32(uint32_t(i == 0 ? introduction.second : introduction.first));
		open.put_u32(introduction.registration_tokens[i]);
		send_control(*member_link, open.bytes);
	}
}

void EnetHostedMeshTransport::host_relay_pair(uint64_t p_key) {
	Introduction *introduction = introductions.getptr(p_key);
	if (introduction == nullptr || introduction->relayed) {
		return;
	}
	introduction->relayed = true;
	for (int i = 0; i < 2; i++) {
		introductions_by_token.erase(introduction->registration_tokens[i]);
	}
	host_send_succession();
	for (int i = 0; i < 2; i++) {
		const int member = i == 0 ? introduction->first : introduction->second;
		const Ref<RefCounted> *member_link = members.getptr(member);
		if (member_link == nullptr) {
			continue;
		}
		HostedMeshWriter relay(CONTROL_PAIR_RELAY);
		relay.put_u32(uint32_t(i == 0 ? introduction->second : introduction->first));
		send_control(*member_link, relay.bytes);
	}
}

void EnetHostedMeshTransport::host_forget_introduction(uint64_t p_key) {
	Introduction *introduction = introductions.getptr(p_key);
	if (introduction == nullptr) {
		return;
	}
	for (int i = 0; i < 2; i++) {
		introductions_by_token.erase(introduction->registration_tokens[i]);
	}
	introductions.erase(p_key);
}

// ------------------------------------------------------------------------------------------------------ Player

void EnetHostedMeshTransport::player_poll() {
	player_finish_key_job();
	player_service_host();
	if (status == STATUS_DISCONNECTED) {
		return;
	}
	LocalVector<int> peers;
	for (const KeyValue<int, Pair> &E : pairs) {
		peers.push_back(E.key);
	}
	for (const int peer : peers) {
		Pair *pair = pairs.getptr(peer);
		if (pair) {
			player_service_pair(peer, *pair);
		}
	}
	if (confirming) {
		player_check_confirmation();
		if (status == STATUS_DISCONNECTED || is_host) {
			return;
		}
	}
	ENetConnection *socket = as_socket(host_socket);
	if (socket) {
		socket->flush();
	}
	for (KeyValue<int, Pair> &E : pairs) {
		ENetConnection *pair_socket = as_socket(E.value.socket);
		if (pair_socket) {
			pair_socket->flush();
		}
	}
}

void EnetHostedMeshTransport::player_service_host() {
	ENetConnection *socket = as_socket(host_socket);
	if (socket == nullptr) {
		return;
	}
	for (int i = 0; i < 4096; i++) {
		ENetConnection::Event event;
		const ENetConnection::EventType type = socket->service(0, event);
		if (type == ENetConnection::EVENT_NONE || type == ENetConnection::EVENT_ERROR) {
			break;
		}
		if (type == ENetConnection::EVENT_DISCONNECT) {
			player_on_host_disconnect(int(event.data));
			return;
		}
		host_heard_usec = OS::get_singleton()->get_ticks_usec();
		if (type != ENetConnection::EVENT_RECEIVE) {
			if (type == ENetConnection::EVENT_CONNECT && status == STATUS_CONNECTING) {
				// Connected: this player asks to join; the id comes with the welcome, once the host admits it.
				HostedMeshWriter join(CONTROL_JOIN);
				join.put_bytes(join_data);
				send_control(host_link, join.bytes);
			}
			continue;
		}
		const int channel = event.channel_id;
		const uint8_t *data = event.packet->data;
		const int size = int(event.packet->dataLength);
		if (channel == CHANNEL_CONTROL) {
			player_on_control(data, size);
		} else if (status == STATUS_CONNECTED && channel < FIRST_RELAY_CHANNEL) {
			deliver(host_id, channel - FIRST_DIRECT_CHANNEL, event.packet->flags, data, size);
		} else if (status == STATUS_CONNECTED && channel < ENET_CHANNEL_COUNT && size >= RELAY_HEADER_SIZE) {
			// Relayed by the host, which declares the origin (the host is trusted: it assigns the ids).
			const int origin = int(decode_uint32(data));
			const Pair *pair = pairs.getptr(origin);
			if (pair && pair->state == PAIR_RELAYED && pair->reported) {
				deliver(origin, channel - FIRST_RELAY_CHANNEL, event.packet->flags, data + RELAY_HEADER_SIZE, size - RELAY_HEADER_SIZE);
			}
		}
		enet_packet_destroy(event.packet);
		if (status == STATUS_DISCONNECTED) {
			return;
		}
	}
}

void EnetHostedMeshTransport::player_on_control(const uint8_t *p_data, int p_size) {
	HostedMeshReader reader(p_data, p_size);
	const uint8_t type = reader.get_u8();
	if (type == CONTROL_HEARTBEAT) {
		// Only tells that the host is alive (`host_heard_usec`).
		return;
	}
	const int peer = int(reader.get_u32());
	if (reader.failed) {
		return;
	}
	switch (type) {
		case CONTROL_WELCOME: {
			if (status != STATUS_CONNECTING || peer <= PEER_SERVER) {
				return;
			}
			const uint32_t rendezvous_port = reader.get_u32();
			const uint32_t player_limit = reader.get_u32();
			if (reader.failed) {
				return;
			}
			local_id = peer;
			host_rendezvous_port = int(rendezvous_port);
			host_max_players = int(CLAMP(player_limit, 2u, 1024u));
			status = STATUS_CONNECTED;
			player_send_certificate();
			push_event(EVENT_PEER_CONNECTED, host_id);
		} break;
		case CONTROL_PAIR_OPEN: {
			// A pair is introduced once, and a player never has more pairs than the mesh has other players.
			const uint32_t token = reader.get_u32();
			if (!reader.failed && peer > 0 && peer != host_id && peer != local_id && !pairs.has(peer) && int(pairs.size()) < host_max_players - 2) {
				player_open_pair(peer, token);
			}
		} break;
		case CONTROL_PAIR_PUNCH: {
			const uint32_t token = reader.get_u32();
			const int port = int(reader.get_u32());
			const String address = reader.get_string();
			const String certificate = reader.get_string();
			Pair *pair = pairs.getptr(peer);
			if (reader.failed || pair == nullptr || pair->state != PAIR_REGISTERING || port <= 0 || port > 65535) {
				return;
			}
			pair->has_endpoint = true;
			pair->address = address;
			pair->port = port;
			pair->connect_token = token;
			pair->peer_certificate = certificate;
			if (pair->registration.is_null()) {
				player_endpoint_ready(peer, *pair);
			}
		} break;
		case CONTROL_PAIR_RELAY: {
			player_set_relayed(peer);
		} break;
		case CONTROL_SUCCESSION: {
			// `peer` is the count here.
			LocalVector<int> ids;
			for (int i = 0; i < peer && i < 1024 && !reader.failed; i++) {
				ids.push_back(int(reader.get_u32()));
			}
			if (!reader.failed) {
				succession = ids;
			}
		} break;
		case CONTROL_MEMBER_LEFT: {
			Pair *pair = pairs.getptr(peer);
			if (pair) {
				player_close_pair(*pair);
				player_report_disconnected(peer, *pair);
				pairs.erase(peer);
			}
		} break;
		case CONTROL_HOST_QUERY: {
			// `peer` is the host asked about: the one this player followed is confirming it's gone.
			player_answer_host_query(host_link, peer);
		} break;
		case CONTROL_HOST_STATUS: {
			const bool alive = reader.get_u32() != 0;
			if (!reader.failed && confirming && peer == confirm_old_host && confirm_asked.has(host_id)) {
				confirm_answers[host_id] = alive;
			}
		} break;
		default:
			break;
	}
}

void EnetHostedMeshTransport::player_on_pair_control(int p_peer, Pair &r_pair, const uint8_t *p_data, int p_size) {
	HostedMeshReader reader(p_data, p_size);
	const uint8_t type = reader.get_u8();
	if (type == CONTROL_PAIR_READY) {
		// The accepting side confirmed the link: from now on it times out like the others.
		if (local_id > p_peer && r_pair.state == PAIR_PUNCHING) {
			r_pair.state = PAIR_DIRECT;
			apply_link_timeout(r_pair.link);
			player_report_connected(p_peer, r_pair);
		}
		return;
	}
	if (r_pair.state != PAIR_DIRECT) {
		return;
	}
	const int value = int(reader.get_u32());
	if (reader.failed) {
		return;
	}
	if (type == CONTROL_HOST_QUERY) {
		player_answer_host_query(r_pair.link, value);
	} else if (type == CONTROL_HOST_STATUS) {
		const bool alive = reader.get_u32() != 0;
		if (!reader.failed && confirming && value == confirm_old_host && confirm_asked.has(p_peer)) {
			confirm_answers[p_peer] = alive;
		}
	} else if (type == CONTROL_REJOIN) {
		// The other player chose this one as the new host before it noticed the old one was gone: kept until it does
		// (then it's handled as a host would, see `player_become_host()`).
		LocalVector<int> direct;
		for (int i = 0; i < value && i < 1024 && !reader.failed; i++) {
			direct.push_back(int(reader.get_u32()));
		}
		if (!reader.failed) {
			pending_rejoins[p_peer] = direct;
		}
	}
}

void EnetHostedMeshTransport::player_send_certificate() {
	if (!encrypted || certificate_sent || status != STATUS_CONNECTED || player_certificate.is_null()) {
		return;
	}
	certificate_sent = true;
	HostedMeshWriter certificate(CONTROL_CERTIFICATE);
	certificate.put_string(player_certificate->save_to_string());
	send_control(host_link, certificate.bytes);
}

void EnetHostedMeshTransport::player_finish_key_job() {
	if (key_job == nullptr || !WorkerThreadPool::get_singleton()->is_task_completed(key_task)) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(key_task);
	player_key = key_job->key;
	player_certificate = key_job->certificate;
	memdelete(key_job);
	key_job = nullptr;
	ERR_FAIL_COND_MSG(player_certificate.is_null(), "Can't generate this player's certificate: the pairs where it would accept the direct link are relayed instead.");
	player_send_certificate();
}

void EnetHostedMeshTransport::player_open_pair(int p_peer, uint32_t p_registration_token) {
	Pair *existing = pairs.getptr(p_peer);
	if (existing) {
		player_close_pair(*existing);
		player_report_disconnected(p_peer, *existing);
		pairs.erase(p_peer);
	}
	pairs.insert(p_peer, Pair());
	Pair &pair = pairs[p_peer];
	if (!direct_connections) {
		player_fail_pair(p_peer, pair);
		return;
	}
	Ref<ENetConnection> socket;
	socket.instantiate();
	// Two peers: the registration with the host, then the other player.
	if (socket->create_host_bound(IPAddress("*"), 0, 2, ENET_CHANNEL_COUNT, 0, 0) != OK) {
		player_fail_pair(p_peer, pair);
		return;
	}
	socket->compress(ENetConnection::CompressionMode(compression));
	// With DTLS, the registration goes to the host's plain rendezvous port; the socket switches to DTLS afterwards.
	const int registration_port = host_rendezvous_port > 0 ? host_rendezvous_port : host_port;
	Ref<ENetPacketPeer> registration = socket->connect_to_host(host_address, registration_port, ENET_CHANNEL_COUNT, int(p_registration_token));
	if (registration.is_null()) {
		socket->destroy();
		player_fail_pair(p_peer, pair);
		return;
	}
	pair.socket = socket;
	pair.registration = registration;
	pair.state = PAIR_REGISTERING;
}

void EnetHostedMeshTransport::player_endpoint_ready(int p_peer, Pair &r_pair) {
	if (!encrypted) {
		player_start_punching(p_peer, r_pair);
		return;
	}
	r_pair.punch_at_usec = OS::get_singleton()->get_ticks_usec() + DTLS_SETTLE_USEC;
}

void EnetHostedMeshTransport::player_start_punching(int p_peer, Pair &r_pair) {
	r_pair.punch_at_usec = 0;
	r_pair.state = PAIR_PUNCHING;
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	r_pair.deadline_usec = now + uint64_t(punch_timeout * 1000000.0);
	r_pair.next_punch_usec = now;
	ENetConnection *socket = as_socket(r_pair.socket);
	if (socket == nullptr) {
		player_fail_pair(p_peer, r_pair);
		return;
	}
	if (local_id < p_peer) {
		// The lower id accepts and decides.
		if (encrypted) {
			// A DTLS socket only sends DTLS: this side punches its NAT first. The socket keeps its port, so the NAT
			// mapping stays.
			PackedByteArray punch;
			punch.push_back(0);
			for (int i = 0; i < DTLS_PUNCHES; i++) {
				socket->socket_send(r_pair.address, r_pair.port, punch);
			}
			if (socket->dtls_server_setup(TLSOptions::server(player_key, player_certificate)) != OK) {
				player_fail_pair(p_peer, r_pair);
			}
		}
		return;
	}
	if (encrypted) {
		// The higher id waits for the other side's punches before switching to DTLS: a DTLS socket reports any other
		// datagram as a handshake error.
		r_pair.connect_at_usec = now + DTLS_CONNECT_DELAY_USEC;
		r_pair.deadline_usec += DTLS_CONNECT_DELAY_USEC;
		return;
	}
	player_connect_pair(p_peer, r_pair);
}

void EnetHostedMeshTransport::player_connect_pair(int p_peer, Pair &r_pair) {
	r_pair.connect_at_usec = 0;
	ENetConnection *socket = as_socket(r_pair.socket);
	if (socket == nullptr) {
		player_fail_pair(p_peer, r_pair);
		return;
	}
	if (encrypted) {
		// The accepting player's certificate, handed over by the host, is the only one trusted.
		Ref<X509Certificate> certificate = Ref<X509Certificate>(X509Certificate::create());
		if (certificate.is_null() || certificate->load_from_string(r_pair.peer_certificate) != OK || socket->dtls_client_setup(PLAYER_COMMON_NAME, TLSOptions::client(certificate, PLAYER_COMMON_NAME)) != OK) {
			player_fail_pair(p_peer, r_pair);
			return;
		}
	}
	Ref<ENetPacketPeer> link = socket->connect_to_host(r_pair.address, r_pair.port, ENET_CHANNEL_COUNT, int(r_pair.connect_token));
	if (link.is_null()) {
		player_fail_pair(p_peer, r_pair);
		return;
	}
	const int timeout_ms = int(punch_timeout * 1000.0);
	link->set_timeout(32, timeout_ms, timeout_ms);
	r_pair.link = link;
}

void EnetHostedMeshTransport::player_service_pair(int p_peer, Pair &r_pair) {
	ENetConnection *socket = as_socket(r_pair.socket);
	const bool accepts = local_id < p_peer;
	for (int i = 0; socket && i < 4096; i++) {
		ENetConnection::Event event;
		const ENetConnection::EventType type = socket->service(0, event);
		if (type == ENetConnection::EVENT_NONE || type == ENetConnection::EVENT_ERROR) {
			break;
		}
		const bool is_registration = r_pair.registration.is_valid() && event.peer == r_pair.registration;
		const bool is_link = r_pair.link.is_valid() && event.peer == r_pair.link;
		if (type == ENetConnection::EVENT_CONNECT) {
			if (is_registration || is_link) {
				// The connector waits for the acceptor's confirmation.
				continue;
			}
			// Incoming: only the introduced player, with the pair's token, before the deadline.
			const bool expected = accepts && r_pair.state == PAIR_PUNCHING && r_pair.link.is_null() && event.data == r_pair.connect_token && event.peer->get_remote_address() == IPAddress(r_pair.address) && OS::get_singleton()->get_ticks_usec() < r_pair.deadline_usec;
			if (!expected) {
				rejected_connections++;
				event.peer->peer_disconnect_now();
				continue;
			}
			r_pair.link = event.peer;
			r_pair.state = PAIR_DIRECT;
			apply_link_timeout(r_pair.link);
			HostedMeshWriter ready(CONTROL_PAIR_READY);
			ready.put_u32(uint32_t(local_id));
			send_control(r_pair.link, ready.bytes);
			player_report_connected(p_peer, r_pair);
		} else if (type == ENetConnection::EVENT_DISCONNECT) {
			if (is_registration) {
				r_pair.registration.unref();
				if (r_pair.state == PAIR_REGISTERING && r_pair.has_endpoint) {
					player_endpoint_ready(p_peer, r_pair);
					return;
				}
			} else if (is_link) {
				r_pair.link.unref();
				if (r_pair.state == PAIR_DIRECT) {
					// A direct link that drops is a disconnection; the pair comes back relayed.
					player_report_disconnected(p_peer, r_pair);
					player_fail_pair(p_peer, r_pair);
					return;
				}
				if (r_pair.state == PAIR_PUNCHING && !accepts && OS::get_singleton()->get_ticks_usec() < r_pair.deadline_usec) {
					// The attempt failed early: try again until the deadline.
					Ref<ENetPacketPeer> link = socket->connect_to_host(r_pair.address, r_pair.port, ENET_CHANNEL_COUNT, int(r_pair.connect_token));
					if (link.is_valid()) {
						const int timeout_ms = int(punch_timeout * 1000.0);
						link->set_timeout(32, timeout_ms, timeout_ms);
						r_pair.link = link;
					}
				}
			}
		} else if (type == ENetConnection::EVENT_RECEIVE) {
			if (is_link) {
				if (event.channel_id == CHANNEL_CONTROL) {
					player_on_pair_control(p_peer, r_pair, event.packet->data, int(event.packet->dataLength));
				} else if (r_pair.state == PAIR_DIRECT && event.channel_id < FIRST_RELAY_CHANNEL) {
					deliver(p_peer, event.channel_id - FIRST_DIRECT_CHANNEL, event.packet->flags, event.packet->data, int(event.packet->dataLength));
				}
			}
			enet_packet_destroy(event.packet);
		}
	}

	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (r_pair.state == PAIR_REGISTERING && r_pair.punch_at_usec != 0 && now >= r_pair.punch_at_usec) {
		player_start_punching(p_peer, r_pair);
		return;
	}
	if (r_pair.state != PAIR_PUNCHING) {
		return;
	}
	if (accepts) {
		if (now >= r_pair.deadline_usec) {
			player_fail_pair(p_peer, r_pair);
			return;
		}
		if (now >= r_pair.next_punch_usec && socket && !encrypted) {
			// Opens this side's NAT for the other player's packets; ENet ignores a one byte datagram.
			PackedByteArray punch;
			punch.push_back(0);
			socket->socket_send(r_pair.address, r_pair.port, punch);
			r_pair.next_punch_usec = now + PUNCH_INTERVAL_USEC;
		}
	} else if (r_pair.connect_at_usec != 0 && now >= r_pair.connect_at_usec) {
		player_connect_pair(p_peer, r_pair);
	} else if (now >= r_pair.deadline_usec + CONNECTOR_GRACE_USEC) {
		player_fail_pair(p_peer, r_pair);
	}
}

void EnetHostedMeshTransport::player_fail_pair(int p_peer, Pair &r_pair) {
	player_close_pair(r_pair);
	r_pair.state = PAIR_FAILED;
	failed_punches += direct_connections ? 1 : 0;
	HostedMeshWriter failed(CONTROL_PAIR_FAILED);
	failed.put_u32(uint32_t(p_peer));
	send_control(host_link, failed.bytes);
}

void EnetHostedMeshTransport::player_set_relayed(int p_peer) {
	if (p_peer <= 0 || p_peer == host_id || p_peer == local_id || (!pairs.has(p_peer) && int(pairs.size()) >= host_max_players - 2)) {
		return;
	}
	if (!pairs.has(p_peer)) {
		pairs.insert(p_peer, Pair());
	}
	Pair &pair = pairs[p_peer];
	if (pair.state == PAIR_RELAYED) {
		return;
	}
	if (pair.state == PAIR_DIRECT) {
		// The other side gave up on the direct link: this one follows, as a new connection.
		player_report_disconnected(p_peer, pair);
	}
	player_close_pair(pair);
	pair.state = PAIR_RELAYED;
	player_report_connected(p_peer, pair);
}

void EnetHostedMeshTransport::player_close_pair(Pair &r_pair) {
	ENetConnection *socket = as_socket(r_pair.socket);
	ENetPacketPeer *link = as_link(r_pair.link);
	if (link && link->is_active()) {
		// Tells the other player right away, instead of letting its link time out.
		link->peer_disconnect_now();
	}
	if (socket) {
		socket->flush();
		socket->destroy();
	}
	r_pair.socket.unref();
	r_pair.registration.unref();
	r_pair.link.unref();
}

void EnetHostedMeshTransport::player_report_connected(int p_peer, Pair &r_pair) {
	if (!r_pair.reported) {
		r_pair.reported = true;
		push_event(EVENT_PEER_CONNECTED, p_peer);
	}
}

void EnetHostedMeshTransport::player_report_disconnected(int p_peer, Pair &r_pair) {
	if (r_pair.reported) {
		r_pair.reported = false;
		push_event(EVENT_PEER_DISCONNECTED, p_peer);
	}
}

void EnetHostedMeshTransport::player_on_host_disconnect(int p_reason) {
	// A player leaving, refused or removed by the host, or whose host ended the mesh, doesn't take the host's place.
	if (leaving) {
		player_lost_host(DISCONNECT_REASON_CLOSED);
		return;
	}
	if (p_reason == DISCONNECT_REMOVED) {
		player_lost_host(DISCONNECT_REASON_REFUSED);
		return;
	}
	if (p_reason == DISCONNECT_FULL) {
		player_lost_host(DISCONNECT_REASON_FULL);
		return;
	}
	if (p_reason == DISCONNECT_BUSY) {
		player_lost_host(DISCONNECT_REASON_BUSY);
		return;
	}
	if ((p_reason & 0xFFFFFF00) == DISCONNECT_VERSION_PREFIX) {
		ERR_PRINT(vformat("The mesh host uses version %d of the hosted mesh protocol, and this node version %d: they can't connect.", (p_reason & 0xFF) - '0', MESH_PROTOCOL_VERSION));
		player_lost_host(DISCONNECT_REASON_VERSION);
		return;
	}
	if (p_reason == DISCONNECT_ENDED) {
		player_lost_host(DISCONNECT_REASON_ENDED);
		return;
	}
	if (!host_migration || status != STATUS_CONNECTED) {
		player_lost_host(DISCONNECT_REASON_LOST);
		return;
	}
	if (p_reason == DISCONNECT_HANDOVER) {
		// The host left on purpose: the successor takes over right away.
		if (!player_migrate(host_id)) {
			player_lost_host(DISCONNECT_REASON_LOST);
		}
		return;
	}
	// A crash, or a network failure: maybe only this player lost the host.
	player_confirm_host_loss();
}

bool EnetHostedMeshTransport::player_sees_host(int p_host) const {
	if (is_host || status != STATUS_CONNECTED || confirming || host_id != p_host) {
		return false;
	}
	const uint64_t window = MAX(HOST_ALIVE_MIN_USEC, uint64_t(host_timeout * 500000.0));
	return OS::get_singleton()->get_ticks_usec() - host_heard_usec < window;
}

void EnetHostedMeshTransport::player_answer_host_query(const Ref<RefCounted> &p_link, int p_host) {
	HostedMeshWriter answer(CONTROL_HOST_STATUS);
	answer.put_u32(uint32_t(p_host));
	answer.put_u32(player_sees_host(p_host) ? 1 : 0);
	send_control(p_link, answer.bytes);
}

void EnetHostedMeshTransport::player_confirm_host_loss() {
	// The host is really gone only if the players this one reaches directly lost it too; if one still hears from it,
	// this player lost it alone (its own connection, or the path to the host), and it leaves instead of taking over.
	confirm_asked.clear();
	confirm_answers.clear();
	HostedMeshWriter query(CONTROL_HOST_QUERY);
	query.put_u32(uint32_t(host_id));
	for (const KeyValue<int, Pair> &E : pairs) {
		if (E.value.reported && E.value.state == PAIR_DIRECT && send_on_link(E.value.link, CHANNEL_CONTROL, ENET_PACKET_FLAG_RELIABLE, query.bytes.ptr(), int(query.bytes.size())) == OK) {
			confirm_asked.push_back(E.key);
		}
	}
	if (confirm_asked.is_empty()) {
		player_lost_host(DISCONNECT_REASON_LOST);
		return;
	}
	confirming = true;
	confirm_old_host = host_id;
	confirm_deadline_usec = OS::get_singleton()->get_ticks_usec() + CONFIRM_WINDOW_USEC;
}

void EnetHostedMeshTransport::player_check_confirmation() {
	bool answered = true;
	int lost = 0;
	for (const int peer : confirm_asked) {
		const bool *alive = confirm_answers.getptr(peer);
		if (alive == nullptr) {
			answered = false;
		} else if (*alive) {
			// Another player still hears from the host.
			player_lost_host(DISCONNECT_REASON_LOST);
			return;
		} else {
			lost++;
		}
	}
	if (!answered && OS::get_singleton()->get_ticks_usec() < confirm_deadline_usec) {
		return;
	}
	confirming = false;
	// Nobody answered: this player is the one cut off.
	if (lost == 0 || !player_migrate(confirm_old_host)) {
		player_lost_host(DISCONNECT_REASON_LOST);
	}
}

void EnetHostedMeshTransport::player_lost_host(DisconnectReason p_reason) {
	const bool was_connected = status == STATUS_CONNECTED;
	disconnect_reason = p_reason;
	confirming = false;
	pending_rejoins.clear();
	for (KeyValue<int, Pair> &E : pairs) {
		player_close_pair(E.value);
		player_report_disconnected(E.key, E.value);
	}
	pairs.clear();
	host_link.unref();
	ENetConnection *socket = as_socket(host_socket);
	if (socket) {
		socket->destroy();
	}
	host_socket.unref();
	status = STATUS_DISCONNECTED;
	if (was_connected) {
		push_event(EVENT_PEER_DISCONNECTED, host_id);
	}
}

bool EnetHostedMeshTransport::player_migrate(int p_old_host) {
	// The first player of the succession still in the mesh. Everyone has the same list, so everyone picks the same
	// one; a player that can't reach it directly leaves (following another one would split the mesh).
	int successor = 0;
	for (const int id : succession) {
		const Pair *pair = pairs.getptr(id);
		if (id == local_id || (pair && pair->reported)) {
			successor = id;
			break;
		}
	}
	if (successor == 0 || (successor != local_id && pairs[successor].state != PAIR_DIRECT)) {
		return false;
	}
	ENetConnection *old_socket = as_socket(host_socket);
	if (old_socket) {
		old_socket->destroy();
	}
	host_socket.unref();
	host_link.unref();
	if (successor == local_id) {
		player_become_host(p_old_host);
	} else {
		player_follow_host(p_old_host, successor);
	}
	return true;
}

void EnetHostedMeshTransport::player_become_host(int p_old_host) {
	// The direct links become the members' links; the players reached only through the old host are lost.
	is_host = true;
	host_id = local_id;
	for (KeyValue<int, Pair> &E : pairs) {
		Pair &pair = E.value;
		if (pair.reported && pair.state == PAIR_DIRECT) {
			members.insert(E.key, pair.link);
			members_by_link.insert(pair.link->get_instance_id(), E.key);
			member_sockets.insert(E.key, pair.socket);
		} else {
			player_close_pair(pair);
			player_report_disconnected(E.key, pair);
		}
	}
	pairs.clear();
	next_player_id = local_id + 1;
	for (const KeyValue<int, Ref<RefCounted>> &E : members) {
		next_player_id = MAX(next_player_id, E.key + 1);
	}
	next_heartbeat_usec = 0;
	push_event(EVENT_HOST_MIGRATED, local_id);
	push_event(EVENT_PEER_DISCONNECTED, p_old_host);
	host_send_succession();
	// The players that followed this one before it noticed the old host was gone.
	for (const KeyValue<int, LocalVector<int>> &E : pending_rejoins) {
		if (members.has(E.key)) {
			host_handle_rejoin(E.key, E.value);
		}
	}
	pending_rejoins.clear();
}

void EnetHostedMeshTransport::player_follow_host(int p_old_host, int p_new_host) {
	// The direct link with the successor becomes the host link; the engines keep it connected.
	Pair &pair = pairs[p_new_host];
	host_socket = pair.socket;
	host_link = pair.link;
	host_id = p_new_host;
	pairs.erase(p_new_host);
	apply_link_timeout(host_link);
	host_heard_usec = OS::get_singleton()->get_ticks_usec();
	pending_rejoins.clear();
	// The pairs that weren't direct went through the old host: the new host relays them again once both players
	// followed it (a player it lost stays gone).
	LocalVector<int> dropped;
	for (KeyValue<int, Pair> &E : pairs) {
		if (!(E.value.reported && E.value.state == PAIR_DIRECT)) {
			player_close_pair(E.value);
			player_report_disconnected(E.key, E.value);
			dropped.push_back(E.key);
		}
	}
	for (const int id : dropped) {
		pairs.erase(id);
	}
	HostedMeshWriter rejoin(CONTROL_REJOIN);
	LocalVector<int> direct;
	for (const KeyValue<int, Pair> &E : pairs) {
		if (E.value.reported && E.value.state == PAIR_DIRECT) {
			direct.push_back(E.key);
		}
	}
	rejoin.put_u32(direct.size());
	for (const int id : direct) {
		rejoin.put_u32(uint32_t(id));
	}
	send_control(host_link, rejoin.bytes);
	push_event(EVENT_HOST_MIGRATED, p_new_host);
	push_event(EVENT_PEER_DISCONNECTED, p_old_host);
}

void EnetHostedMeshTransport::apply_link_timeout(const Ref<RefCounted> &p_link) {
	ENetPacketPeer *link = as_link(p_link);
	if (link) {
		const int timeout_ms = int(host_timeout * 1000.0);
		link->set_timeout(32, timeout_ms, timeout_ms);
	}
}

void EnetHostedMeshTransport::set_host_timeout(double p_seconds) {
	ERR_FAIL_COND_MSG(!(p_seconds > 0.0), "The host timeout must be positive.");
	host_timeout = p_seconds;
	apply_link_timeout(host_link);
	for (const KeyValue<int, Ref<RefCounted>> &E : members) {
		apply_link_timeout(E.value);
	}
	for (const KeyValue<int, Pair> &E : pairs) {
		if (E.value.state == PAIR_DIRECT) {
			apply_link_timeout(E.value.link);
		}
	}
}

void EnetHostedMeshTransport::set_join_timeout(double p_seconds) {
	ERR_FAIL_COND_MSG(!(p_seconds > 0.0), "The join timeout must be positive.");
	join_timeout = p_seconds;
}

PackedInt32Array EnetHostedMeshTransport::get_succession() const {
	PackedInt32Array result;
	for (const int id : succession) {
		result.push_back(id);
	}
	return result;
}

// ------------------------------------------------------------------------------------------------------ Multiplayer

Ref<TickMultiplayerPeer> EnetHostedMeshTransport::get_multiplayer_peer() {
	if (multiplayer_peer) {
		return Ref<TickMultiplayerPeer>(multiplayer_peer);
	}
	Ref<TickMultiplayerPeer> peer;
	peer.instantiate();
	peer->transport = Ref<EnetHostedMeshTransport>(this);
	attach_multiplayer_peer(peer.ptr());
	return peer;
}

void EnetHostedMeshTransport::attach_multiplayer_peer(TickMultiplayerPeer *p_peer) {
	multiplayer_peer = p_peer;
	multiplayer_events.clear();
	multiplayer_packets.clear();
	next_multiplayer_packet = 0;
	multiplayer_queued_bytes = 0;
	if (p_peer == nullptr) {
		return;
	}
	p_peer->host_peer = host_id;
	// The peers connected before the multiplayer peer existed.
	LocalVector<int> connected;
	get_connected_peers(connected);
	for (const int peer : connected) {
		Event event;
		event.type = EVENT_PEER_CONNECTED;
		event.peer = peer;
		multiplayer_events.push_back(event);
	}
}

Error EnetHostedMeshTransport::send_multiplayer(int p_target, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) {
	ERR_FAIL_INDEX_V_MSG(p_channel, MULTIPLAYER_CHANNEL_COUNT, ERR_INVALID_PARAMETER, vformat("The transfer channel must be between 0 and %d.", MULTIPLAYER_CHANNEL_COUNT - 1));
	const int logical = TICK_CHANNEL_COUNT + p_channel * 2 + (p_mode == TRANSFER_MODE_RELIABLE ? 0 : 1);
	const int flags = flags_for_mode(p_mode);
	if (p_target > 0) {
		return send_logical(p_target, logical, flags, p_data, p_size);
	}
	// 0: everyone; negative: everyone but `-p_target`.
	LocalVector<int> connected;
	get_connected_peers(connected);
	for (const int peer : connected) {
		if (peer != -p_target) {
			send_logical(peer, logical, flags, p_data, p_size);
		}
	}
	return OK;
}

void EnetHostedMeshTransport::_bind_methods() {
	ClassDB::bind_static_method("EnetHostedMeshTransport", D_METHOD("create_host", "port", "max_players", "bind_address", "compression", "tls_options", "rendezvous_port"), &EnetHostedMeshTransport::create_host, DEFVAL(32), DEFVAL("*"), DEFVAL(COMPRESSION_RANGE_CODER), DEFVAL(Ref<TLSOptions>()), DEFVAL(0));
	ClassDB::bind_static_method("EnetHostedMeshTransport", D_METHOD("create_player", "address", "port", "compression", "tls_options", "tls_hostname", "join_data"), &EnetHostedMeshTransport::create_player, DEFVAL(COMPRESSION_RANGE_CODER), DEFVAL(Ref<TLSOptions>()), DEFVAL(String()), DEFVAL(PackedByteArray()));
	ClassDB::bind_method(D_METHOD("is_encrypted"), &EnetHostedMeshTransport::is_encrypted);
	ClassDB::bind_method(D_METHOD("get_host_peer"), &EnetHostedMeshTransport::get_host_peer);
	ClassDB::bind_method(D_METHOD("get_succession"), &EnetHostedMeshTransport::get_succession);
	ClassDB::bind_method(D_METHOD("set_host_migration", "enabled"), &EnetHostedMeshTransport::set_host_migration);
	ClassDB::bind_method(D_METHOD("is_host_migration_enabled"), &EnetHostedMeshTransport::is_host_migration_enabled);
	ClassDB::bind_method(D_METHOD("set_host_timeout", "seconds"), &EnetHostedMeshTransport::set_host_timeout);
	ClassDB::bind_method(D_METHOD("get_host_timeout"), &EnetHostedMeshTransport::get_host_timeout);
	ClassDB::bind_method(D_METHOD("get_status"), &EnetHostedMeshTransport::get_status);
	ClassDB::bind_method(D_METHOD("get_disconnect_reason"), &EnetHostedMeshTransport::get_disconnect_reason);
	ClassDB::bind_method(D_METHOD("is_hosting"), &EnetHostedMeshTransport::is_hosting);
	ClassDB::bind_method(D_METHOD("get_peer_path", "peer"), &EnetHostedMeshTransport::get_peer_path);
	ClassDB::bind_method(D_METHOD("get_peers"), &EnetHostedMeshTransport::get_peers);
	ClassDB::bind_method(D_METHOD("get_stats"), &EnetHostedMeshTransport::get_stats);
	ClassDB::bind_method(D_METHOD("set_punch_timeout", "seconds"), &EnetHostedMeshTransport::set_punch_timeout);
	ClassDB::bind_method(D_METHOD("get_punch_timeout"), &EnetHostedMeshTransport::get_punch_timeout);
	ClassDB::bind_method(D_METHOD("set_direct_connections", "enabled"), &EnetHostedMeshTransport::set_direct_connections);
	ClassDB::bind_method(D_METHOD("is_direct_connections_enabled"), &EnetHostedMeshTransport::is_direct_connections_enabled);
	ClassDB::bind_method(D_METHOD("set_join_validator", "validator"), &EnetHostedMeshTransport::set_join_validator);
	ClassDB::bind_method(D_METHOD("get_join_validator"), &EnetHostedMeshTransport::get_join_validator);
	ClassDB::bind_method(D_METHOD("set_join_timeout", "seconds"), &EnetHostedMeshTransport::set_join_timeout);
	ClassDB::bind_method(D_METHOD("get_join_timeout"), &EnetHostedMeshTransport::get_join_timeout);
	ClassDB::bind_method(D_METHOD("admit_player", "peer"), &EnetHostedMeshTransport::admit_player);
	ClassDB::bind_method(D_METHOD("refuse_player", "peer"), &EnetHostedMeshTransport::refuse_player);
	ClassDB::bind_method(D_METHOD("hand_over"), &EnetHostedMeshTransport::hand_over);
	ClassDB::bind_method(D_METHOD("get_multiplayer_peer"), &EnetHostedMeshTransport::get_multiplayer_peer);
	ClassDB::bind_method(D_METHOD("close"), &EnetHostedMeshTransport::close);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "punch_timeout", PROPERTY_HINT_RANGE, "0.1,30,0.1,suffix:s"), "set_punch_timeout", "get_punch_timeout");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "direct_connections"), "set_direct_connections", "is_direct_connections_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "host_migration"), "set_host_migration", "is_host_migration_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "host_timeout", PROPERTY_HINT_RANGE, "0.1,60,0.1,suffix:s"), "set_host_timeout", "get_host_timeout");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "join_validator"), "set_join_validator", "get_join_validator");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "join_timeout", PROPERTY_HINT_RANGE, "0.1,120,0.1,suffix:s"), "set_join_timeout", "get_join_timeout");

	BIND_ENUM_CONSTANT(COMPRESSION_NONE);
	BIND_ENUM_CONSTANT(COMPRESSION_RANGE_CODER);
	BIND_ENUM_CONSTANT(COMPRESSION_FASTLZ);
	BIND_ENUM_CONSTANT(COMPRESSION_ZLIB);
	BIND_ENUM_CONSTANT(COMPRESSION_ZSTD);
	BIND_ENUM_CONSTANT(STATUS_DISCONNECTED);
	BIND_ENUM_CONSTANT(STATUS_CONNECTING);
	BIND_ENUM_CONSTANT(STATUS_CONNECTED);
	BIND_ENUM_CONSTANT(DISCONNECT_REASON_NONE);
	BIND_ENUM_CONSTANT(DISCONNECT_REASON_CLOSED);
	BIND_ENUM_CONSTANT(DISCONNECT_REASON_LOST);
	BIND_ENUM_CONSTANT(DISCONNECT_REASON_REFUSED);
	BIND_ENUM_CONSTANT(DISCONNECT_REASON_FULL);
	BIND_ENUM_CONSTANT(DISCONNECT_REASON_BUSY);
	BIND_ENUM_CONSTANT(DISCONNECT_REASON_VERSION);
	BIND_ENUM_CONSTANT(DISCONNECT_REASON_ENDED);
	BIND_ENUM_CONSTANT(PATH_NONE);
	BIND_ENUM_CONSTANT(PATH_HOST);
	BIND_ENUM_CONSTANT(PATH_CONNECTING);
	BIND_ENUM_CONSTANT(PATH_DIRECT);
	BIND_ENUM_CONSTANT(PATH_RELAYED);
	BIND_CONSTANT(MULTIPLAYER_CHANNEL_COUNT);
}

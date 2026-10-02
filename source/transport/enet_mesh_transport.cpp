#include "enet_mesh_transport.h"

#include "../sync/tick_protocol.h"
#include "enet_link.h"

#include "core/crypto/crypto_core.h"
#include "core/io/ip.h"
#include "core/io/marshalls.h"
#include "core/os/os.h"

#include "modules/enet/enet_connection.h"
#include "modules/enet/enet_packet_peer.h"

// Implementation of `EnetMeshTransport`: the sockets of a mesh between servers, who is accepted as a node (the id it
// declares, the address it comes from, and the proof of the mesh's secret), and the queues of events and packets the
// engines consume.

static_assert(int(EnetMeshTransport::COMPRESSION_ZSTD) == int(ENetConnection::COMPRESS_ZSTD), "The compression modes must match ENet's.");

// ENet's default MTU minus the ENet headers (protocol and send command).
static constexpr int ENET_MESH_MAX_PAYLOAD = 1400 - 6 - 12;
// A link has this long to prove the mesh's secret.
static constexpr uint64_t PROOF_TIMEOUT_USEC = 3000000;
// Most links proving the secret at once for the same node id; a new one replaces the oldest.
static constexpr int MAX_PROVING_PER_NODE = 4;
// The proof messages, on the control channel: a magic, a type, and the challenge or the proof.
static constexpr uint8_t PROOF_MAGIC[4] = { 'T', 'K', 'M', 'A' };
static constexpr uint8_t PROOF_CHALLENGE = 1;
static constexpr uint8_t PROOF_ANSWER = 2;
static constexpr int PROOF_HEADER_SIZE = 5;
// What a proof signs besides the challenge and the ids: a proof is good for nothing else.
static const char *PROOF_LABEL = "tick-mesh-proof-1";


// The `ENetConnection` behind a socket kept as `RefCounted`.
static ENetConnection *as_host(const Ref<RefCounted> &p_host) {
	return Object::cast_to<ENetConnection>(p_host.ptr());
}


// The `ENetPacketPeer` behind a link kept as `RefCounted`.
static ENetPacketPeer *as_peer(const Ref<RefCounted> &p_peer) {
	return Object::cast_to<ENetPacketPeer>(p_peer.ptr());
}


// The addresses a node's address stands for: itself, or the ones its name resolves to (it may block on the lookup).
static void resolve_node_addresses(const String &p_address, LocalVector<IPAddress> &r_addresses) {
	r_addresses.clear();
	if (p_address.is_valid_ip_address()) {
		r_addresses.push_back(IPAddress(p_address));
		return;
	}
	const PackedStringArray resolved = IP::get_singleton()->resolve_hostname_addresses(p_address, IP::TYPE_ANY);
	for (const String &address : resolved) {
		if (address.is_valid_ip_address()) {
			r_addresses.push_back(IPAddress(address));
		}
	}
}


// HMAC-SHA256 of a message (RFC 2104), with the engine's SHA-256.
static void hmac_sha256(const uint8_t *p_key, int p_key_size, const uint8_t *p_message, int p_message_size, uint8_t *r_mac) {
	uint8_t block[64] = {};
	if (p_key_size > 64) {
		CryptoCore::sha256(p_key, p_key_size, block);
	} else if (p_key_size > 0) {
		memcpy(block, p_key, p_key_size);
	}
	uint8_t inner_pad[64];
	uint8_t outer_pad[64];
	for (int i = 0; i < 64; i++) {
		inner_pad[i] = block[i] ^ 0x36;
		outer_pad[i] = block[i] ^ 0x5C;
	}
	uint8_t inner[32];
	CryptoCore::SHA256Context inner_hash;
	inner_hash.start();
	inner_hash.update(inner_pad, 64);
	inner_hash.update(p_message, p_message_size);
	inner_hash.finish(inner);
	CryptoCore::SHA256Context outer_hash;
	outer_hash.start();
	outer_hash.update(outer_pad, 64);
	outer_hash.update(inner, 32);
	outer_hash.finish(r_mac);
}


// Creates the transport of node `p_local_id`, listening on `p_port`.
Ref<EnetMeshTransport> EnetMeshTransport::create(int p_local_id, int p_port, const String &p_bind_address, Compression p_compression, int p_max_nodes) {
	ERR_FAIL_COND_V_MSG(p_local_id <= 0, Ref<EnetMeshTransport>(), "The node id must be positive.");
	Ref<ENetConnection> connection;
	connection.instantiate();
	// Bandwidth limits stay at 0 (unlimited): see ADR-030.
	const Error err = connection->create_host_bound(IPAddress(p_bind_address), p_port, p_max_nodes, TICK_CHANNEL_COUNT, 0, 0);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<EnetMeshTransport>(), vformat("Can't bind the mesh node %d on port %d.", p_local_id, p_port));
	connection->compress(ENetConnection::CompressionMode(p_compression));

	Ref<EnetMeshTransport> transport;
	transport.instantiate();
	transport->host = connection;
	transport->local_id = p_local_id;
	transport->compression = p_compression;
	return transport;
}


// Closes every link and socket.
EnetMeshTransport::~EnetMeshTransport() {
	for (KeyValue<int, MeshNode> &E : nodes) {
		close_node(E.value);
	}
	proving.clear();
	ENetConnection *connection = as_host(host);
	if (connection) {
		connection->destroy();
	}
}


// Closes the link with a node and its outgoing socket, without reporting anything.
void EnetMeshTransport::close_node(MeshNode &r_node) {
	ENetPacketPeer *peer = as_peer(r_node.peer);
	if (peer && peer->is_active()) {
		if (r_node.outgoing_host.is_valid()) {
			// Its own socket, destroyed below.
			peer->peer_disconnect_now();
		} else {
			// A link of the listening socket, which goes on being serviced.
			enet_close_link(peer);
		}
	}
	if (r_node.peer.is_valid()) {
		ids_by_peer.erase(r_node.peer->get_instance_id());
		proving.erase(r_node.peer->get_instance_id());
	}
	r_node.peer.unref();
	ENetConnection *outgoing = as_host(r_node.outgoing_host);
	if (outgoing) {
		outgoing->destroy();
	}
	r_node.outgoing_host.unref();
	r_node.connected = false;
}


// Adds a node of the mesh; its address is resolved here, so the lookup of a name doesn't happen while polling.
Error EnetMeshTransport::add_node(int p_id, const String &p_address, int p_port) {
	ERR_FAIL_COND_V_MSG(p_id <= 0 || p_id == local_id, ERR_INVALID_PARAMETER, "A node id must be positive and different from this node's.");
	ERR_FAIL_COND_V_MSG(nodes.has(p_id), ERR_ALREADY_EXISTS, vformat("Node %d is already in the mesh.", p_id));
	MeshNode node;
	node.address = p_address;
	node.port = p_port;
	resolve_node_addresses(p_address, node.addresses);
	nodes.insert(p_id, node);
	return OK;
}


// Removes a node of the mesh: closes its link (and the ones still proving to be it), and reports it as disconnected.
void EnetMeshTransport::remove_node(int p_id) {
	MeshNode *node = nodes.getptr(p_id);
	ERR_FAIL_NULL(node);
	if (node->connected) {
		Event event;
		event.type = EVENT_PEER_DISCONNECTED;
		event.peer = p_id;
		events.push_back(event);
	}
	close_node(*node);
	nodes.erase(p_id);
	LocalVector<ObjectID> links;
	for (const KeyValue<ObjectID, Proving> &E : proving) {
		if (E.value.id == p_id) {
			links.push_back(E.key);
		}
	}
	for (const ObjectID link : links) {
		drop_proving_link(link);
	}
}


// The ids of the nodes added, in order.
PackedInt32Array EnetMeshTransport::get_nodes() const {
	PackedInt32Array ids;
	for (const KeyValue<int, MeshNode> &E : nodes) {
		ids.push_back(E.key);
	}
	ids.sort();
	return ids;
}


// Sets the seconds between the attempts to connect to a node that isn't connected.
void EnetMeshTransport::set_retry_interval(double p_seconds) {
	ERR_FAIL_COND_MSG(!(p_seconds > 0.0), "The retry interval must be positive.");
	retry_interval = p_seconds;
}


// Applies `node_timeout` to a link.
void EnetMeshTransport::apply_node_timeout(const Ref<RefCounted> &p_peer) {
	ENetPacketPeer *peer = as_peer(p_peer);
	if (peer) {
		const int timeout_ms = int(node_timeout * 1000.0);
		peer->set_timeout(32, timeout_ms, timeout_ms);
	}
}


// Sets how long a node that stopped answering is waited for, on the links that are up and on the next ones.
void EnetMeshTransport::set_node_timeout(double p_seconds) {
	ERR_FAIL_COND_MSG(!(p_seconds > 0.0), "The node timeout must be positive.");
	node_timeout = p_seconds;
	for (const KeyValue<int, MeshNode> &E : nodes) {
		if (E.value.connected) {
			apply_node_timeout(E.value.peer);
		}
	}
}


// Sets the secret the nodes prove to each other; an empty one asks for no proof.
void EnetMeshTransport::set_secret(const PackedByteArray &p_secret) {
	secret = p_secret;
}


// The port this node listens on.
int EnetMeshTransport::get_local_port() const {
	ENetConnection *connection = as_host(host);
	return connection ? connection->get_local_port() : 0;
}


// Whether the link with a node is up and, with a secret, proved.
bool EnetMeshTransport::is_peer_connected(int p_peer) const {
	const MeshNode *node = nodes.getptr(p_peer);
	return node && node->connected;
}


// The connected nodes, in order.
void EnetMeshTransport::get_connected_peers(LocalVector<int> &r_peers) const {
	r_peers.clear();
	for (const KeyValue<int, MeshNode> &E : nodes) {
		if (E.value.connected) {
			r_peers.push_back(E.key);
		}
	}
	r_peers.sort();
}


// The engines' channels.
int EnetMeshTransport::get_channel_count() const {
	return TICK_CHANNEL_COUNT;
}


// The largest payload that fits a datagram.
int EnetMeshTransport::get_max_payload_size() const {
	return ENET_MESH_MAX_PAYLOAD;
}


// Sends a packet to a connected node right away.
Error EnetMeshTransport::send_now(int p_node, MeshNode &r_node, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) {
	enet_uint32 flags = 0;
	if (p_mode == TRANSFER_MODE_RELIABLE) {
		flags = ENET_PACKET_FLAG_RELIABLE;
	} else if (p_mode == TRANSFER_MODE_UNRELIABLE) {
		flags = ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
	} else {
		flags = ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
	}
	ENetPacketPeer *peer = as_peer(r_node.peer);
	ERR_FAIL_NULL_V(peer, ERR_UNAVAILABLE);
	if (p_channel >= peer->get_channels()) {
		// ENet already reset the link: its disconnection is on the way.
		return ERR_UNAVAILABLE;
	}
	ENetPacket *packet = enet_packet_create(p_data, p_size, flags);
	ERR_FAIL_NULL_V(packet, ERR_OUT_OF_MEMORY);
	if (peer->send(uint8_t(p_channel), packet) < 0) {
		enet_packet_destroy(packet);
		ERR_FAIL_V_MSG(ERR_CANT_CONNECT, vformat("Can't send to mesh node %d.", p_node));
	}
	return OK;
}


// Queues bytes for a node, or for every connected node; with a simulated latency, they're sent when it passes.
Error EnetMeshTransport::send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) {
	ERR_FAIL_INDEX_V(p_channel, TICK_CHANNEL_COUNT, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_size <= 0 || p_data == nullptr, ERR_INVALID_PARAMETER);

	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	for (KeyValue<int, MeshNode> &E : nodes) {
		if (!E.value.connected || (p_peer != PEER_BROADCAST && p_peer != E.key)) {
			continue;
		}
		if (E.value.simulated_latency_usec == 0) {
			// A link being closed is skipped: its disconnection is on the way.
			const Error err = send_now(E.key, E.value, p_channel, p_mode, p_data, p_size);
			ERR_FAIL_COND_V(err != OK && err != ERR_UNAVAILABLE, err);
			continue;
		}
		// The latency of a node is constant, so its packets keep their order.
		Outgoing packet;
		packet.send_at_usec = now + E.value.simulated_latency_usec;
		packet.sequence = next_sequence++;
		packet.node = E.key;
		packet.channel = p_channel;
		packet.mode = p_mode;
		packet.data.resize(p_size);
		memcpy(packet.data.ptrw(), p_data, p_size);
		delayed_packets.push_back(packet);
	}
	ERR_FAIL_COND_V_MSG(p_peer != PEER_BROADCAST && !is_peer_connected(p_peer), ERR_UNAVAILABLE, vformat("Mesh node %d isn't connected.", p_peer));
	return OK;
}


// Sends the delayed packets (simulated latency) that are due.
void EnetMeshTransport::flush_simulated() {
	if (delayed_packets.is_empty()) {
		return;
	}
	delayed_packets.sort();
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	uint32_t sent = 0;
	while (sent < delayed_packets.size() && delayed_packets[sent].send_at_usec <= now) {
		const Outgoing &packet = delayed_packets[sent];
		MeshNode *node = nodes.getptr(packet.node);
		if (node && node->connected) {
			send_now(packet.node, *node, packet.channel, packet.mode, packet.data.ptr(), packet.data.size());
		}
		sent++;
	}
	if (sent == delayed_packets.size()) {
		delayed_packets.clear();
	} else if (sent > 0) {
		LocalVector<Outgoing> remaining;
		for (uint32_t i = sent; i < delayed_packets.size(); i++) {
			remaining.push_back(delayed_packets[i]);
		}
		delayed_packets = remaining;
	}
}


// Delays what this node sends to `p_id` (one way), for debugging; 0 disables it.
void EnetMeshTransport::set_node_simulated_latency(int p_id, double p_seconds) {
	MeshNode *node = nodes.getptr(p_id);
	ERR_FAIL_NULL_MSG(node, vformat("Mesh node %d wasn't added.", p_id));
	ERR_FAIL_COND_MSG(!(p_seconds >= 0.0), "The simulated latency can't be negative.");
	node->simulated_latency_usec = uint64_t(p_seconds * 1000000.0);
}


// The latency simulated for what this node sends to `p_id`, in seconds.
double EnetMeshTransport::get_node_simulated_latency(int p_id) const {
	const MeshNode *node = nodes.getptr(p_id);
	ERR_FAIL_NULL_V_MSG(node, 0.0, vformat("Mesh node %d wasn't added.", p_id));
	return double(node->simulated_latency_usec) / 1000000.0;
}


// Closes the link with a node gracefully; it's connected to again after the retry interval.
void EnetMeshTransport::disconnect_peer(int p_peer) {
	MeshNode *node = nodes.getptr(p_peer);
	ERR_FAIL_NULL(node);
	ENetPacketPeer *peer = as_peer(node->peer);
	if (peer && peer->is_active()) {
		peer->peer_disconnect();
	}
}


// Starts the connections to the nodes with a lower id that have none, once their retry interval passed.
void EnetMeshTransport::connect_pending_nodes(uint64_t p_now_usec) {
	for (KeyValue<int, MeshNode> &E : nodes) {
		MeshNode &node = E.value;
		// The higher id connects to the lower one, so each pair has a single connection.
		if (E.key > local_id || node.connected || node.peer.is_valid() || p_now_usec < node.next_attempt_usec) {
			continue;
		}
		node.next_attempt_usec = p_now_usec + uint64_t(retry_interval * 1000000.0);
		Ref<ENetConnection> outgoing;
		outgoing.instantiate();
		if (outgoing->create_host(1, TICK_CHANNEL_COUNT, 0, 0) != OK) {
			continue;
		}
		outgoing->compress(ENetConnection::CompressionMode(compression));
		Ref<ENetPacketPeer> peer = outgoing->connect_to_host(node.address, node.port, TICK_CHANNEL_COUNT, local_id);
		if (peer.is_null()) {
			outgoing->destroy();
			continue;
		}
		node.outgoing_host = outgoing;
		node.peer = peer;
		ids_by_peer.insert(peer->get_instance_id(), E.key);
	}
}


// Whether a connection from `p_address` may be the node's: the address is one of those the node was added with. A
// name that didn't resolve when the node was added is looked up again, at most once per retry interval.
bool EnetMeshTransport::is_node_address(MeshNode &r_node, const IPAddress &p_address, uint64_t p_now_usec) {
	if (r_node.addresses.is_empty() && p_now_usec >= r_node.next_resolve_usec) {
		r_node.next_resolve_usec = p_now_usec + uint64_t(retry_interval * 1000000.0);
		resolve_node_addresses(r_node.address, r_node.addresses);
	}
	for (const IPAddress &address : r_node.addresses) {
		if (address == p_address) {
			return true;
		}
	}
	return false;
}


// Writes the proof `p_sender` gives `p_receiver` for the challenge `p_nonce`: an HMAC-SHA256, keyed with the secret,
// of a label, the challenge and the two ids. Only a node with the secret can write it, it's good for that challenge
// alone, and a proof given to one node isn't one another node would take.
void EnetMeshTransport::compute_proof(const uint8_t *p_nonce, int p_sender, int p_receiver, uint8_t *r_proof) const {
	const int label_size = int(strlen(PROOF_LABEL));
	LocalVector<uint8_t> message;
	message.resize(label_size + NONCE_SIZE + 8);
	memcpy(message.ptr(), PROOF_LABEL, label_size);
	memcpy(message.ptr() + label_size, p_nonce, NONCE_SIZE);
	encode_uint32(uint32_t(p_sender), message.ptr() + label_size + NONCE_SIZE);
	encode_uint32(uint32_t(p_receiver), message.ptr() + label_size + NONCE_SIZE + 4);
	hmac_sha256(secret.ptr(), secret.size(), message.ptr(), int(message.size()), r_proof);
}


// An ENet link with node `p_id` is up. Without a secret it's reported at once. With one, this side sends its
// challenge, and the link waits in `proving` until the other side answers it.
void EnetMeshTransport::begin_link(int p_id, const Ref<RefCounted> &p_peer, bool p_incoming) {
	if (secret.is_empty()) {
		on_connected(p_id, p_peer);
		return;
	}
	const ObjectID link = p_peer->get_instance_id();
	if (p_incoming) {
		// A few at most for the same node: a new one takes the place of the oldest, so links that never prove
		// anything don't keep the node itself out.
		int count = 0;
		ObjectID oldest;
		uint64_t oldest_deadline = UINT64_MAX;
		for (const KeyValue<ObjectID, Proving> &E : proving) {
			if (E.value.incoming && E.value.id == p_id) {
				count++;
				if (E.value.deadline_usec < oldest_deadline) {
					oldest_deadline = E.value.deadline_usec;
					oldest = E.key;
				}
			}
		}
		if (count >= MAX_PROVING_PER_NODE) {
			drop_proving_link(oldest);
		}
	}
	Proving entry;
	entry.id = p_id;
	entry.peer = p_peer;
	entry.incoming = p_incoming;
	entry.deadline_usec = OS::get_singleton()->get_ticks_usec() + PROOF_TIMEOUT_USEC;
	const bool has_nonce = OS::get_singleton()->get_entropy(entry.nonce, NONCE_SIZE) == OK;
	proving.insert(link, entry);
	if (!has_nonce) {
		// No challenge that can't be guessed, no proof worth anything.
		ERR_PRINT_ONCE("The system gave no random bytes: the links of the mesh can't prove its secret.");
		drop_proving_link(link);
		return;
	}

	uint8_t challenge[PROOF_HEADER_SIZE + NONCE_SIZE];
	memcpy(challenge, PROOF_MAGIC, 4);
	challenge[4] = PROOF_CHALLENGE;
	memcpy(challenge + PROOF_HEADER_SIZE, entry.nonce, NONCE_SIZE);
	ENetPacket *packet = enet_packet_create(challenge, sizeof(challenge), ENET_PACKET_FLAG_RELIABLE);
	ERR_FAIL_NULL(packet);
	if (as_peer(p_peer)->send(uint8_t(TICK_CHANNEL_CONTROL), packet) < 0) {
		enet_packet_destroy(packet);
	}
}


// Handles what a link that is still proving the secret sent. The other side's challenge is answered with this
// node's proof. The other side's proof, when it's the one expected for this side's challenge, makes the link ready;
// a wrong one closes it. Anything else is dropped: the engines see nothing of a link that proved nothing.
void EnetMeshTransport::handle_proof_message(ObjectID p_link, const uint8_t *p_data, int p_size) {
	Proving *entry = proving.getptr(p_link);
	if (entry == nullptr || p_size < PROOF_HEADER_SIZE || memcmp(p_data, PROOF_MAGIC, 4) != 0) {
		return;
	}
	if (p_data[4] == PROOF_CHALLENGE && p_size == PROOF_HEADER_SIZE + NONCE_SIZE) {
		uint8_t answer[PROOF_HEADER_SIZE + PROOF_SIZE];
		memcpy(answer, PROOF_MAGIC, 4);
		answer[4] = PROOF_ANSWER;
		compute_proof(p_data + PROOF_HEADER_SIZE, local_id, entry->id, answer + PROOF_HEADER_SIZE);
		ENetPacket *packet = enet_packet_create(answer, sizeof(answer), ENET_PACKET_FLAG_RELIABLE);
		ERR_FAIL_NULL(packet);
		if (as_peer(entry->peer)->send(uint8_t(TICK_CHANNEL_CONTROL), packet) < 0) {
			enet_packet_destroy(packet);
		}
		return;
	}
	if (p_data[4] != PROOF_ANSWER || p_size != PROOF_HEADER_SIZE + PROOF_SIZE) {
		return;
	}
	uint8_t expected[PROOF_SIZE];
	compute_proof(entry->nonce, entry->id, local_id, expected);
	// Compared without stopping at the first difference, so the time taken tells nothing about the proof.
	uint8_t difference = 0;
	for (int i = 0; i < PROOF_SIZE; i++) {
		difference |= expected[i] ^ p_data[PROOF_HEADER_SIZE + i];
	}
	if (difference != 0) {
		ERR_PRINT_ONCE("A connection to the mesh didn't prove the mesh's secret: it was closed. Every node needs the same secret.");
		drop_proving_link(p_link);
		return;
	}
	const int id = entry->id;
	const Ref<RefCounted> peer = entry->peer;
	const bool incoming = entry->incoming;
	proving.erase(p_link);
	MeshNode *node = nodes.getptr(id);
	if (node == nullptr || node->connected || (!incoming && node->peer != peer)) {
		// The node was removed, or another link of it proved the secret first.
		if (incoming) {
			enet_close_link(as_peer(peer));
		}
		return;
	}
	on_connected(id, peer);
}


// Closes a link that didn't prove the secret. One this node accepted is closed on the listening socket; one it
// started goes with its own socket, and its node is connected to again after the retry interval.
void EnetMeshTransport::drop_proving_link(ObjectID p_link) {
	Proving *entry = proving.getptr(p_link);
	if (entry == nullptr) {
		return;
	}
	const int id = entry->id;
	const Ref<RefCounted> peer = entry->peer;
	const bool incoming = entry->incoming;
	proving.erase(p_link);
	if (incoming) {
		ENetPacketPeer *link = as_peer(peer);
		if (link && link->is_active()) {
			enet_close_link(link);
		}
		return;
	}
	MeshNode *node = nodes.getptr(id);
	if (node && node->peer == peer) {
		close_node(*node);
	}
}


// Closes the links that took too long to prove the secret.
void EnetMeshTransport::check_proof_deadlines(uint64_t p_now_usec) {
	if (proving.is_empty()) {
		return;
	}
	LocalVector<ObjectID> late;
	for (const KeyValue<ObjectID, Proving> &E : proving) {
		if (p_now_usec >= E.value.deadline_usec) {
			late.push_back(E.key);
		}
	}
	if (!late.is_empty()) {
		ERR_PRINT_ONCE("A connection to the mesh didn't prove the mesh's secret in time: it was closed. Every node needs the same secret.");
	}
	for (const ObjectID link : late) {
		drop_proving_link(link);
	}
}


// The link with node `p_id` is ready for the engines: reports the node as connected.
void EnetMeshTransport::on_connected(int p_id, const Ref<RefCounted> &p_peer) {
	MeshNode &node = nodes[p_id];
	node.peer = p_peer;
	node.connected = true;
	apply_node_timeout(p_peer);
	ids_by_peer.insert(p_peer->get_instance_id(), p_id);
	Event event;
	event.type = EVENT_PEER_CONNECTED;
	event.peer = p_id;
	events.push_back(event);
}


// A link closed: forgets it, and reports its node as disconnected if it was connected.
void EnetMeshTransport::on_disconnected(const Ref<RefCounted> &p_peer) {
	proving.erase(p_peer->get_instance_id());
	const int *id = ids_by_peer.getptr(p_peer->get_instance_id());
	if (id == nullptr) {
		return;
	}
	const int node_id = *id;
	ids_by_peer.erase(p_peer->get_instance_id());
	MeshNode *node = nodes.getptr(node_id);
	if (node == nullptr || node->peer != p_peer) {
		return;
	}
	const bool was_connected = node->connected;
	node->peer.unref();
	node->connected = false;
	// A failed or closed outgoing connection gets a new host on the next attempt; the old one is destroyed after
	// its events are serviced (see `service_host()`).
	node->outgoing_host.unref();
	if (was_connected) {
		Event event;
		event.type = EVENT_PEER_DISCONNECTED;
		event.peer = node_id;
		events.push_back(event);
	}
}


// Takes the events of a socket: the listening one (`p_accepts_incoming`), or the one of an outgoing connection. New
// links are checked (see `begin_link()`), the packets of the connected nodes go to the queue of the engines, and the
// packets of the links still proving the secret go to `handle_proof_message()`.
void EnetMeshTransport::service_host(const Ref<RefCounted> &p_host, bool p_accepts_incoming) {
	ENetConnection *connection = as_host(p_host);
	if (connection == nullptr) {
		return;
	}
	// Bounded, so a flood can't stall the frame.
	for (int i = 0; i < 4096; i++) {
		ENetConnection::Event event;
		const ENetConnection::EventType type = connection->service(0, event);
		if (type == ENetConnection::EVENT_NONE || type == ENetConnection::EVENT_ERROR) {
			break;
		}
		if (type == ENetConnection::EVENT_CONNECT) {
			const int *outgoing = ids_by_peer.getptr(event.peer->get_instance_id());
			if (outgoing) {
				begin_link(*outgoing, event.peer, false);
				continue;
			}
			// Incoming: only known nodes with a higher id, one connection each, from the address they were added with.
			const int id = int(event.data);
			MeshNode *node = p_accepts_incoming ? nodes.getptr(id) : nullptr;
			if (node == nullptr || id < local_id || node->connected || (check_addresses && !is_node_address(*node, event.peer->get_remote_address(), OS::get_singleton()->get_ticks_usec()))) {
				enet_close_link(event.peer.ptr());
				continue;
			}
			begin_link(id, event.peer, true);
		} else if (type == ENetConnection::EVENT_DISCONNECT) {
			on_disconnected(event.peer);
			if (!p_accepts_incoming) {
				// An outgoing host has a single connection.
				break;
			}
		} else if (type == ENetConnection::EVENT_RECEIVE) {
			const ObjectID link = event.peer->get_instance_id();
			const int *id = ids_by_peer.getptr(link);
			if (proving.has(link)) {
				if (event.channel_id == TICK_CHANNEL_CONTROL) {
					handle_proof_message(link, event.packet->data, int(event.packet->dataLength));
				}
			} else if (id && is_peer_connected(*id) && !queue_has_room(packets.size() - next_packet, queued_bytes, int(event.packet->dataLength))) {
				WARN_PRINT_ONCE("EnetMeshTransport drops the packets it receives: nothing consumes them (is the TickNetwork running?).");
			} else if (id && is_peer_connected(*id)) {
				Packet packet;
				packet.from_peer = *id;
				packet.channel = event.channel_id;
				packet.data.resize(event.packet->dataLength);
				if (event.packet->dataLength > 0) {
					memcpy(packet.data.ptr(), event.packet->data, event.packet->dataLength);
				}
				queued_bytes += event.packet->dataLength;
				packets.push_back(packet);
			}
			enet_packet_destroy(event.packet);
		}
	}

	if (!p_accepts_incoming) {
		// Destroys an outgoing host no node uses anymore.
		for (const KeyValue<int, MeshNode> &E : nodes) {
			if (E.value.outgoing_host == p_host) {
				return;
			}
		}
		connection->destroy();
	}
}


// Connects to the nodes that are due, sends what is queued and takes what arrived.
void EnetMeshTransport::poll() {
	ERR_FAIL_COND_MSG(host.is_null(), "The mesh transport isn't created; use `create()`.");
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	connect_pending_nodes(now);
	flush_simulated();

	service_host(host, true);
	LocalVector<Ref<RefCounted>> outgoing_hosts;
	for (const KeyValue<int, MeshNode> &E : nodes) {
		if (E.value.outgoing_host.is_valid()) {
			outgoing_hosts.push_back(E.value.outgoing_host);
		}
	}
	for (const Ref<RefCounted> &outgoing : outgoing_hosts) {
		service_host(outgoing, false);
	}
	check_proof_deadlines(now);

	as_host(host)->flush();
	for (const KeyValue<int, MeshNode> &E : nodes) {
		ENetConnection *outgoing = as_host(E.value.outgoing_host);
		if (outgoing) {
			outgoing->flush();
		}
	}
}


// The next connection or disconnection, if any.
bool EnetMeshTransport::pop_event(Event &r_event) {
	if (next_event >= events.size()) {
		events.clear();
		next_event = 0;
		return false;
	}
	r_event = events[next_event++];
	return true;
}


// The next packet received, if any.
bool EnetMeshTransport::pop_packet(Packet &r_packet) {
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


// Exposes the class to scripts.
void EnetMeshTransport::_bind_methods() {
	ClassDB::bind_static_method("EnetMeshTransport", D_METHOD("create", "local_id", "port", "bind_address", "compression", "max_nodes"), &EnetMeshTransport::create, DEFVAL("*"), DEFVAL(COMPRESSION_RANGE_CODER), DEFVAL(32));
	ClassDB::bind_method(D_METHOD("add_node", "id", "address", "port"), &EnetMeshTransport::add_node);
	ClassDB::bind_method(D_METHOD("remove_node", "id"), &EnetMeshTransport::remove_node);
	ClassDB::bind_method(D_METHOD("get_nodes"), &EnetMeshTransport::get_nodes);
	ClassDB::bind_method(D_METHOD("set_node_simulated_latency", "id", "seconds"), &EnetMeshTransport::set_node_simulated_latency);
	ClassDB::bind_method(D_METHOD("get_node_simulated_latency", "id"), &EnetMeshTransport::get_node_simulated_latency);
	ClassDB::bind_method(D_METHOD("set_retry_interval", "seconds"), &EnetMeshTransport::set_retry_interval);
	ClassDB::bind_method(D_METHOD("get_retry_interval"), &EnetMeshTransport::get_retry_interval);
	ClassDB::bind_method(D_METHOD("set_node_timeout", "seconds"), &EnetMeshTransport::set_node_timeout);
	ClassDB::bind_method(D_METHOD("get_node_timeout"), &EnetMeshTransport::get_node_timeout);
	ClassDB::bind_method(D_METHOD("set_check_addresses", "enabled"), &EnetMeshTransport::set_check_addresses);
	ClassDB::bind_method(D_METHOD("is_checking_addresses"), &EnetMeshTransport::is_checking_addresses);
	ClassDB::bind_method(D_METHOD("set_secret", "secret"), &EnetMeshTransport::set_secret);
	ClassDB::bind_method(D_METHOD("has_secret"), &EnetMeshTransport::has_secret);
	ClassDB::bind_method(D_METHOD("get_local_port"), &EnetMeshTransport::get_local_port);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "retry_interval", PROPERTY_HINT_RANGE, "0.05,30,0.01,suffix:s"), "set_retry_interval", "get_retry_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "node_timeout", PROPERTY_HINT_RANGE, "0.1,60,0.1,suffix:s"), "set_node_timeout", "get_node_timeout");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "check_addresses"), "set_check_addresses", "is_checking_addresses");

	BIND_ENUM_CONSTANT(COMPRESSION_NONE);
	BIND_ENUM_CONSTANT(COMPRESSION_RANGE_CODER);
	BIND_ENUM_CONSTANT(COMPRESSION_FASTLZ);
	BIND_ENUM_CONSTANT(COMPRESSION_ZLIB);
	BIND_ENUM_CONSTANT(COMPRESSION_ZSTD);
}

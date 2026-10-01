#include "enet_mesh_transport.h"

#include "../sync/tick_protocol.h"

#include "core/os/os.h"
#include "core/variant/variant.h"

#include "modules/enet/enet_connection.h"
#include "modules/enet/enet_packet_peer.h"

static_assert(int(EnetMeshTransport::COMPRESSION_ZSTD) == int(ENetConnection::COMPRESS_ZSTD), "The compression modes must match ENet's.");

// ENet's default MTU minus the ENet headers (protocol and send command).
static constexpr int ENET_MESH_MAX_PAYLOAD = 1400 - 6 - 12;

static ENetConnection *as_host(const Ref<RefCounted> &p_host) {
	return Object::cast_to<ENetConnection>(p_host.ptr());
}

static ENetPacketPeer *as_peer(const Ref<RefCounted> &p_peer) {
	return Object::cast_to<ENetPacketPeer>(p_peer.ptr());
}

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

EnetMeshTransport::~EnetMeshTransport() {
	for (KeyValue<int, MeshNode> &E : nodes) {
		close_node(E.value);
	}
	ENetConnection *connection = as_host(host);
	if (connection) {
		connection->destroy();
	}
}

void EnetMeshTransport::close_node(MeshNode &r_node) {
	ENetPacketPeer *peer = as_peer(r_node.peer);
	if (peer && peer->is_active()) {
		peer->peer_disconnect_now();
	}
	if (r_node.peer.is_valid()) {
		ids_by_peer.erase(r_node.peer->get_instance_id());
	}
	r_node.peer.unref();
	ENetConnection *outgoing = as_host(r_node.outgoing_host);
	if (outgoing) {
		outgoing->destroy();
	}
	r_node.outgoing_host.unref();
	r_node.connected = false;
}

Error EnetMeshTransport::add_node(int p_id, const String &p_address, int p_port) {
	ERR_FAIL_COND_V_MSG(p_id <= 0 || p_id == local_id, ERR_INVALID_PARAMETER, "A node id must be positive and different from this node's.");
	ERR_FAIL_COND_V_MSG(nodes.has(p_id), ERR_ALREADY_EXISTS, vformat("Node %d is already in the mesh.", p_id));
	MeshNode node;
	node.address = p_address;
	node.port = p_port;
	nodes.insert(p_id, node);
	return OK;
}

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
}

PackedInt32Array EnetMeshTransport::get_nodes() const {
	PackedInt32Array ids;
	for (const KeyValue<int, MeshNode> &E : nodes) {
		ids.push_back(E.key);
	}
	ids.sort();
	return ids;
}

void EnetMeshTransport::set_retry_interval(double p_seconds) {
	ERR_FAIL_COND_MSG(!(p_seconds > 0.0), "The retry interval must be positive.");
	retry_interval = p_seconds;
}

void EnetMeshTransport::apply_node_timeout(const Ref<RefCounted> &p_peer) {
	ENetPacketPeer *peer = as_peer(p_peer);
	if (peer) {
		const int timeout_ms = int(node_timeout * 1000.0);
		peer->set_timeout(32, timeout_ms, timeout_ms);
	}
}

void EnetMeshTransport::set_node_timeout(double p_seconds) {
	ERR_FAIL_COND_MSG(!(p_seconds > 0.0), "The node timeout must be positive.");
	node_timeout = p_seconds;
	for (const KeyValue<int, MeshNode> &E : nodes) {
		if (E.value.connected) {
			apply_node_timeout(E.value.peer);
		}
	}
}

int EnetMeshTransport::get_local_port() const {
	ENetConnection *connection = as_host(host);
	return connection ? connection->get_local_port() : 0;
}

bool EnetMeshTransport::is_peer_connected(int p_peer) const {
	const MeshNode *node = nodes.getptr(p_peer);
	return node && node->connected;
}

void EnetMeshTransport::get_connected_peers(LocalVector<int> &r_peers) const {
	r_peers.clear();
	for (const KeyValue<int, MeshNode> &E : nodes) {
		if (E.value.connected) {
			r_peers.push_back(E.key);
		}
	}
	r_peers.sort();
}

int EnetMeshTransport::get_channel_count() const {
	return TICK_CHANNEL_COUNT;
}

int EnetMeshTransport::get_max_payload_size() const {
	return ENET_MESH_MAX_PAYLOAD;
}

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

void EnetMeshTransport::set_node_simulated_latency(int p_id, double p_seconds) {
	MeshNode *node = nodes.getptr(p_id);
	ERR_FAIL_NULL_MSG(node, vformat("Mesh node %d wasn't added.", p_id));
	ERR_FAIL_COND_MSG(!(p_seconds >= 0.0), "The simulated latency can't be negative.");
	node->simulated_latency_usec = uint64_t(p_seconds * 1000000.0);
}

double EnetMeshTransport::get_node_simulated_latency(int p_id) const {
	const MeshNode *node = nodes.getptr(p_id);
	ERR_FAIL_NULL_V_MSG(node, 0.0, vformat("Mesh node %d wasn't added.", p_id));
	return double(node->simulated_latency_usec) / 1000000.0;
}

void EnetMeshTransport::disconnect_peer(int p_peer) {
	MeshNode *node = nodes.getptr(p_peer);
	ERR_FAIL_NULL(node);
	ENetPacketPeer *peer = as_peer(node->peer);
	if (peer && peer->is_active()) {
		peer->peer_disconnect();
	}
}

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

void EnetMeshTransport::on_disconnected(const Ref<RefCounted> &p_peer) {
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
				on_connected(*outgoing, event.peer);
				continue;
			}
			// Incoming: only known nodes with a higher id, one connection each.
			const int id = int(event.data);
			MeshNode *node = p_accepts_incoming ? nodes.getptr(id) : nullptr;
			if (node == nullptr || id < local_id || node->connected) {
				event.peer->peer_disconnect_now();
				continue;
			}
			on_connected(id, event.peer);
		} else if (type == ENetConnection::EVENT_DISCONNECT) {
			on_disconnected(event.peer);
			if (!p_accepts_incoming) {
				// An outgoing host has a single connection.
				break;
			}
		} else if (type == ENetConnection::EVENT_RECEIVE) {
			const int *id = ids_by_peer.getptr(event.peer->get_instance_id());
			if (id && is_peer_connected(*id) && !queue_has_room(packets.size() - next_packet, queued_bytes, int(event.packet->dataLength))) {
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

void EnetMeshTransport::poll() {
	ERR_FAIL_COND_MSG(host.is_null(), "The mesh transport isn't created; use `create()`.");
	connect_pending_nodes(OS::get_singleton()->get_ticks_usec());
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

	as_host(host)->flush();
	for (const KeyValue<int, MeshNode> &E : nodes) {
		ENetConnection *outgoing = as_host(E.value.outgoing_host);
		if (outgoing) {
			outgoing->flush();
		}
	}
}

bool EnetMeshTransport::pop_event(Event &r_event) {
	if (next_event >= events.size()) {
		events.clear();
		next_event = 0;
		return false;
	}
	r_event = events[next_event++];
	return true;
}

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
	ClassDB::bind_method(D_METHOD("get_local_port"), &EnetMeshTransport::get_local_port);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "retry_interval", PROPERTY_HINT_RANGE, "0.05,30,0.01,suffix:s"), "set_retry_interval", "get_retry_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "node_timeout", PROPERTY_HINT_RANGE, "0.1,60,0.1,suffix:s"), "set_node_timeout", "get_node_timeout");

	BIND_ENUM_CONSTANT(COMPRESSION_NONE);
	BIND_ENUM_CONSTANT(COMPRESSION_RANGE_CODER);
	BIND_ENUM_CONSTANT(COMPRESSION_FASTLZ);
	BIND_ENUM_CONSTANT(COMPRESSION_ZLIB);
	BIND_ENUM_CONSTANT(COMPRESSION_ZSTD);
}

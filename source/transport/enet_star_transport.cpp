#include "enet_star_transport.h"

#include "../sync/tick_protocol.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"

#include "modules/enet/enet_connection.h"
#include "modules/enet/enet_multiplayer_peer.h"

static_assert(int(EnetStarTransport::COMPRESSION_ZSTD) == int(ENetConnection::COMPRESS_ZSTD), "The compression modes must match ENet's.");

// ENet's default MTU minus the ENet headers (protocol and send command) and the `SceneMultiplayer` command byte.
static constexpr int ENET_MAX_PAYLOAD = 1400 - 6 - 12 - 1;
// Record header and authentication tag of a DTLS 1.2 datagram with AES-GCM.
static constexpr int DTLS_OVERHEAD = 37;

EnetStarTransport::EnetStarTransport() {
	rng.seed(OS::get_singleton()->get_ticks_usec());
}

EnetStarTransport::~EnetStarTransport() {
	if (multiplayer.is_valid()) {
		multiplayer->disconnect(SNAME("peer_connected"), callable_mp(this, &EnetStarTransport::_on_peer_connected));
		multiplayer->disconnect(SNAME("peer_disconnected"), callable_mp(this, &EnetStarTransport::_on_peer_disconnected));
		multiplayer->disconnect(SNAME("peer_packet"), callable_mp(this, &EnetStarTransport::_on_peer_packet));
	}
}

Ref<EnetStarTransport> EnetStarTransport::create_server(int p_port, int p_max_clients, Compression p_compression, const Ref<TLSOptions> &p_tls_options) {
	Ref<ENetMultiplayerPeer> peer;
	peer.instantiate();
	// The channel count is left at 0 (ENet's maximum): `ENetMultiplayerPeer::create_server()` passes it as the host's
	// incoming bandwidth (Godot 4.0 to 4.8), which makes ENet throttle almost every unreliable packet of the
	// clients. The clients still ask for `TICK_CHANNEL_COUNT` channels.
	Error err = peer->create_server(p_port, p_max_clients, 0);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<EnetStarTransport>(), vformat("Can't create the ENet server on port %d.", p_port));
	peer->get_host()->compress(ENetConnection::CompressionMode(p_compression));
	if (p_tls_options.is_valid()) {
		err = peer->get_host()->dtls_server_setup(p_tls_options);
		ERR_FAIL_COND_V_MSG(err != OK, Ref<EnetStarTransport>(), "Can't set up DTLS on the ENet server.");
	}

	Ref<SceneMultiplayer> multiplayer;
	multiplayer.instantiate();
	multiplayer->set_server_relay_enabled(false);
	// A standalone SceneMultiplayer needs a root path to process packets; RPCs resolve from the scene root.
	multiplayer->set_root_path(NodePath("/root"));
	multiplayer->set_multiplayer_peer(peer);

	Ref<EnetStarTransport> transport;
	transport.instantiate();
	transport->setup(multiplayer, p_tls_options.is_valid());
	return transport;
}

Ref<EnetStarTransport> EnetStarTransport::create_client(const String &p_address, int p_port, Compression p_compression, const Ref<TLSOptions> &p_tls_options, const String &p_tls_hostname) {
	Ref<ENetMultiplayerPeer> peer;
	peer.instantiate();
	Error err = peer->create_client(p_address, p_port, TICK_CHANNEL_COUNT);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<EnetStarTransport>(), vformat("Can't create the ENet client for %s:%d.", p_address, p_port));
	// The connection request is only queued by `create_client()`; it's sent on the first poll, so compression and
	// DTLS still apply to it.
	peer->get_host()->compress(ENetConnection::CompressionMode(p_compression));
	if (p_tls_options.is_valid()) {
		err = peer->get_host()->dtls_client_setup(p_tls_hostname.is_empty() ? p_address : p_tls_hostname, p_tls_options);
		ERR_FAIL_COND_V_MSG(err != OK, Ref<EnetStarTransport>(), "Can't set up DTLS on the ENet client.");
	}

	Ref<SceneMultiplayer> multiplayer;
	multiplayer.instantiate();
	multiplayer->set_server_relay_enabled(false);
	// A standalone SceneMultiplayer needs a root path to process packets; RPCs resolve from the scene root.
	multiplayer->set_root_path(NodePath("/root"));
	multiplayer->set_multiplayer_peer(peer);

	Ref<EnetStarTransport> transport;
	transport.instantiate();
	transport->setup(multiplayer, p_tls_options.is_valid());
	return transport;
}

Error EnetStarTransport::setup(const Ref<SceneMultiplayer> &p_multiplayer, bool p_encrypted) {
	ERR_FAIL_COND_V_MSG(p_multiplayer.is_null(), ERR_INVALID_PARAMETER, "The multiplayer is null.");
	ERR_FAIL_COND_V_MSG(multiplayer.is_valid(), ERR_ALREADY_IN_USE, "The transport is already set up.");
	multiplayer = p_multiplayer;
	encrypted = p_encrypted;
	multiplayer->connect(SNAME("peer_connected"), callable_mp(this, &EnetStarTransport::_on_peer_connected));
	multiplayer->connect(SNAME("peer_disconnected"), callable_mp(this, &EnetStarTransport::_on_peer_disconnected));
	multiplayer->connect(SNAME("peer_packet"), callable_mp(this, &EnetStarTransport::_on_peer_packet));

	// Peers connected before the setup.
	const Vector<int> peers = multiplayer->get_peer_ids();
	for (const int peer : peers) {
		_on_peer_connected(peer);
	}
	return OK;
}

void EnetStarTransport::_on_peer_connected(int p_peer) {
	Event event;
	event.type = EVENT_PEER_CONNECTED;
	event.peer = p_peer;
	events.push_back(event);
}

void EnetStarTransport::_on_peer_disconnected(int p_peer) {
	Event event;
	event.type = EVENT_PEER_DISCONNECTED;
	event.peer = p_peer;
	events.push_back(event);
}

void EnetStarTransport::_on_peer_packet(int p_peer, const PackedByteArray &p_packet) {
	Packet packet;
	// The sender id comes from `SceneMultiplayer`, which takes it from the connection.
	packet.from_peer = p_peer;
	packet.data.resize(p_packet.size());
	if (p_packet.size() > 0) {
		memcpy(packet.data.ptr(), p_packet.ptr(), p_packet.size());
	}
	packets.push_back(packet);
}

void EnetStarTransport::set_simulated_latency(double p_seconds) {
	ERR_FAIL_COND_MSG(!(p_seconds >= 0.0), "The latency can't be negative.");
	simulated_latency_usec = uint64_t(p_seconds * 1000000.0);
}

double EnetStarTransport::get_simulated_latency() const {
	return double(simulated_latency_usec) / 1000000.0;
}

void EnetStarTransport::set_simulated_jitter(double p_seconds) {
	ERR_FAIL_COND_MSG(!(p_seconds >= 0.0), "The jitter can't be negative.");
	simulated_jitter_usec = uint64_t(p_seconds * 1000000.0);
}

double EnetStarTransport::get_simulated_jitter() const {
	return double(simulated_jitter_usec) / 1000000.0;
}

void EnetStarTransport::set_simulated_packet_loss(double p_ratio) {
	ERR_FAIL_COND_MSG(!(p_ratio >= 0.0 && p_ratio <= 1.0), "The packet loss must be between 0 and 1.");
	simulated_packet_loss = p_ratio;
}

double EnetStarTransport::get_simulated_packet_loss() const {
	return simulated_packet_loss;
}

int EnetStarTransport::get_local_peer_id() const {
	return multiplayer.is_valid() ? multiplayer->get_unique_id() : 0;
}

bool EnetStarTransport::is_peer_connected(int p_peer) const {
	return multiplayer.is_valid() && multiplayer->get_peer_ids().has(p_peer);
}

void EnetStarTransport::get_connected_peers(LocalVector<int> &r_peers) const {
	r_peers.clear();
	if (multiplayer.is_null()) {
		return;
	}
	for (const int peer : multiplayer->get_peer_ids()) {
		r_peers.push_back(peer);
	}
}

int EnetStarTransport::get_channel_count() const {
	return TICK_CHANNEL_COUNT;
}

int EnetStarTransport::get_max_payload_size() const {
	return ENET_MAX_PAYLOAD - (encrypted ? DTLS_OVERHEAD : 0);
}

Error EnetStarTransport::send_now(int p_peer, int p_channel, TransferMode p_mode, const Vector<uint8_t> &p_data) {
	MultiplayerPeer::TransferMode mode = MultiplayerPeer::TRANSFER_MODE_RELIABLE;
	if (p_mode == TRANSFER_MODE_UNRELIABLE) {
		mode = MultiplayerPeer::TRANSFER_MODE_UNRELIABLE;
	} else if (p_mode == TRANSFER_MODE_UNRELIABLE_ORDERED) {
		mode = MultiplayerPeer::TRANSFER_MODE_UNRELIABLE_ORDERED;
	}
	// `SceneMultiplayer` channel 0 is its default channel; the module's channels start at 1.
	return multiplayer->send_bytes(p_data, p_peer, mode, p_channel + 1);
}

Error EnetStarTransport::send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) {
	ERR_FAIL_COND_V_MSG(multiplayer.is_null(), ERR_UNCONFIGURED, "The transport isn't set up.");
	ERR_FAIL_INDEX_V(p_channel, TICK_CHANNEL_COUNT, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_size <= 0 || p_data == nullptr, ERR_INVALID_PARAMETER);

	Vector<uint8_t> data;
	data.resize(p_size);
	memcpy(data.ptrw(), p_data, p_size);

	const bool simulating = simulated_latency_usec > 0 || simulated_jitter_usec > 0 || simulated_packet_loss > 0.0;
	if (!simulating) {
		return send_now(p_peer, p_channel, p_mode, data);
	}

	if (p_mode != TRANSFER_MODE_RELIABLE && simulated_packet_loss > 0.0 && rng.randf() < simulated_packet_loss) {
		return OK;
	}
	Outgoing packet;
	packet.send_at_usec = OS::get_singleton()->get_ticks_usec() + simulated_latency_usec;
	if (simulated_jitter_usec > 0) {
		packet.send_at_usec += uint64_t(rng.rand()) % (simulated_jitter_usec + 1);
	}
	if (p_mode == TRANSFER_MODE_RELIABLE) {
		// Reliable packets keep their order.
		packet.send_at_usec = MAX(packet.send_at_usec, last_reliable_send_usec);
		last_reliable_send_usec = packet.send_at_usec;
	}
	packet.sequence = next_sequence++;
	packet.peer = p_peer;
	packet.channel = p_channel;
	packet.mode = p_mode;
	packet.data = data;
	outgoing.push_back(packet);
	return OK;
}

void EnetStarTransport::flush_simulated(bool p_all) {
	if (outgoing.is_empty()) {
		return;
	}
	outgoing.sort();
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	uint32_t sent = 0;
	while (sent < outgoing.size() && (p_all || outgoing[sent].send_at_usec <= now)) {
		const Outgoing &packet = outgoing[sent];
		if (packet.peer == PEER_BROADCAST || is_peer_connected(packet.peer)) {
			send_now(packet.peer, packet.channel, packet.mode, packet.data);
		}
		sent++;
	}
	if (sent == outgoing.size()) {
		outgoing.clear();
	} else if (sent > 0) {
		LocalVector<Outgoing> remaining;
		for (uint32_t i = sent; i < outgoing.size(); i++) {
			remaining.push_back(outgoing[i]);
		}
		outgoing = remaining;
	}
}

void EnetStarTransport::disconnect_peer(int p_peer) {
	ERR_FAIL_COND(multiplayer.is_null());
	multiplayer->disconnect_peer(p_peer);
}

void EnetStarTransport::poll() {
	ERR_FAIL_COND(multiplayer.is_null());
	flush_simulated(false);
	if (poll_multiplayer) {
		multiplayer->poll();
	}
}

bool EnetStarTransport::pop_event(Event &r_event) {
	if (next_event >= events.size()) {
		events.clear();
		next_event = 0;
		return false;
	}
	r_event = events[next_event++];
	return true;
}

bool EnetStarTransport::pop_packet(Packet &r_packet) {
	if (next_packet >= packets.size()) {
		packets.clear();
		next_packet = 0;
		return false;
	}
	r_packet = packets[next_packet++];
	return true;
}

void EnetStarTransport::_bind_methods() {
	ClassDB::bind_static_method("EnetStarTransport", D_METHOD("create_server", "port", "max_clients", "compression", "tls_options"), &EnetStarTransport::create_server, DEFVAL(32), DEFVAL(COMPRESSION_RANGE_CODER), DEFVAL(Ref<TLSOptions>()));
	ClassDB::bind_static_method("EnetStarTransport", D_METHOD("create_client", "address", "port", "compression", "tls_options", "tls_hostname"), &EnetStarTransport::create_client, DEFVAL(COMPRESSION_RANGE_CODER), DEFVAL(Ref<TLSOptions>()), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("setup", "multiplayer", "encrypted"), &EnetStarTransport::setup, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_multiplayer"), &EnetStarTransport::get_multiplayer);
	ClassDB::bind_method(D_METHOD("set_poll_multiplayer", "enabled"), &EnetStarTransport::set_poll_multiplayer);
	ClassDB::bind_method(D_METHOD("is_polling_multiplayer"), &EnetStarTransport::is_polling_multiplayer);
	ClassDB::bind_method(D_METHOD("set_simulated_latency", "seconds"), &EnetStarTransport::set_simulated_latency);
	ClassDB::bind_method(D_METHOD("get_simulated_latency"), &EnetStarTransport::get_simulated_latency);
	ClassDB::bind_method(D_METHOD("set_simulated_jitter", "seconds"), &EnetStarTransport::set_simulated_jitter);
	ClassDB::bind_method(D_METHOD("get_simulated_jitter"), &EnetStarTransport::get_simulated_jitter);
	ClassDB::bind_method(D_METHOD("set_simulated_packet_loss", "ratio"), &EnetStarTransport::set_simulated_packet_loss);
	ClassDB::bind_method(D_METHOD("get_simulated_packet_loss"), &EnetStarTransport::get_simulated_packet_loss);

	BIND_ENUM_CONSTANT(COMPRESSION_NONE);
	BIND_ENUM_CONSTANT(COMPRESSION_RANGE_CODER);
	BIND_ENUM_CONSTANT(COMPRESSION_FASTLZ);
	BIND_ENUM_CONSTANT(COMPRESSION_ZLIB);
	BIND_ENUM_CONSTANT(COMPRESSION_ZSTD);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "poll_multiplayer"), "set_poll_multiplayer", "is_polling_multiplayer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simulated_latency", PROPERTY_HINT_RANGE, "0,2,0.001,suffix:s"), "set_simulated_latency", "get_simulated_latency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simulated_jitter", PROPERTY_HINT_RANGE, "0,1,0.001,suffix:s"), "set_simulated_jitter", "get_simulated_jitter");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simulated_packet_loss", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_simulated_packet_loss", "get_simulated_packet_loss");
}

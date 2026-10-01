#pragma once

#include "tick_transport.h"

#include "core/templates/hash_map.h"

// Mesh transport over a plain `ENetConnection` (ADR-037), for networks between servers: it leaves the process's
// `SceneMultiplayer` free for the star with the clients.
//
// Every node has a fixed id and knows the others (`add_node()`); the node with the higher id connects to the lower
// one and retries while the connection is down (the LAN mesh builder). A connection is accepted only from a known
// id, declared in the connection request: use it in trusted networks only.
//
// For debugging, a latency can be simulated per node on the sending side (ADR-029).
class EnetMeshTransport : public TickTransport {
	GDCLASS(EnetMeshTransport, TickTransport);

public:
	// Same values as `EnetStarTransport::Compression`.
	enum Compression {
		COMPRESSION_NONE,
		COMPRESSION_RANGE_CODER,
		COMPRESSION_FASTLZ,
		COMPRESSION_ZLIB,
		COMPRESSION_ZSTD,
	};

private:
	struct MeshNode {
		String address;
		int port = 0;
		// `ENetPacketPeer` of the connection (kept as `RefCounted`, so this header doesn't need ENet's).
		Ref<RefCounted> peer;
		// For connections this node starts: their own `ENetConnection` (Godot's allows one outgoing connection per
		// host, and none on a host with peers).
		Ref<RefCounted> outgoing_host;
		bool connected = false;
		uint64_t next_attempt_usec = 0;
		uint64_t simulated_latency_usec = 0;
	};

	struct Outgoing {
		uint64_t send_at_usec = 0;
		uint64_t sequence = 0;
		int node = 0;
		int channel = 0;
		TransferMode mode = TRANSFER_MODE_RELIABLE;
		Vector<uint8_t> data;

		bool operator<(const Outgoing &p_other) const {
			return send_at_usec != p_other.send_at_usec ? send_at_usec < p_other.send_at_usec : sequence < p_other.sequence;
		}
	};

	// Accepts the incoming connections.
	Ref<RefCounted> host;
	int local_id = 0;
	Compression compression = COMPRESSION_RANGE_CODER;
	HashMap<int, MeshNode> nodes;
	HashMap<ObjectID, int> ids_by_peer;
	double retry_interval = 1.0;
	double node_timeout = 5.0;

	LocalVector<Event> events;
	LocalVector<Packet> packets;
	uint32_t next_event = 0;
	uint32_t next_packet = 0;
	uint64_t queued_bytes = 0;
	LocalVector<Outgoing> delayed_packets;
	uint64_t next_sequence = 0;

	Error send_now(int p_node, MeshNode &r_node, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size);
	void flush_simulated();
	void connect_pending_nodes(uint64_t p_now_usec);
	void on_connected(int p_id, const Ref<RefCounted> &p_peer);
	void apply_node_timeout(const Ref<RefCounted> &p_peer);
	void on_disconnected(const Ref<RefCounted> &p_peer);
	void service_host(const Ref<RefCounted> &p_host, bool p_accepts_incoming);
	void close_node(MeshNode &r_node);

protected:
	static void _bind_methods();

public:
	// Binds a host on `p_port` for this node (`p_local_id`, positive).
	static Ref<EnetMeshTransport> create(int p_local_id, int p_port, const String &p_bind_address = "*", Compression p_compression = COMPRESSION_RANGE_CODER, int p_max_nodes = 32);

	// Adds a node of the mesh. Nodes with a lower id than this one are connected to; the others connect here.
	Error add_node(int p_id, const String &p_address, int p_port);
	void remove_node(int p_id);
	PackedInt32Array get_nodes() const;

	// Delays what this node sends to `p_id` (one way), for debugging; 0 disables it.
	void set_node_simulated_latency(int p_id, double p_seconds);
	double get_node_simulated_latency(int p_id) const;

	void set_retry_interval(double p_seconds);
	double get_retry_interval() const { return retry_interval; }
	// Seconds without an answer before a node is considered gone (ENet's own limits vary from 5 to 30 seconds).
	void set_node_timeout(double p_seconds);
	double get_node_timeout() const { return node_timeout; }
	int get_local_port() const;

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

	~EnetMeshTransport();
};

VARIANT_ENUM_CAST(EnetMeshTransport::Compression);

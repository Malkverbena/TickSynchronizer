// The transport of a mesh between servers: `EnetMeshTransport`.
//
// It runs over plain `ENetConnection` sockets (ADR-037), so the process's `SceneMultiplayer` stays free for the star
// with the clients. Every node has a fixed id and knows the others (`add_node()`); the node with the higher id
// connects to the lower one and retries while the connection is down (the LAN mesh builder).
//
// Who may take the place of a node (ADR-076):
// - A connection is accepted only for a known id, declared in the connection request, with a higher id than this
//   node's, once per id.
// - It must come from the address the node was added with (`check_addresses`), so a stranger that reaches the port
//   can't take the place of a node that isn't connected.
// - With a secret (`set_secret()`), both sides of every link prove they know it before the link is reported: each
//   sends a random challenge and answers the other's with an HMAC-SHA256 of the challenge and the two ids.
// The links aren't encrypted: this keeps strangers out, it doesn't hide the traffic. The mesh is still meant for a
// trusted network.
//
// For debugging, a latency can be simulated per node on the sending side (ADR-029).

#pragma once

#include "tick_transport.h"

#include "core/io/ip_address.h"
#include "core/templates/hash_map.h"
#include "core/variant/variant.h"

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
	// Size of the challenge of a proof, and of the proof itself (an HMAC-SHA256).
	static constexpr int NONCE_SIZE = 16;
	static constexpr int PROOF_SIZE = 32;

	struct MeshNode {
		String address;
		int port = 0;
		// The addresses `address` stands for (itself, or the ones its name resolves to): where the node's connections
		// may come from. A name that didn't resolve is tried again, at most once per retry interval.
		LocalVector<IPAddress> addresses;
		uint64_t next_resolve_usec = 0;
		// `ENetPacketPeer` of the connection (kept as `RefCounted`, so this header doesn't need ENet's).
		Ref<RefCounted> peer;
		// For connections this node starts: their own `ENetConnection` (Godot's allows one outgoing connection per
		// host, and none on a host with peers).
		Ref<RefCounted> outgoing_host;
		bool connected = false;
		uint64_t next_attempt_usec = 0;
		uint64_t simulated_latency_usec = 0;
	};

	// A link whose other side still has to prove it knows the mesh's secret.
	struct Proving {
		int id = 0;
		Ref<RefCounted> peer;
		// Accepted by this node (on the listening socket), or started by it (on a socket of its own).
		bool incoming = false;
		// This side's challenge.
		uint8_t nonce[NONCE_SIZE] = {};
		uint64_t deadline_usec = 0;
	};

	struct Outgoing {
		uint64_t send_at_usec = 0;
		uint64_t sequence = 0;
		int node = 0;
		int channel = 0;
		TransferMode mode = TRANSFER_MODE_RELIABLE;
		Vector<uint8_t> data;

		// Orders the delayed packets by the time they're due, then by the order they were sent in.
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
	bool check_addresses = true;
	PackedByteArray secret;
	// The links still proving the secret, by their `ENetPacketPeer`.
	HashMap<ObjectID, Proving> proving;

	LocalVector<Event> events;
	LocalVector<Packet> packets;
	uint32_t next_event = 0;
	uint32_t next_packet = 0;
	uint64_t queued_bytes = 0;
	LocalVector<Outgoing> delayed_packets;
	uint64_t next_sequence = 0;

	// Sends a packet to a connected node right away.
	Error send_now(int p_node, MeshNode &r_node, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size);


	// Sends the delayed packets (simulated latency) that are due.
	void flush_simulated();


	// Starts the connections to the nodes with a lower id that have none, once their retry interval passed.
	void connect_pending_nodes(uint64_t p_now_usec);


	// Whether a connection from `p_address` may be the node's; resolves the node's name again if it had no address.
	bool is_node_address(MeshNode &r_node, const IPAddress &p_address, uint64_t p_now_usec);


	// An ENet link with node `p_id` is up: reports it, or first asks the other side to prove the secret.
	void begin_link(int p_id, const Ref<RefCounted> &p_peer, bool p_incoming);


	// Writes the proof `p_sender` gives `p_receiver` for the challenge `p_nonce`.
	void compute_proof(const uint8_t *p_nonce, int p_sender, int p_receiver, uint8_t *r_proof) const;


	// Handles what a link that is still proving the secret sent: the other side's challenge, or its proof.
	void handle_proof_message(ObjectID p_link, const uint8_t *p_data, int p_size);


	// Closes a link that didn't prove the secret.
	void drop_proving_link(ObjectID p_link);


	// Closes the links that took too long to prove the secret.
	void check_proof_deadlines(uint64_t p_now_usec);


	// The link with node `p_id` is ready for the engines: reports the node as connected.
	void on_connected(int p_id, const Ref<RefCounted> &p_peer);


	// Applies `node_timeout` to a link.
	void apply_node_timeout(const Ref<RefCounted> &p_peer);


	// A link closed: forgets it, and reports its node as disconnected if it was connected.
	void on_disconnected(const Ref<RefCounted> &p_peer);


	// Takes the events of a socket: the listening one (`p_accepts_incoming`), or the one of an outgoing connection.
	void service_host(const Ref<RefCounted> &p_host, bool p_accepts_incoming);


	// Closes the link with a node and its outgoing socket, without reporting anything.
	void close_node(MeshNode &r_node);


protected:
	// Exposes the class to scripts.
	static void _bind_methods();


public:
	// Binds a host on `p_port` for this node (`p_local_id`, positive).
	static Ref<EnetMeshTransport> create(int p_local_id, int p_port, const String &p_bind_address = "*", Compression p_compression = COMPRESSION_RANGE_CODER, int p_max_nodes = 32);


	// Adds a node of the mesh. Nodes with a lower id than this one are connected to; the others connect here.
	Error add_node(int p_id, const String &p_address, int p_port);


	// Removes a node of the mesh, closing its link.
	void remove_node(int p_id);


	// The ids of the nodes added, in order.
	PackedInt32Array get_nodes() const;


	// Delays what this node sends to `p_id` (one way), for debugging; 0 disables it.
	void set_node_simulated_latency(int p_id, double p_seconds);


	// The latency simulated for what this node sends to `p_id`, in seconds.
	double get_node_simulated_latency(int p_id) const;


	// Seconds between the attempts to connect to a node that isn't connected.
	void set_retry_interval(double p_seconds);


	// See `set_retry_interval()`.
	double get_retry_interval() const { return retry_interval; }


	// Seconds without an answer before a node is considered gone (ENet's own limits vary from 5 to 30 seconds).
	void set_node_timeout(double p_seconds);


	// See `set_node_timeout()`.
	double get_node_timeout() const { return node_timeout; }


	// Whether a connection must come from the address its node was added with. Disable it only when the nodes reach
	// each other through addresses that aren't the ones they connect from (several interfaces, a NAT), and then set a
	// secret.
	void set_check_addresses(bool p_enabled) { check_addresses = p_enabled; }


	// See `set_check_addresses()`.
	bool is_checking_addresses() const { return check_addresses; }


	// The secret every node of the mesh must know (the same bytes on all of them; at least 16 random ones). With one,
	// a link is only reported once the other side proved it knows it; empty, nothing is asked. Set it before the
	// transport is polled.
	void set_secret(const PackedByteArray &p_secret);


	// Whether a secret is set.
	bool has_secret() const { return !secret.is_empty(); }


	// The port this node listens on.
	int get_local_port() const;


	// `TickTransport`: this node's id.
	virtual int get_local_peer_id() const override { return local_id; }


	// `TickTransport`: whether the link with a node is up and, with a secret, proved.
	virtual bool is_peer_connected(int p_peer) const override;


	// `TickTransport`: the connected nodes, in order.
	virtual void get_connected_peers(LocalVector<int> &r_peers) const override;


	// `TickTransport`: the engines' channels.
	virtual int get_channel_count() const override;


	// `TickTransport`: the largest payload that fits a datagram.
	virtual int get_max_payload_size() const override;


	// `TickTransport`: queues bytes for a node, or for every connected node.
	virtual Error send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) override;


	// `TickTransport`: closes the link with a node; it's connected to again after the retry interval.
	virtual void disconnect_peer(int p_peer) override;


	// `TickTransport`: connects to the nodes that are due, sends what is queued and takes what arrived.
	virtual void poll() override;


	// `TickTransport`: the next connection or disconnection, if any.
	virtual bool pop_event(Event &r_event) override;


	// `TickTransport`: the next packet received, if any.
	virtual bool pop_packet(Packet &r_packet) override;


	// Closes every link and socket.
	~EnetMeshTransport();
};

VARIANT_ENUM_CAST(EnetMeshTransport::Compression);

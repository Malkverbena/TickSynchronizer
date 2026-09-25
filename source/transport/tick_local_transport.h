#pragma once

#include "tick_transport.h"

#include "core/math/random_pcg.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

class TickLocalNetwork;

// In-process transport endpoint, created and owned by a `TickLocalNetwork`. Used by the tests.
class TickLocalTransport : public TickTransport {
	GDSOFTCLASS(TickLocalTransport, TickTransport);
	friend class TickLocalNetwork;

	TickLocalNetwork *network = nullptr;
	int peer_id = 0;
	HashSet<int> connected_peers;
	LocalVector<Event> events;
	LocalVector<Packet> inbox;
	uint32_t next_event = 0;
	uint32_t next_packet = 0;

public:
	TickLocalTransport() {}

	virtual int get_local_peer_id() const override { return peer_id; }
	virtual bool is_peer_connected(int p_peer) const override { return connected_peers.has(p_peer); }
	virtual void get_connected_peers(LocalVector<int> &r_peers) const override;
	virtual int get_channel_count() const override;
	virtual int get_max_payload_size() const override;

	virtual Error send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) override;

	virtual void disconnect_peer(int p_peer) override;

	// The network delivers the packets in `TickLocalNetwork::process()`; this does nothing.
	virtual void poll() override {}

	virtual bool pop_event(Event &r_event) override;
	virtual bool pop_packet(Packet &r_packet) override;
};

// Simulated network with a virtual clock, latency, jitter and packet loss, deterministic for a given seed.
//
// Peers are connected explicitly with `connect_peers()`, so tests can build star or mesh topologies. Time only
// advances in `process()`; packets are delivered once their simulated arrival time is reached.
class TickLocalNetwork {
	typedef TickTransport::Packet Packet;
	typedef TickTransport::Event Event;

	struct InFlightPacket {
		uint64_t deliver_at_usec = 0;
		uint64_t sequence = 0;
		int to_peer = 0;
		Packet packet;

		bool operator<(const InFlightPacket &p_other) const {
			return deliver_at_usec != p_other.deliver_at_usec ? deliver_at_usec < p_other.deliver_at_usec : sequence < p_other.sequence;
		}
	};

	HashMap<int, Ref<TickLocalTransport>> peers;
	int next_peer_id = 1;
	int channel_count = 5;
	int max_payload_size = 1200;

	uint64_t time_usec = 0;
	uint64_t latency_usec = 0;
	// One way latency of specific links (both directions), overriding `latency_usec`.
	HashMap<uint64_t, uint64_t> link_latencies;
	uint64_t jitter_usec = 0;
	double packet_loss = 0.0;
	RandomPCG rng;

	uint64_t next_sequence = 0;
	LocalVector<InFlightPacket> in_flight;
	// Per link (sender, receiver, channel): arrival time of the last reliable packet, to keep them in order.
	HashMap<uint64_t, uint64_t> last_reliable_arrival;
	// Per link: sequence of the last unreliable ordered packet delivered, to drop older ones.
	HashMap<uint64_t, uint64_t> last_ordered_sequence;

	uint64_t sent_packets = 0;
	uint64_t sent_bytes = 0;
	uint64_t lost_packets = 0;

	static uint64_t make_link_key(int p_from, int p_to, int p_channel);
	void drop_in_flight_between(int p_peer_a, int p_peer_b);

public:
	TickLocalNetwork();
	~TickLocalNetwork();

	// Creates a peer with the next free id (starting at 1).
	Ref<TickLocalTransport> add_peer();
	// Disconnects a peer and removes it from the network; its links' in-flight packets are dropped.
	void remove_peer(int p_peer);
	Ref<TickLocalTransport> get_peer(int p_peer) const;

	// Links two peers; both receive `EVENT_PEER_CONNECTED`.
	Error connect_peers(int p_peer_a, int p_peer_b);
	// Unlinks two peers; both receive `EVENT_PEER_DISCONNECTED`, and in-flight packets are dropped.
	void disconnect_peers(int p_peer_a, int p_peer_b);
	// Links every pair of peers.
	void connect_all();

	void set_channel_count(int p_channel_count);
	int get_channel_count() const { return channel_count; }
	void set_max_payload_size(int p_size) { max_payload_size = p_size; }
	int get_max_payload_size() const { return max_payload_size; }

	// One way latency; each packet gets a uniform random extra delay in [0, jitter].
	void set_latency_usec(uint64_t p_latency_usec) { latency_usec = p_latency_usec; }
	// One way latency between two peers, in both directions, instead of the network's.
	void set_link_latency_usec(int p_peer_a, int p_peer_b, uint64_t p_latency_usec);
	void set_jitter_usec(uint64_t p_jitter_usec) { jitter_usec = p_jitter_usec; }
	// Probability, in [0, 1], of losing an unreliable packet. Reliable packets are never lost.
	void set_packet_loss(double p_packet_loss);
	void set_seed(uint64_t p_seed);

	uint64_t get_time_usec() const { return time_usec; }
	uint64_t get_sent_packets() const { return sent_packets; }
	uint64_t get_sent_bytes() const { return sent_bytes; }
	uint64_t get_lost_packets() const { return lost_packets; }
	int get_in_flight_count() const { return int(in_flight.size()); }

	// Advances the virtual clock and delivers the packets that arrived.
	void process_usec(uint64_t p_delta_usec);
	void process(double p_delta);

	// Called by `TickLocalTransport::send()`.
	Error send(int p_from, int p_to, int p_channel, TickTransport::TransferMode p_mode, const uint8_t *p_data, int p_size);
};

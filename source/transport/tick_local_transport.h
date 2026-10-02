// A network inside the process, for the tests: `TickLocalNetwork` and `TickLocalTransport`.
//
// `TickLocalNetwork` simulates a network with a virtual clock, latency, jitter and packet loss, deterministic for a
// given seed. `TickLocalTransport` is the endpoint of one peer in it: the `TickTransport` the engines talk to. Nothing
// here touches a socket, so a test runs many peers in one thread and repeats exactly.

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
	// An endpoint that belongs to no network yet; `TickLocalNetwork::add_peer()` makes the ones that work.
	TickLocalTransport() {}


	// `TickTransport`: the id the network gave this peer.
	virtual int get_local_peer_id() const override { return peer_id; }


	// `TickTransport`: whether this peer is linked to `p_peer`.
	virtual bool is_peer_connected(int p_peer) const override { return connected_peers.has(p_peer); }


	// `TickTransport`: the peers this one is linked to, in order.
	virtual void get_connected_peers(LocalVector<int> &r_peers) const override;


	// `TickTransport`: the channels of the network.
	virtual int get_channel_count() const override;


	// `TickTransport`: the largest payload of the network.
	virtual int get_max_payload_size() const override;


	// `TickTransport`: hands the bytes to the network, for one linked peer or for all of them.
	virtual Error send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) override;


	// `TickTransport`: unlinks this peer from `p_peer`.
	virtual void disconnect_peer(int p_peer) override;


	// `TickTransport`: nothing to do; the network delivers the packets in `TickLocalNetwork::process()`.
	virtual void poll() override {}


	// `TickTransport`: the next connection or disconnection, if any.
	virtual bool pop_event(Event &r_event) override;


	// `TickTransport`: the next packet delivered, if any.
	virtual bool pop_packet(Packet &r_packet) override;
};

// Simulated network with a virtual clock, latency, jitter and packet loss, deterministic for a given seed.
//
// Peers are connected explicitly with `connect_peers()`, so tests can build star or mesh topologies. Time only
// advances in `process()`; packets are delivered once their simulated arrival time is reached.
class TickLocalNetwork {
	typedef TickTransport::Packet Packet;
	typedef TickTransport::Event Event;

	// A packet on its way, with the virtual time it arrives at.
	struct InFlightPacket {
		uint64_t deliver_at_usec = 0;
		uint64_t sequence = 0;
		int to_peer = 0;
		Packet packet;

		// Orders the packets by arrival time, then by the order they were sent in.
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

	// The key of a link in the maps: who sends, who receives and the channel.
	static uint64_t make_link_key(int p_from, int p_to, int p_channel);


	// Drops the packets on their way between two peers, in both directions, and forgets the order kept for their links.
	void drop_in_flight_between(int p_peer_a, int p_peer_b);


public:
	// A network without peers, seeded with 1.
	TickLocalNetwork();


	// Detaches the endpoints, which may outlive the network.
	~TickLocalNetwork();


	// Creates a peer with the next free id (starting at 1).
	Ref<TickLocalTransport> add_peer();


	// Disconnects a peer and removes it from the network; its links' in-flight packets are dropped.
	void remove_peer(int p_peer);


	// The endpoint of a peer; null when the peer doesn't exist.
	Ref<TickLocalTransport> get_peer(int p_peer) const;


	// Links two peers; both receive `EVENT_PEER_CONNECTED`.
	Error connect_peers(int p_peer_a, int p_peer_b);


	// Unlinks two peers; both receive `EVENT_PEER_DISCONNECTED`, and in-flight packets are dropped.
	void disconnect_peers(int p_peer_a, int p_peer_b);


	// Links every pair of peers.
	void connect_all();


	// Sets how many channels the endpoints carry (1 to 255).
	void set_channel_count(int p_channel_count);


	// How many channels the endpoints carry.
	int get_channel_count() const { return channel_count; }


	// Sets the largest payload the endpoints report; nothing is enforced.
	void set_max_payload_size(int p_size) { max_payload_size = p_size; }


	// The largest payload the endpoints report.
	int get_max_payload_size() const { return max_payload_size; }


	// One way latency; each packet gets a uniform random extra delay in [0, jitter].
	void set_latency_usec(uint64_t p_latency_usec) { latency_usec = p_latency_usec; }


	// One way latency between two peers, in both directions, instead of the network's.
	void set_link_latency_usec(int p_peer_a, int p_peer_b, uint64_t p_latency_usec);


	// Sets the largest random delay added to each packet.
	void set_jitter_usec(uint64_t p_jitter_usec) { jitter_usec = p_jitter_usec; }


	// Probability, in [0, 1], of losing an unreliable packet. Reliable packets are never lost.
	void set_packet_loss(double p_packet_loss);


	// Seeds the generator of the jitter and of the packet loss: the same seed repeats the same run.
	void set_seed(uint64_t p_seed);


	// The virtual clock, in microseconds.
	uint64_t get_time_usec() const { return time_usec; }


	// Packets sent so far, lost ones included.
	uint64_t get_sent_packets() const { return sent_packets; }


	// Bytes sent so far, lost ones included.
	uint64_t get_sent_bytes() const { return sent_bytes; }


	// Packets dropped by the simulated loss so far.
	uint64_t get_lost_packets() const { return lost_packets; }


	// Packets sent and not delivered yet.
	int get_in_flight_count() const { return int(in_flight.size()); }


	// Advances the virtual clock and delivers the packets that arrived.
	void process_usec(uint64_t p_delta_usec);


	// Same as `process_usec()`, with the time in seconds.
	void process(double p_delta);


	// Puts a packet on its way from `p_from` to `p_to`: it may be lost (unless reliable), and arrives after the link's
	// latency and jitter. Called by `TickLocalTransport::send()`.
	Error send(int p_from, int p_to, int p_channel, TickTransport::TransferMode p_mode, const uint8_t *p_data, int p_size);
};

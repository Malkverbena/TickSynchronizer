#include "tick_local_transport.h"

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "core/variant/variant.h"

#include <cstring>

void TickLocalTransport::get_connected_peers(LocalVector<int> &r_peers) const {
	r_peers.clear();
	for (const int peer : connected_peers) {
		r_peers.push_back(peer);
	}
	r_peers.sort();
}

int TickLocalTransport::get_channel_count() const {
	ERR_FAIL_NULL_V(network, 0);
	return network->get_channel_count();
}

int TickLocalTransport::get_max_payload_size() const {
	ERR_FAIL_NULL_V(network, 0);
	return network->get_max_payload_size();
}

void TickLocalTransport::disconnect_peer(int p_peer) {
	ERR_FAIL_NULL(network);
	network->disconnect_peers(peer_id, p_peer);
}

Error TickLocalTransport::send(int p_peer, int p_channel, TransferMode p_mode, const uint8_t *p_data, int p_size) {
	ERR_FAIL_NULL_V_MSG(network, ERR_UNCONFIGURED, "This transport was removed from its network.");
	ERR_FAIL_COND_V_MSG(p_peer < 0, ERR_INVALID_PARAMETER, "The target peer can't be negative.");
	if (p_peer == PEER_BROADCAST) {
		LocalVector<int> targets;
		get_connected_peers(targets);
		for (const int target : targets) {
			const Error err = network->send(peer_id, target, p_channel, p_mode, p_data, p_size);
			ERR_FAIL_COND_V(err != OK, err);
		}
		return OK;
	}
	ERR_FAIL_COND_V_MSG(!connected_peers.has(p_peer), ERR_UNAVAILABLE, vformat("Peer %d isn't connected to peer %d.", p_peer, peer_id));
	return network->send(peer_id, p_peer, p_channel, p_mode, p_data, p_size);
}

bool TickLocalTransport::pop_event(Event &r_event) {
	if (next_event >= events.size()) {
		events.clear();
		next_event = 0;
		return false;
	}
	r_event = events[next_event++];
	return true;
}

bool TickLocalTransport::pop_packet(Packet &r_packet) {
	if (next_packet >= inbox.size()) {
		inbox.clear();
		next_packet = 0;
		return false;
	}
	r_packet = inbox[next_packet++];
	return true;
}

TickLocalNetwork::TickLocalNetwork() {
	rng.seed(1);
}

TickLocalNetwork::~TickLocalNetwork() {
	// The transports may outlive the network.
	for (KeyValue<int, Ref<TickLocalTransport>> &E : peers) {
		E.value->network = nullptr;
	}
	peers.clear();
}

Ref<TickLocalTransport> TickLocalNetwork::add_peer() {
	const int peer_id = next_peer_id++;
	Ref<TickLocalTransport> peer;
	peer.instantiate();
	peer->network = this;
	peer->peer_id = peer_id;
	peers.insert(peer_id, peer);
	return peer;
}

void TickLocalNetwork::remove_peer(int p_peer) {
	Ref<TickLocalTransport> peer = get_peer(p_peer);
	ERR_FAIL_COND_MSG(peer.is_null(), vformat("Peer %d doesn't exist.", p_peer));

	LocalVector<int> connected;
	peer->get_connected_peers(connected);
	for (const int other : connected) {
		disconnect_peers(p_peer, other);
	}
	peers.erase(p_peer);
	peer->network = nullptr;
}

Ref<TickLocalTransport> TickLocalNetwork::get_peer(int p_peer) const {
	const Ref<TickLocalTransport> *peer = peers.getptr(p_peer);
	return peer ? *peer : Ref<TickLocalTransport>();
}

Error TickLocalNetwork::connect_peers(int p_peer_a, int p_peer_b) {
	ERR_FAIL_COND_V_MSG(p_peer_a == p_peer_b, ERR_INVALID_PARAMETER, "A peer can't connect to itself.");
	Ref<TickLocalTransport> a = get_peer(p_peer_a);
	Ref<TickLocalTransport> b = get_peer(p_peer_b);
	ERR_FAIL_COND_V_MSG(a.is_null(), ERR_DOES_NOT_EXIST, vformat("Peer %d doesn't exist.", p_peer_a));
	ERR_FAIL_COND_V_MSG(b.is_null(), ERR_DOES_NOT_EXIST, vformat("Peer %d doesn't exist.", p_peer_b));
	if (a->connected_peers.has(p_peer_b)) {
		return ERR_ALREADY_EXISTS;
	}

	a->connected_peers.insert(p_peer_b);
	b->connected_peers.insert(p_peer_a);

	Event event;
	event.type = TickTransport::EVENT_PEER_CONNECTED;
	event.peer = p_peer_b;
	a->events.push_back(event);
	event.peer = p_peer_a;
	b->events.push_back(event);
	return OK;
}

void TickLocalNetwork::disconnect_peers(int p_peer_a, int p_peer_b) {
	Ref<TickLocalTransport> a = get_peer(p_peer_a);
	Ref<TickLocalTransport> b = get_peer(p_peer_b);
	ERR_FAIL_COND(a.is_null());
	ERR_FAIL_COND(b.is_null());
	if (!a->connected_peers.has(p_peer_b)) {
		return;
	}

	a->connected_peers.erase(p_peer_b);
	b->connected_peers.erase(p_peer_a);
	drop_in_flight_between(p_peer_a, p_peer_b);

	Event event;
	event.type = TickTransport::EVENT_PEER_DISCONNECTED;
	event.peer = p_peer_b;
	a->events.push_back(event);
	event.peer = p_peer_a;
	b->events.push_back(event);
}

void TickLocalNetwork::connect_all() {
	LocalVector<int> ids;
	for (const KeyValue<int, Ref<TickLocalTransport>> &E : peers) {
		ids.push_back(E.key);
	}
	ids.sort();
	for (uint32_t i = 0; i < ids.size(); i++) {
		for (uint32_t j = i + 1; j < ids.size(); j++) {
			if (!get_peer(ids[i])->is_peer_connected(ids[j])) {
				connect_peers(ids[i], ids[j]);
			}
		}
	}
}

void TickLocalNetwork::set_channel_count(int p_channel_count) {
	ERR_FAIL_COND_MSG(p_channel_count <= 0 || p_channel_count > 255, "The channel count must be between 1 and 255.");
	channel_count = p_channel_count;
}

void TickLocalNetwork::set_packet_loss(double p_packet_loss) {
	ERR_FAIL_COND_MSG(!(p_packet_loss >= 0.0 && p_packet_loss <= 1.0), "The packet loss must be between 0 and 1.");
	packet_loss = p_packet_loss;
}

void TickLocalNetwork::set_seed(uint64_t p_seed) {
	rng.seed(p_seed);
}

uint64_t TickLocalNetwork::make_link_key(int p_from, int p_to, int p_channel) {
	return (uint64_t(uint32_t(p_from) & 0xFFFFFF) << 40) | (uint64_t(uint32_t(p_to) & 0xFFFFFF) << 16) | uint64_t(uint32_t(p_channel) & 0xFFFF);
}

void TickLocalNetwork::drop_in_flight_between(int p_peer_a, int p_peer_b) {
	LocalVector<InFlightPacket> kept;
	for (const InFlightPacket &packet : in_flight) {
		const int from = packet.packet.from_peer;
		const bool between = (from == p_peer_a && packet.to_peer == p_peer_b) || (from == p_peer_b && packet.to_peer == p_peer_a);
		if (!between) {
			kept.push_back(packet);
		}
	}
	in_flight = kept;
	for (int channel = 0; channel < channel_count; channel++) {
		last_reliable_arrival.erase(make_link_key(p_peer_a, p_peer_b, channel));
		last_reliable_arrival.erase(make_link_key(p_peer_b, p_peer_a, channel));
		last_ordered_sequence.erase(make_link_key(p_peer_a, p_peer_b, channel));
		last_ordered_sequence.erase(make_link_key(p_peer_b, p_peer_a, channel));
	}
}

Error TickLocalNetwork::send(int p_from, int p_to, int p_channel, TickTransport::TransferMode p_mode, const uint8_t *p_data, int p_size) {
	ERR_FAIL_INDEX_V_MSG(p_channel, channel_count, ERR_INVALID_PARAMETER, vformat("The channel must be between 0 and %d.", channel_count - 1));
	ERR_FAIL_COND_V_MSG(p_size < 0, ERR_INVALID_PARAMETER, "The packet size can't be negative.");
	ERR_FAIL_COND_V_MSG(p_size > 0 && p_data == nullptr, ERR_INVALID_PARAMETER, "The packet data is null.");

	sent_packets++;
	if (p_mode != TickTransport::TRANSFER_MODE_RELIABLE && packet_loss > 0.0 && rng.randf() < packet_loss) {
		lost_packets++;
		return OK;
	}

	InFlightPacket in_flight_packet;
	in_flight_packet.sequence = next_sequence++;
	in_flight_packet.to_peer = p_to;
	in_flight_packet.packet.from_peer = p_from;
	in_flight_packet.packet.channel = p_channel;
	in_flight_packet.packet.mode = p_mode;
	in_flight_packet.packet.data.resize(p_size);
	if (p_size > 0) {
		memcpy(in_flight_packet.packet.data.ptr(), p_data, p_size);
	}

	uint64_t delay = latency_usec;
	if (jitter_usec > 0) {
		delay += uint64_t(rng.rand()) % (jitter_usec + 1);
	}
	in_flight_packet.deliver_at_usec = time_usec + delay;

	if (p_mode == TickTransport::TRANSFER_MODE_RELIABLE) {
		// Reliable packets of a channel never overtake each other.
		const uint64_t key = make_link_key(p_from, p_to, p_channel);
		uint64_t *last_arrival = last_reliable_arrival.getptr(key);
		if (last_arrival) {
			in_flight_packet.deliver_at_usec = MAX(in_flight_packet.deliver_at_usec, *last_arrival);
			*last_arrival = in_flight_packet.deliver_at_usec;
		} else {
			last_reliable_arrival.insert(key, in_flight_packet.deliver_at_usec);
		}
	}

	in_flight.push_back(in_flight_packet);
	return OK;
}

void TickLocalNetwork::process(double p_delta) {
	ERR_FAIL_COND_MSG(!(p_delta >= 0.0), "The delta can't be negative.");
	process_usec(uint64_t(p_delta * 1000000.0 + 0.5));
}

void TickLocalNetwork::process_usec(uint64_t p_delta_usec) {
	time_usec += p_delta_usec;
	if (in_flight.is_empty()) {
		return;
	}

	in_flight.sort();

	uint32_t delivered = 0;
	while (delivered < in_flight.size() && in_flight[delivered].deliver_at_usec <= time_usec) {
		const InFlightPacket &packet = in_flight[delivered];
		delivered++;

		Ref<TickLocalTransport> recipient = get_peer(packet.to_peer);
		if (recipient.is_null() || !recipient->is_peer_connected(packet.packet.from_peer)) {
			continue;
		}

		if (packet.packet.mode == TickTransport::TRANSFER_MODE_UNRELIABLE_ORDERED) {
			const uint64_t key = make_link_key(packet.packet.from_peer, packet.to_peer, packet.packet.channel);
			uint64_t *last_sequence = last_ordered_sequence.getptr(key);
			if (last_sequence) {
				if (packet.sequence < *last_sequence) {
					// A newer packet already arrived.
					continue;
				}
				*last_sequence = packet.sequence;
			} else {
				last_ordered_sequence.insert(key, packet.sequence);
			}
		}

		recipient->inbox.push_back(packet.packet);
	}

	if (delivered == in_flight.size()) {
		in_flight.clear();
	} else if (delivered > 0) {
		LocalVector<InFlightPacket> remaining;
		remaining.resize(in_flight.size() - delivered);
		for (uint32_t i = delivered; i < in_flight.size(); i++) {
			remaining[i - delivered] = in_flight[i];
		}
		in_flight = remaining;
	}
}

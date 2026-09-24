#pragma once

#include "../source/transport/tick_local_transport.h"

#include "tests/test_macros.h"

namespace TestTickLocalTransport {

inline void send_byte(const Ref<TickTransport> &p_from, int p_to, int p_channel, TickTransport::TransferMode p_mode, uint8_t p_value) {
	CHECK(p_from->send(p_to, p_channel, p_mode, &p_value, 1) == OK);
}

inline LocalVector<uint8_t> receive_bytes(const Ref<TickTransport> &p_transport, int p_expected_sender = -1) {
	LocalVector<uint8_t> values;
	TickTransport::Packet packet;
	while (p_transport->pop_packet(packet)) {
		if (p_expected_sender >= 0) {
			CHECK(packet.from_peer == p_expected_sender);
		}
		REQUIRE(packet.data.size() == 1);
		values.push_back(packet.data[0]);
	}
	return values;
}

TEST_CASE("[Modules][TickSynchronizer][TickLocalTransport] Connection events and topology") {
	TickLocalNetwork network;
	Ref<TickTransport> server = network.add_peer();
	Ref<TickTransport> client_a = network.add_peer();
	Ref<TickTransport> client_b = network.add_peer();
	CHECK(server->get_local_peer_id() == 1);
	CHECK(client_a->get_local_peer_id() == 2);
	CHECK(client_b->get_local_peer_id() == 3);

	// Star: the clients only see the server.
	CHECK(network.connect_peers(1, 2) == OK);
	CHECK(network.connect_peers(1, 3) == OK);
	CHECK(network.connect_peers(1, 2) == ERR_ALREADY_EXISTS);

	LocalVector<int> peers;
	server->get_connected_peers(peers);
	CHECK(peers.size() == 2);
	CHECK(peers[0] == 2);
	CHECK(peers[1] == 3);
	CHECK_FALSE(client_a->is_peer_connected(3));

	TickTransport::Event event;
	CHECK(client_a->pop_event(event));
	CHECK(event.type == TickTransport::EVENT_PEER_CONNECTED);
	CHECK(event.peer == 1);
	CHECK_FALSE(client_a->pop_event(event));

	// A client can't reach another client directly in a star.
	ERR_PRINT_OFF;
	CHECK(client_a->send(3, 0, TickTransport::TRANSFER_MODE_RELIABLE, nullptr, 0) == ERR_UNAVAILABLE);
	ERR_PRINT_ON;

	network.remove_peer(2);
	CHECK(network.get_peer(2).is_null());
	server->get_connected_peers(peers);
	CHECK(peers.size() == 1);
	int disconnected = 0;
	while (server->pop_event(event)) {
		if (event.type == TickTransport::EVENT_PEER_DISCONNECTED) {
			CHECK(event.peer == 2);
			disconnected++;
		}
	}
	CHECK(disconnected == 1);

	// Mesh.
	network.add_peer();
	network.connect_all();
	CHECK(client_b->is_peer_connected(4));
}

TEST_CASE("[Modules][TickSynchronizer][TickLocalTransport] Latency and sender identity") {
	TickLocalNetwork network;
	Ref<TickTransport> a = network.add_peer();
	Ref<TickTransport> b = network.add_peer();
	network.connect_all();
	network.set_latency_usec(50000);

	send_byte(a, 2, 0, TickTransport::TRANSFER_MODE_RELIABLE, 7);
	network.process_usec(49999);
	CHECK(receive_bytes(b).is_empty());
	network.process_usec(1);
	TickTransport::Packet packet;
	REQUIRE(b->pop_packet(packet));
	// The sender comes from the transport.
	CHECK(packet.from_peer == 1);
	CHECK(packet.channel == 0);
	CHECK(packet.mode == TickTransport::TRANSFER_MODE_RELIABLE);
	CHECK(packet.data[0] == 7);

	// Broadcast.
	send_byte(b, TickTransport::PEER_BROADCAST, 1, TickTransport::TRANSFER_MODE_UNRELIABLE, 9);
	network.process(0.05);
	const LocalVector<uint8_t> values = receive_bytes(a, 2);
	REQUIRE(values.size() == 1);
	CHECK(values[0] == 9);

	ERR_PRINT_OFF;
	CHECK(a->send(2, 99, TickTransport::TRANSFER_MODE_RELIABLE, nullptr, 0) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
}

TEST_CASE("[Modules][TickSynchronizer][TickLocalTransport] Reliable packets arrive in order despite jitter") {
	TickLocalNetwork network;
	Ref<TickTransport> a = network.add_peer();
	Ref<TickTransport> b = network.add_peer();
	network.connect_all();
	network.set_seed(42);
	network.set_latency_usec(20000);
	network.set_jitter_usec(30000);
	network.set_packet_loss(0.5);

	for (int i = 0; i < 100; i++) {
		send_byte(a, 2, 0, TickTransport::TRANSFER_MODE_RELIABLE, uint8_t(i));
		network.process_usec(1000);
	}
	network.process(1.0);

	const LocalVector<uint8_t> values = receive_bytes(b, 1);
	REQUIRE(values.size() == 100);
	for (int i = 0; i < 100; i++) {
		CHECK(values[i] == i);
	}
	CHECK(network.get_lost_packets() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][TickLocalTransport] Unreliable packets are lost and reordered") {
	TickLocalNetwork network;
	Ref<TickTransport> a = network.add_peer();
	Ref<TickTransport> b = network.add_peer();
	network.connect_all();
	network.set_seed(7);
	network.set_latency_usec(20000);
	network.set_jitter_usec(30000);
	network.set_packet_loss(0.25);

	for (int i = 0; i < 200; i++) {
		send_byte(a, 2, 1, TickTransport::TRANSFER_MODE_UNRELIABLE, uint8_t(i));
		network.process_usec(1000);
	}
	network.process(1.0);

	const LocalVector<uint8_t> values = receive_bytes(b, 1);
	CHECK(network.get_lost_packets() > 0);
	CHECK(values.size() == 200 - network.get_lost_packets());
	bool reordered = false;
	for (uint32_t i = 1; i < values.size(); i++) {
		reordered = reordered || values[i] < values[i - 1];
	}
	CHECK(reordered);
}

TEST_CASE("[Modules][TickSynchronizer][TickLocalTransport] Unreliable ordered packets never go back in time") {
	TickLocalNetwork network;
	Ref<TickTransport> a = network.add_peer();
	Ref<TickTransport> b = network.add_peer();
	network.connect_all();
	network.set_seed(3);
	network.set_latency_usec(20000);
	network.set_jitter_usec(30000);

	for (int i = 0; i < 200; i++) {
		send_byte(a, 2, 3, TickTransport::TRANSFER_MODE_UNRELIABLE_ORDERED, uint8_t(i));
		network.process_usec(1000);
	}
	network.process(1.0);

	const LocalVector<uint8_t> values = receive_bytes(b, 1);
	CHECK(values.size() > 0);
	// With jitter bigger than the send interval some packets arrive late and are dropped.
	CHECK(values.size() < 200);
	for (uint32_t i = 1; i < values.size(); i++) {
		CHECK(values[i] > values[i - 1]);
	}
}

TEST_CASE("[Modules][TickSynchronizer][TickLocalTransport] Disconnection drops in-flight packets") {
	TickLocalNetwork network;
	Ref<TickTransport> a = network.add_peer();
	Ref<TickTransport> b = network.add_peer();
	network.connect_all();
	network.set_latency_usec(10000);

	send_byte(a, 2, 0, TickTransport::TRANSFER_MODE_RELIABLE, 1);
	CHECK(network.get_in_flight_count() == 1);
	network.disconnect_peers(1, 2);
	CHECK(network.get_in_flight_count() == 0);
	network.process(1.0);
	CHECK(receive_bytes(b).is_empty());
}

} // namespace TestTickLocalTransport

// Tests of `EnetStarTransport` over real sockets on the loopback interface: a server and a client connect and exchange
// packets on every channel, with compression and with DTLS.

#pragma once

#include "../source/sync/tick_protocol.h"
#include "../source/transport/enet_star_transport.h"

#include "core/os/os.h"
#include "tests/test_macros.h"

namespace TestEnetStarTransport {

// A port that changes from run to run, so a socket of a previous run that is still closing doesn't get in the way.
inline int pick_port() {
	return 40000 + int(OS::get_singleton()->get_ticks_usec() % 20000);
}


// Polls both transports `p_times` times, a millisecond apart.
inline void poll_both(const Ref<EnetStarTransport> &p_a, const Ref<EnetStarTransport> &p_b, int p_times) {
	for (int i = 0; i < p_times; i++) {
		p_a->poll();
		p_b->poll();
		OS::get_singleton()->delay_usec(1000);
	}
}


// Waits for the client to connect, checks who each side says it is, then sends packets both ways: reliable ones on
// every channel to the server, an unreliable one to the client.
inline void exchange_packets(const Ref<EnetStarTransport> &p_server, const Ref<EnetStarTransport> &p_client) {
	// Connects (up to two seconds).
	for (int i = 0; i < 2000 && !p_client->is_peer_connected(TickTransport::PEER_SERVER); i++) {
		poll_both(p_server, p_client, 1);
	}
	REQUIRE(p_client->is_peer_connected(TickTransport::PEER_SERVER));
	CHECK(p_client->get_local_peer_id() != TickTransport::PEER_SERVER);
	CHECK(p_server->get_local_peer_id() == TickTransport::PEER_SERVER);
	poll_both(p_server, p_client, 50);

	TickTransport::Event event;
	bool server_saw_client = false;
	while (p_server->pop_event(event)) {
		server_saw_client = server_saw_client || (event.type == TickTransport::EVENT_PEER_CONNECTED && event.peer == p_client->get_local_peer_id());
	}
	CHECK(server_saw_client);

	const uint8_t payload[3] = { 7, 8, 9 };
	for (int channel = 0; channel < TICK_CHANNEL_COUNT; channel++) {
		CHECK(p_client->send(TickTransport::PEER_SERVER, channel, TickTransport::TRANSFER_MODE_RELIABLE, payload, 3) == OK);
	}
	CHECK(p_server->send(p_client->get_local_peer_id(), TICK_CHANNEL_STATE, TickTransport::TRANSFER_MODE_UNRELIABLE, payload, 2) == OK);

	int server_received = 0;
	int client_received = 0;
	for (int i = 0; i < 2000 && (server_received < TICK_CHANNEL_COUNT || client_received < 1); i++) {
		p_server->poll();
		p_client->poll();
		TickTransport::Packet packet;
		while (p_server->pop_packet(packet)) {
			CHECK(packet.from_peer == p_client->get_local_peer_id());
			CHECK(packet.data.size() == 3);
			server_received++;
		}
		while (p_client->pop_packet(packet)) {
			CHECK(packet.from_peer == TickTransport::PEER_SERVER);
			CHECK(packet.data.size() == 2);
			client_received++;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(server_received == TICK_CHANNEL_COUNT);
	CHECK(client_received == 1);
}


// A server and a client with range coder compression exchange packets.
TEST_CASE("[Modules][TickSynchronizer][EnetStarTransport] Loopback with compression") {
	const int port = pick_port();
	Ref<EnetStarTransport> server = EnetStarTransport::create_server(port, 4, EnetStarTransport::COMPRESSION_RANGE_CODER);
	REQUIRE(server.is_valid());
	Ref<EnetStarTransport> client = EnetStarTransport::create_client("127.0.0.1", port, EnetStarTransport::COMPRESSION_RANGE_CODER);
	REQUIRE(client.is_valid());
	CHECK(server->get_max_payload_size() > 1300);
	exchange_packets(server, client);
}


// A server and a client exchange packets over DTLS, with a self-signed certificate; the payload shrinks by the DTLS
// overhead.
TEST_CASE("[Modules][TickSynchronizer][EnetStarTransport] Loopback with DTLS") {
	Ref<Crypto> crypto = Crypto::create();
	REQUIRE(crypto.is_valid());
	Ref<CryptoKey> key = crypto->generate_rsa(2048);
	Ref<X509Certificate> certificate = crypto->generate_self_signed_certificate(key, "CN=localhost,O=TickSynchronizer,C=BR", "20250101000000", "20350101000000");
	REQUIRE(certificate.is_valid());

	const int port = pick_port() + 1;
	Ref<EnetStarTransport> server = EnetStarTransport::create_server(port, 4, EnetStarTransport::COMPRESSION_NONE, TLSOptions::server(key, certificate));
	REQUIRE(server.is_valid());
	Ref<EnetStarTransport> client = EnetStarTransport::create_client("127.0.0.1", port, EnetStarTransport::COMPRESSION_NONE, TLSOptions::client_unsafe(certificate));
	REQUIRE(client.is_valid());
	// The DTLS overhead is subtracted from the payload size.
	CHECK(client->get_max_payload_size() < 1350);
	exchange_packets(server, client);
}

} // namespace TestEnetStarTransport

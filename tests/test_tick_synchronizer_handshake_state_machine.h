// Tests initiator and responder handshake state transitions.
// Covers valid flows, protocol violations, disconnects, and terminal metadata.

#ifndef TEST_TICK_SYNCHRONIZER_HANDSHAKE_STATE_MACHINE_H
#define TEST_TICK_SYNCHRONIZER_HANDSHAKE_STATE_MACHINE_H

#include "../src/protocol/tick_synchronizer_handshake_state_machine.h"
#include "tests/test_macros.h"

namespace TestTickSynchronizerHandshakeStateMachine {

using namespace tick_synchronizer;

static ProtocolCompatibilityProfile make_state_machine_profile(
		ProtocolPrecisionMode p_precision = ProtocolPrecisionMode::DOUBLE,
		uint8_t p_seed = 1) {
	ProtocolCompatibilityProfile profile;
	profile.precision = p_precision;
	constexpr char godot_version[] = "4.7.1-stable";
	for (std::size_t index = 0; index < sizeof(godot_version) - 1; index++) {
		profile.godot_version[index] = static_cast<uint8_t>(godot_version[index]);
	}
	for (std::size_t index = 0; index < profile.godot_commit.size(); index++) {
		profile.godot_commit[index] = static_cast<uint8_t>(p_seed + index);
		profile.module_build_id[index] = static_cast<uint8_t>(p_seed + 0x20 + index);
	}
	for (std::size_t index = 0; index < profile.game_build_id.size(); index++) {
		profile.game_build_id[index] = static_cast<uint8_t>(p_seed + 0x40 + index);
		profile.schema_compatibility_id[index] = static_cast<uint8_t>(p_seed + 0x60 + index);
	}
	profile.capabilities.supported = TickSynchronizerPacketCodec::CURRENT_SUPPORTED_CAPABILITIES;
	profile.capabilities.required = TickSynchronizerPacketCodec::CURRENT_REQUIRED_CAPABILITIES;
	return profile;
}

static ProtocolHandshakeMachineConfig make_initiator_config(
		uint64_t p_session_id = UINT64_C(0x1122334455667788),
		uint64_t p_nonce = UINT64_C(0x8877665544332211)) {
	ProtocolHandshakeMachineConfig config;
	config.role = ProtocolHandshakeRole::INITIATOR;
	config.local_profile = make_state_machine_profile();
	config.remote_peer_id = 2;
	config.session_id = p_session_id;
	config.hello_nonce = p_nonce;
	return config;
}

static ProtocolHandshakeMachineConfig make_responder_config() {
	ProtocolHandshakeMachineConfig config;
	config.role = ProtocolHandshakeRole::RESPONDER;
	config.local_profile = make_state_machine_profile();
	config.remote_peer_id = 1;
	return config;
}

static ProtocolPacket require_packet_from_action(const ProtocolHandshakeAction &p_action) {
	ProtocolPacket packet;
	REQUIRE(ProtocolHandshakeStateMachine::build_outbound_packet(p_action, packet) ==
			ProtocolCodecError::OK);
	return packet;
}

static ProtocolPacket make_remote_disconnect_packet(
		uint64_t p_session_id,
		ProtocolDisconnectReason p_reason = ProtocolDisconnectReason::MALFORMED_HANDSHAKE) {
	ProtocolDisconnectPayload disconnect;
	disconnect.reason = p_reason;
	disconnect.sender_precision = ProtocolPrecisionMode::DOUBLE;
	disconnect.required_precision = ProtocolPrecisionMode::DOUBLE;
	disconnect.peer_precision = ProtocolPrecisionMode::DOUBLE;
	disconnect.local_protocol_major = TickSynchronizerPacketCodec::PROTOCOL_MAJOR;
	disconnect.local_protocol_minor = TickSynchronizerPacketCodec::PROTOCOL_MINOR;
	disconnect.peer_protocol_major = TickSynchronizerPacketCodec::PROTOCOL_MAJOR;
	disconnect.peer_protocol_minor = TickSynchronizerPacketCodec::PROTOCOL_MINOR;
	disconnect.peer_id = 1;

	PackedByteArray payload;
	REQUIRE(TickSynchronizerPacketCodec::encode_disconnect_payload(disconnect, payload) ==
			ProtocolCodecError::OK);
	ProtocolPacket packet;
	// Constructs the canonical current control header for a validated payload.
	packet.header = TickSynchronizerPacketCodec::make_header(
			ProtocolPacketType::DISCONNECT_REASON,
			p_session_id,
			0,
			0,
			static_cast<uint32_t>(payload.size()),
			static_cast<uint32_t>(payload.size()) * 8U);
	packet.payload = payload;
	return packet;
}

static void establish_pair(
		ProtocolHandshakeStateMachine &r_initiator,
		ProtocolHandshakeStateMachine &r_responder) {
	ProtocolHandshakeAction initiator_action;
	ProtocolHandshakeAction responder_start_action;
	REQUIRE(r_initiator.start(initiator_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(r_responder.start(responder_start_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolPacket hello_packet = require_packet_from_action(initiator_action);
	ProtocolHandshakeAction responder_action;
	REQUIRE(r_responder.receive_packet(hello_packet, responder_action) ==
			ProtocolHandshakeMachineResult::OK);
	ProtocolPacket ack_packet = require_packet_from_action(responder_action);
	ProtocolHandshakeAction initiator_ack_action;
	REQUIRE(r_initiator.receive_packet(ack_packet, initiator_ack_action) ==
			ProtocolHandshakeMachineResult::OK);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Initiator start emits HELLO") {
	ProtocolHandshakeStateMachine machine(make_initiator_config());
	ProtocolHandshakeAction action;
	CHECK(machine.start(action) == ProtocolHandshakeMachineResult::OK);
	CHECK(machine.get_state() == ProtocolHandshakeState::WAITING_FOR_HELLO_ACK);
	CHECK(machine.get_session_id() == UINT64_C(0x1122334455667788));
	CHECK(action.has_outbound_packet);
	CHECK(action.outbound_packet_type == ProtocolPacketType::HELLO);
	CHECK(action.outbound_session_id == UINT64_C(0x1122334455667788));
	CHECK(action.hello.hello_nonce == UINT64_C(0x8877665544332211));
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Responder start waits for HELLO") {
	ProtocolHandshakeStateMachine machine(make_responder_config());
	ProtocolHandshakeAction action;
	CHECK(machine.start(action) == ProtocolHandshakeMachineResult::OK);
	CHECK(machine.get_state() == ProtocolHandshakeState::WAITING_FOR_HELLO);
	CHECK(machine.get_session_id() == 0);
	CHECK_FALSE(action.has_outbound_packet);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Invalid initiator config is atomic") {
	ProtocolHandshakeMachineConfig config = make_initiator_config();
	config.hello_nonce = 0;
	ProtocolHandshakeStateMachine machine(config);
	ProtocolHandshakeAction action;
	action.has_outbound_packet = true;
	action.outbound_session_id = 99;
	CHECK(machine.start(action) == ProtocolHandshakeMachineResult::INVALID_CONFIGURATION);
	CHECK(machine.get_state() == ProtocolHandshakeState::IDLE);
	CHECK(action.has_outbound_packet);
	CHECK(action.outbound_session_id == 99);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Invalid responder config is rejected") {
	ProtocolHandshakeMachineConfig config = make_responder_config();
	config.session_id = 7;
	ProtocolHandshakeStateMachine machine(config);
	ProtocolHandshakeAction action;
	CHECK(machine.start(action) == ProtocolHandshakeMachineResult::INVALID_CONFIGURATION);
	CHECK(machine.get_state() == ProtocolHandshakeState::IDLE);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Complete handshake establishes both peers") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	establish_pair(initiator, responder);
	CHECK(initiator.is_established());
	CHECK(responder.is_established());
	CHECK(initiator.has_established_handshake());
	CHECK(responder.has_established_handshake());
	CHECK(initiator.get_established_handshake().session_id ==
			// Returns established metadata; callers must first check availability.
			responder.get_established_handshake().session_id);
	CHECK(initiator.get_established_handshake().negotiated_capabilities ==
			TickSynchronizerPacketCodec::CURRENT_SUPPORTED_CAPABILITIES);
	CHECK(responder.get_established_handshake().remote_peer_id == 1);
	CHECK(initiator.get_established_handshake().warning_flags == 0);
	CHECK(responder.get_established_handshake().warning_flags == 0);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Commit mismatch establishes with warnings") {
	ProtocolHandshakeMachineConfig initiator_config = make_initiator_config();
	ProtocolHandshakeMachineConfig responder_config = make_responder_config();
	responder_config.local_profile.godot_commit[0] ^= 0xFF;
	ProtocolHandshakeStateMachine initiator(initiator_config);
	ProtocolHandshakeStateMachine responder(responder_config);

	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction ack_action;
	REQUIRE(responder.receive_packet(require_packet_from_action(hello_action), ack_action) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(ack_action.became_established);
	CHECK(ack_action.warning_flags ==
			static_cast<uint32_t>(ProtocolHandshakeWarning::GODOT_COMMIT_MISMATCH));
	CHECK(responder.get_established_handshake().warning_flags == ack_action.warning_flags);

	ProtocolHandshakeAction initiator_action;
	REQUIRE(initiator.receive_packet(require_packet_from_action(ack_action), initiator_action) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(initiator_action.became_established);
	CHECK(initiator_action.warning_flags ==
			static_cast<uint32_t>(ProtocolHandshakeWarning::GODOT_COMMIT_MISMATCH));
	CHECK(initiator.get_established_handshake().warning_flags == initiator_action.warning_flags);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Godot version mismatch is rejected") {
	ProtocolHandshakeMachineConfig responder_config = make_responder_config();
	responder_config.local_profile.godot_version[4] = '2';
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(responder_config);
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(require_packet_from_action(hello_action), reject_action) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(responder.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK(reject_action.disconnect.reason == ProtocolDisconnectReason::GODOT_VERSION_MISMATCH);
	CHECK(reject_action.disconnect.identity_field == ProtocolIdentityField::GODOT_VERSION);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Outbound action builds canonical packet") {
	ProtocolHandshakeStateMachine machine(make_initiator_config());
	ProtocolHandshakeAction action;
	REQUIRE(machine.start(action) == ProtocolHandshakeMachineResult::OK);
	ProtocolPacket packet;
	CHECK(ProtocolHandshakeStateMachine::build_outbound_packet(action, packet) ==
			ProtocolCodecError::OK);
	CHECK(packet.header.packet_type == ProtocolPacketType::HELLO);
	CHECK(packet.header.sequence == 0);
	CHECK(packet.header.tick == 0);
	CHECK(packet.header.payload_size_bytes == TickSynchronizerPacketCodec::HELLO_PAYLOAD_SIZE);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Empty action cannot build packet atomically") {
	ProtocolHandshakeAction action;
	ProtocolPacket packet;
	packet.header.session_id = 77;
	CHECK(ProtocolHandshakeStateMachine::build_outbound_packet(action, packet) ==
			ProtocolCodecError::INVALID_ARGUMENT);
	CHECK(packet.header.session_id == 77);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Precision mismatch rejects both peers") {
	ProtocolHandshakeMachineConfig responder_config = make_responder_config();
	responder_config.local_profile.precision = ProtocolPrecisionMode::SINGLE;
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(responder_config);
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(require_packet_from_action(hello_action), reject_action) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(responder.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK(reject_action.disconnect.reason == ProtocolDisconnectReason::PRECISION_MISMATCH);
	ProtocolHandshakeAction initiator_reject;
	REQUIRE(initiator.receive_packet(require_packet_from_action(reject_action), initiator_reject) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(initiator.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK(initiator_reject.remote_disconnect);
	CHECK(initiator_reject.disconnect.reason == ProtocolDisconnectReason::PRECISION_MISMATCH);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] ACK nonce mismatch is rejected") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction ack_action;
	REQUIRE(responder.receive_packet(require_packet_from_action(hello_action), ack_action) ==
			ProtocolHandshakeMachineResult::OK);
	ack_action.hello_ack.echoed_hello_nonce ^= 1;
	ProtocolHandshakeAction reject_action;
	REQUIRE(initiator.receive_packet(require_packet_from_action(ack_action), reject_action) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(initiator.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK(reject_action.disconnect.reason == ProtocolDisconnectReason::HELLO_NONCE_MISMATCH);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] ACK session mismatch is rejected") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeAction hello_action;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolHelloAckPayload ack;
	ack.echoed_hello_nonce = hello_action.hello.hello_nonce;
	ack.profile = make_state_machine_profile();
	ack.negotiated_capabilities = TickSynchronizerPacketCodec::CURRENT_SUPPORTED_CAPABILITIES;
	PackedByteArray payload;
	REQUIRE(TickSynchronizerPacketCodec::encode_hello_ack_payload(ack, payload) ==
			ProtocolCodecError::OK);
	ProtocolPacket packet;
	// Constructs the canonical current control header for a validated payload.
	packet.header = TickSynchronizerPacketCodec::make_header(
			ProtocolPacketType::HELLO_ACK,
			UINT64_C(0xAABBCCDD),
			0,
			0,
			static_cast<uint32_t>(payload.size()),
			static_cast<uint32_t>(payload.size()) * 8U);
	packet.payload = payload;
	ProtocolHandshakeAction reject_action;
	REQUIRE(initiator.receive_packet(packet, reject_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(reject_action.disconnect.detail_code ==
			static_cast<uint32_t>(ProtocolHandshakeViolation::SESSION_ID_MISMATCH));
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Zero session HELLO is rejected without reply") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	ProtocolPacket hello_packet = require_packet_from_action(hello_action);
	hello_packet.header.session_id = 0;
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(hello_packet, reject_action) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(responder.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK_FALSE(reject_action.has_outbound_packet);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Nonzero handshake sequence is rejected") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	ProtocolPacket packet = require_packet_from_action(hello_action);
	packet.header.sequence = 1;
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(packet, reject_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(reject_action.disconnect.detail_code ==
			static_cast<uint32_t>(ProtocolHandshakeViolation::NONZERO_SEQUENCE));
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Nonzero handshake tick is rejected") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	ProtocolPacket packet = require_packet_from_action(hello_action);
	packet.header.tick = 1;
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(packet, reject_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(reject_action.disconnect.detail_code ==
			static_cast<uint32_t>(ProtocolHandshakeViolation::NONZERO_TICK));
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Packet before start preserves state and action") {
	ProtocolHandshakeStateMachine machine(make_initiator_config());
	ProtocolHandshakeAction action;
	action.outbound_session_id = 99;
	ProtocolPacket packet = make_remote_disconnect_packet(UINT64_C(7));
	CHECK(machine.receive_packet(packet, action) == ProtocolHandshakeMachineResult::INVALID_STATE);
	CHECK(machine.get_state() == ProtocolHandshakeState::IDLE);
	CHECK(action.outbound_session_id == 99);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Initiator rejects unexpected HELLO") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeAction local_hello;
	REQUIRE(initiator.start(local_hello) == ProtocolHandshakeMachineResult::OK);
	ProtocolPacket packet = require_packet_from_action(local_hello);
	ProtocolHandshakeAction reject_action;
	REQUIRE(initiator.receive_packet(packet, reject_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(initiator.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK(reject_action.disconnect.detail_code ==
			static_cast<uint32_t>(ProtocolHandshakeViolation::UNEXPECTED_PACKET_TYPE));
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Responder rejects unexpected ACK") {
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction start_action;
	REQUIRE(responder.start(start_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolHelloAckPayload ack;
	ack.echoed_hello_nonce = 7;
	ack.profile = make_state_machine_profile();
	ack.negotiated_capabilities = TickSynchronizerPacketCodec::CURRENT_SUPPORTED_CAPABILITIES;
	ProtocolHandshakeAction action;
	action.has_outbound_packet = true;
	action.outbound_packet_type = ProtocolPacketType::HELLO_ACK;
	action.outbound_session_id = 9;
	action.hello_ack = ack;
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(require_packet_from_action(action), reject_action) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(responder.get_state() == ProtocolHandshakeState::REJECTED);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Duplicate HELLO after establishment is fatal") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	ProtocolPacket hello_packet = require_packet_from_action(hello_action);
	ProtocolHandshakeAction ack_action;
	REQUIRE(responder.receive_packet(hello_packet, ack_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction duplicate_action;
	REQUIRE(responder.receive_packet(hello_packet, duplicate_action) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(responder.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK(duplicate_action.disconnect.detail_code ==
			static_cast<uint32_t>(ProtocolHandshakeViolation::DUPLICATE_HANDSHAKE_MESSAGE));
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Duplicate ACK after establishment is fatal") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction ack_action;
	REQUIRE(responder.receive_packet(require_packet_from_action(hello_action), ack_action) ==
			ProtocolHandshakeMachineResult::OK);
	ProtocolPacket ack_packet = require_packet_from_action(ack_action);
	ProtocolHandshakeAction established_action;
	REQUIRE(initiator.receive_packet(ack_packet, established_action) ==
			ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction duplicate_action;
	REQUIRE(initiator.receive_packet(ack_packet, duplicate_action) ==
			ProtocolHandshakeMachineResult::OK);
	CHECK(initiator.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK(duplicate_action.disconnect.detail_code ==
			static_cast<uint32_t>(ProtocolHandshakeViolation::DUPLICATE_HANDSHAKE_MESSAGE));
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Remote disconnect terminates handshake") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeAction hello_action;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction disconnect_action;
	REQUIRE(initiator.receive_packet(
			make_remote_disconnect_packet(hello_action.outbound_session_id),
			disconnect_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(initiator.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK(disconnect_action.remote_disconnect);
	CHECK_FALSE(disconnect_action.has_outbound_packet);
	CHECK(initiator.was_terminal_disconnect_remote());
	CHECK(disconnect_action.disconnect.reason == ProtocolDisconnectReason::MALFORMED_HANDSHAKE);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Malformed disconnect terminates without reply") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeAction hello_action;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolPacket packet = make_remote_disconnect_packet(hello_action.outbound_session_id);
	packet.payload.resize_initialized(1);
	packet.header.payload_size_bytes = 1;
	packet.header.payload_bit_size = 8;
	ProtocolHandshakeAction reject_action;
	REQUIRE(initiator.receive_packet(packet, reject_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(initiator.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK_FALSE(reject_action.has_outbound_packet);
	CHECK_FALSE(initiator.was_terminal_disconnect_remote());
	CHECK(initiator.get_terminal_disconnect().detail_code ==
			static_cast<uint32_t>(ProtocolHandshakeViolation::INVALID_DISCONNECT_PAYLOAD));
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Initiator cancellation emits disconnect") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeAction start_action;
	REQUIRE(initiator.start(start_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction cancel_action;
	REQUIRE(initiator.cancel(
			ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
			77,
			cancel_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(initiator.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK(cancel_action.has_outbound_packet);
	CHECK(cancel_action.disconnect.detail_code == 77);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Responder cancellation before HELLO is local only") {
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction start_action;
	REQUIRE(responder.start(start_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction cancel_action;
	REQUIRE(responder.cancel(
			ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
			5,
			cancel_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(responder.get_state() == ProtocolHandshakeState::REJECTED);
	CHECK_FALSE(cancel_action.has_outbound_packet);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Invalid cancel reason is atomic") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeAction start_action;
	REQUIRE(initiator.start(start_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction cancel_action;
	cancel_action.outbound_session_id = 88;
	CHECK(initiator.cancel(ProtocolDisconnectReason::NONE, 0, cancel_action) ==
			ProtocolHandshakeMachineResult::INVALID_ARGUMENT);
	CHECK(initiator.get_state() == ProtocolHandshakeState::WAITING_FOR_HELLO_ACK);
	CHECK(cancel_action.outbound_session_id == 88);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Close is idempotent") {
	ProtocolHandshakeStateMachine machine(make_initiator_config());
	ProtocolHandshakeAction first;
	CHECK(machine.close(first) == ProtocolHandshakeMachineResult::OK);
	CHECK(first.became_closed);
	CHECK(machine.get_state() == ProtocolHandshakeState::CLOSED);
	ProtocolHandshakeAction second;
	second.became_closed = true;
	CHECK(machine.close(second) == ProtocolHandshakeMachineResult::OK);
	CHECK_FALSE(second.became_closed);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Machine cannot restart") {
	ProtocolHandshakeStateMachine machine(make_initiator_config());
	ProtocolHandshakeAction first;
	REQUIRE(machine.start(first) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction second;
	CHECK(machine.start(second) == ProtocolHandshakeMachineResult::INVALID_STATE);
	CHECK(machine.get_state() == ProtocolHandshakeState::WAITING_FOR_HELLO_ACK);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Terminal machine rejects further packets") {
	ProtocolHandshakeStateMachine machine(make_initiator_config());
	ProtocolHandshakeAction start_action;
	REQUIRE(machine.start(start_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction cancel_action;
	REQUIRE(machine.cancel(
			ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
			0,
			cancel_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolHandshakeAction output;
	CHECK(machine.receive_packet(make_remote_disconnect_packet(machine.get_session_id()), output) ==
			ProtocolHandshakeMachineResult::INVALID_STATE);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] API mismatch creates structured disconnect") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	hello_action.hello.profile.api_version += 1;
	ProtocolPacket packet = require_packet_from_action(hello_action);
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(packet, reject_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(reject_action.disconnect.reason == ProtocolDisconnectReason::API_VERSION_MISMATCH);
	CHECK(reject_action.disconnect.local_api_version == version::API_VERSION);
	CHECK(reject_action.disconnect.peer_api_version == version::API_VERSION + 1);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Wire version mismatch creates structured disconnect") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	hello_action.hello.profile.wire_protocol_version = 1;
	hello_action.hello.profile.wire_protocol_revision = 0;
	ProtocolPacket packet = require_packet_from_action(hello_action);
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(packet, reject_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(reject_action.disconnect.reason ==
			ProtocolDisconnectReason::WIRE_PROTOCOL_VERSION_MISMATCH);
	CHECK(reject_action.disconnect.peer_wire_protocol_version == 1);
	CHECK(reject_action.disconnect.peer_wire_protocol_revision == 0);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Wire revision mismatch creates structured disconnect") {
	ProtocolHandshakeStateMachine initiator(make_initiator_config());
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction hello_action;
	ProtocolHandshakeAction responder_start;
	REQUIRE(initiator.start(hello_action) == ProtocolHandshakeMachineResult::OK);
	REQUIRE(responder.start(responder_start) == ProtocolHandshakeMachineResult::OK);
	hello_action.hello.profile.wire_protocol_revision += 1;
	ProtocolPacket packet = require_packet_from_action(hello_action);
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(packet, reject_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(reject_action.disconnect.reason ==
			ProtocolDisconnectReason::WIRE_PROTOCOL_REVISION_MISMATCH);
	CHECK(reject_action.disconnect.peer_wire_protocol_revision ==
			version::WIRE_PROTOCOL_REVISION + 1);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Version mismatch creates structured disconnect") {
	ProtocolHandshakeStateMachine responder(make_responder_config());
	ProtocolHandshakeAction start_action;
	REQUIRE(responder.start(start_action) == ProtocolHandshakeMachineResult::OK);
	ProtocolPacket packet = make_remote_disconnect_packet(9);
	packet.header.protocol_minor += 1;
	ProtocolHandshakeAction reject_action;
	REQUIRE(responder.receive_packet(packet, reject_action) == ProtocolHandshakeMachineResult::OK);
	CHECK(reject_action.disconnect.reason == ProtocolDisconnectReason::PROTOCOL_VERSION_MISMATCH);
	CHECK(reject_action.disconnect.peer_protocol_minor ==
			TickSynchronizerPacketCodec::PROTOCOL_MINOR + 1);
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Deterministic handshake stress") {
	for (uint64_t index = 1; index <= 4096; index++) {
		const uint64_t session = UINT64_C(0x1000000000000000) + index;
		const uint64_t nonce = UINT64_C(0x2000000000000000) + index;
		ProtocolHandshakeStateMachine initiator(make_initiator_config(session, nonce));
		ProtocolHandshakeStateMachine responder(make_responder_config());
		establish_pair(initiator, responder);
		CHECK(initiator.is_established());
		CHECK(responder.is_established());
		CHECK(initiator.get_session_id() == session);
		CHECK(responder.get_session_id() == session);
	}
}

TEST_CASE("[Modules][TickSynchronizer][HandshakeState] Diagnostic names are stable") {
	CHECK(String(ProtocolHandshakeStateMachine::get_role_name(
			ProtocolHandshakeRole::INITIATOR)) == "INITIATOR");
	CHECK(String(ProtocolHandshakeStateMachine::get_state_name(
			ProtocolHandshakeState::ESTABLISHED)) == "ESTABLISHED");
	CHECK(String(ProtocolHandshakeStateMachine::get_result_name(
			ProtocolHandshakeMachineResult::INVALID_STATE)) == "INVALID_STATE");
	CHECK(String(ProtocolHandshakeStateMachine::get_violation_name(
			ProtocolHandshakeViolation::SESSION_ID_MISMATCH)) == "SESSION_ID_MISMATCH");
}

} // namespace TestTickSynchronizerHandshakeStateMachine

#endif // TEST_TICK_SYNCHRONIZER_HANDSHAKE_STATE_MACHINE_H

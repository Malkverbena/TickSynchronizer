#pragma once

#include "tests/test_macros.h"

#include "src/protocol/tick_synchronizer_handshake.h"

#include <cstdint>

namespace TestTickSynchronizerHandshake {

using namespace tick_synchronizer;

static ProtocolSha1 make_sha1(uint8_t p_start) {
	ProtocolSha1 value = {};
	for (std::size_t index = 0; index < value.size(); index++) {
		value[index] = static_cast<uint8_t>(p_start + index);
	}
	return value;
}

static ProtocolOpaqueId make_opaque_id(uint8_t p_start) {
	ProtocolOpaqueId value = {};
	for (std::size_t index = 0; index < value.size(); index++) {
		value[index] = static_cast<uint8_t>(p_start + index);
	}
	return value;
}

static ProtocolCompatibilityProfile make_profile(
		ProtocolPrecisionMode p_precision = ProtocolPrecisionMode::DOUBLE,
		uint64_t p_supported = TickSynchronizerPacketCodec::CURRENT_SUPPORTED_CAPABILITIES,
		uint64_t p_required = TickSynchronizerPacketCodec::CURRENT_REQUIRED_CAPABILITIES) {
	ProtocolCompatibilityProfile profile;
	profile.precision = p_precision;
	profile.godot_commit = make_sha1(0x01);
	profile.module_build_id = make_sha1(0x21);
	profile.game_build_id = make_opaque_id(0x41);
	profile.schema_compatibility_id = make_opaque_id(0x61);
	profile.capabilities.supported = p_supported;
	profile.capabilities.required = p_required;
	return profile;
}

static ProtocolHelloPayload make_hello(const ProtocolCompatibilityProfile &p_profile) {
	ProtocolHelloPayload hello;
	hello.hello_nonce = UINT64_C(0x1122334455667788);
	hello.profile = p_profile;
	return hello;
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Complete profile is well formed") {
	CHECK(ProtocolHandshakeEvaluator::is_profile_well_formed(make_profile()));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Zero identities are rejected") {
	ProtocolCompatibilityProfile profile = make_profile();
	profile.godot_commit = {};
	CHECK_FALSE(ProtocolHandshakeEvaluator::is_profile_well_formed(profile));
	profile = make_profile();
	profile.module_build_id = {};
	CHECK_FALSE(ProtocolHandshakeEvaluator::is_profile_well_formed(profile));
	profile = make_profile();
	profile.game_build_id = {};
	CHECK_FALSE(ProtocolHandshakeEvaluator::is_profile_well_formed(profile));
	profile = make_profile();
	profile.schema_compatibility_id = {};
	CHECK_FALSE(ProtocolHandshakeEvaluator::is_profile_well_formed(profile));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Required capabilities must be locally supported") {
	ProtocolCompatibilityProfile profile = make_profile(ProtocolPrecisionMode::DOUBLE, UINT64_C(1), UINT64_C(3));
	CHECK_FALSE(ProtocolHandshakeEvaluator::is_profile_well_formed(profile));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Matching profiles are accepted") {
	const ProtocolCompatibilityProfile local = make_profile();
	const ProtocolCompatibilityProfile remote = make_profile();
	const ProtocolHandshakeEvaluation evaluation = ProtocolHandshakeEvaluator::evaluate_profiles(local, remote);
	CHECK(evaluation.result == ProtocolHandshakeResult::ACCEPTED);
	CHECK(evaluation.negotiated_capabilities == UINT64_C(7));
	CHECK(evaluation.missing_capabilities == 0);
	CHECK(evaluation.identity_field == ProtocolIdentityField::NONE);
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Unknown optional remote capabilities are ignored safely") {
	ProtocolCompatibilityProfile local = make_profile();
	ProtocolCompatibilityProfile remote = make_profile();
	remote.capabilities.supported |= UINT64_C(1) << 63;
	const ProtocolHandshakeEvaluation evaluation = ProtocolHandshakeEvaluator::evaluate_profiles(local, remote);
	CHECK(evaluation.result == ProtocolHandshakeResult::ACCEPTED);
	CHECK(evaluation.negotiated_capabilities == UINT64_C(7));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Precision mismatch is rejected") {
	const ProtocolHandshakeEvaluation evaluation = ProtocolHandshakeEvaluator::evaluate_profiles(
			make_profile(ProtocolPrecisionMode::DOUBLE),
			make_profile(ProtocolPrecisionMode::SINGLE));
	CHECK(evaluation.result == ProtocolHandshakeResult::PRECISION_MISMATCH);
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Godot commit mismatch is classified") {
	ProtocolCompatibilityProfile remote = make_profile();
	remote.godot_commit[0] ^= 0xFF;
	const ProtocolHandshakeEvaluation evaluation = ProtocolHandshakeEvaluator::evaluate_profiles(make_profile(), remote);
	CHECK(evaluation.result == ProtocolHandshakeResult::GODOT_COMMIT_MISMATCH);
	CHECK(evaluation.identity_field == ProtocolIdentityField::GODOT_COMMIT);
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Module build mismatch is classified") {
	ProtocolCompatibilityProfile remote = make_profile();
	remote.module_build_id[3] ^= 0xFF;
	const ProtocolHandshakeEvaluation evaluation = ProtocolHandshakeEvaluator::evaluate_profiles(make_profile(), remote);
	CHECK(evaluation.result == ProtocolHandshakeResult::MODULE_BUILD_MISMATCH);
	CHECK(evaluation.identity_field == ProtocolIdentityField::MODULE_BUILD);
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Game build mismatch is classified") {
	ProtocolCompatibilityProfile remote = make_profile();
	remote.game_build_id[5] ^= 0xFF;
	const ProtocolHandshakeEvaluation evaluation = ProtocolHandshakeEvaluator::evaluate_profiles(make_profile(), remote);
	CHECK(evaluation.result == ProtocolHandshakeResult::GAME_BUILD_MISMATCH);
	CHECK(evaluation.identity_field == ProtocolIdentityField::GAME_BUILD);
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Schema mismatch is classified") {
	ProtocolCompatibilityProfile remote = make_profile();
	remote.schema_compatibility_id[7] ^= 0xFF;
	const ProtocolHandshakeEvaluation evaluation = ProtocolHandshakeEvaluator::evaluate_profiles(make_profile(), remote);
	CHECK(evaluation.result == ProtocolHandshakeResult::SCHEMA_MISMATCH);
	CHECK(evaluation.identity_field == ProtocolIdentityField::SCHEMA_COMPATIBILITY);
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Local required capabilities must be supported remotely") {
	ProtocolCompatibilityProfile local = make_profile(ProtocolPrecisionMode::DOUBLE, UINT64_C(7), UINT64_C(5));
	ProtocolCompatibilityProfile remote = make_profile(ProtocolPrecisionMode::DOUBLE, UINT64_C(3), UINT64_C(3));
	const ProtocolHandshakeEvaluation evaluation = ProtocolHandshakeEvaluator::evaluate_profiles(local, remote);
	CHECK(evaluation.result == ProtocolHandshakeResult::CAPABILITY_MISMATCH);
	CHECK(evaluation.missing_capabilities == UINT64_C(4));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Remote required capabilities must be supported locally") {
	ProtocolCompatibilityProfile local = make_profile(ProtocolPrecisionMode::DOUBLE, UINT64_C(3), UINT64_C(3));
	ProtocolCompatibilityProfile remote = make_profile(ProtocolPrecisionMode::DOUBLE, UINT64_C(7), UINT64_C(5));
	const ProtocolHandshakeEvaluation evaluation = ProtocolHandshakeEvaluator::evaluate_profiles(local, remote);
	CHECK(evaluation.result == ProtocolHandshakeResult::CAPABILITY_MISMATCH);
	CHECK(evaluation.missing_capabilities == UINT64_C(4));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] HELLO evaluation produces correlated ACK") {
	const ProtocolCompatibilityProfile local = make_profile();
	const ProtocolHelloPayload remote = make_hello(make_profile());
	ProtocolHelloAckPayload ack;
	ProtocolDisconnectPayload disconnect;
	disconnect.peer_id = UINT64_C(0xDEADBEEF);
	CHECK(ProtocolHandshakeEvaluator::evaluate_hello(local, remote, 77, ack, disconnect) ==
			ProtocolHandshakeResult::ACCEPTED);
	CHECK(ack.echoed_hello_nonce == remote.hello_nonce);
	CHECK(ack.profile.module_build_id == local.module_build_id);
	CHECK(ack.negotiated_capabilities == UINT64_C(7));
	CHECK(disconnect.peer_id == UINT64_C(0xDEADBEEF));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Zero HELLO nonce is a structured malformed handshake") {
	ProtocolHelloPayload remote = make_hello(make_profile());
	remote.hello_nonce = 0;
	ProtocolHelloAckPayload ack;
	ack.echoed_hello_nonce = UINT64_C(0xCAFEBABE);
	ProtocolDisconnectPayload disconnect;
	CHECK(ProtocolHandshakeEvaluator::evaluate_hello(make_profile(), remote, 91, ack, disconnect) ==
			ProtocolHandshakeResult::MALFORMED_REMOTE_HANDSHAKE);
	CHECK(ack.echoed_hello_nonce == UINT64_C(0xCAFEBABE));
	CHECK(disconnect.reason == ProtocolDisconnectReason::MALFORMED_HANDSHAKE);
	CHECK(disconnect.detail_code == 1);
	CHECK(disconnect.peer_id == UINT64_C(91));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Malformed remote profile is rejected atomically") {
	ProtocolHelloPayload remote = make_hello(make_profile());
	remote.profile.game_build_id = {};
	ProtocolHelloAckPayload ack;
	ack.negotiated_capabilities = UINT64_C(0xA5);
	ProtocolDisconnectPayload disconnect;
	CHECK(ProtocolHandshakeEvaluator::evaluate_hello(make_profile(), remote, 12, ack, disconnect) ==
			ProtocolHandshakeResult::MALFORMED_REMOTE_HANDSHAKE);
	CHECK(ack.negotiated_capabilities == UINT64_C(0xA5));
	CHECK(disconnect.reason == ProtocolDisconnectReason::MALFORMED_HANDSHAKE);
	CHECK(disconnect.detail_code == 2);
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Identity failure produces actionable disconnect") {
	ProtocolHelloPayload remote = make_hello(make_profile());
	remote.profile.module_build_id[0] ^= 0x80;
	ProtocolHelloAckPayload ack;
	ProtocolDisconnectPayload disconnect;
	CHECK(ProtocolHandshakeEvaluator::evaluate_hello(make_profile(), remote, 55, ack, disconnect) ==
			ProtocolHandshakeResult::MODULE_BUILD_MISMATCH);
	CHECK(disconnect.reason == ProtocolDisconnectReason::MODULE_BUILD_MISMATCH);
	CHECK(disconnect.identity_field == ProtocolIdentityField::MODULE_BUILD);
	CHECK(disconnect.peer_id == UINT64_C(55));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Capability failure reports missing mask and direction") {
	ProtocolCompatibilityProfile local = make_profile(ProtocolPrecisionMode::DOUBLE, UINT64_C(7), UINT64_C(5));
	ProtocolCompatibilityProfile remote_profile = make_profile(ProtocolPrecisionMode::DOUBLE, UINT64_C(3), UINT64_C(3));
	ProtocolHelloAckPayload ack;
	ProtocolDisconnectPayload disconnect;
	CHECK(ProtocolHandshakeEvaluator::evaluate_hello(local, make_hello(remote_profile), 88, ack, disconnect) ==
			ProtocolHandshakeResult::CAPABILITY_MISMATCH);
	CHECK(disconnect.reason == ProtocolDisconnectReason::CAPABILITY_MISMATCH);
	CHECK(disconnect.detail_mask == UINT64_C(4));
	CHECK(disconnect.detail_code == 1);
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Valid HELLO_ACK is accepted atomically") {
	const ProtocolCompatibilityProfile local = make_profile();
	ProtocolHelloAckPayload ack;
	ack.echoed_hello_nonce = UINT64_C(0x1122334455667788);
	ack.profile = make_profile();
	ack.negotiated_capabilities = UINT64_C(7);
	uint64_t negotiated = UINT64_C(0xDEADBEEF);
	ProtocolDisconnectPayload disconnect;
	disconnect.peer_id = UINT64_C(0xCAFEBABE);
	CHECK(ProtocolHandshakeEvaluator::validate_hello_ack(
			local,
			UINT64_C(0x1122334455667788),
			ack,
			9,
			negotiated,
			disconnect) == ProtocolHandshakeResult::ACCEPTED);
	CHECK(negotiated == UINT64_C(7));
	CHECK(disconnect.peer_id == UINT64_C(0xCAFEBABE));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] HELLO_ACK nonce mismatch preserves negotiated output") {
	ProtocolHelloAckPayload ack;
	ack.echoed_hello_nonce = UINT64_C(2);
	ack.profile = make_profile();
	ack.negotiated_capabilities = UINT64_C(7);
	uint64_t negotiated = UINT64_C(0xDEADBEEF);
	ProtocolDisconnectPayload disconnect;
	CHECK(ProtocolHandshakeEvaluator::validate_hello_ack(
			make_profile(),
			UINT64_C(1),
			ack,
			33,
			negotiated,
			disconnect) == ProtocolHandshakeResult::HELLO_NONCE_MISMATCH);
	CHECK(negotiated == UINT64_C(0xDEADBEEF));
	CHECK(disconnect.reason == ProtocolDisconnectReason::HELLO_NONCE_MISMATCH);
	CHECK(disconnect.peer_id == UINT64_C(33));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] HELLO_ACK negotiated mask must be exact intersection") {
	ProtocolHelloAckPayload ack;
	ack.echoed_hello_nonce = UINT64_C(1);
	ack.profile = make_profile();
	ack.negotiated_capabilities = UINT64_C(3);
	uint64_t negotiated = UINT64_C(0xDEADBEEF);
	ProtocolDisconnectPayload disconnect;
	CHECK(ProtocolHandshakeEvaluator::validate_hello_ack(
			make_profile(),
			UINT64_C(1),
			ack,
			44,
			negotiated,
			disconnect) == ProtocolHandshakeResult::NEGOTIATED_CAPABILITIES_MISMATCH);
	CHECK(negotiated == UINT64_C(0xDEADBEEF));
	CHECK(disconnect.reason == ProtocolDisconnectReason::MALFORMED_HANDSHAKE);
	CHECK(disconnect.detail_code == 4);
	CHECK(disconnect.detail_mask == UINT64_C(4));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] HELLO_ACK profile mismatch is rejected") {
	ProtocolHelloAckPayload ack;
	ack.echoed_hello_nonce = UINT64_C(1);
	ack.profile = make_profile();
	ack.profile.schema_compatibility_id[0] ^= 0xFF;
	ack.negotiated_capabilities = UINT64_C(7);
	uint64_t negotiated = 0;
	ProtocolDisconnectPayload disconnect;
	CHECK(ProtocolHandshakeEvaluator::validate_hello_ack(
			make_profile(),
			UINT64_C(1),
			ack,
			66,
			negotiated,
			disconnect) == ProtocolHandshakeResult::SCHEMA_MISMATCH);
	CHECK(disconnect.reason == ProtocolDisconnectReason::SCHEMA_MISMATCH);
	CHECK(disconnect.identity_field == ProtocolIdentityField::SCHEMA_COMPATIBILITY);
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Incompatible header creates version disconnect") {
	ProtocolPacketHeader observed;
	observed.protocol_major = 2;
	observed.protocol_minor = 9;
	const ProtocolDisconnectPayload disconnect = ProtocolHandshakeEvaluator::make_protocol_version_disconnect(
			make_profile(),
			observed,
			UINT64_C(77));
	CHECK(disconnect.reason == ProtocolDisconnectReason::PROTOCOL_VERSION_MISMATCH);
	CHECK(disconnect.local_protocol_major == 1);
	CHECK(disconnect.local_protocol_minor == 1);
	CHECK(disconnect.peer_protocol_major == 2);
	CHECK(disconnect.peer_protocol_minor == 9);
	CHECK(disconnect.peer_id == UINT64_C(77));
}

TEST_CASE("[Modules][TickSynchronizer][Handshake] Result names are stable") {
	CHECK(String(ProtocolHandshakeEvaluator::get_result_name(ProtocolHandshakeResult::ACCEPTED)) == "ACCEPTED");
	CHECK(String(ProtocolHandshakeEvaluator::get_result_name(
			ProtocolHandshakeResult::CAPABILITY_MISMATCH)) == "CAPABILITY_MISMATCH");
	CHECK(String(ProtocolHandshakeEvaluator::get_result_name(
			ProtocolHandshakeResult::NEGOTIATED_CAPABILITIES_MISMATCH)) ==
			"NEGOTIATED_CAPABILITIES_MISMATCH");
}

} // namespace TestTickSynchronizerHandshake

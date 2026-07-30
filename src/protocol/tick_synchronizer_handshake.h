#pragma once

#include "tick_synchronizer_packet_codec.h"

#include <cstdint>

namespace tick_synchronizer {

enum class ProtocolHandshakeResult : uint16_t {
	ACCEPTED = 0,
	INVALID_LOCAL_PROFILE,
	MALFORMED_REMOTE_HANDSHAKE,
	PROTOCOL_VERSION_MISMATCH,
	PRECISION_MISMATCH,
	GODOT_COMMIT_MISMATCH,
	MODULE_BUILD_MISMATCH,
	GAME_BUILD_MISMATCH,
	SCHEMA_MISMATCH,
	CAPABILITY_MISMATCH,
	HELLO_NONCE_MISMATCH,
	NEGOTIATED_CAPABILITIES_MISMATCH,
};

struct ProtocolHandshakeEvaluation {
	ProtocolHandshakeResult result = ProtocolHandshakeResult::INVALID_LOCAL_PROFILE;
	uint64_t negotiated_capabilities = 0;
	uint64_t missing_capabilities = 0;
	ProtocolIdentityField identity_field = ProtocolIdentityField::NONE;
};

class ProtocolHandshakeEvaluator {
public:
	static bool is_zero_sha1(const ProtocolSha1 &p_value);
	static bool is_zero_opaque_id(const ProtocolOpaqueId &p_value);
	static bool is_profile_well_formed(const ProtocolCompatibilityProfile &p_profile);

	static ProtocolHandshakeEvaluation evaluate_profiles(
			const ProtocolCompatibilityProfile &p_local,
			const ProtocolCompatibilityProfile &p_remote);

	static ProtocolHandshakeResult evaluate_hello(
			const ProtocolCompatibilityProfile &p_local,
			const ProtocolHelloPayload &p_remote_hello,
			uint64_t p_peer_id,
			ProtocolHelloAckPayload &r_ack,
			ProtocolDisconnectPayload &r_disconnect);

	static ProtocolHandshakeResult validate_hello_ack(
			const ProtocolCompatibilityProfile &p_local,
			uint64_t p_expected_hello_nonce,
			const ProtocolHelloAckPayload &p_remote_ack,
			uint64_t p_peer_id,
			uint64_t &r_negotiated_capabilities,
			ProtocolDisconnectPayload &r_disconnect);

	static ProtocolDisconnectPayload make_protocol_version_disconnect(
			const ProtocolCompatibilityProfile &p_local,
			const ProtocolPacketHeader &p_observed_header,
			uint64_t p_peer_id);

	static const char *get_result_name(ProtocolHandshakeResult p_result);
};

} // namespace tick_synchronizer

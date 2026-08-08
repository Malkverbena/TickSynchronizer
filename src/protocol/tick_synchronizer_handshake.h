// Declares handshake compatibility results and the pure evaluator.
// Separates identity/capability decisions from packet parsing and message order.

#pragma once

#include "tick_synchronizer_packet_codec.h"

#include <cstdint>

namespace tick_synchronizer {

enum class ProtocolHandshakeResult : uint16_t {
	ACCEPTED = 0,
	INVALID_LOCAL_PROFILE,
	MALFORMED_REMOTE_HANDSHAKE,
	PROTOCOL_VERSION_MISMATCH,
	API_VERSION_MISMATCH,
	WIRE_PROTOCOL_VERSION_MISMATCH,
	WIRE_PROTOCOL_REVISION_MISMATCH,
	BUILD_COMPATIBILITY_MISMATCH,
	PRECISION_MISMATCH,
	GODOT_COMMIT_MISMATCH,
	MODULE_BUILD_MISMATCH,
	GAME_BUILD_MISMATCH,
	SCHEMA_MISMATCH,
	CAPABILITY_MISMATCH,
	HELLO_NONCE_MISMATCH,
	NEGOTIATED_CAPABILITIES_MISMATCH,
	GODOT_VERSION_MISMATCH,
};

enum class ProtocolHandshakeWarning : uint32_t {
	NONE = 0,
	GODOT_COMMIT_MISMATCH = UINT32_C(1) << 0,
};

struct ProtocolHandshakeEvaluation {
	ProtocolHandshakeResult result = ProtocolHandshakeResult::INVALID_LOCAL_PROFILE;
	uint64_t negotiated_capabilities = 0;
	uint64_t missing_capabilities = 0;
	ProtocolIdentityField identity_field = ProtocolIdentityField::NONE;
	uint32_t warning_flags = 0;
};

class ProtocolHandshakeEvaluator {
public:
	// Checks whether a fixed SHA-1 identity field is entirely unset.
	static bool is_zero_sha1(const ProtocolSha1 &p_value);

	// Checks whether an opaque fixed-size identity field is entirely unset.
	static bool is_zero_opaque_id(const ProtocolOpaqueId &p_value);

	// Checks major.minor.patch-status ASCII syntax and required zero padding.
	static bool is_valid_godot_version(const ProtocolGodotVersion &p_value);

	// Validates local profile structure before compatibility comparison.
	static bool is_profile_well_formed(const ProtocolCompatibilityProfile &p_profile);

	// Compares two complete profiles in deterministic diagnostic precedence.
	static ProtocolHandshakeEvaluation evaluate_profiles(
			const ProtocolCompatibilityProfile &p_local,
			const ProtocolCompatibilityProfile &p_remote);

	// Evaluates a remote HELLO and produces either an ACK or structured disconnect.
	static ProtocolHandshakeResult evaluate_hello(
			const ProtocolCompatibilityProfile &p_local,
			const ProtocolHelloPayload &p_remote_hello,
			uint64_t p_peer_id,
			ProtocolHelloAckPayload &r_ack,
			ProtocolDisconnectPayload &r_disconnect,
			uint32_t &r_warning_flags);

	// Validates a responder profile, echoed nonce, and negotiated capabilities.
	static ProtocolHandshakeResult validate_hello_ack(
			const ProtocolCompatibilityProfile &p_local,
			uint64_t p_expected_hello_nonce,
			const ProtocolHelloAckPayload &p_remote_ack,
			uint64_t p_peer_id,
			uint64_t &r_negotiated_capabilities,
			ProtocolDisconnectPayload &r_disconnect,
			uint32_t &r_warning_flags);

	// Builds a disconnect from an inspected incompatible control header.
	static ProtocolDisconnectPayload make_protocol_version_disconnect(
			const ProtocolCompatibilityProfile &p_local,
			const ProtocolPacketHeader &p_observed_header,
			uint64_t p_peer_id);

	// Returns a stable diagnostic name for the result enum.
	static const char *get_result_name(ProtocolHandshakeResult p_result);
	// Returns a stable diagnostic name for one warning flag.
	static const char *get_warning_name(ProtocolHandshakeWarning p_warning);
};

} // namespace tick_synchronizer

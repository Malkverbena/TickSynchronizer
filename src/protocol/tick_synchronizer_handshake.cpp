// Implements pure handshake compatibility evaluation and disconnect diagnostics.
// Compares peer profiles without transport, scene, or global-state dependencies.

#include "tick_synchronizer_handshake.h"

namespace tick_synchronizer {
namespace {

// Initializes fields shared by all locally generated disconnect diagnostics.
static ProtocolDisconnectPayload make_disconnect_base(
		const ProtocolCompatibilityProfile &p_local,
		ProtocolPrecisionMode p_peer_precision,
		uint64_t p_peer_id) {
	ProtocolDisconnectPayload disconnect;
	disconnect.sender_precision = p_local.precision;
	disconnect.required_precision = p_local.precision;
	disconnect.peer_precision = p_peer_precision;
	disconnect.local_protocol_major = TickSynchronizerPacketCodec::PROTOCOL_MAJOR;
	disconnect.local_protocol_minor = TickSynchronizerPacketCodec::PROTOCOL_MINOR;
	disconnect.peer_protocol_major = TickSynchronizerPacketCodec::PROTOCOL_MAJOR;
	disconnect.peer_protocol_minor = TickSynchronizerPacketCodec::PROTOCOL_MINOR;
	disconnect.local_api_version = p_local.api_version;
	disconnect.peer_api_version = p_local.api_version;
	disconnect.local_wire_protocol_version = p_local.wire_protocol_version;
	disconnect.local_wire_protocol_revision = p_local.wire_protocol_revision;
	disconnect.peer_wire_protocol_version = p_local.wire_protocol_version;
	disconnect.peer_wire_protocol_revision = p_local.wire_protocol_revision;
	disconnect.peer_id = p_peer_id;
	return disconnect;
}


// Maps evaluator outcomes to their wire-level disconnect reason.
static ProtocolDisconnectReason result_to_disconnect_reason(ProtocolHandshakeResult p_result) {
	switch (p_result) {
		case ProtocolHandshakeResult::PROTOCOL_VERSION_MISMATCH:
			return ProtocolDisconnectReason::PROTOCOL_VERSION_MISMATCH;
		case ProtocolHandshakeResult::API_VERSION_MISMATCH:
			return ProtocolDisconnectReason::API_VERSION_MISMATCH;
		case ProtocolHandshakeResult::WIRE_PROTOCOL_VERSION_MISMATCH:
			return ProtocolDisconnectReason::WIRE_PROTOCOL_VERSION_MISMATCH;
		case ProtocolHandshakeResult::WIRE_PROTOCOL_REVISION_MISMATCH:
			return ProtocolDisconnectReason::WIRE_PROTOCOL_REVISION_MISMATCH;
		case ProtocolHandshakeResult::BUILD_COMPATIBILITY_MISMATCH:
			return ProtocolDisconnectReason::BUILD_COMPATIBILITY_MISMATCH;
		case ProtocolHandshakeResult::PRECISION_MISMATCH:
			return ProtocolDisconnectReason::PRECISION_MISMATCH;
		case ProtocolHandshakeResult::GODOT_VERSION_MISMATCH:
			return ProtocolDisconnectReason::GODOT_VERSION_MISMATCH;
		case ProtocolHandshakeResult::GODOT_COMMIT_MISMATCH:
			return ProtocolDisconnectReason::GODOT_COMMIT_MISMATCH;
		case ProtocolHandshakeResult::MODULE_BUILD_MISMATCH:
			return ProtocolDisconnectReason::MODULE_BUILD_MISMATCH;
		case ProtocolHandshakeResult::GAME_BUILD_MISMATCH:
			return ProtocolDisconnectReason::GAME_BUILD_MISMATCH;
		case ProtocolHandshakeResult::SCHEMA_MISMATCH:
			return ProtocolDisconnectReason::SCHEMA_MISMATCH;
		case ProtocolHandshakeResult::CAPABILITY_MISMATCH:
			return ProtocolDisconnectReason::CAPABILITY_MISMATCH;
		case ProtocolHandshakeResult::HELLO_NONCE_MISMATCH:
			return ProtocolDisconnectReason::HELLO_NONCE_MISMATCH;
		case ProtocolHandshakeResult::MALFORMED_REMOTE_HANDSHAKE:
		case ProtocolHandshakeResult::NEGOTIATED_CAPABILITIES_MISMATCH:
			return ProtocolDisconnectReason::MALFORMED_HANDSHAKE;
		case ProtocolHandshakeResult::ACCEPTED:
		case ProtocolHandshakeResult::INVALID_LOCAL_PROFILE:
		default:
			return ProtocolDisconnectReason::NONE;
	}
}


// Builds detailed disconnect data from a failed profile evaluation.
static ProtocolDisconnectPayload make_evaluation_disconnect(
		const ProtocolCompatibilityProfile &p_local,
		const ProtocolCompatibilityProfile &p_remote,
		const ProtocolHandshakeEvaluation &p_evaluation,
		uint64_t p_peer_id) {
	// Initializes fields shared by all locally generated disconnect diagnostics.
	ProtocolDisconnectPayload disconnect = make_disconnect_base(
			p_local,
			p_remote.precision,
			p_peer_id);
	// Maps evaluator outcomes to their wire-level disconnect reason.
	disconnect.reason = result_to_disconnect_reason(p_evaluation.result);
	disconnect.identity_field = p_evaluation.identity_field;
	disconnect.peer_api_version = p_remote.api_version;
	disconnect.peer_wire_protocol_version = p_remote.wire_protocol_version;
	disconnect.peer_wire_protocol_revision = p_remote.wire_protocol_revision;
	disconnect.detail_mask = p_evaluation.missing_capabilities;

	if (p_evaluation.result == ProtocolHandshakeResult::CAPABILITY_MISMATCH) {
		const uint64_t local_missing = p_local.capabilities.required & ~p_remote.capabilities.supported;
		const uint64_t remote_missing = p_remote.capabilities.required & ~p_local.capabilities.supported;
		disconnect.detail_code = (local_missing != 0 ? 1U : 0U) |
				(remote_missing != 0 ? 2U : 0U);
	}
	return disconnect;
}

} // namespace

bool ProtocolHandshakeEvaluator::is_zero_sha1(const ProtocolSha1 &p_value) {
	for (uint8_t byte : p_value) {
		if (byte != 0) {
			return false;
		}
	}
	return true;
}


bool ProtocolHandshakeEvaluator::is_zero_opaque_id(const ProtocolOpaqueId &p_value) {
	for (uint8_t byte : p_value) {
		if (byte != 0) {
			return false;
		}
	}
	return true;
}


bool ProtocolHandshakeEvaluator::is_valid_godot_version(
		const ProtocolGodotVersion &p_value) {
	std::size_t length = 0;
	while (length < p_value.size() && p_value[length] != 0) {
		length++;
	}
	if (length == 0 || length == p_value.size()) {
		return false;
	}
	for (std::size_t index = length + 1; index < p_value.size(); index++) {
		if (p_value[index] != 0) {
			return false;
		}
	}

	std::size_t index = 0;
	auto consume_decimal_component = [&]() {
		const std::size_t start = index;
		while (index < length && p_value[index] >= '0' && p_value[index] <= '9') {
			index++;
		}
		if (index == start) {
			return false;
		}
		return index - start == 1 || p_value[start] != '0';
	};

	for (int component = 0; component < 3; component++) {
		if (!consume_decimal_component()) {
			return false;
		}
		if (component < 2) {
			if (index >= length || p_value[index] != '.') {
				return false;
			}
			index++;
		}
	}
	if (index >= length || p_value[index] != '-') {
		return false;
	}
	index++;
	if (index >= length || p_value[index] < 'a' || p_value[index] > 'z') {
		return false;
	}
	for (; index < length; index++) {
		const uint8_t byte = p_value[index];
		const bool is_ascii_digit = byte >= '0' && byte <= '9';
		const bool is_ascii_lower = byte >= 'a' && byte <= 'z';
		if (!is_ascii_digit && !is_ascii_lower) {
			return false;
		}
	}
	return true;
}


bool ProtocolHandshakeEvaluator::is_profile_well_formed(
		const ProtocolCompatibilityProfile &p_profile) {
	if (!TickSynchronizerPacketCodec::is_valid_version_contract(
			p_profile.api_version,
			p_profile.wire_protocol_version,
			p_profile.wire_protocol_revision)) {
		return false;
	}
	if (!TickSynchronizerPacketCodec::is_valid_precision(p_profile.precision)) {
		return false;
	}
	if (!is_valid_godot_version(p_profile.godot_version) ||
			is_zero_sha1(p_profile.godot_commit) ||
			is_zero_sha1(p_profile.module_build_id) ||
			is_zero_opaque_id(p_profile.game_build_id) ||
			is_zero_opaque_id(p_profile.schema_compatibility_id)) {
		return false;
	}
	return (p_profile.capabilities.required & ~p_profile.capabilities.supported) == 0;
}


ProtocolHandshakeEvaluation ProtocolHandshakeEvaluator::evaluate_profiles(
		const ProtocolCompatibilityProfile &p_local,
		const ProtocolCompatibilityProfile &p_remote) {
	ProtocolHandshakeEvaluation evaluation;
	if (!is_profile_well_formed(p_local)) {
		evaluation.result = ProtocolHandshakeResult::INVALID_LOCAL_PROFILE;
		return evaluation;
	}
	if (!is_profile_well_formed(p_remote)) {
		evaluation.result = ProtocolHandshakeResult::MALFORMED_REMOTE_HANDSHAKE;
		return evaluation;
	}
	if (p_local.api_version != p_remote.api_version) {
		evaluation.result = ProtocolHandshakeResult::API_VERSION_MISMATCH;
		return evaluation;
	}
	if (p_local.wire_protocol_version != p_remote.wire_protocol_version) {
		evaluation.result = ProtocolHandshakeResult::WIRE_PROTOCOL_VERSION_MISMATCH;
		return evaluation;
	}
	if (p_local.wire_protocol_revision != p_remote.wire_protocol_revision) {
		evaluation.result = ProtocolHandshakeResult::WIRE_PROTOCOL_REVISION_MISMATCH;
		return evaluation;
	}
	if (version::EXACT_BUILD_MATCH_REQUIRED &&
			p_local.module_build_id != p_remote.module_build_id) {
		evaluation.result = ProtocolHandshakeResult::BUILD_COMPATIBILITY_MISMATCH;
		evaluation.identity_field = ProtocolIdentityField::MODULE_BUILD;
		return evaluation;
	}
	if (p_local.precision != p_remote.precision) {
		evaluation.result = ProtocolHandshakeResult::PRECISION_MISMATCH;
		return evaluation;
	}
	if (p_local.godot_version != p_remote.godot_version) {
		evaluation.result = ProtocolHandshakeResult::GODOT_VERSION_MISMATCH;
		evaluation.identity_field = ProtocolIdentityField::GODOT_VERSION;
		return evaluation;
	}
	if (p_local.game_build_id != p_remote.game_build_id) {
		evaluation.result = ProtocolHandshakeResult::GAME_BUILD_MISMATCH;
		evaluation.identity_field = ProtocolIdentityField::GAME_BUILD;
		return evaluation;
	}
	if (p_local.schema_compatibility_id != p_remote.schema_compatibility_id) {
		evaluation.result = ProtocolHandshakeResult::SCHEMA_MISMATCH;
		evaluation.identity_field = ProtocolIdentityField::SCHEMA_COMPATIBILITY;
		return evaluation;
	}

	const uint64_t local_missing = p_local.capabilities.required & ~p_remote.capabilities.supported;
	const uint64_t remote_missing = p_remote.capabilities.required & ~p_local.capabilities.supported;
	if (local_missing != 0 || remote_missing != 0) {
		evaluation.result = ProtocolHandshakeResult::CAPABILITY_MISMATCH;
		evaluation.missing_capabilities = local_missing | remote_missing;
		return evaluation;
	}
	if (p_local.godot_commit != p_remote.godot_commit) {
		evaluation.warning_flags |=
				static_cast<uint32_t>(ProtocolHandshakeWarning::GODOT_COMMIT_MISMATCH);
	}

	evaluation.result = ProtocolHandshakeResult::ACCEPTED;
	evaluation.negotiated_capabilities =
			p_local.capabilities.supported & p_remote.capabilities.supported;
	return evaluation;
}


ProtocolHandshakeResult ProtocolHandshakeEvaluator::evaluate_hello(
		const ProtocolCompatibilityProfile &p_local,
		const ProtocolHelloPayload &p_remote_hello,
		uint64_t p_peer_id,
		ProtocolHelloAckPayload &r_ack,
		ProtocolDisconnectPayload &r_disconnect,
		uint32_t &r_warning_flags) {
	if (!is_profile_well_formed(p_local)) {
		return ProtocolHandshakeResult::INVALID_LOCAL_PROFILE;
	}
	if (p_remote_hello.payload_version != TickSynchronizerPacketCodec::HELLO_PAYLOAD_VERSION ||
			p_remote_hello.hello_nonce == 0 ||
			!is_profile_well_formed(p_remote_hello.profile)) {
		// Initializes fields shared by all locally generated disconnect diagnostics.
		ProtocolDisconnectPayload disconnect = make_disconnect_base(
				p_local,
				p_remote_hello.profile.precision,
				p_peer_id);
		disconnect.reason = ProtocolDisconnectReason::MALFORMED_HANDSHAKE;
		disconnect.peer_api_version = p_remote_hello.profile.api_version;
		disconnect.peer_wire_protocol_version = p_remote_hello.profile.wire_protocol_version;
		disconnect.peer_wire_protocol_revision = p_remote_hello.profile.wire_protocol_revision;
		disconnect.detail_code = p_remote_hello.hello_nonce == 0 ? 1U : 2U;
		r_disconnect = disconnect;
		return ProtocolHandshakeResult::MALFORMED_REMOTE_HANDSHAKE;
	}

	const ProtocolHandshakeEvaluation evaluation = evaluate_profiles(
			p_local,
			p_remote_hello.profile);
	if (evaluation.result != ProtocolHandshakeResult::ACCEPTED) {
		// Builds detailed disconnect data from a failed profile evaluation.
		r_disconnect = make_evaluation_disconnect(
				p_local,
				p_remote_hello.profile,
				evaluation,
				p_peer_id);
		return evaluation.result;
	}

	ProtocolHelloAckPayload ack;
	ack.payload_version = TickSynchronizerPacketCodec::HELLO_PAYLOAD_VERSION;
	ack.echoed_hello_nonce = p_remote_hello.hello_nonce;
	ack.profile = p_local;
	ack.negotiated_capabilities = evaluation.negotiated_capabilities;
	r_ack = ack;
	r_warning_flags = evaluation.warning_flags;
	return ProtocolHandshakeResult::ACCEPTED;
}


ProtocolHandshakeResult ProtocolHandshakeEvaluator::validate_hello_ack(
		const ProtocolCompatibilityProfile &p_local,
		uint64_t p_expected_hello_nonce,
		const ProtocolHelloAckPayload &p_remote_ack,
		uint64_t p_peer_id,
		uint64_t &r_negotiated_capabilities,
		ProtocolDisconnectPayload &r_disconnect,
		uint32_t &r_warning_flags) {
	if (!is_profile_well_formed(p_local) || p_expected_hello_nonce == 0) {
		return ProtocolHandshakeResult::INVALID_LOCAL_PROFILE;
	}
	if (p_remote_ack.payload_version != TickSynchronizerPacketCodec::HELLO_PAYLOAD_VERSION ||
			!is_profile_well_formed(p_remote_ack.profile)) {
		// Initializes fields shared by all locally generated disconnect diagnostics.
		ProtocolDisconnectPayload disconnect = make_disconnect_base(
				p_local,
				p_remote_ack.profile.precision,
				p_peer_id);
		disconnect.reason = ProtocolDisconnectReason::MALFORMED_HANDSHAKE;
		disconnect.peer_api_version = p_remote_ack.profile.api_version;
		disconnect.peer_wire_protocol_version = p_remote_ack.profile.wire_protocol_version;
		disconnect.peer_wire_protocol_revision = p_remote_ack.profile.wire_protocol_revision;
		disconnect.detail_code = 3;
		r_disconnect = disconnect;
		return ProtocolHandshakeResult::MALFORMED_REMOTE_HANDSHAKE;
	}
	if (p_remote_ack.echoed_hello_nonce != p_expected_hello_nonce) {
		// Initializes fields shared by all locally generated disconnect diagnostics.
		ProtocolDisconnectPayload disconnect = make_disconnect_base(
				p_local,
				p_remote_ack.profile.precision,
				p_peer_id);
		disconnect.reason = ProtocolDisconnectReason::HELLO_NONCE_MISMATCH;
		disconnect.detail_code = 1;
		r_disconnect = disconnect;
		return ProtocolHandshakeResult::HELLO_NONCE_MISMATCH;
	}

	const ProtocolHandshakeEvaluation evaluation = evaluate_profiles(
			p_local,
			p_remote_ack.profile);
	if (evaluation.result != ProtocolHandshakeResult::ACCEPTED) {
		// Builds detailed disconnect data from a failed profile evaluation.
		r_disconnect = make_evaluation_disconnect(
				p_local,
				p_remote_ack.profile,
				evaluation,
				p_peer_id);
		return evaluation.result;
	}
	if (p_remote_ack.negotiated_capabilities != evaluation.negotiated_capabilities) {
		// Initializes fields shared by all locally generated disconnect diagnostics.
		ProtocolDisconnectPayload disconnect = make_disconnect_base(
				p_local,
				p_remote_ack.profile.precision,
				p_peer_id);
		disconnect.reason = ProtocolDisconnectReason::MALFORMED_HANDSHAKE;
		disconnect.detail_code = 4;
		disconnect.detail_mask = p_remote_ack.negotiated_capabilities ^
				evaluation.negotiated_capabilities;
		r_disconnect = disconnect;
		return ProtocolHandshakeResult::NEGOTIATED_CAPABILITIES_MISMATCH;
	}

	r_negotiated_capabilities = evaluation.negotiated_capabilities;
	r_warning_flags = evaluation.warning_flags;
	return ProtocolHandshakeResult::ACCEPTED;
}


ProtocolDisconnectPayload ProtocolHandshakeEvaluator::make_protocol_version_disconnect(
		const ProtocolCompatibilityProfile &p_local,
		const ProtocolPacketHeader &p_observed_header,
		uint64_t p_peer_id) {
	// Initializes fields shared by all locally generated disconnect diagnostics.
	ProtocolDisconnectPayload disconnect = make_disconnect_base(
			p_local,
			ProtocolPrecisionMode::INVALID,
			p_peer_id);
	disconnect.reason = ProtocolDisconnectReason::PROTOCOL_VERSION_MISMATCH;
	disconnect.peer_protocol_major = p_observed_header.protocol_major;
	disconnect.peer_protocol_minor = p_observed_header.protocol_minor;
	return disconnect;
}

const char *ProtocolHandshakeEvaluator::get_result_name(ProtocolHandshakeResult p_result) {
	switch (p_result) {
		case ProtocolHandshakeResult::ACCEPTED: return "ACCEPTED";
		case ProtocolHandshakeResult::INVALID_LOCAL_PROFILE: return "INVALID_LOCAL_PROFILE";
		case ProtocolHandshakeResult::MALFORMED_REMOTE_HANDSHAKE: return "MALFORMED_REMOTE_HANDSHAKE";
		case ProtocolHandshakeResult::PROTOCOL_VERSION_MISMATCH: return "PROTOCOL_VERSION_MISMATCH";
		case ProtocolHandshakeResult::API_VERSION_MISMATCH: return "API_VERSION_MISMATCH";
		case ProtocolHandshakeResult::WIRE_PROTOCOL_VERSION_MISMATCH:
			return "WIRE_PROTOCOL_VERSION_MISMATCH";
		case ProtocolHandshakeResult::WIRE_PROTOCOL_REVISION_MISMATCH:
			return "WIRE_PROTOCOL_REVISION_MISMATCH";
		case ProtocolHandshakeResult::BUILD_COMPATIBILITY_MISMATCH:
			return "BUILD_COMPATIBILITY_MISMATCH";
		case ProtocolHandshakeResult::PRECISION_MISMATCH: return "PRECISION_MISMATCH";
		case ProtocolHandshakeResult::GODOT_VERSION_MISMATCH: return "GODOT_VERSION_MISMATCH";
		case ProtocolHandshakeResult::GODOT_COMMIT_MISMATCH: return "GODOT_COMMIT_MISMATCH";
		case ProtocolHandshakeResult::MODULE_BUILD_MISMATCH: return "MODULE_BUILD_MISMATCH";
		case ProtocolHandshakeResult::GAME_BUILD_MISMATCH: return "GAME_BUILD_MISMATCH";
		case ProtocolHandshakeResult::SCHEMA_MISMATCH: return "SCHEMA_MISMATCH";
		case ProtocolHandshakeResult::CAPABILITY_MISMATCH: return "CAPABILITY_MISMATCH";
		case ProtocolHandshakeResult::HELLO_NONCE_MISMATCH: return "HELLO_NONCE_MISMATCH";
		case ProtocolHandshakeResult::NEGOTIATED_CAPABILITIES_MISMATCH:
			return "NEGOTIATED_CAPABILITIES_MISMATCH";
		default: return "UNKNOWN_HANDSHAKE_RESULT";
	}
}

const char *ProtocolHandshakeEvaluator::get_warning_name(
		ProtocolHandshakeWarning p_warning) {
	switch (p_warning) {
		case ProtocolHandshakeWarning::GODOT_COMMIT_MISMATCH:
			return "GODOT_COMMIT_MISMATCH";
		case ProtocolHandshakeWarning::NONE:
		default: return "NONE";
	}
}

} // namespace tick_synchronizer

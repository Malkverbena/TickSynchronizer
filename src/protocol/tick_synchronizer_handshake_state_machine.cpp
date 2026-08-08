// Implements legal handshake message ordering and terminal state transitions.
// Consumes decoded control packets and returns declarative session actions.

#include "tick_synchronizer_handshake_state_machine.h"

namespace tick_synchronizer {
namespace {

// Checks the fixed control-envelope identity before state processing.
static bool is_current_protocol_header(const ProtocolPacketHeader &p_header) {
	return p_header.magic == TickSynchronizerPacketCodec::PROTOCOL_MAGIC &&
			p_header.protocol_major == TickSynchronizerPacketCodec::PROTOCOL_MAJOR &&
			p_header.protocol_minor == TickSynchronizerPacketCodec::PROTOCOL_MINOR &&
			p_header.header_size == TickSynchronizerPacketCodec::CONTROL_HEADER_SIZE &&
			p_header.flags == 0 && p_header.reserved == 0;
}


static bool is_valid_role(ProtocolHandshakeRole p_role) {
	return p_role == ProtocolHandshakeRole::INITIATOR ||
			p_role == ProtocolHandshakeRole::RESPONDER;
}

} // namespace

ProtocolHandshakeStateMachine::ProtocolHandshakeStateMachine(
		const ProtocolHandshakeMachineConfig &p_config) :
		config(p_config) {
}


bool ProtocolHandshakeStateMachine::is_config_well_formed() const {
	if (!is_valid_role(config.role) || config.remote_peer_id == 0 ||
			!ProtocolHandshakeEvaluator::is_profile_well_formed(config.local_profile)) {
		return false;
	}
	if (config.role == ProtocolHandshakeRole::INITIATOR) {
		return config.session_id != 0 && config.hello_nonce != 0;
	}
	return config.session_id == 0 && config.hello_nonce == 0;
}


ProtocolDisconnectPayload ProtocolHandshakeStateMachine::make_local_disconnect(
		ProtocolDisconnectReason p_reason,
		ProtocolHandshakeViolation p_violation,
		uint64_t p_detail_mask,
		ProtocolPrecisionMode p_peer_precision) const {
	ProtocolDisconnectPayload disconnect;
	disconnect.reason = p_reason;
	disconnect.sender_precision = config.local_profile.precision;
	disconnect.required_precision = config.local_profile.precision;
	disconnect.peer_precision = p_peer_precision;
	disconnect.local_protocol_major = TickSynchronizerPacketCodec::PROTOCOL_MAJOR;
	disconnect.local_protocol_minor = TickSynchronizerPacketCodec::PROTOCOL_MINOR;
	disconnect.peer_protocol_major = TickSynchronizerPacketCodec::PROTOCOL_MAJOR;
	disconnect.peer_protocol_minor = TickSynchronizerPacketCodec::PROTOCOL_MINOR;
	disconnect.local_api_version = config.local_profile.api_version;
	disconnect.peer_api_version = config.local_profile.api_version;
	disconnect.local_wire_protocol_version = config.local_profile.wire_protocol_version;
	disconnect.local_wire_protocol_revision = config.local_profile.wire_protocol_revision;
	disconnect.peer_wire_protocol_version = config.local_profile.wire_protocol_version;
	disconnect.peer_wire_protocol_revision = config.local_profile.wire_protocol_revision;
	disconnect.detail_code = static_cast<uint32_t>(p_violation);
	disconnect.peer_id = config.remote_peer_id;
	disconnect.detail_mask = p_detail_mask;
	return disconnect;
}


ProtocolHandshakeMachineResult ProtocolHandshakeStateMachine::reject_with_disconnect(
		uint64_t p_outbound_session_id,
		const ProtocolDisconnectPayload &p_disconnect,
		bool p_send_disconnect,
		ProtocolHandshakeAction &r_action) {
	ProtocolHandshakeAction action;
	action.became_rejected = true;
	if (p_send_disconnect && p_outbound_session_id != 0) {
		action.has_outbound_packet = true;
		action.outbound_packet_type = ProtocolPacketType::DISCONNECT_REASON;
		action.outbound_session_id = p_outbound_session_id;
		action.disconnect = p_disconnect;
	}
	state = ProtocolHandshakeState::REJECTED;
	has_terminal_disconnect_data = true;
	terminal_disconnect_remote = false;
	terminal_disconnect = p_disconnect;
	r_action = action;
	return ProtocolHandshakeMachineResult::OK;
}


ProtocolHandshakeMachineResult ProtocolHandshakeStateMachine::reject_violation(
		uint64_t p_outbound_session_id,
		ProtocolDisconnectReason p_reason,
		ProtocolHandshakeViolation p_violation,
		ProtocolCodecError p_codec_error,
		bool p_send_disconnect,
		ProtocolHandshakeAction &r_action) {
	ProtocolPrecisionMode peer_precision = ProtocolPrecisionMode::INVALID;
	if (has_established_data) {
		peer_precision = established.remote_profile.precision;
	}
	ProtocolDisconnectPayload disconnect = make_local_disconnect(
			p_reason,
			p_violation,
			static_cast<uint64_t>(p_codec_error),
			peer_precision);
	return reject_with_disconnect(
			p_outbound_session_id,
			disconnect,
			p_send_disconnect,
			r_action);
}


ProtocolHandshakeMachineResult ProtocolHandshakeStateMachine::handle_remote_disconnect(
		const ProtocolPacket &p_packet,
		ProtocolHandshakeAction &r_action) {
	ProtocolDisconnectPayload decoded;
	const ProtocolCodecError codec_error =
			TickSynchronizerPacketCodec::decode_disconnect_payload(p_packet, decoded);
	if (codec_error != ProtocolCodecError::OK) {
		return reject_violation(
				p_packet.header.session_id,
				ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
				ProtocolHandshakeViolation::INVALID_DISCONNECT_PAYLOAD,
				codec_error,
				false,
				r_action);
	}

	ProtocolHandshakeAction action;
	action.became_rejected = true;
	action.remote_disconnect = true;
	action.disconnect = decoded;
	state = ProtocolHandshakeState::REJECTED;
	if (session_id == 0) {
		session_id = p_packet.header.session_id;
	}
	has_terminal_disconnect_data = true;
	terminal_disconnect_remote = true;
	terminal_disconnect = decoded;
	r_action = action;
	return ProtocolHandshakeMachineResult::OK;
}


ProtocolHandshakeMachineResult ProtocolHandshakeStateMachine::start(
		ProtocolHandshakeAction &r_action) {
	if (state != ProtocolHandshakeState::IDLE) {
		return ProtocolHandshakeMachineResult::INVALID_STATE;
	}
	if (!is_config_well_formed()) {
		return ProtocolHandshakeMachineResult::INVALID_CONFIGURATION;
	}

	ProtocolHandshakeAction action;
	if (config.role == ProtocolHandshakeRole::INITIATOR) {
		session_id = config.session_id;
		action.has_outbound_packet = true;
		action.outbound_packet_type = ProtocolPacketType::HELLO;
		action.outbound_session_id = session_id;
		action.hello.payload_version = TickSynchronizerPacketCodec::HELLO_PAYLOAD_VERSION;
		action.hello.hello_nonce = config.hello_nonce;
		action.hello.profile = config.local_profile;
		state = ProtocolHandshakeState::WAITING_FOR_HELLO_ACK;
	} else {
		state = ProtocolHandshakeState::WAITING_FOR_HELLO;
	}
	r_action = action;
	return ProtocolHandshakeMachineResult::OK;
}


ProtocolHandshakeMachineResult ProtocolHandshakeStateMachine::receive_packet(
		const ProtocolPacket &p_packet,
		ProtocolHandshakeAction &r_action) {
	if (state == ProtocolHandshakeState::IDLE ||
			state == ProtocolHandshakeState::REJECTED ||
			state == ProtocolHandshakeState::CLOSED) {
		return ProtocolHandshakeMachineResult::INVALID_STATE;
	}

	if (p_packet.header.protocol_major != TickSynchronizerPacketCodec::PROTOCOL_MAJOR ||
			p_packet.header.protocol_minor != TickSynchronizerPacketCodec::PROTOCOL_MINOR) {
		ProtocolDisconnectPayload disconnect =
				ProtocolHandshakeEvaluator::make_protocol_version_disconnect(
						config.local_profile,
						p_packet.header,
						config.remote_peer_id);
		return reject_with_disconnect(
				p_packet.header.session_id,
				disconnect,
				p_packet.header.session_id != 0,
				r_action);
	}
	if (!is_current_protocol_header(p_packet.header)) {
		return reject_violation(
				p_packet.header.session_id,
				ProtocolDisconnectReason::MALFORMED_PACKET,
				ProtocolHandshakeViolation::INVALID_HEADER,
				ProtocolCodecError::MALFORMED_PAYLOAD,
				p_packet.header.session_id != 0,
				r_action);
	}
	if (p_packet.header.session_id == 0) {
		return reject_violation(
				0,
				ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
				ProtocolHandshakeViolation::ZERO_SESSION_ID,
				ProtocolCodecError::OK,
				false,
				r_action);
	}
	if (state != ProtocolHandshakeState::WAITING_FOR_HELLO &&
			p_packet.header.session_id != session_id) {
		return reject_violation(
				session_id,
				ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
				ProtocolHandshakeViolation::SESSION_ID_MISMATCH,
				ProtocolCodecError::OK,
				true,
				r_action);
	}
	if (p_packet.header.sequence != 0) {
		return reject_violation(
				p_packet.header.session_id,
				ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
				ProtocolHandshakeViolation::NONZERO_SEQUENCE,
				ProtocolCodecError::OK,
				true,
				r_action);
	}
	if (p_packet.header.tick != 0) {
		return reject_violation(
				p_packet.header.session_id,
				ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
				ProtocolHandshakeViolation::NONZERO_TICK,
				ProtocolCodecError::OK,
				true,
				r_action);
	}

	if (p_packet.header.packet_type == ProtocolPacketType::DISCONNECT_REASON) {
		return handle_remote_disconnect(p_packet, r_action);
	}

	if (state == ProtocolHandshakeState::ESTABLISHED) {
		const ProtocolHandshakeViolation violation =
				(p_packet.header.packet_type == ProtocolPacketType::HELLO ||
						p_packet.header.packet_type == ProtocolPacketType::HELLO_ACK) ?
				ProtocolHandshakeViolation::DUPLICATE_HANDSHAKE_MESSAGE :
				ProtocolHandshakeViolation::UNEXPECTED_PACKET_TYPE;
		return reject_violation(
				session_id,
				ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
				violation,
				ProtocolCodecError::OK,
				true,
				r_action);
	}

	if (state == ProtocolHandshakeState::WAITING_FOR_HELLO) {
		if (p_packet.header.packet_type != ProtocolPacketType::HELLO) {
			return reject_violation(
					p_packet.header.session_id,
					ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
					ProtocolHandshakeViolation::UNEXPECTED_PACKET_TYPE,
					ProtocolCodecError::OK,
					true,
					r_action);
		}

		ProtocolHelloPayload hello;
		const ProtocolCodecError codec_error =
				TickSynchronizerPacketCodec::decode_hello_payload(p_packet, hello);
		if (codec_error != ProtocolCodecError::OK) {
			return reject_violation(
					p_packet.header.session_id,
					ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
					ProtocolHandshakeViolation::MALFORMED_PAYLOAD,
					codec_error,
					true,
					r_action);
		}

		ProtocolHelloAckPayload ack;
		ProtocolDisconnectPayload disconnect;
		uint32_t warning_flags = 0;
		const ProtocolHandshakeResult evaluation = ProtocolHandshakeEvaluator::evaluate_hello(
				config.local_profile,
				hello,
				config.remote_peer_id,
				ack,
				disconnect,
				warning_flags);
		if (evaluation == ProtocolHandshakeResult::INVALID_LOCAL_PROFILE) {
			return ProtocolHandshakeMachineResult::INVALID_CONFIGURATION;
		}
		if (evaluation != ProtocolHandshakeResult::ACCEPTED) {
			session_id = p_packet.header.session_id;
			return reject_with_disconnect(
					session_id,
					disconnect,
					true,
					r_action);
		}

		ProtocolHandshakeAction action;
		action.has_outbound_packet = true;
		action.outbound_packet_type = ProtocolPacketType::HELLO_ACK;
		action.outbound_session_id = p_packet.header.session_id;
		action.hello_ack = ack;
		action.warning_flags = warning_flags;
		action.became_established = true;
		session_id = p_packet.header.session_id;
		established.session_id = session_id;
		established.remote_peer_id = config.remote_peer_id;
		established.negotiated_capabilities = ack.negotiated_capabilities;
		established.warning_flags = warning_flags;
		established.remote_profile = hello.profile;
		has_established_data = true;
		state = ProtocolHandshakeState::ESTABLISHED;
		r_action = action;
		return ProtocolHandshakeMachineResult::OK;
	}

	if (p_packet.header.packet_type != ProtocolPacketType::HELLO_ACK) {
		return reject_violation(
				session_id,
				ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
				ProtocolHandshakeViolation::UNEXPECTED_PACKET_TYPE,
				ProtocolCodecError::OK,
				true,
				r_action);
	}

	ProtocolHelloAckPayload ack;
	const ProtocolCodecError codec_error =
			TickSynchronizerPacketCodec::decode_hello_ack_payload(p_packet, ack);
	if (codec_error != ProtocolCodecError::OK) {
		return reject_violation(
				session_id,
				ProtocolDisconnectReason::MALFORMED_HANDSHAKE,
				ProtocolHandshakeViolation::MALFORMED_PAYLOAD,
				codec_error,
				true,
				r_action);
	}

	uint64_t negotiated_capabilities = 0;
	ProtocolDisconnectPayload disconnect;
	uint32_t warning_flags = 0;
	const ProtocolHandshakeResult evaluation = ProtocolHandshakeEvaluator::validate_hello_ack(
			config.local_profile,
			config.hello_nonce,
			ack,
			config.remote_peer_id,
			negotiated_capabilities,
			disconnect,
			warning_flags);
	if (evaluation == ProtocolHandshakeResult::INVALID_LOCAL_PROFILE) {
		return ProtocolHandshakeMachineResult::INVALID_CONFIGURATION;
	}
	if (evaluation != ProtocolHandshakeResult::ACCEPTED) {
		return reject_with_disconnect(
				session_id,
				disconnect,
				true,
				r_action);
	}

	ProtocolHandshakeAction action;
	action.warning_flags = warning_flags;
	action.became_established = true;
	established.session_id = session_id;
	established.remote_peer_id = config.remote_peer_id;
	established.negotiated_capabilities = negotiated_capabilities;
	established.warning_flags = warning_flags;
	established.remote_profile = ack.profile;
	has_established_data = true;
	state = ProtocolHandshakeState::ESTABLISHED;
	r_action = action;
	return ProtocolHandshakeMachineResult::OK;
}


ProtocolHandshakeMachineResult ProtocolHandshakeStateMachine::cancel(
		ProtocolDisconnectReason p_reason,
		uint32_t p_detail_code,
		ProtocolHandshakeAction &r_action) {
	if (state != ProtocolHandshakeState::WAITING_FOR_HELLO &&
			state != ProtocolHandshakeState::WAITING_FOR_HELLO_ACK) {
		return ProtocolHandshakeMachineResult::INVALID_STATE;
	}
	if (!TickSynchronizerPacketCodec::is_known_disconnect_reason(p_reason)) {
		return ProtocolHandshakeMachineResult::INVALID_ARGUMENT;
	}

	ProtocolDisconnectPayload disconnect = make_local_disconnect(
			p_reason,
			ProtocolHandshakeViolation::NONE,
			0,
			ProtocolPrecisionMode::INVALID);
	disconnect.detail_code = p_detail_code;
	return reject_with_disconnect(
			session_id,
			disconnect,
			session_id != 0,
			r_action);
}


ProtocolHandshakeMachineResult ProtocolHandshakeStateMachine::close(
		ProtocolHandshakeAction &r_action) {
	ProtocolHandshakeAction action;
	if (state != ProtocolHandshakeState::CLOSED) {
		state = ProtocolHandshakeState::CLOSED;
		action.became_closed = true;
	}
	r_action = action;
	return ProtocolHandshakeMachineResult::OK;
}


ProtocolHandshakeRole ProtocolHandshakeStateMachine::get_role() const {
	return config.role;
}


ProtocolHandshakeState ProtocolHandshakeStateMachine::get_state() const {
	return state;
}


uint64_t ProtocolHandshakeStateMachine::get_session_id() const {
	return session_id;
}


bool ProtocolHandshakeStateMachine::is_established() const {
	return state == ProtocolHandshakeState::ESTABLISHED;
}


bool ProtocolHandshakeStateMachine::is_terminal() const {
	return state == ProtocolHandshakeState::REJECTED ||
			state == ProtocolHandshakeState::CLOSED;
}


bool ProtocolHandshakeStateMachine::has_established_handshake() const {
	return has_established_data;
}

const ProtocolEstablishedHandshake &ProtocolHandshakeStateMachine::get_established_handshake() const {
	return established;
}


bool ProtocolHandshakeStateMachine::has_terminal_disconnect() const {
	return has_terminal_disconnect_data;
}


bool ProtocolHandshakeStateMachine::was_terminal_disconnect_remote() const {
	return has_terminal_disconnect_data && terminal_disconnect_remote;
}

const ProtocolDisconnectPayload &ProtocolHandshakeStateMachine::get_terminal_disconnect() const {
	return terminal_disconnect;
}


ProtocolCodecError ProtocolHandshakeStateMachine::build_outbound_packet(
		const ProtocolHandshakeAction &p_action,
		ProtocolPacket &r_packet) {
	if (!p_action.has_outbound_packet || p_action.outbound_session_id == 0) {
		return ProtocolCodecError::INVALID_ARGUMENT;
	}

	PackedByteArray payload;
	ProtocolCodecError error = ProtocolCodecError::INVALID_ARGUMENT;
	switch (p_action.outbound_packet_type) {
		case ProtocolPacketType::HELLO:
			error = TickSynchronizerPacketCodec::encode_hello_payload(
					p_action.hello,
					payload);
			break;
		case ProtocolPacketType::HELLO_ACK:
			error = TickSynchronizerPacketCodec::encode_hello_ack_payload(
					p_action.hello_ack,
					payload);
			break;
		case ProtocolPacketType::DISCONNECT_REASON:
			error = TickSynchronizerPacketCodec::encode_disconnect_payload(
					p_action.disconnect,
					payload);
			break;
		case ProtocolPacketType::INVALID:
		default:
			return ProtocolCodecError::INVALID_ARGUMENT;
	}
	if (error != ProtocolCodecError::OK) {
		return error;
	}

	ProtocolPacket packet;
	packet.header = TickSynchronizerPacketCodec::make_header(
			p_action.outbound_packet_type,
			p_action.outbound_session_id,
			0,
			0,
			static_cast<uint32_t>(payload.size()),
			static_cast<uint32_t>(payload.size()) * 8U);
	packet.payload = payload;
	r_packet = packet;
	return ProtocolCodecError::OK;
}

const char *ProtocolHandshakeStateMachine::get_role_name(ProtocolHandshakeRole p_role) {
	switch (p_role) {
		case ProtocolHandshakeRole::INITIATOR: return "INITIATOR";
		case ProtocolHandshakeRole::RESPONDER: return "RESPONDER";
		case ProtocolHandshakeRole::INVALID:
		default: return "INVALID";
	}
}

const char *ProtocolHandshakeStateMachine::get_state_name(ProtocolHandshakeState p_state) {
	switch (p_state) {
		case ProtocolHandshakeState::IDLE: return "IDLE";
		case ProtocolHandshakeState::WAITING_FOR_HELLO: return "WAITING_FOR_HELLO";
		case ProtocolHandshakeState::WAITING_FOR_HELLO_ACK: return "WAITING_FOR_HELLO_ACK";
		case ProtocolHandshakeState::ESTABLISHED: return "ESTABLISHED";
		case ProtocolHandshakeState::REJECTED: return "REJECTED";
		case ProtocolHandshakeState::CLOSED: return "CLOSED";
		default: return "UNKNOWN_HANDSHAKE_STATE";
	}
}

const char *ProtocolHandshakeStateMachine::get_result_name(
		ProtocolHandshakeMachineResult p_result) {
	switch (p_result) {
		case ProtocolHandshakeMachineResult::OK: return "OK";
		case ProtocolHandshakeMachineResult::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
		case ProtocolHandshakeMachineResult::INVALID_CONFIGURATION: return "INVALID_CONFIGURATION";
		case ProtocolHandshakeMachineResult::INVALID_STATE: return "INVALID_STATE";
		default: return "UNKNOWN_HANDSHAKE_MACHINE_RESULT";
	}
}

const char *ProtocolHandshakeStateMachine::get_violation_name(
		ProtocolHandshakeViolation p_violation) {
	switch (p_violation) {
		case ProtocolHandshakeViolation::NONE: return "NONE";
		case ProtocolHandshakeViolation::UNEXPECTED_PACKET_TYPE: return "UNEXPECTED_PACKET_TYPE";
		case ProtocolHandshakeViolation::ZERO_SESSION_ID: return "ZERO_SESSION_ID";
		case ProtocolHandshakeViolation::SESSION_ID_MISMATCH: return "SESSION_ID_MISMATCH";
		case ProtocolHandshakeViolation::NONZERO_SEQUENCE: return "NONZERO_SEQUENCE";
		case ProtocolHandshakeViolation::NONZERO_TICK: return "NONZERO_TICK";
		case ProtocolHandshakeViolation::MALFORMED_PAYLOAD: return "MALFORMED_PAYLOAD";
		case ProtocolHandshakeViolation::DUPLICATE_HANDSHAKE_MESSAGE:
			return "DUPLICATE_HANDSHAKE_MESSAGE";
		case ProtocolHandshakeViolation::INVALID_DISCONNECT_PAYLOAD:
			return "INVALID_DISCONNECT_PAYLOAD";
		case ProtocolHandshakeViolation::INVALID_HEADER: return "INVALID_HEADER";
		default: return "UNKNOWN_HANDSHAKE_VIOLATION";
	}
}

} // namespace tick_synchronizer

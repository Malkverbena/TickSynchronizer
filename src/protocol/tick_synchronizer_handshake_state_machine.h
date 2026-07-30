#pragma once

#include "tick_synchronizer_handshake.h"

#include <cstdint>

namespace tick_synchronizer {

enum class ProtocolHandshakeRole : uint8_t {
	INVALID = 0,
	INITIATOR = 1,
	RESPONDER = 2,
};

enum class ProtocolHandshakeState : uint8_t {
	IDLE = 0,
	WAITING_FOR_HELLO,
	WAITING_FOR_HELLO_ACK,
	ESTABLISHED,
	REJECTED,
	CLOSED,
};

enum class ProtocolHandshakeMachineResult : uint16_t {
	OK = 0,
	INVALID_ARGUMENT,
	INVALID_CONFIGURATION,
	INVALID_STATE,
};

enum class ProtocolHandshakeViolation : uint32_t {
	NONE = 0,
	UNEXPECTED_PACKET_TYPE = 1,
	ZERO_SESSION_ID = 2,
	SESSION_ID_MISMATCH = 3,
	NONZERO_SEQUENCE = 4,
	NONZERO_TICK = 5,
	MALFORMED_PAYLOAD = 6,
	DUPLICATE_HANDSHAKE_MESSAGE = 7,
	INVALID_DISCONNECT_PAYLOAD = 8,
	INVALID_HEADER = 9,
};

struct ProtocolHandshakeMachineConfig {
	ProtocolHandshakeRole role = ProtocolHandshakeRole::INVALID;
	ProtocolCompatibilityProfile local_profile;
	uint64_t remote_peer_id = 0;
	uint64_t session_id = 0;
	uint64_t hello_nonce = 0;
};

struct ProtocolEstablishedHandshake {
	uint64_t session_id = 0;
	uint64_t remote_peer_id = 0;
	uint64_t negotiated_capabilities = 0;
	ProtocolCompatibilityProfile remote_profile;
};

struct ProtocolHandshakeAction {
	bool has_outbound_packet = false;
	ProtocolPacketType outbound_packet_type = ProtocolPacketType::INVALID;
	uint64_t outbound_session_id = 0;
	ProtocolHelloPayload hello;
	ProtocolHelloAckPayload hello_ack;
	ProtocolDisconnectPayload disconnect;
	bool became_established = false;
	bool became_rejected = false;
	bool became_closed = false;
	bool remote_disconnect = false;
};

class ProtocolHandshakeStateMachine {
	ProtocolHandshakeMachineConfig config;
	ProtocolHandshakeState state = ProtocolHandshakeState::IDLE;
	uint64_t session_id = 0;
	bool has_established_data = false;
	ProtocolEstablishedHandshake established;
	bool has_terminal_disconnect_data = false;
	bool terminal_disconnect_remote = false;
	ProtocolDisconnectPayload terminal_disconnect;

	bool is_config_well_formed() const;
	ProtocolDisconnectPayload make_local_disconnect(
			ProtocolDisconnectReason p_reason,
			ProtocolHandshakeViolation p_violation,
			uint64_t p_detail_mask,
			ProtocolPrecisionMode p_peer_precision) const;
	ProtocolHandshakeMachineResult reject_with_disconnect(
			uint64_t p_outbound_session_id,
			const ProtocolDisconnectPayload &p_disconnect,
			bool p_send_disconnect,
			ProtocolHandshakeAction &r_action);
	ProtocolHandshakeMachineResult reject_violation(
			uint64_t p_outbound_session_id,
			ProtocolDisconnectReason p_reason,
			ProtocolHandshakeViolation p_violation,
			ProtocolCodecError p_codec_error,
			bool p_send_disconnect,
			ProtocolHandshakeAction &r_action);
	ProtocolHandshakeMachineResult handle_remote_disconnect(
			const ProtocolPacket &p_packet,
			ProtocolHandshakeAction &r_action);

public:
	explicit ProtocolHandshakeStateMachine(const ProtocolHandshakeMachineConfig &p_config);
	ProtocolHandshakeStateMachine(const ProtocolHandshakeStateMachine &) = delete;
	ProtocolHandshakeStateMachine &operator=(const ProtocolHandshakeStateMachine &) = delete;

	ProtocolHandshakeMachineResult start(ProtocolHandshakeAction &r_action);
	ProtocolHandshakeMachineResult receive_packet(
			const ProtocolPacket &p_packet,
			ProtocolHandshakeAction &r_action);
	ProtocolHandshakeMachineResult cancel(
			ProtocolDisconnectReason p_reason,
			uint32_t p_detail_code,
			ProtocolHandshakeAction &r_action);
	ProtocolHandshakeMachineResult close(ProtocolHandshakeAction &r_action);

	ProtocolHandshakeRole get_role() const;
	ProtocolHandshakeState get_state() const;
	uint64_t get_session_id() const;
	bool is_established() const;
	bool is_terminal() const;
	bool has_established_handshake() const;
	const ProtocolEstablishedHandshake &get_established_handshake() const;
	bool has_terminal_disconnect() const;
	bool was_terminal_disconnect_remote() const;
	const ProtocolDisconnectPayload &get_terminal_disconnect() const;

	static ProtocolCodecError build_outbound_packet(
			const ProtocolHandshakeAction &p_action,
			ProtocolPacket &r_packet);
	static const char *get_role_name(ProtocolHandshakeRole p_role);
	static const char *get_state_name(ProtocolHandshakeState p_state);
	static const char *get_result_name(ProtocolHandshakeMachineResult p_result);
	static const char *get_violation_name(ProtocolHandshakeViolation p_violation);
};

} // namespace tick_synchronizer

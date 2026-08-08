// Declares the transport-independent handshake state machine.
// Defines roles, states, violations, actions, and established-session metadata.

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
	uint32_t warning_flags = 0;
	ProtocolCompatibilityProfile remote_profile;
};

struct ProtocolHandshakeAction {
	bool has_outbound_packet = false;
	ProtocolPacketType outbound_packet_type = ProtocolPacketType::INVALID;
	uint64_t outbound_session_id = 0;
	ProtocolHelloPayload hello;
	ProtocolHelloAckPayload hello_ack;
	ProtocolDisconnectPayload disconnect;
	uint32_t warning_flags = 0;
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

	// Validates role-specific state-machine configuration before startup.
	bool is_config_well_formed() const;

	// Builds a structured local rejection with violation details.
	ProtocolDisconnectPayload make_local_disconnect(
			ProtocolDisconnectReason p_reason,
			ProtocolHandshakeViolation p_violation,
			uint64_t p_detail_mask,
			ProtocolPrecisionMode p_peer_precision) const;

	// Transitions to rejection while optionally returning an outbound disconnect.
	ProtocolHandshakeMachineResult reject_with_disconnect(
			uint64_t p_outbound_session_id,
			const ProtocolDisconnectPayload &p_disconnect,
			bool p_send_disconnect,
			ProtocolHandshakeAction &r_action);

	// Converts a protocol-order violation into a deterministic rejection action.
	ProtocolHandshakeMachineResult reject_violation(
			uint64_t p_outbound_session_id,
			ProtocolDisconnectReason p_reason,
			ProtocolHandshakeViolation p_violation,
			ProtocolCodecError p_codec_error,
			bool p_send_disconnect,
			ProtocolHandshakeAction &r_action);

	// Validates and stores a peer-initiated terminal disconnect.
	ProtocolHandshakeMachineResult handle_remote_disconnect(
			const ProtocolPacket &p_packet,
			ProtocolHandshakeAction &r_action);

public:
	// Creates a pure state machine from immutable role and identity configuration.
	explicit ProtocolHandshakeStateMachine(const ProtocolHandshakeMachineConfig &p_config);
	// Creates a pure state machine from immutable role and identity configuration.
	ProtocolHandshakeStateMachine(const ProtocolHandshakeStateMachine &) = delete;
	ProtocolHandshakeStateMachine &operator=(const ProtocolHandshakeStateMachine &) = delete;

	// Starts the configured role and emits the initiator HELLO when required.
	ProtocolHandshakeMachineResult start(ProtocolHandshakeAction &r_action);

	// Consumes one decoded control packet according to the current legal state.
	ProtocolHandshakeMachineResult receive_packet(
			const ProtocolPacket &p_packet,
			ProtocolHandshakeAction &r_action);

	// Terminates the local handshake and emits a structured disconnect when possible.
	ProtocolHandshakeMachineResult cancel(
			ProtocolDisconnectReason p_reason,
			uint32_t p_detail_code,
			ProtocolHandshakeAction &r_action);

	// Moves the machine to CLOSED without accepting further handshake traffic.
	ProtocolHandshakeMachineResult close(ProtocolHandshakeAction &r_action);

	// Returns whether the machine acts as initiator or responder.
	ProtocolHandshakeRole get_role() const;

	// Returns the current handshake lifecycle state.
	ProtocolHandshakeState get_state() const;

	// Returns the active session identifier after role initialization.
	uint64_t get_session_id() const;

	// Reports whether compatibility and message ordering completed successfully.
	bool is_established() const;

	// Reports whether the machine is established, rejected, or closed.
	bool is_terminal() const;

	// Reports whether immutable established-peer metadata is available.
	bool has_established_handshake() const;
	// Returns established metadata; callers must first check availability.
	const ProtocolEstablishedHandshake &get_established_handshake() const;

	// Reports whether a terminal disconnect payload was stored.
	bool has_terminal_disconnect() const;

	// Reports whether stored terminal disconnect data came from the peer.
	bool was_terminal_disconnect_remote() const;
	// Returns terminal disconnect data; callers must first check availability.
	const ProtocolDisconnectPayload &get_terminal_disconnect() const;

	// Encodes a declarative handshake action as a control packet.
	static ProtocolCodecError build_outbound_packet(
			const ProtocolHandshakeAction &p_action,
			ProtocolPacket &r_packet);
	// Returns a stable diagnostic name for a handshake role.
	static const char *get_role_name(ProtocolHandshakeRole p_role);
	// Returns a stable diagnostic name for a handshake state.
	static const char *get_state_name(ProtocolHandshakeState p_state);
	// Returns a stable diagnostic name for the result enum.
	static const char *get_result_name(ProtocolHandshakeMachineResult p_result);
	// Returns a stable diagnostic name for a protocol-order violation.
	static const char *get_violation_name(ProtocolHandshakeViolation p_violation);
};

} // namespace tick_synchronizer

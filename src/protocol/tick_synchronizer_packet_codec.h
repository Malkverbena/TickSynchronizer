// Declares control packet structures, wire enums, limits, and codec operations.
// Defines the audited experimental handshake wire contract.

#pragma once

#include "src/internal/tick_synchronizer_build_config.h"
#include "src/internal/tick_synchronizer_version.h"

#include "core/string/ustring.h"
#include "core/variant/variant.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tick_synchronizer {

static constexpr std::size_t PROTOCOL_SHA1_SIZE = 20;
static constexpr std::size_t PROTOCOL_OPAQUE_ID_SIZE = 16;
static constexpr std::size_t PROTOCOL_GODOT_VERSION_SIZE = 32;

using ProtocolSha1 = std::array<uint8_t, PROTOCOL_SHA1_SIZE>;
using ProtocolOpaqueId = std::array<uint8_t, PROTOCOL_OPAQUE_ID_SIZE>;
using ProtocolGodotVersion = std::array<uint8_t, PROTOCOL_GODOT_VERSION_SIZE>;

enum class ProtocolPrecisionMode : uint8_t {
	INVALID = 0,
	SINGLE = 1,
	DOUBLE = 2,
};

enum class ProtocolPacketType : uint8_t {
	INVALID = 0,
	HELLO = 1,
	HELLO_ACK = 2,
	DISCONNECT_REASON = 3,
};

// Negotiated feature bits carried by the strict compatibility profile.
enum class ProtocolCapability : uint64_t {
	NONE = 0,
	CONTROL_PACKET_V1 = UINT64_C(1) << 0,
	STRICT_COMPATIBILITY = UINT64_C(1) << 1,
	LOGICAL_PAYLOAD_BITS = UINT64_C(1) << 2,
};

enum class ProtocolIdentityField : uint8_t {
	NONE = 0,
	GODOT_COMMIT = 1,
	MODULE_BUILD = 2,
	GAME_BUILD = 3,
	SCHEMA_COMPATIBILITY = 4,
	GODOT_VERSION = 5,
};

enum class ProtocolDisconnectReason : uint16_t {
	NONE = 0,
	PROTOCOL_VERSION_MISMATCH = 1,
	PRECISION_MISMATCH = 2,
	MALFORMED_PACKET = 3,
	PAYLOAD_TOO_LARGE = 4,
	UNSUPPORTED_PACKET_TYPE = 5,
	CAPABILITY_MISMATCH = 6,
	GODOT_COMMIT_MISMATCH = 7,
	MODULE_BUILD_MISMATCH = 8,
	GAME_BUILD_MISMATCH = 9,
	SCHEMA_MISMATCH = 10,
	MALFORMED_HANDSHAKE = 11,
	HELLO_NONCE_MISMATCH = 12,
	API_VERSION_MISMATCH = 13,
	WIRE_PROTOCOL_VERSION_MISMATCH = 14,
	WIRE_PROTOCOL_REVISION_MISMATCH = 15,
	BUILD_COMPATIBILITY_MISMATCH = 16,
	GODOT_VERSION_MISMATCH = 17,
};

enum class ProtocolCodecError : uint16_t {
	OK = 0,
	INVALID_ARGUMENT,
	OUT_OF_MEMORY,
	PACKET_TOO_SMALL,
	PACKET_TOO_LARGE,
	PAYLOAD_TOO_LARGE,
	MAGIC_MISMATCH,
	PROTOCOL_VERSION_MISMATCH,
	HEADER_SIZE_MISMATCH,
	UNKNOWN_PACKET_TYPE,
	UNKNOWN_FLAGS,
	RESERVED_NONZERO,
	PAYLOAD_SIZE_MISMATCH,
	PACKET_TRUNCATED,
	TRAILING_DATA,
	PAYLOAD_BIT_SIZE_INVALID,
	PAYLOAD_PADDING_NONZERO,
	MALFORMED_PAYLOAD,
	HELLO_VERSION_MISMATCH,
	INVALID_VERSION_CONTRACT,
	INVALID_PRECISION,
	UNKNOWN_DISCONNECT_REASON,
	UNKNOWN_IDENTITY_FIELD,
};

struct ProtocolPacketHeader {
	uint32_t magic = 0;
	uint8_t protocol_major = 0;
	uint8_t protocol_minor = 0;
	ProtocolPacketType packet_type = ProtocolPacketType::INVALID;
	uint8_t header_size = 0;
	uint16_t flags = 0;
	uint16_t reserved = 0;
	uint64_t session_id = 0;
	uint32_t sequence = 0;
	uint64_t tick = 0;
	uint32_t payload_size_bytes = 0;
	uint32_t payload_bit_size = 0;
};

struct ProtocolPacket {
	ProtocolPacketHeader header;
	PackedByteArray payload;
};

struct ProtocolCapabilitySet {
	uint64_t supported = 0;
	uint64_t required = 0;
};

struct ProtocolCompatibilityProfile {
	uint32_t api_version = version::API_VERSION;
	uint32_t wire_protocol_version = version::WIRE_PROTOCOL_VERSION;
	uint32_t wire_protocol_revision = version::WIRE_PROTOCOL_REVISION;
	ProtocolPrecisionMode precision = ProtocolPrecisionMode::INVALID;
	ProtocolGodotVersion godot_version = {};
	ProtocolSha1 godot_commit = {};
	ProtocolSha1 module_build_id = {};
	ProtocolOpaqueId game_build_id = {};
	ProtocolOpaqueId schema_compatibility_id = {};
	ProtocolCapabilitySet capabilities;
};

struct ProtocolHelloPayload {
	uint8_t payload_version = 4;
	uint64_t hello_nonce = 0;
	ProtocolCompatibilityProfile profile;
};

struct ProtocolHelloAckPayload {
	uint8_t payload_version = 4;
	uint64_t echoed_hello_nonce = 0;
	ProtocolCompatibilityProfile profile;
	uint64_t negotiated_capabilities = 0;
};

struct ProtocolDisconnectPayload {
	ProtocolDisconnectReason reason = ProtocolDisconnectReason::NONE;
	ProtocolPrecisionMode sender_precision = ProtocolPrecisionMode::INVALID;
	ProtocolPrecisionMode required_precision = ProtocolPrecisionMode::INVALID;
	ProtocolPrecisionMode peer_precision = ProtocolPrecisionMode::INVALID;
	ProtocolIdentityField identity_field = ProtocolIdentityField::NONE;
	uint8_t local_protocol_major = 0;
	uint8_t local_protocol_minor = 0;
	uint8_t peer_protocol_major = 0;
	uint8_t peer_protocol_minor = 0;
	uint32_t local_api_version = 0;
	uint32_t peer_api_version = 0;
	uint32_t local_wire_protocol_version = 0;
	uint32_t local_wire_protocol_revision = 0;
	uint32_t peer_wire_protocol_version = 0;
	uint32_t peer_wire_protocol_revision = 0;
	uint32_t detail_code = 0;
	uint64_t peer_id = 0;
	uint64_t detail_mask = 0;
};

class TickSynchronizerPacketCodec {
public:
	static constexpr uint32_t PROTOCOL_MAGIC = UINT32_C(0x4E595354);
	static constexpr uint8_t PROTOCOL_MAJOR = 1;
	static constexpr uint8_t PROTOCOL_MINOR = 1;
	static constexpr uint8_t CONTROL_HEADER_SIZE = 40;
	static constexpr uint16_t KNOWN_FLAGS_MASK = 0;
	// PROTOCOL_MAJOR/MINOR identify the fixed control envelope. The separate
	// wire compatibility contract is carried by HELLO/HELLO_ACK profiles.
	static constexpr uint8_t HELLO_PAYLOAD_VERSION = 4;
	static constexpr uint32_t COMPATIBILITY_PROFILE_SIZE = 132;
	static constexpr uint32_t HELLO_PAYLOAD_SIZE = 144;
	static constexpr uint32_t HELLO_ACK_PAYLOAD_SIZE = 152;
	static constexpr uint32_t DISCONNECT_PAYLOAD_SIZE = 56;
	static constexpr uint32_t PUBLIC_API_VERSION = version::API_VERSION;
	static constexpr uint32_t WIRE_PROTOCOL_VERSION = version::WIRE_PROTOCOL_VERSION;
	static constexpr uint32_t WIRE_PROTOCOL_REVISION = version::WIRE_PROTOCOL_REVISION;
	static constexpr bool EXACT_BUILD_MATCH_REQUIRED = version::EXACT_BUILD_MATCH_REQUIRED;
	static constexpr uint32_t DEFAULT_MAX_PACKET_SIZE_BYTES = 64 * 1024;
	static constexpr uint32_t MAX_PACKET_SIZE_BYTES = 1024 * 1024;
	static constexpr uint64_t CURRENT_SUPPORTED_CAPABILITIES =
			static_cast<uint64_t>(ProtocolCapability::CONTROL_PACKET_V1) |
			static_cast<uint64_t>(ProtocolCapability::STRICT_COMPATIBILITY) |
			static_cast<uint64_t>(ProtocolCapability::LOGICAL_PAYLOAD_BITS);
	static constexpr uint64_t CURRENT_REQUIRED_CAPABILITIES =
			static_cast<uint64_t>(ProtocolCapability::CONTROL_PACKET_V1) |
			static_cast<uint64_t>(ProtocolCapability::STRICT_COMPATIBILITY);

	// Maps the compile-time Godot precision to the wire enum.
	static ProtocolPrecisionMode get_build_precision_mode();

	// Checks API/wire values without consulting mutable runtime state.
	static bool is_valid_version_contract(
			uint32_t p_api_version,
			uint32_t p_wire_protocol_version,
			uint32_t p_wire_protocol_revision);

	// Checks whether a precision enum is valid on the wire.
	static bool is_valid_precision(ProtocolPrecisionMode p_precision);

	// Checks whether a control packet type is implemented by this revision.
	static bool is_known_packet_type(ProtocolPacketType p_packet_type);

	// Checks whether a structured disconnect reason is recognized.
	static bool is_known_disconnect_reason(ProtocolDisconnectReason p_reason);

	// Checks whether a disconnect identity-field selector is recognized.
	static bool is_known_identity_field(ProtocolIdentityField p_field);

	// Constructs the canonical current control header for a validated payload.
	static ProtocolPacketHeader make_header(
			ProtocolPacketType p_packet_type,
			uint64_t p_session_id,
			uint32_t p_sequence,
			uint64_t p_tick,
			uint32_t p_payload_size_bytes,
			uint32_t p_payload_bit_size);

	// Reads safe fixed fields for diagnostics without accepting the packet.
	static ProtocolCodecError inspect_control_header(
			const PackedByteArray &p_encoded,
			ProtocolPacketHeader &r_header);

	// Serializes a validated control packet without exceeding the caller limit.
	static ProtocolCodecError encode_packet(
			const ProtocolPacket &p_packet,
			PackedByteArray &r_encoded,
			uint32_t p_max_packet_size_bytes = DEFAULT_MAX_PACKET_SIZE_BYTES);

	// Atomically validates and decodes untrusted control packet bytes.
	static ProtocolCodecError decode_packet(
			const PackedByteArray &p_encoded,
			ProtocolPacket &r_packet,
			uint32_t p_max_packet_size_bytes = DEFAULT_MAX_PACKET_SIZE_BYTES);

	// Serializes the current HELLO payload version and compatibility profile.
	static ProtocolCodecError encode_hello_payload(
			const ProtocolHelloPayload &p_hello,
			PackedByteArray &r_payload);

	// Atomically validates and decodes a HELLO control payload.
	static ProtocolCodecError decode_hello_payload(
			const ProtocolPacket &p_packet,
			ProtocolHelloPayload &r_hello);

	// Serializes the current HELLO_ACK payload and negotiated capabilities.
	static ProtocolCodecError encode_hello_ack_payload(
			const ProtocolHelloAckPayload &p_ack,
			PackedByteArray &r_payload);

	// Atomically validates and decodes a HELLO_ACK payload.
	static ProtocolCodecError decode_hello_ack_payload(
			const ProtocolPacket &p_packet,
			ProtocolHelloAckPayload &r_ack);

	// Serializes a structured disconnect diagnostic payload.
	static ProtocolCodecError encode_disconnect_payload(
			const ProtocolDisconnectPayload &p_disconnect,
			PackedByteArray &r_payload);

	// Atomically validates and decodes a structured disconnect payload.
	static ProtocolCodecError decode_disconnect_payload(
			const ProtocolPacket &p_packet,
			ProtocolDisconnectPayload &r_disconnect);

	// Returns a stable diagnostic name for a codec error.
	static const char *get_error_name(ProtocolCodecError p_error);
	// Returns a stable diagnostic name for a wire precision.
	static const char *get_precision_name(ProtocolPrecisionMode p_precision);
	// Returns a stable diagnostic name for a control packet type.
	static const char *get_packet_type_name(ProtocolPacketType p_packet_type);
	// Returns a stable diagnostic name for a disconnect reason.
	static const char *get_disconnect_reason_name(ProtocolDisconnectReason p_reason);
	// Returns a stable diagnostic name for an identity selector.
	static const char *get_identity_field_name(ProtocolIdentityField p_field);

	// Returns the printable four-byte control-envelope magic.
	static String get_magic_string();
};

} // namespace tick_synchronizer

#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "core/string/ustring.h"
#include "core/variant/variant.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tick_synchronizer {

static constexpr std::size_t PROTOCOL_SHA1_SIZE = 20;
static constexpr std::size_t PROTOCOL_OPAQUE_ID_SIZE = 16;

using ProtocolSha1 = std::array<uint8_t, PROTOCOL_SHA1_SIZE>;
using ProtocolOpaqueId = std::array<uint8_t, PROTOCOL_OPAQUE_ID_SIZE>;

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
	ProtocolPrecisionMode precision = ProtocolPrecisionMode::INVALID;
	ProtocolSha1 godot_commit = {};
	ProtocolSha1 module_build_id = {};
	ProtocolOpaqueId game_build_id = {};
	ProtocolOpaqueId schema_compatibility_id = {};
	ProtocolCapabilitySet capabilities;
};

struct ProtocolHelloPayload {
	uint8_t payload_version = 2;
	uint64_t hello_nonce = 0;
	ProtocolCompatibilityProfile profile;
};

struct ProtocolHelloAckPayload {
	uint8_t payload_version = 2;
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
	static constexpr uint8_t HELLO_PAYLOAD_VERSION = 2;
	static constexpr uint32_t HELLO_PAYLOAD_SIZE = 100;
	static constexpr uint32_t HELLO_ACK_PAYLOAD_SIZE = 108;
	static constexpr uint32_t DISCONNECT_PAYLOAD_SIZE = 32;
	static constexpr uint32_t DEFAULT_MAX_PACKET_SIZE_BYTES = 64 * 1024;
	static constexpr uint32_t MAX_PACKET_SIZE_BYTES = 1024 * 1024;
	static constexpr uint64_t CURRENT_SUPPORTED_CAPABILITIES =
			static_cast<uint64_t>(ProtocolCapability::CONTROL_PACKET_V1) |
			static_cast<uint64_t>(ProtocolCapability::STRICT_COMPATIBILITY) |
			static_cast<uint64_t>(ProtocolCapability::LOGICAL_PAYLOAD_BITS);
	static constexpr uint64_t CURRENT_REQUIRED_CAPABILITIES =
			static_cast<uint64_t>(ProtocolCapability::CONTROL_PACKET_V1) |
			static_cast<uint64_t>(ProtocolCapability::STRICT_COMPATIBILITY);

	static ProtocolPrecisionMode get_build_precision_mode();
	static bool is_valid_precision(ProtocolPrecisionMode p_precision);
	static bool is_known_packet_type(ProtocolPacketType p_packet_type);
	static bool is_known_disconnect_reason(ProtocolDisconnectReason p_reason);
	static bool is_known_identity_field(ProtocolIdentityField p_field);

	static ProtocolPacketHeader make_header(
			ProtocolPacketType p_packet_type,
			uint64_t p_session_id,
			uint32_t p_sequence,
			uint64_t p_tick,
			uint32_t p_payload_size_bytes,
			uint32_t p_payload_bit_size);

	static ProtocolCodecError inspect_control_header(
			const PackedByteArray &p_encoded,
			ProtocolPacketHeader &r_header);

	static ProtocolCodecError encode_packet(
			const ProtocolPacket &p_packet,
			PackedByteArray &r_encoded,
			uint32_t p_max_packet_size_bytes = DEFAULT_MAX_PACKET_SIZE_BYTES);

	static ProtocolCodecError decode_packet(
			const PackedByteArray &p_encoded,
			ProtocolPacket &r_packet,
			uint32_t p_max_packet_size_bytes = DEFAULT_MAX_PACKET_SIZE_BYTES);

	static ProtocolCodecError encode_hello_payload(
			const ProtocolHelloPayload &p_hello,
			PackedByteArray &r_payload);
	static ProtocolCodecError decode_hello_payload(
			const ProtocolPacket &p_packet,
			ProtocolHelloPayload &r_hello);

	static ProtocolCodecError encode_hello_ack_payload(
			const ProtocolHelloAckPayload &p_ack,
			PackedByteArray &r_payload);
	static ProtocolCodecError decode_hello_ack_payload(
			const ProtocolPacket &p_packet,
			ProtocolHelloAckPayload &r_ack);

	static ProtocolCodecError encode_disconnect_payload(
			const ProtocolDisconnectPayload &p_disconnect,
			PackedByteArray &r_payload);
	static ProtocolCodecError decode_disconnect_payload(
			const ProtocolPacket &p_packet,
			ProtocolDisconnectPayload &r_disconnect);

	static const char *get_error_name(ProtocolCodecError p_error);
	static const char *get_precision_name(ProtocolPrecisionMode p_precision);
	static const char *get_packet_type_name(ProtocolPacketType p_packet_type);
	static const char *get_disconnect_reason_name(ProtocolDisconnectReason p_reason);
	static const char *get_identity_field_name(ProtocolIdentityField p_field);
	static String get_magic_string();
};

} // namespace tick_synchronizer

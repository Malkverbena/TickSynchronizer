// Implements the fixed control envelope and versioned handshake payload codecs.
// Validates untrusted lengths and fields before allocation or output mutation.

#include "tick_synchronizer_packet_codec.h"

#include <cstring>
#include <limits>

namespace tick_synchronizer {
namespace {

static void write_u16_le(uint8_t *p_destination, uint16_t p_value) {
	p_destination[0] = static_cast<uint8_t>(p_value & UINT16_C(0x00FF));
	p_destination[1] = static_cast<uint8_t>((p_value >> 8) & UINT16_C(0x00FF));
}


static void write_u32_le(uint8_t *p_destination, uint32_t p_value) {
	for (uint32_t index = 0; index < 4; index++) {
		p_destination[index] = static_cast<uint8_t>((p_value >> (index * 8)) & UINT32_C(0xFF));
	}
}


static void write_u64_le(uint8_t *p_destination, uint64_t p_value) {
	for (uint32_t index = 0; index < 8; index++) {
		p_destination[index] = static_cast<uint8_t>((p_value >> (index * 8)) & UINT64_C(0xFF));
	}
}


static uint16_t read_u16_le(const uint8_t *p_source) {
	return static_cast<uint16_t>(p_source[0]) |
			(static_cast<uint16_t>(p_source[1]) << 8);
}


static uint32_t read_u32_le(const uint8_t *p_source) {
	uint32_t value = 0;
	for (uint32_t index = 0; index < 4; index++) {
		value |= static_cast<uint32_t>(p_source[index]) << (index * 8);
	}
	return value;
}


static uint64_t read_u64_le(const uint8_t *p_source) {
	uint64_t value = 0;
	for (uint32_t index = 0; index < 8; index++) {
		value |= static_cast<uint64_t>(p_source[index]) << (index * 8);
	}
	return value;
}

template <std::size_t N>
static void write_fixed_bytes(uint8_t *p_destination, const std::array<uint8_t, N> &p_value) {
	std::memcpy(p_destination, p_value.data(), N);
}

template <std::size_t N>
static std::array<uint8_t, N> read_fixed_bytes(const uint8_t *p_source) {
	std::array<uint8_t, N> value = {};
	std::memcpy(value.data(), p_source, N);
	return value;
}


static bool is_valid_max_packet_size(uint32_t p_max_packet_size_bytes) {
	return p_max_packet_size_bytes >= TickSynchronizerPacketCodec::CONTROL_HEADER_SIZE &&
			p_max_packet_size_bytes <= TickSynchronizerPacketCodec::MAX_PACKET_SIZE_BYTES;
}


static bool has_canonical_payload_size(uint32_t p_payload_size_bytes, uint32_t p_payload_bit_size) {
	if (p_payload_bit_size == 0) {
		return p_payload_size_bytes == 0;
	}

	const uint64_t expected_size = (static_cast<uint64_t>(p_payload_bit_size) + 7) / 8;
	return expected_size == p_payload_size_bytes;
}


static bool has_zero_payload_padding(
		const uint8_t *p_payload,
		uint32_t p_payload_size_bytes,
		uint32_t p_payload_bit_size) {
	if (p_payload_size_bytes == 0 || (p_payload_bit_size & 7U) == 0) {
		return true;
	}

	const uint32_t used_bits = p_payload_bit_size & 7U;
	const uint8_t used_mask = static_cast<uint8_t>((UINT32_C(1) << used_bits) - 1U);
	return (p_payload[p_payload_size_bytes - 1] & static_cast<uint8_t>(~used_mask)) == 0;
}


static ProtocolCodecError validate_header(const ProtocolPacketHeader &p_header) {
	if (p_header.magic != TickSynchronizerPacketCodec::PROTOCOL_MAGIC) {
		return ProtocolCodecError::MAGIC_MISMATCH;
	}
	if (p_header.protocol_major != TickSynchronizerPacketCodec::PROTOCOL_MAJOR ||
			p_header.protocol_minor != TickSynchronizerPacketCodec::PROTOCOL_MINOR) {
		return ProtocolCodecError::PROTOCOL_VERSION_MISMATCH;
	}
	if (p_header.header_size != TickSynchronizerPacketCodec::CONTROL_HEADER_SIZE) {
		return ProtocolCodecError::HEADER_SIZE_MISMATCH;
	}
	if (!TickSynchronizerPacketCodec::is_known_packet_type(p_header.packet_type)) {
		return ProtocolCodecError::UNKNOWN_PACKET_TYPE;
	}
	if ((p_header.flags & static_cast<uint16_t>(~TickSynchronizerPacketCodec::KNOWN_FLAGS_MASK)) != 0) {
		return ProtocolCodecError::UNKNOWN_FLAGS;
	}
	if (p_header.reserved != 0) {
		return ProtocolCodecError::RESERVED_NONZERO;
	}
	return ProtocolCodecError::OK;
}


static ProtocolPacketHeader parse_header(const uint8_t *p_source) {
	ProtocolPacketHeader header;
	header.magic = read_u32_le(p_source + 0);
	header.protocol_major = p_source[4];
	header.protocol_minor = p_source[5];
	header.packet_type = static_cast<ProtocolPacketType>(p_source[6]);
	header.header_size = p_source[7];
	header.flags = read_u16_le(p_source + 8);
	header.reserved = read_u16_le(p_source + 10);
	header.session_id = read_u64_le(p_source + 12);
	header.sequence = read_u32_le(p_source + 20);
	header.tick = read_u64_le(p_source + 24);
	header.payload_size_bytes = read_u32_le(p_source + 32);
	header.payload_bit_size = read_u32_le(p_source + 36);
	return header;
}


static ProtocolCodecError resize_payload(PackedByteArray &r_payload, int64_t p_size) {
	const Error resize_error = r_payload.resize_initialized(p_size);
	return resize_error == OK ? ProtocolCodecError::OK : ProtocolCodecError::OUT_OF_MEMORY;
}


static bool is_optional_precision_valid(ProtocolPrecisionMode p_precision) {
	return p_precision == ProtocolPrecisionMode::INVALID ||
			TickSynchronizerPacketCodec::is_valid_precision(p_precision);
}


static void write_profile(uint8_t *p_destination, const ProtocolCompatibilityProfile &p_profile) {
	write_u32_le(p_destination + 0, p_profile.api_version);
	write_u32_le(p_destination + 4, p_profile.wire_protocol_version);
	write_u32_le(p_destination + 8, p_profile.wire_protocol_revision);
	write_fixed_bytes(p_destination + 12, p_profile.godot_version);
	write_fixed_bytes(p_destination + 44, p_profile.godot_commit);
	write_fixed_bytes(p_destination + 64, p_profile.module_build_id);
	write_fixed_bytes(p_destination + 84, p_profile.game_build_id);
	write_fixed_bytes(p_destination + 100, p_profile.schema_compatibility_id);
	write_u64_le(p_destination + 116, p_profile.capabilities.supported);
	write_u64_le(p_destination + 124, p_profile.capabilities.required);
}


static ProtocolCompatibilityProfile read_profile(
		const uint8_t *p_source,
		ProtocolPrecisionMode p_precision) {
	ProtocolCompatibilityProfile profile;
	profile.api_version = read_u32_le(p_source + 0);
	profile.wire_protocol_version = read_u32_le(p_source + 4);
	profile.wire_protocol_revision = read_u32_le(p_source + 8);
	profile.precision = p_precision;
	profile.godot_version = read_fixed_bytes<PROTOCOL_GODOT_VERSION_SIZE>(p_source + 12);
	profile.godot_commit = read_fixed_bytes<PROTOCOL_SHA1_SIZE>(p_source + 44);
	profile.module_build_id = read_fixed_bytes<PROTOCOL_SHA1_SIZE>(p_source + 64);
	profile.game_build_id = read_fixed_bytes<PROTOCOL_OPAQUE_ID_SIZE>(p_source + 84);
	profile.schema_compatibility_id = read_fixed_bytes<PROTOCOL_OPAQUE_ID_SIZE>(p_source + 100);
	profile.capabilities.supported = read_u64_le(p_source + 116);
	profile.capabilities.required = read_u64_le(p_source + 124);
	return profile;
}

} // namespace

ProtocolPrecisionMode TickSynchronizerPacketCodec::get_build_precision_mode() {
#ifdef REAL_T_IS_DOUBLE
	return ProtocolPrecisionMode::DOUBLE;
#else
	return ProtocolPrecisionMode::SINGLE;
#endif
}


bool TickSynchronizerPacketCodec::is_valid_version_contract(
		uint32_t p_api_version,
		uint32_t p_wire_protocol_version,
		uint32_t p_wire_protocol_revision) {
	if (p_api_version == 0) {
		return false;
	}
	if (p_wire_protocol_version == 0) {
		return p_wire_protocol_revision > 0;
	}
	return p_wire_protocol_revision == 0;
}


bool TickSynchronizerPacketCodec::is_valid_precision(ProtocolPrecisionMode p_precision) {
	return p_precision == ProtocolPrecisionMode::SINGLE || p_precision == ProtocolPrecisionMode::DOUBLE;
}


bool TickSynchronizerPacketCodec::is_known_packet_type(ProtocolPacketType p_packet_type) {
	return p_packet_type == ProtocolPacketType::HELLO ||
			p_packet_type == ProtocolPacketType::HELLO_ACK ||
			p_packet_type == ProtocolPacketType::DISCONNECT_REASON;
}


bool TickSynchronizerPacketCodec::is_known_disconnect_reason(ProtocolDisconnectReason p_reason) {
	switch (p_reason) {
		case ProtocolDisconnectReason::PROTOCOL_VERSION_MISMATCH:
		case ProtocolDisconnectReason::PRECISION_MISMATCH:
		case ProtocolDisconnectReason::MALFORMED_PACKET:
		case ProtocolDisconnectReason::PAYLOAD_TOO_LARGE:
		case ProtocolDisconnectReason::UNSUPPORTED_PACKET_TYPE:
		case ProtocolDisconnectReason::CAPABILITY_MISMATCH:
		case ProtocolDisconnectReason::GODOT_COMMIT_MISMATCH:
		case ProtocolDisconnectReason::MODULE_BUILD_MISMATCH:
		case ProtocolDisconnectReason::GAME_BUILD_MISMATCH:
		case ProtocolDisconnectReason::SCHEMA_MISMATCH:
		case ProtocolDisconnectReason::MALFORMED_HANDSHAKE:
		case ProtocolDisconnectReason::HELLO_NONCE_MISMATCH:
		case ProtocolDisconnectReason::API_VERSION_MISMATCH:
		case ProtocolDisconnectReason::WIRE_PROTOCOL_VERSION_MISMATCH:
		case ProtocolDisconnectReason::WIRE_PROTOCOL_REVISION_MISMATCH:
		case ProtocolDisconnectReason::BUILD_COMPATIBILITY_MISMATCH:
		case ProtocolDisconnectReason::GODOT_VERSION_MISMATCH:
			return true;
		case ProtocolDisconnectReason::NONE:
		default:
			return false;
	}
}


bool TickSynchronizerPacketCodec::is_known_identity_field(ProtocolIdentityField p_field) {
	switch (p_field) {
		case ProtocolIdentityField::NONE:
		case ProtocolIdentityField::GODOT_COMMIT:
		case ProtocolIdentityField::MODULE_BUILD:
		case ProtocolIdentityField::GAME_BUILD:
		case ProtocolIdentityField::SCHEMA_COMPATIBILITY:
		case ProtocolIdentityField::GODOT_VERSION:
			return true;
		default:
			return false;
	}
}


ProtocolPacketHeader TickSynchronizerPacketCodec::make_header(
		ProtocolPacketType p_packet_type,
		uint64_t p_session_id,
		uint32_t p_sequence,
		uint64_t p_tick,
		uint32_t p_payload_size_bytes,
		uint32_t p_payload_bit_size) {
	ProtocolPacketHeader header;
	header.magic = PROTOCOL_MAGIC;
	header.protocol_major = PROTOCOL_MAJOR;
	header.protocol_minor = PROTOCOL_MINOR;
	header.packet_type = p_packet_type;
	header.header_size = CONTROL_HEADER_SIZE;
	header.flags = 0;
	header.reserved = 0;
	header.session_id = p_session_id;
	header.sequence = p_sequence;
	header.tick = p_tick;
	header.payload_size_bytes = p_payload_size_bytes;
	header.payload_bit_size = p_payload_bit_size;
	return header;
}


ProtocolCodecError TickSynchronizerPacketCodec::inspect_control_header(
		const PackedByteArray &p_encoded,
		ProtocolPacketHeader &r_header) {
	if (p_encoded.size() < CONTROL_HEADER_SIZE) {
		return ProtocolCodecError::PACKET_TOO_SMALL;
	}

	const ProtocolPacketHeader inspected = parse_header(p_encoded.ptr());
	if (inspected.magic != PROTOCOL_MAGIC) {
		return ProtocolCodecError::MAGIC_MISMATCH;
	}

	r_header = inspected;
	return ProtocolCodecError::OK;
}


ProtocolCodecError TickSynchronizerPacketCodec::encode_packet(
		const ProtocolPacket &p_packet,
		PackedByteArray &r_encoded,
		uint32_t p_max_packet_size_bytes) {
	if (!is_valid_max_packet_size(p_max_packet_size_bytes)) {
		return ProtocolCodecError::INVALID_ARGUMENT;
	}

	const ProtocolCodecError header_error = validate_header(p_packet.header);
	if (header_error != ProtocolCodecError::OK) {
		return header_error;
	}

	const int64_t payload_size = p_packet.payload.size();
	if (payload_size < 0 || payload_size > std::numeric_limits<uint32_t>::max()) {
		return ProtocolCodecError::PAYLOAD_TOO_LARGE;
	}
	if (p_packet.header.payload_size_bytes != static_cast<uint32_t>(payload_size)) {
		return ProtocolCodecError::PAYLOAD_SIZE_MISMATCH;
	}
	if (!has_canonical_payload_size(p_packet.header.payload_size_bytes, p_packet.header.payload_bit_size)) {
		return ProtocolCodecError::PAYLOAD_BIT_SIZE_INVALID;
	}
	if (!has_zero_payload_padding(
			p_packet.payload.ptr(),
			p_packet.header.payload_size_bytes,
			p_packet.header.payload_bit_size)) {
		return ProtocolCodecError::PAYLOAD_PADDING_NONZERO;
	}

	const uint64_t total_size = static_cast<uint64_t>(CONTROL_HEADER_SIZE) +
			static_cast<uint64_t>(p_packet.header.payload_size_bytes);
	if (total_size > p_max_packet_size_bytes || total_size > MAX_PACKET_SIZE_BYTES) {
		return ProtocolCodecError::PACKET_TOO_LARGE;
	}

	PackedByteArray encoded;
	if (resize_payload(encoded, static_cast<int64_t>(total_size)) != ProtocolCodecError::OK) {
		return ProtocolCodecError::OUT_OF_MEMORY;
	}

	uint8_t *destination = encoded.ptrw();
	write_u32_le(destination + 0, p_packet.header.magic);
	destination[4] = p_packet.header.protocol_major;
	destination[5] = p_packet.header.protocol_minor;
	destination[6] = static_cast<uint8_t>(p_packet.header.packet_type);
	destination[7] = p_packet.header.header_size;
	write_u16_le(destination + 8, p_packet.header.flags);
	write_u16_le(destination + 10, p_packet.header.reserved);
	write_u64_le(destination + 12, p_packet.header.session_id);
	write_u32_le(destination + 20, p_packet.header.sequence);
	write_u64_le(destination + 24, p_packet.header.tick);
	write_u32_le(destination + 32, p_packet.header.payload_size_bytes);
	write_u32_le(destination + 36, p_packet.header.payload_bit_size);

	if (p_packet.header.payload_size_bytes > 0) {
		std::memcpy(
				destination + CONTROL_HEADER_SIZE,
				p_packet.payload.ptr(),
				p_packet.header.payload_size_bytes);
	}

	r_encoded = encoded;
	return ProtocolCodecError::OK;
}


ProtocolCodecError TickSynchronizerPacketCodec::decode_packet(
		const PackedByteArray &p_encoded,
		ProtocolPacket &r_packet,
		uint32_t p_max_packet_size_bytes) {
	if (!is_valid_max_packet_size(p_max_packet_size_bytes)) {
		return ProtocolCodecError::INVALID_ARGUMENT;
	}
	if (p_encoded.size() > static_cast<int64_t>(p_max_packet_size_bytes) ||
			p_encoded.size() > static_cast<int64_t>(MAX_PACKET_SIZE_BYTES)) {
		return ProtocolCodecError::PACKET_TOO_LARGE;
	}
	if (p_encoded.size() < CONTROL_HEADER_SIZE) {
		return ProtocolCodecError::PACKET_TOO_SMALL;
	}

	ProtocolPacket decoded;
	decoded.header = parse_header(p_encoded.ptr());
	const ProtocolCodecError header_error = validate_header(decoded.header);
	if (header_error != ProtocolCodecError::OK) {
		return header_error;
	}

	const uint64_t expected_total_size = static_cast<uint64_t>(decoded.header.header_size) +
			static_cast<uint64_t>(decoded.header.payload_size_bytes);
	if (expected_total_size > p_max_packet_size_bytes || expected_total_size > MAX_PACKET_SIZE_BYTES) {
		return ProtocolCodecError::PACKET_TOO_LARGE;
	}
	if (expected_total_size > static_cast<uint64_t>(p_encoded.size())) {
		return ProtocolCodecError::PACKET_TRUNCATED;
	}
	if (expected_total_size < static_cast<uint64_t>(p_encoded.size())) {
		return ProtocolCodecError::TRAILING_DATA;
	}
	if (!has_canonical_payload_size(decoded.header.payload_size_bytes, decoded.header.payload_bit_size)) {
		return ProtocolCodecError::PAYLOAD_BIT_SIZE_INVALID;
	}

	const uint8_t *payload_source = p_encoded.ptr() + decoded.header.header_size;
	if (!has_zero_payload_padding(
			payload_source,
			decoded.header.payload_size_bytes,
			decoded.header.payload_bit_size)) {
		return ProtocolCodecError::PAYLOAD_PADDING_NONZERO;
	}

	if (resize_payload(decoded.payload, decoded.header.payload_size_bytes) != ProtocolCodecError::OK) {
		return ProtocolCodecError::OUT_OF_MEMORY;
	}
	if (decoded.header.payload_size_bytes > 0) {
		std::memcpy(
				decoded.payload.ptrw(),
				payload_source,
				decoded.header.payload_size_bytes);
	}

	r_packet = decoded;
	return ProtocolCodecError::OK;
}


ProtocolCodecError TickSynchronizerPacketCodec::encode_hello_payload(
		const ProtocolHelloPayload &p_hello,
		PackedByteArray &r_payload) {
	if (p_hello.payload_version != HELLO_PAYLOAD_VERSION) {
		return ProtocolCodecError::HELLO_VERSION_MISMATCH;
	}
	if (!is_valid_version_contract(
			p_hello.profile.api_version,
			p_hello.profile.wire_protocol_version,
			p_hello.profile.wire_protocol_revision)) {
		return ProtocolCodecError::INVALID_VERSION_CONTRACT;
	}
	if (!is_valid_precision(p_hello.profile.precision)) {
		return ProtocolCodecError::INVALID_PRECISION;
	}

	PackedByteArray payload;
	if (resize_payload(payload, HELLO_PAYLOAD_SIZE) != ProtocolCodecError::OK) {
		return ProtocolCodecError::OUT_OF_MEMORY;
	}
	uint8_t *destination = payload.ptrw();
	destination[0] = p_hello.payload_version;
	destination[1] = static_cast<uint8_t>(p_hello.profile.precision);
	write_u16_le(destination + 2, 0);
	write_u64_le(destination + 4, p_hello.hello_nonce);
	write_profile(destination + 12, p_hello.profile);
	r_payload = payload;
	return ProtocolCodecError::OK;
}


ProtocolCodecError TickSynchronizerPacketCodec::decode_hello_payload(
		const ProtocolPacket &p_packet,
		ProtocolHelloPayload &r_hello) {
	if (p_packet.header.packet_type != ProtocolPacketType::HELLO) {
		return ProtocolCodecError::UNKNOWN_PACKET_TYPE;
	}
	if (p_packet.header.payload_size_bytes != HELLO_PAYLOAD_SIZE ||
			p_packet.header.payload_bit_size != HELLO_PAYLOAD_SIZE * 8U ||
			p_packet.payload.size() != HELLO_PAYLOAD_SIZE) {
		return ProtocolCodecError::MALFORMED_PAYLOAD;
	}

	const uint8_t *source = p_packet.payload.ptr();
	if (source[0] != HELLO_PAYLOAD_VERSION) {
		return ProtocolCodecError::HELLO_VERSION_MISMATCH;
	}
	const ProtocolPrecisionMode precision = static_cast<ProtocolPrecisionMode>(source[1]);
	if (!is_valid_precision(precision)) {
		return ProtocolCodecError::INVALID_PRECISION;
	}
	if (read_u16_le(source + 2) != 0) {
		return ProtocolCodecError::RESERVED_NONZERO;
	}

	ProtocolHelloPayload decoded;
	decoded.payload_version = source[0];
	decoded.hello_nonce = read_u64_le(source + 4);
	decoded.profile = read_profile(source + 12, precision);
	if (!is_valid_version_contract(
			decoded.profile.api_version,
			decoded.profile.wire_protocol_version,
			decoded.profile.wire_protocol_revision)) {
		return ProtocolCodecError::INVALID_VERSION_CONTRACT;
	}
	r_hello = decoded;
	return ProtocolCodecError::OK;
}


ProtocolCodecError TickSynchronizerPacketCodec::encode_hello_ack_payload(
		const ProtocolHelloAckPayload &p_ack,
		PackedByteArray &r_payload) {
	if (p_ack.payload_version != HELLO_PAYLOAD_VERSION) {
		return ProtocolCodecError::HELLO_VERSION_MISMATCH;
	}
	if (!is_valid_version_contract(
			p_ack.profile.api_version,
			p_ack.profile.wire_protocol_version,
			p_ack.profile.wire_protocol_revision)) {
		return ProtocolCodecError::INVALID_VERSION_CONTRACT;
	}
	if (!is_valid_precision(p_ack.profile.precision)) {
		return ProtocolCodecError::INVALID_PRECISION;
	}

	PackedByteArray payload;
	if (resize_payload(payload, HELLO_ACK_PAYLOAD_SIZE) != ProtocolCodecError::OK) {
		return ProtocolCodecError::OUT_OF_MEMORY;
	}
	uint8_t *destination = payload.ptrw();
	destination[0] = p_ack.payload_version;
	destination[1] = static_cast<uint8_t>(p_ack.profile.precision);
	write_u16_le(destination + 2, 0);
	write_u64_le(destination + 4, p_ack.echoed_hello_nonce);
	write_profile(destination + 12, p_ack.profile);
	write_u64_le(destination + 144, p_ack.negotiated_capabilities);
	r_payload = payload;
	return ProtocolCodecError::OK;
}


ProtocolCodecError TickSynchronizerPacketCodec::decode_hello_ack_payload(
		const ProtocolPacket &p_packet,
		ProtocolHelloAckPayload &r_ack) {
	if (p_packet.header.packet_type != ProtocolPacketType::HELLO_ACK) {
		return ProtocolCodecError::UNKNOWN_PACKET_TYPE;
	}
	if (p_packet.header.payload_size_bytes != HELLO_ACK_PAYLOAD_SIZE ||
			p_packet.header.payload_bit_size != HELLO_ACK_PAYLOAD_SIZE * 8U ||
			p_packet.payload.size() != HELLO_ACK_PAYLOAD_SIZE) {
		return ProtocolCodecError::MALFORMED_PAYLOAD;
	}

	const uint8_t *source = p_packet.payload.ptr();
	if (source[0] != HELLO_PAYLOAD_VERSION) {
		return ProtocolCodecError::HELLO_VERSION_MISMATCH;
	}
	const ProtocolPrecisionMode precision = static_cast<ProtocolPrecisionMode>(source[1]);
	if (!is_valid_precision(precision)) {
		return ProtocolCodecError::INVALID_PRECISION;
	}
	if (read_u16_le(source + 2) != 0) {
		return ProtocolCodecError::RESERVED_NONZERO;
	}

	ProtocolHelloAckPayload decoded;
	decoded.payload_version = source[0];
	decoded.echoed_hello_nonce = read_u64_le(source + 4);
	decoded.profile = read_profile(source + 12, precision);
	if (!is_valid_version_contract(
			decoded.profile.api_version,
			decoded.profile.wire_protocol_version,
			decoded.profile.wire_protocol_revision)) {
		return ProtocolCodecError::INVALID_VERSION_CONTRACT;
	}
	decoded.negotiated_capabilities = read_u64_le(source + 144);
	r_ack = decoded;
	return ProtocolCodecError::OK;
}


ProtocolCodecError TickSynchronizerPacketCodec::encode_disconnect_payload(
		const ProtocolDisconnectPayload &p_disconnect,
		PackedByteArray &r_payload) {
	if (!is_known_disconnect_reason(p_disconnect.reason)) {
		return ProtocolCodecError::UNKNOWN_DISCONNECT_REASON;
	}
	if (!is_valid_precision(p_disconnect.sender_precision) ||
			!is_optional_precision_valid(p_disconnect.required_precision) ||
			!is_optional_precision_valid(p_disconnect.peer_precision)) {
		return ProtocolCodecError::INVALID_PRECISION;
	}
	if (!is_known_identity_field(p_disconnect.identity_field)) {
		return ProtocolCodecError::UNKNOWN_IDENTITY_FIELD;
	}

	PackedByteArray payload;
	if (resize_payload(payload, DISCONNECT_PAYLOAD_SIZE) != ProtocolCodecError::OK) {
		return ProtocolCodecError::OUT_OF_MEMORY;
	}
	uint8_t *destination = payload.ptrw();
	write_u16_le(destination + 0, static_cast<uint16_t>(p_disconnect.reason));
	destination[2] = static_cast<uint8_t>(p_disconnect.sender_precision);
	destination[3] = static_cast<uint8_t>(p_disconnect.required_precision);
	destination[4] = static_cast<uint8_t>(p_disconnect.peer_precision);
	destination[5] = static_cast<uint8_t>(p_disconnect.identity_field);
	destination[6] = p_disconnect.local_protocol_major;
	destination[7] = p_disconnect.local_protocol_minor;
	destination[8] = p_disconnect.peer_protocol_major;
	destination[9] = p_disconnect.peer_protocol_minor;
	write_u16_le(destination + 10, 0);
	write_u32_le(destination + 12, p_disconnect.local_api_version);
	write_u32_le(destination + 16, p_disconnect.peer_api_version);
	write_u32_le(destination + 20, p_disconnect.local_wire_protocol_version);
	write_u32_le(destination + 24, p_disconnect.local_wire_protocol_revision);
	write_u32_le(destination + 28, p_disconnect.peer_wire_protocol_version);
	write_u32_le(destination + 32, p_disconnect.peer_wire_protocol_revision);
	write_u32_le(destination + 36, p_disconnect.detail_code);
	write_u64_le(destination + 40, p_disconnect.peer_id);
	write_u64_le(destination + 48, p_disconnect.detail_mask);
	r_payload = payload;
	return ProtocolCodecError::OK;
}


ProtocolCodecError TickSynchronizerPacketCodec::decode_disconnect_payload(
		const ProtocolPacket &p_packet,
		ProtocolDisconnectPayload &r_disconnect) {
	if (p_packet.header.packet_type != ProtocolPacketType::DISCONNECT_REASON) {
		return ProtocolCodecError::UNKNOWN_PACKET_TYPE;
	}
	if (p_packet.header.payload_size_bytes != DISCONNECT_PAYLOAD_SIZE ||
			p_packet.header.payload_bit_size != DISCONNECT_PAYLOAD_SIZE * 8U ||
			p_packet.payload.size() != DISCONNECT_PAYLOAD_SIZE) {
		return ProtocolCodecError::MALFORMED_PAYLOAD;
	}

	const uint8_t *source = p_packet.payload.ptr();
	ProtocolDisconnectPayload decoded;
	decoded.reason = static_cast<ProtocolDisconnectReason>(read_u16_le(source + 0));
	decoded.sender_precision = static_cast<ProtocolPrecisionMode>(source[2]);
	decoded.required_precision = static_cast<ProtocolPrecisionMode>(source[3]);
	decoded.peer_precision = static_cast<ProtocolPrecisionMode>(source[4]);
	decoded.identity_field = static_cast<ProtocolIdentityField>(source[5]);
	decoded.local_protocol_major = source[6];
	decoded.local_protocol_minor = source[7];
	decoded.peer_protocol_major = source[8];
	decoded.peer_protocol_minor = source[9];
	decoded.local_api_version = read_u32_le(source + 12);
	decoded.peer_api_version = read_u32_le(source + 16);
	decoded.local_wire_protocol_version = read_u32_le(source + 20);
	decoded.local_wire_protocol_revision = read_u32_le(source + 24);
	decoded.peer_wire_protocol_version = read_u32_le(source + 28);
	decoded.peer_wire_protocol_revision = read_u32_le(source + 32);
	decoded.detail_code = read_u32_le(source + 36);
	decoded.peer_id = read_u64_le(source + 40);
	decoded.detail_mask = read_u64_le(source + 48);

	if (!is_known_disconnect_reason(decoded.reason)) {
		return ProtocolCodecError::UNKNOWN_DISCONNECT_REASON;
	}
	if (!is_valid_precision(decoded.sender_precision) ||
			!is_optional_precision_valid(decoded.required_precision) ||
			!is_optional_precision_valid(decoded.peer_precision)) {
		return ProtocolCodecError::INVALID_PRECISION;
	}
	if (!is_known_identity_field(decoded.identity_field)) {
		return ProtocolCodecError::UNKNOWN_IDENTITY_FIELD;
	}
	if (read_u16_le(source + 10) != 0) {
		return ProtocolCodecError::RESERVED_NONZERO;
	}

	r_disconnect = decoded;
	return ProtocolCodecError::OK;
}

const char *TickSynchronizerPacketCodec::get_error_name(ProtocolCodecError p_error) {
	switch (p_error) {
		case ProtocolCodecError::OK: return "OK";
		case ProtocolCodecError::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
		case ProtocolCodecError::OUT_OF_MEMORY: return "OUT_OF_MEMORY";
		case ProtocolCodecError::PACKET_TOO_SMALL: return "PACKET_TOO_SMALL";
		case ProtocolCodecError::PACKET_TOO_LARGE: return "PACKET_TOO_LARGE";
		case ProtocolCodecError::PAYLOAD_TOO_LARGE: return "PAYLOAD_TOO_LARGE";
		case ProtocolCodecError::MAGIC_MISMATCH: return "MAGIC_MISMATCH";
		case ProtocolCodecError::PROTOCOL_VERSION_MISMATCH: return "PROTOCOL_VERSION_MISMATCH";
		case ProtocolCodecError::HEADER_SIZE_MISMATCH: return "HEADER_SIZE_MISMATCH";
		case ProtocolCodecError::UNKNOWN_PACKET_TYPE: return "UNKNOWN_PACKET_TYPE";
		case ProtocolCodecError::UNKNOWN_FLAGS: return "UNKNOWN_FLAGS";
		case ProtocolCodecError::RESERVED_NONZERO: return "RESERVED_NONZERO";
		case ProtocolCodecError::PAYLOAD_SIZE_MISMATCH: return "PAYLOAD_SIZE_MISMATCH";
		case ProtocolCodecError::PACKET_TRUNCATED: return "PACKET_TRUNCATED";
		case ProtocolCodecError::TRAILING_DATA: return "TRAILING_DATA";
		case ProtocolCodecError::PAYLOAD_BIT_SIZE_INVALID: return "PAYLOAD_BIT_SIZE_INVALID";
		case ProtocolCodecError::PAYLOAD_PADDING_NONZERO: return "PAYLOAD_PADDING_NONZERO";
		case ProtocolCodecError::MALFORMED_PAYLOAD: return "MALFORMED_PAYLOAD";
		case ProtocolCodecError::HELLO_VERSION_MISMATCH: return "HELLO_VERSION_MISMATCH";
		case ProtocolCodecError::INVALID_VERSION_CONTRACT: return "INVALID_VERSION_CONTRACT";
		case ProtocolCodecError::INVALID_PRECISION: return "INVALID_PRECISION";
		case ProtocolCodecError::UNKNOWN_DISCONNECT_REASON: return "UNKNOWN_DISCONNECT_REASON";
		case ProtocolCodecError::UNKNOWN_IDENTITY_FIELD: return "UNKNOWN_IDENTITY_FIELD";
		default: return "UNKNOWN_ERROR";
	}
}

const char *TickSynchronizerPacketCodec::get_precision_name(ProtocolPrecisionMode p_precision) {
	switch (p_precision) {
		case ProtocolPrecisionMode::SINGLE: return "single";
		case ProtocolPrecisionMode::DOUBLE: return "double";
		case ProtocolPrecisionMode::INVALID:
		default: return "invalid";
	}
}

const char *TickSynchronizerPacketCodec::get_packet_type_name(ProtocolPacketType p_packet_type) {
	switch (p_packet_type) {
		case ProtocolPacketType::HELLO: return "HELLO";
		case ProtocolPacketType::HELLO_ACK: return "HELLO_ACK";
		case ProtocolPacketType::DISCONNECT_REASON: return "DISCONNECT_REASON";
		case ProtocolPacketType::INVALID:
		default: return "INVALID";
	}
}

const char *TickSynchronizerPacketCodec::get_disconnect_reason_name(ProtocolDisconnectReason p_reason) {
	switch (p_reason) {
		case ProtocolDisconnectReason::PROTOCOL_VERSION_MISMATCH: return "PROTOCOL_VERSION_MISMATCH";
		case ProtocolDisconnectReason::PRECISION_MISMATCH: return "PRECISION_MISMATCH";
		case ProtocolDisconnectReason::MALFORMED_PACKET: return "MALFORMED_PACKET";
		case ProtocolDisconnectReason::PAYLOAD_TOO_LARGE: return "PAYLOAD_TOO_LARGE";
		case ProtocolDisconnectReason::UNSUPPORTED_PACKET_TYPE: return "UNSUPPORTED_PACKET_TYPE";
		case ProtocolDisconnectReason::CAPABILITY_MISMATCH: return "CAPABILITY_MISMATCH";
		case ProtocolDisconnectReason::GODOT_COMMIT_MISMATCH: return "GODOT_COMMIT_MISMATCH";
		case ProtocolDisconnectReason::MODULE_BUILD_MISMATCH: return "MODULE_BUILD_MISMATCH";
		case ProtocolDisconnectReason::GAME_BUILD_MISMATCH: return "GAME_BUILD_MISMATCH";
		case ProtocolDisconnectReason::SCHEMA_MISMATCH: return "SCHEMA_MISMATCH";
		case ProtocolDisconnectReason::MALFORMED_HANDSHAKE: return "MALFORMED_HANDSHAKE";
		case ProtocolDisconnectReason::HELLO_NONCE_MISMATCH: return "HELLO_NONCE_MISMATCH";
		case ProtocolDisconnectReason::API_VERSION_MISMATCH: return "API_VERSION_MISMATCH";
		case ProtocolDisconnectReason::WIRE_PROTOCOL_VERSION_MISMATCH:
			return "WIRE_PROTOCOL_VERSION_MISMATCH";
		case ProtocolDisconnectReason::WIRE_PROTOCOL_REVISION_MISMATCH:
			return "WIRE_PROTOCOL_REVISION_MISMATCH";
		case ProtocolDisconnectReason::BUILD_COMPATIBILITY_MISMATCH:
			return "BUILD_COMPATIBILITY_MISMATCH";
		case ProtocolDisconnectReason::GODOT_VERSION_MISMATCH:
			return "GODOT_VERSION_MISMATCH";
		case ProtocolDisconnectReason::NONE:
		default: return "NONE";
	}
}

const char *TickSynchronizerPacketCodec::get_identity_field_name(ProtocolIdentityField p_field) {
	switch (p_field) {
		case ProtocolIdentityField::GODOT_COMMIT: return "GODOT_COMMIT";
		case ProtocolIdentityField::MODULE_BUILD: return "MODULE_BUILD";
		case ProtocolIdentityField::GAME_BUILD: return "GAME_BUILD";
		case ProtocolIdentityField::SCHEMA_COMPATIBILITY: return "SCHEMA_COMPATIBILITY";
		case ProtocolIdentityField::GODOT_VERSION: return "GODOT_VERSION";
		case ProtocolIdentityField::NONE:
		default: return "NONE";
	}
}


String TickSynchronizerPacketCodec::get_magic_string() {
	return "TSYN";
}

} // namespace tick_synchronizer

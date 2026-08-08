// Tests control packet and versioned payload encoding and decoding.
// Protects wire layout, canonical validation, limits, and golden packets.

#pragma once

#include "tests/test_macros.h"

#include "src/protocol/tick_synchronizer_packet_codec.h"

#include <cstdint>

namespace TestTickSynchronizerPacketCodec {

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

static ProtocolGodotVersion make_godot_version() {
	ProtocolGodotVersion value = {};
	constexpr char text[] = "4.7.1-stable";
	for (std::size_t index = 0; index < sizeof(text) - 1; index++) {
		value[index] = static_cast<uint8_t>(text[index]);
	}
	return value;
}

static ProtocolCompatibilityProfile make_profile(ProtocolPrecisionMode p_precision) {
	ProtocolCompatibilityProfile profile;
	profile.precision = p_precision;
	profile.godot_version = make_godot_version();
	profile.godot_commit = make_sha1(0x01);
	profile.module_build_id = make_sha1(0x21);
	profile.game_build_id = make_opaque_id(0x41);
	profile.schema_compatibility_id = make_opaque_id(0x61);
	profile.capabilities.supported = TickSynchronizerPacketCodec::CURRENT_SUPPORTED_CAPABILITIES;
	profile.capabilities.required = TickSynchronizerPacketCodec::CURRENT_REQUIRED_CAPABILITIES;
	return profile;
}

static ProtocolPacket make_packet(
		ProtocolPacketType p_type,
		const PackedByteArray &p_payload,
		uint32_t p_payload_bit_size,
		uint64_t p_session_id = UINT64_C(0x0123456789ABCDEF),
		uint32_t p_sequence = UINT32_C(0x10203040),
		uint64_t p_tick = UINT64_C(0x0102030405060708)) {
	ProtocolPacket packet;
	packet.payload = p_payload;
	// Constructs the canonical current control header for a validated payload.
	packet.header = TickSynchronizerPacketCodec::make_header(
			p_type,
			p_session_id,
			p_sequence,
			p_tick,
			static_cast<uint32_t>(p_payload.size()),
			p_payload_bit_size);
	return packet;
}

static PackedByteArray make_bytes(const uint8_t *p_values, int64_t p_size) {
	PackedByteArray data;
	if (data.resize_initialized(p_size) != OK) {
		return PackedByteArray();
	}
	for (int64_t index = 0; index < p_size; index++) {
		data.ptrw()[index] = p_values[index];
	}
	return data;
}

static ProtocolCodecError encode_default_hello(PackedByteArray &r_encoded) {
	ProtocolHelloPayload hello;
	hello.hello_nonce = UINT64_C(0x8877665544332211);
	hello.profile = make_profile(ProtocolPrecisionMode::DOUBLE);
	PackedByteArray payload;
	// Serializes the current HELLO payload version and compatibility profile.
	ProtocolCodecError error = TickSynchronizerPacketCodec::encode_hello_payload(hello, payload);
	if (error != ProtocolCodecError::OK) {
		return error;
	}
	const ProtocolPacket packet = make_packet(
			ProtocolPacketType::HELLO,
			payload,
			TickSynchronizerPacketCodec::HELLO_PAYLOAD_SIZE * 8U);
	return TickSynchronizerPacketCodec::encode_packet(packet, r_encoded);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Constants and build precision are stable") {
	CHECK(TickSynchronizerPacketCodec::PROTOCOL_MAGIC == UINT32_C(0x4E595354));
	CHECK(TickSynchronizerPacketCodec::get_magic_string() == "TSYN");
	CHECK(TickSynchronizerPacketCodec::PROTOCOL_MAJOR == 1);
	CHECK(TickSynchronizerPacketCodec::PROTOCOL_MINOR == 1);
	CHECK(TickSynchronizerPacketCodec::PUBLIC_API_VERSION == 4);
	CHECK(TickSynchronizerPacketCodec::WIRE_PROTOCOL_VERSION == 0);
	CHECK(TickSynchronizerPacketCodec::WIRE_PROTOCOL_REVISION == 2);
	CHECK(TickSynchronizerPacketCodec::EXACT_BUILD_MATCH_REQUIRED);
	CHECK(TickSynchronizerPacketCodec::CONTROL_HEADER_SIZE == 40);
	CHECK(TickSynchronizerPacketCodec::COMPATIBILITY_PROFILE_SIZE == 132);
	CHECK(TickSynchronizerPacketCodec::HELLO_PAYLOAD_SIZE == 144);
	CHECK(TickSynchronizerPacketCodec::HELLO_ACK_PAYLOAD_SIZE == 152);
	CHECK(TickSynchronizerPacketCodec::DISCONNECT_PAYLOAD_SIZE == 56);
	CHECK(TickSynchronizerPacketCodec::DEFAULT_MAX_PACKET_SIZE_BYTES == 64 * 1024);
	CHECK(TickSynchronizerPacketCodec::MAX_PACKET_SIZE_BYTES == 1024 * 1024);
#ifdef REAL_T_IS_DOUBLE
	CHECK(TickSynchronizerPacketCodec::get_build_precision_mode() == ProtocolPrecisionMode::DOUBLE);
#else
	CHECK(TickSynchronizerPacketCodec::get_build_precision_mode() == ProtocolPrecisionMode::SINGLE);
#endif
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] HELLO packet matches the v4 version-contract golden vector") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	const uint8_t expected[] = {
		0x54, 0x53, 0x59, 0x4E, 0x01, 0x01, 0x01, 0x28,
		0x00, 0x00, 0x00, 0x00, 0xEF, 0xCD, 0xAB, 0x89,
		0x67, 0x45, 0x23, 0x01, 0x40, 0x30, 0x20, 0x10,
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
		0x90, 0x00, 0x00, 0x00, 0x80, 0x04, 0x00, 0x00,
		0x04, 0x02, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x77, 0x88, 0x04, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x34, 0x2E, 0x37, 0x2E, 0x31, 0x2D, 0x73, 0x74,
		0x61, 0x62, 0x6C, 0x65, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
		0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24,
		0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C,
		0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34,
		0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
		0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50,
		0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
		0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70,
		0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	REQUIRE(encoded.size() == static_cast<int64_t>(sizeof(expected)));
	for (int64_t index = 0; index < encoded.size(); index++) {
		CHECK(encoded[index] == expected[index]);
	}
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] HELLO payload round trips with identities") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	ProtocolPacket packet;
	REQUIRE(TickSynchronizerPacketCodec::decode_packet(encoded, packet) == ProtocolCodecError::OK);
	CHECK(packet.header.protocol_major == 1);
	CHECK(packet.header.protocol_minor == 1);
	CHECK(packet.header.payload_size_bytes == TickSynchronizerPacketCodec::HELLO_PAYLOAD_SIZE);
	CHECK(packet.header.payload_bit_size == TickSynchronizerPacketCodec::HELLO_PAYLOAD_SIZE * 8U);

	ProtocolHelloPayload hello;
	REQUIRE(TickSynchronizerPacketCodec::decode_hello_payload(packet, hello) == ProtocolCodecError::OK);
	CHECK(hello.payload_version == 4);
	CHECK(hello.hello_nonce == UINT64_C(0x8877665544332211));
	CHECK(hello.profile.api_version == version::API_VERSION);
	CHECK(hello.profile.wire_protocol_version == version::WIRE_PROTOCOL_VERSION);
	CHECK(hello.profile.wire_protocol_revision == version::WIRE_PROTOCOL_REVISION);
	CHECK(hello.profile.precision == ProtocolPrecisionMode::DOUBLE);
	CHECK(hello.profile.godot_version == make_godot_version());
	CHECK(hello.profile.godot_commit == make_sha1(0x01));
	CHECK(hello.profile.module_build_id == make_sha1(0x21));
	CHECK(hello.profile.game_build_id == make_opaque_id(0x41));
	CHECK(hello.profile.schema_compatibility_id == make_opaque_id(0x61));
	CHECK(hello.profile.capabilities.supported == UINT64_C(7));
	CHECK(hello.profile.capabilities.required == UINT64_C(3));
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] HELLO_ACK payload round trips") {
	ProtocolHelloAckPayload expected;
	expected.echoed_hello_nonce = UINT64_C(0x1122334455667788);
	expected.profile = make_profile(ProtocolPrecisionMode::SINGLE);
	expected.negotiated_capabilities = UINT64_C(5);
	PackedByteArray payload;
	REQUIRE(TickSynchronizerPacketCodec::encode_hello_ack_payload(expected, payload) == ProtocolCodecError::OK);
	REQUIRE(payload.size() == TickSynchronizerPacketCodec::HELLO_ACK_PAYLOAD_SIZE);

	ProtocolPacket packet = make_packet(
			ProtocolPacketType::HELLO_ACK,
			payload,
			TickSynchronizerPacketCodec::HELLO_ACK_PAYLOAD_SIZE * 8U);
	ProtocolHelloAckPayload decoded;
	REQUIRE(TickSynchronizerPacketCodec::decode_hello_ack_payload(packet, decoded) == ProtocolCodecError::OK);
	CHECK(decoded.payload_version == 4);
	CHECK(decoded.echoed_hello_nonce == expected.echoed_hello_nonce);
	CHECK(decoded.profile.precision == ProtocolPrecisionMode::SINGLE);
	CHECK(decoded.profile.module_build_id == expected.profile.module_build_id);
	CHECK(decoded.negotiated_capabilities == UINT64_C(5));
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Disconnect payload round trips with compatibility detail") {
	ProtocolDisconnectPayload expected;
	expected.reason = ProtocolDisconnectReason::CAPABILITY_MISMATCH;
	expected.sender_precision = ProtocolPrecisionMode::DOUBLE;
	expected.required_precision = ProtocolPrecisionMode::DOUBLE;
	expected.peer_precision = ProtocolPrecisionMode::SINGLE;
	expected.identity_field = ProtocolIdentityField::SCHEMA_COMPATIBILITY;
	expected.local_protocol_major = 1;
	expected.local_protocol_minor = 1;
	expected.peer_protocol_major = 1;
	expected.peer_protocol_minor = 0;
	expected.local_api_version = 4;
	expected.peer_api_version = 5;
	expected.local_wire_protocol_version = 0;
	expected.local_wire_protocol_revision = 1;
	expected.peer_wire_protocol_version = 0;
	expected.peer_wire_protocol_revision = 2;
	expected.detail_code = 3;
	expected.peer_id = UINT64_C(0x1122334455667788);
	expected.detail_mask = UINT64_C(0x8000000000000004);
	PackedByteArray payload;
	REQUIRE(TickSynchronizerPacketCodec::encode_disconnect_payload(expected, payload) == ProtocolCodecError::OK);
	REQUIRE(payload.size() == TickSynchronizerPacketCodec::DISCONNECT_PAYLOAD_SIZE);

	ProtocolPacket packet = make_packet(
			ProtocolPacketType::DISCONNECT_REASON,
			payload,
			TickSynchronizerPacketCodec::DISCONNECT_PAYLOAD_SIZE * 8U);
	ProtocolDisconnectPayload decoded;
	REQUIRE(TickSynchronizerPacketCodec::decode_disconnect_payload(packet, decoded) == ProtocolCodecError::OK);
	CHECK(decoded.reason == expected.reason);
	CHECK(decoded.sender_precision == expected.sender_precision);
	CHECK(decoded.required_precision == expected.required_precision);
	CHECK(decoded.peer_precision == expected.peer_precision);
	CHECK(decoded.identity_field == expected.identity_field);
	CHECK(decoded.local_protocol_minor == 1);
	CHECK(decoded.peer_protocol_minor == 0);
	CHECK(decoded.local_api_version == 4);
	CHECK(decoded.peer_api_version == 5);
	CHECK(decoded.local_wire_protocol_version == 0);
	CHECK(decoded.local_wire_protocol_revision == 1);
	CHECK(decoded.peer_wire_protocol_version == 0);
	CHECK(decoded.peer_wire_protocol_revision == 2);
	CHECK(decoded.detail_code == 3);
	CHECK(decoded.peer_id == expected.peer_id);
	CHECK(decoded.detail_mask == expected.detail_mask);
}

TEST_CASE(
		"[Modules][TickSynchronizer][Protocol] "
		"Header inspection exposes incompatible versions without accepting packet") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[5] = 0;
	ProtocolPacketHeader inspected;
	REQUIRE(TickSynchronizerPacketCodec::inspect_control_header(encoded, inspected) == ProtocolCodecError::OK);
	CHECK(inspected.protocol_major == 1);
	CHECK(inspected.protocol_minor == 0);
	ProtocolPacket packet;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, packet) ==
			ProtocolCodecError::PROTOCOL_VERSION_MISMATCH);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Header inspection is atomic on small packets and wrong magic") {
	ProtocolPacketHeader inspected;
	inspected.sequence = UINT32_C(0xDEADBEEF);
	PackedByteArray small;
	REQUIRE(small.resize_initialized(39) == OK);
	CHECK(TickSynchronizerPacketCodec::inspect_control_header(small, inspected) == ProtocolCodecError::PACKET_TOO_SMALL);
	CHECK(inspected.sequence == UINT32_C(0xDEADBEEF));

	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[0] = 0;
	CHECK(TickSynchronizerPacketCodec::inspect_control_header(encoded, inspected) == ProtocolCodecError::MAGIC_MISMATCH);
	CHECK(inspected.sequence == UINT32_C(0xDEADBEEF));
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Every truncated control header is rejected atomically") {
	PackedByteArray complete;
	REQUIRE(encode_default_hello(complete) == ProtocolCodecError::OK);
	for (int64_t size = 0; size < TickSynchronizerPacketCodec::CONTROL_HEADER_SIZE; size++) {
		PackedByteArray truncated;
		REQUIRE(truncated.resize_initialized(size) == OK);
		for (int64_t index = 0; index < size; index++) {
			truncated.ptrw()[index] = complete[index];
		}
		ProtocolPacket output;
		output.header.session_id = UINT64_C(0xAABBCCDD);
		CHECK(TickSynchronizerPacketCodec::decode_packet(truncated, output) == ProtocolCodecError::PACKET_TOO_SMALL);
		CHECK(output.header.session_id == UINT64_C(0xAABBCCDD));
	}
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Invalid magic is rejected atomically") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[0] ^= 0xFF;
	ProtocolPacket output;
	output.header.sequence = UINT32_C(0xCAFEBABE);
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::MAGIC_MISMATCH);
	CHECK(output.header.sequence == UINT32_C(0xCAFEBABE));
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Protocol major and minor mismatches are rejected") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[4] = 2;
	ProtocolPacket output;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::PROTOCOL_VERSION_MISMATCH);
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[5] = 0;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::PROTOCOL_VERSION_MISMATCH);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Header size mismatch is rejected") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[7] = 39;
	ProtocolPacket output;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::HEADER_SIZE_MISMATCH);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Unknown packet type is rejected") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[6] = 0xFF;
	ProtocolPacket output;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::UNKNOWN_PACKET_TYPE);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Unknown flags and reserved fields are rejected") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[8] = 1;
	ProtocolPacket output;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::UNKNOWN_FLAGS);
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[10] = 1;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::RESERVED_NONZERO);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Declared payload truncation is rejected before allocation") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	REQUIRE(encoded.resize_initialized(encoded.size() - 1) == OK);
	ProtocolPacket output;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::PACKET_TRUNCATED);
	CHECK(output.payload.is_empty());
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Trailing bytes are rejected") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	const int64_t original_size = encoded.size();
	REQUIRE(encoded.resize_initialized(original_size + 1) == OK);
	encoded.ptrw()[original_size] = 0;
	ProtocolPacket output;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::TRAILING_DATA);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Oversized declared payload is rejected before copying") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	const uint32_t oversized = TickSynchronizerPacketCodec::DEFAULT_MAX_PACKET_SIZE_BYTES;
	encoded.ptrw()[32] = static_cast<uint8_t>(oversized & 0xFF);
	encoded.ptrw()[33] = static_cast<uint8_t>((oversized >> 8) & 0xFF);
	encoded.ptrw()[34] = static_cast<uint8_t>((oversized >> 16) & 0xFF);
	encoded.ptrw()[35] = static_cast<uint8_t>((oversized >> 24) & 0xFF);
	ProtocolPacket output;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::PACKET_TOO_LARGE);
	CHECK(output.payload.is_empty());
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Payload bit size requiring fewer bytes is rejected") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[36] = 0x18; // 792 bits require 99 bytes, not the declared 144.
	encoded.ptrw()[37] = 0x03;
	ProtocolPacket output;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) ==
			ProtocolCodecError::PAYLOAD_BIT_SIZE_INVALID);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Partial-byte payload with zero padding round trips") {
	const uint8_t payload_values[] = { 0xA5, 0x01 };
	const ProtocolPacket packet = make_packet(
			ProtocolPacketType::HELLO,
			make_bytes(payload_values, 2),
			9);
	PackedByteArray encoded;
	REQUIRE(TickSynchronizerPacketCodec::encode_packet(packet, encoded) == ProtocolCodecError::OK);
	ProtocolPacket decoded;
	REQUIRE(TickSynchronizerPacketCodec::decode_packet(encoded, decoded) == ProtocolCodecError::OK);
	CHECK(decoded.header.payload_bit_size == 9);
	CHECK(decoded.payload == packet.payload);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Decode rejects nonzero final padding atomically") {
	const uint8_t payload_values[] = { 0xA5, 0x01 };
	const ProtocolPacket packet = make_packet(ProtocolPacketType::HELLO, make_bytes(payload_values, 2), 9);
	PackedByteArray encoded;
	REQUIRE(TickSynchronizerPacketCodec::encode_packet(packet, encoded) == ProtocolCodecError::OK);
	encoded.ptrw()[TickSynchronizerPacketCodec::CONTROL_HEADER_SIZE + 1] |= 0x80;
	ProtocolPacket output;
	output.header.sequence = UINT32_C(0xCAFEBABE);
	output.payload.append(0xCC);
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output) ==
			ProtocolCodecError::PAYLOAD_PADDING_NONZERO);
	CHECK(output.header.sequence == UINT32_C(0xCAFEBABE));
	REQUIRE(output.payload.size() == 1);
	CHECK(output.payload[0] == 0xCC);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Encode rejects nonzero final padding atomically") {
	const uint8_t payload_values[] = { 0xA5, 0xFE };
	ProtocolPacket packet = make_packet(ProtocolPacketType::HELLO, make_bytes(payload_values, 2), 9);
	PackedByteArray output;
	output.append(0xCC);
	CHECK(TickSynchronizerPacketCodec::encode_packet(packet, output) ==
			ProtocolCodecError::PAYLOAD_PADDING_NONZERO);
	REQUIRE(output.size() == 1);
	CHECK(output[0] == 0xCC);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Encode rejects inconsistent payload size atomically") {
	const uint8_t payload_values[] = { 1, 2, 3 };
	ProtocolPacket packet = make_packet(ProtocolPacketType::HELLO, make_bytes(payload_values, 3), 24);
	packet.header.payload_size_bytes = 4;
	PackedByteArray output;
	output.append(0xAA);
	CHECK(TickSynchronizerPacketCodec::encode_packet(packet, output) ==
			ProtocolCodecError::PAYLOAD_SIZE_MISMATCH);
	REQUIRE(output.size() == 1);
	CHECK(output[0] == 0xAA);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Packet size limits are validated") {
	PackedByteArray encoded;
	REQUIRE(encode_default_hello(encoded) == ProtocolCodecError::OK);
	ProtocolPacket output;
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output, 39) ==
			ProtocolCodecError::INVALID_ARGUMENT);
	CHECK(TickSynchronizerPacketCodec::decode_packet(
			encoded,
			output,
			TickSynchronizerPacketCodec::MAX_PACKET_SIZE_BYTES + 1) ==
			ProtocolCodecError::INVALID_ARGUMENT);
	CHECK(TickSynchronizerPacketCodec::decode_packet(encoded, output, 139) ==
			ProtocolCodecError::PACKET_TOO_LARGE);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] HELLO payload rejects malformed fixed fields atomically") {
	ProtocolHelloPayload hello;
	hello.hello_nonce = 1;
	hello.profile = make_profile(ProtocolPrecisionMode::DOUBLE);
	PackedByteArray payload;
	REQUIRE(TickSynchronizerPacketCodec::encode_hello_payload(hello, payload) == ProtocolCodecError::OK);
	ProtocolPacket packet = make_packet(
			ProtocolPacketType::HELLO,
			payload,
			TickSynchronizerPacketCodec::HELLO_PAYLOAD_SIZE * 8U);
	ProtocolHelloPayload output;
	output.hello_nonce = UINT64_C(0xDEADBEEF);

	packet.payload.ptrw()[0] = 1;
	CHECK(TickSynchronizerPacketCodec::decode_hello_payload(packet, output) ==
			ProtocolCodecError::HELLO_VERSION_MISMATCH);
	CHECK(output.hello_nonce == UINT64_C(0xDEADBEEF));
	packet.payload.ptrw()[0] = 4;
	packet.payload.ptrw()[1] = 0;
	CHECK(TickSynchronizerPacketCodec::decode_hello_payload(packet, output) ==
			ProtocolCodecError::INVALID_PRECISION);
	packet.payload.ptrw()[1] = 2;
	packet.payload.ptrw()[2] = 1;
	CHECK(TickSynchronizerPacketCodec::decode_hello_payload(packet, output) ==
			ProtocolCodecError::RESERVED_NONZERO);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] HELLO_ACK payload rejects malformed fixed fields") {
	ProtocolHelloAckPayload ack;
	ack.echoed_hello_nonce = 1;
	ack.profile = make_profile(ProtocolPrecisionMode::DOUBLE);
	PackedByteArray payload;
	REQUIRE(TickSynchronizerPacketCodec::encode_hello_ack_payload(ack, payload) == ProtocolCodecError::OK);
	ProtocolPacket packet = make_packet(
			ProtocolPacketType::HELLO_ACK,
			payload,
			TickSynchronizerPacketCodec::HELLO_ACK_PAYLOAD_SIZE * 8U);
	ProtocolHelloAckPayload output;
	packet.payload.ptrw()[2] = 1;
	CHECK(TickSynchronizerPacketCodec::decode_hello_ack_payload(packet, output) ==
			ProtocolCodecError::RESERVED_NONZERO);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Payload decoders require matching packet types and sizes") {
	ProtocolHelloPayload hello;
	hello.hello_nonce = 1;
	hello.profile = make_profile(ProtocolPrecisionMode::DOUBLE);
	PackedByteArray payload;
	REQUIRE(TickSynchronizerPacketCodec::encode_hello_payload(hello, payload) == ProtocolCodecError::OK);
	ProtocolPacket wrong_type = make_packet(
			ProtocolPacketType::HELLO_ACK,
			payload,
			TickSynchronizerPacketCodec::HELLO_PAYLOAD_SIZE * 8U);
	ProtocolHelloPayload decoded;
	CHECK(TickSynchronizerPacketCodec::decode_hello_payload(wrong_type, decoded) ==
			ProtocolCodecError::UNKNOWN_PACKET_TYPE);
	ProtocolPacket wrong_size = make_packet(
			ProtocolPacketType::HELLO,
			payload,
			TickSynchronizerPacketCodec::HELLO_PAYLOAD_SIZE * 8U);
	wrong_size.header.payload_size_bytes = TickSynchronizerPacketCodec::HELLO_PAYLOAD_SIZE - 1;
	CHECK(TickSynchronizerPacketCodec::decode_hello_payload(wrong_size, decoded) ==
			ProtocolCodecError::MALFORMED_PAYLOAD);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Disconnect decoder rejects malformed enums and reserved fields") {
	ProtocolDisconnectPayload disconnect;
	disconnect.reason = ProtocolDisconnectReason::MALFORMED_HANDSHAKE;
	disconnect.sender_precision = ProtocolPrecisionMode::DOUBLE;
	PackedByteArray payload;
	REQUIRE(TickSynchronizerPacketCodec::encode_disconnect_payload(disconnect, payload) == ProtocolCodecError::OK);
	ProtocolPacket packet = make_packet(
			ProtocolPacketType::DISCONNECT_REASON,
			payload,
			TickSynchronizerPacketCodec::DISCONNECT_PAYLOAD_SIZE * 8U);
	ProtocolDisconnectPayload output;
	packet.payload.ptrw()[0] = 0xFF;
	packet.payload.ptrw()[1] = 0xFF;
	CHECK(TickSynchronizerPacketCodec::decode_disconnect_payload(packet, output) ==
			ProtocolCodecError::UNKNOWN_DISCONNECT_REASON);
	REQUIRE(TickSynchronizerPacketCodec::encode_disconnect_payload(disconnect, payload) == ProtocolCodecError::OK);
	packet = make_packet(
			ProtocolPacketType::DISCONNECT_REASON,
			payload,
			TickSynchronizerPacketCodec::DISCONNECT_PAYLOAD_SIZE * 8U);
	packet.payload.ptrw()[5] = 0xFF;
	CHECK(TickSynchronizerPacketCodec::decode_disconnect_payload(packet, output) ==
			ProtocolCodecError::UNKNOWN_IDENTITY_FIELD);
	REQUIRE(TickSynchronizerPacketCodec::encode_disconnect_payload(disconnect, payload) == ProtocolCodecError::OK);
	packet = make_packet(
			ProtocolPacketType::DISCONNECT_REASON,
			payload,
			TickSynchronizerPacketCodec::DISCONNECT_PAYLOAD_SIZE * 8U);
	packet.payload.ptrw()[10] = 1;
	CHECK(TickSynchronizerPacketCodec::decode_disconnect_payload(packet, output) ==
			ProtocolCodecError::RESERVED_NONZERO);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] HELLO rejects malformed version contracts atomically") {
	ProtocolHelloPayload hello;
	hello.hello_nonce = 1;
	hello.profile = make_profile(ProtocolPrecisionMode::DOUBLE);
	hello.profile.api_version = 0;
	PackedByteArray output;
	output.append(0xA5);
	CHECK(TickSynchronizerPacketCodec::encode_hello_payload(hello, output) ==
			ProtocolCodecError::INVALID_VERSION_CONTRACT);
	REQUIRE(output.size() == 1);
	CHECK(output[0] == 0xA5);

	hello.profile.api_version = version::API_VERSION;
	hello.profile.wire_protocol_version = 0;
	hello.profile.wire_protocol_revision = 0;
	CHECK(TickSynchronizerPacketCodec::encode_hello_payload(hello, output) ==
			ProtocolCodecError::INVALID_VERSION_CONTRACT);
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Error and enum names are stable") {
	CHECK(String(TickSynchronizerPacketCodec::get_error_name(ProtocolCodecError::TRAILING_DATA)) == "TRAILING_DATA");
	CHECK(String(TickSynchronizerPacketCodec::get_precision_name(ProtocolPrecisionMode::DOUBLE)) == "double");
	CHECK(String(TickSynchronizerPacketCodec::get_packet_type_name(ProtocolPacketType::HELLO_ACK)) == "HELLO_ACK");
	CHECK(String(TickSynchronizerPacketCodec::get_disconnect_reason_name(
			ProtocolDisconnectReason::BUILD_COMPATIBILITY_MISMATCH)) ==
			"BUILD_COMPATIBILITY_MISMATCH");
	CHECK(String(TickSynchronizerPacketCodec::get_disconnect_reason_name(
			ProtocolDisconnectReason::GODOT_VERSION_MISMATCH)) ==
			"GODOT_VERSION_MISMATCH");
	CHECK(String(TickSynchronizerPacketCodec::get_identity_field_name(
			ProtocolIdentityField::SCHEMA_COMPATIBILITY)) == "SCHEMA_COMPATIBILITY");
	CHECK(String(TickSynchronizerPacketCodec::get_identity_field_name(
			ProtocolIdentityField::GODOT_VERSION)) == "GODOT_VERSION");
}

TEST_CASE("[Modules][TickSynchronizer][Protocol] Deterministic packet sequence round trips") {
	uint64_t state = UINT64_C(0xA17E5EED12345678);
	for (uint32_t index = 0; index < 1024; index++) {
		state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
		const uint32_t size = static_cast<uint32_t>((state >> 32) % 65);
		PackedByteArray payload;
		REQUIRE(payload.resize_initialized(size) == OK);
		for (uint32_t byte_index = 0; byte_index < size; byte_index++) {
			state = state * UINT64_C(2862933555777941757) + UINT64_C(3037000493);
			payload.ptrw()[byte_index] = static_cast<uint8_t>(state >> 56);
		}
		ProtocolPacket input = make_packet(ProtocolPacketType::HELLO, payload, size * 8U, index + 1, index, state);
		PackedByteArray encoded;
		REQUIRE(TickSynchronizerPacketCodec::encode_packet(input, encoded) == ProtocolCodecError::OK);
		ProtocolPacket output;
		REQUIRE(TickSynchronizerPacketCodec::decode_packet(encoded, output) == ProtocolCodecError::OK);
		CHECK(output.header.session_id == input.header.session_id);
		CHECK(output.header.sequence == input.header.sequence);
		CHECK(output.header.tick == input.header.tick);
		CHECK(output.payload == input.payload);
	}
}

} // namespace TestTickSynchronizerPacketCodec

// Implements the isolated canonical-varint protocol candidate.
// Uses ULEB128, ZigZag, fixed-width floats, explicit bounds, and atomic decode.

#include "varint_zigzag_fixed_float_candidate.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace tick_synchronizer::benchmarks {
namespace {

// Returns the minimal ULEB128 width of one unsigned value.
std::size_t uleb128_size(std::uint64_t value) noexcept {
	std::size_t size = 1;
	while (value >= UINT64_C(0x80)) {
		value >>= 7U;
		++size;
	}
	return size;
}


// Maps every signed 64-bit value to its portable ZigZag representation.
std::uint64_t zigzag_encode(std::int64_t value) noexcept {
	if (value >= 0) {
		return static_cast<std::uint64_t>(value) << 1U;
	}
	return (static_cast<std::uint64_t>(-(value + 1)) << 1U) | UINT64_C(1);
}


// Reconstructs a signed 64-bit value without out-of-range signed conversion.
std::int64_t zigzag_decode(std::uint64_t value) noexcept {
	const std::uint64_t magnitude = value >> 1U;
	if ((value & UINT64_C(1)) == 0) {
		return static_cast<std::int64_t>(magnitude);
	}
	if (magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
		return std::numeric_limits<std::int64_t>::min();
	}
	return -static_cast<std::int64_t>(magnitude) - 1;
}


// Appends one canonical ULEB128 value.
void append_uleb128(std::vector<std::uint8_t> &output, std::uint64_t value) {
	do {
		std::uint8_t byte = static_cast<std::uint8_t>(value & UINT64_C(0x7F));
		value >>= 7U;
		if (value != 0) {
			byte |= UINT8_C(0x80);
		}
		output.push_back(byte);
	} while (value != 0);
}


// Appends one signed value as portable ZigZag followed by canonical ULEB128.
void append_zigzag(std::vector<std::uint8_t> &output, std::int64_t value) {
	// Maps every signed 64-bit value to its portable ZigZag representation.
	append_uleb128(output, zigzag_encode(value));
}


class Cursor {
	ByteView input;
	std::size_t position = 0;

public:
	explicit Cursor(ByteView view) : input(view) {}

	CandidateDecodeError read_u8(std::uint8_t &value) noexcept {
		if (remaining() < 1) {
			return CandidateDecodeError::TRUNCATED;
		}
		value = input.data[position++];
		return CandidateDecodeError::OK;
	}

	CandidateDecodeError read_uleb128(std::uint64_t &value) noexcept {
		std::size_t cursor = position;
		std::uint64_t decoded = 0;
		for (unsigned index = 0; index < 10; ++index) {
			if (cursor >= input.size) {
				return CandidateDecodeError::TRUNCATED;
			}
			const std::uint8_t byte = input.data[cursor++];
			const std::uint8_t payload = byte & UINT8_C(0x7F);
			if (index == 9 && (byte & UINT8_C(0xFE)) != 0) {
				return CandidateDecodeError::MALFORMED;
			}
			decoded |= static_cast<std::uint64_t>(payload) << (index * 7U);
			if ((byte & UINT8_C(0x80)) == 0) {
				if (index != 0 && payload == 0) {
					return CandidateDecodeError::MALFORMED;
				}
				position = cursor;
				value = decoded;
				return CandidateDecodeError::OK;
			}
		}
		return CandidateDecodeError::MALFORMED;
	}

	template <typename T>
	CandidateDecodeError read_unsigned(T &value) noexcept {
		std::uint64_t decoded = 0;
		const CandidateDecodeError error = read_uleb128(decoded);
		if (error != CandidateDecodeError::OK) {
			return error;
		}
		if (decoded > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
			return CandidateDecodeError::MALFORMED;
		}
		value = static_cast<T>(decoded);
		return CandidateDecodeError::OK;
	}

	CandidateDecodeError read_i16(std::int16_t &value) noexcept {
		std::uint64_t decoded = 0;
		const CandidateDecodeError error = read_uleb128(decoded);
		if (error != CandidateDecodeError::OK) {
			return error;
		}
		const std::int64_t signed_value = zigzag_decode(decoded);
		if (signed_value < std::numeric_limits<std::int16_t>::min() ||
				signed_value > std::numeric_limits<std::int16_t>::max()) {
			return CandidateDecodeError::MALFORMED;
		}
		value = static_cast<std::int16_t>(signed_value);
		return CandidateDecodeError::OK;
	}

	CandidateDecodeError read_i64(std::int64_t &value) noexcept {
		std::uint64_t decoded = 0;
		const CandidateDecodeError error = read_uleb128(decoded);
		if (error != CandidateDecodeError::OK) {
			return error;
		}
		value = zigzag_decode(decoded);
		return CandidateDecodeError::OK;
	}

	CandidateDecodeError read_scalar(double &value) noexcept {
		// Reads one canonical selected-precision scalar and advances only on success.
		return read_benchmark_wire_scalar(input, position, value);
	}

	CandidateDecodeError read_bytes(std::vector<std::uint8_t> &output, std::size_t count) {
		if (remaining() < count) {
			return CandidateDecodeError::TRUNCATED;
		}
		output.assign(input.data + position, input.data + position + count);
		position += count;
		return CandidateDecodeError::OK;
	}

	std::size_t remaining() const noexcept {
		return input.size - position;
	}
};


// Returns at the first cursor error while keeping decode control flow explicit.
bool capture_error(CandidateDecodeError next, CandidateDecodeError &error) noexcept {
	if (next == CandidateDecodeError::OK) {
		return false;
	}
	error = next;
	return true;
}


// Appends a minimal control prefix through the tick field.
void append_minimal_common(
		std::vector<std::uint8_t> &bytes,
		BenchmarkMessageKind kind = BenchmarkMessageKind::CONTROL) {
	bytes.push_back(static_cast<std::uint8_t>(kind));
	bytes.push_back(0);
	append_uleb128(bytes, 0);
	append_uleb128(bytes, 0);
	append_uleb128(bytes, 0);
}

} // namespace

ProtocolCandidateInfo VarintZigZagFixedFloatCandidate::info() noexcept {
	return ProtocolCandidateInfo{
		CANDIDATE_ID,
		"varint_zigzag_fixed_float",
		"Canonical ULEB128 unsigned integers and ZigZag signed integers with unchanged fixed-width floats.",
	};
}


const char *VarintZigZagFixedFloatCandidate::wire_precision_name() noexcept {
	// Returns the explicit floating-point width selected for this benchmark build.
	return benchmark_wire_precision_name();
}


std::size_t VarintZigZagFixedFloatCandidate::estimate_encoded_size(
		const BenchmarkMessage &message) noexcept {
	std::size_t size = 2 + uleb128_size(message.flags) + uleb128_size(message.sequence) +
			uleb128_size(message.tick);
	switch (message.kind) {
		case BenchmarkMessageKind::CONTROL:
			return size + uleb128_size(message.reference_tick) + uleb128_size(message.buttons);
		case BenchmarkMessageKind::PLAYER_INPUT:
			size += uleb128_size(message.reference_tick) + uleb128_size(message.buttons);
			for (const std::int16_t axis : message.axes) {
				size += uleb128_size(zigzag_encode(axis));
			}
			return size + uleb128_size(message.blob.size()) + message.blob.size();
		case BenchmarkMessageKind::SNAPSHOT:
			size += uleb128_size(message.reference_tick) + uleb128_size(message.entities.size()) +
					uleb128_size(message.blob.size()) + message.blob.size();
			for (const BenchmarkEntityState &entity : message.entities) {
				size += uleb128_size(entity.entity_id) + uleb128_size(entity.change_mask) +
						uleb128_size(zigzag_encode(entity.signed_value)) +
						uleb128_size(entity.unsigned_value) +
						6 * benchmark_wire_scalar_size() + uleb128_size(entity.flags);
			}
			return size;
	}
	return 0;
}


bool VarintZigZagFixedFloatCandidate::encode(
		const BenchmarkMessage &message,
		std::vector<std::uint8_t> &output) {
	if (message.entities.size() > MAX_ENTITIES || message.blob.size() > MAX_BLOB_SIZE) {
		return false;
	}
	if (!benchmark_message_kind_semantics_are_valid(message)) {
		return false;
	}
	output.clear();
	output.reserve(estimate_encoded_size(message));
	output.push_back(static_cast<std::uint8_t>(message.kind));
	output.push_back(message.subtype);
	append_uleb128(output, message.flags);
	append_uleb128(output, message.sequence);
	append_uleb128(output, message.tick);

	switch (message.kind) {
		case BenchmarkMessageKind::CONTROL:
			append_uleb128(output, message.reference_tick);
			append_uleb128(output, message.buttons);
			break;
		case BenchmarkMessageKind::PLAYER_INPUT:
			append_uleb128(output, message.reference_tick);
			append_uleb128(output, message.buttons);
			for (const std::int16_t axis : message.axes) {
				append_zigzag(output, axis);
			}
			append_uleb128(output, message.blob.size());
			output.insert(output.end(), message.blob.begin(), message.blob.end());
			break;
		case BenchmarkMessageKind::SNAPSHOT:
			append_uleb128(output, message.reference_tick);
			append_uleb128(output, message.entities.size());
			append_uleb128(output, message.blob.size());
			for (const BenchmarkEntityState &entity : message.entities) {
				append_uleb128(output, entity.entity_id);
				append_uleb128(output, entity.change_mask);
				append_zigzag(output, entity.signed_value);
				append_uleb128(output, entity.unsigned_value);
				append_benchmark_wire_scalar(output, entity.position_x);
				append_benchmark_wire_scalar(output, entity.position_y);
				append_benchmark_wire_scalar(output, entity.position_z);
				append_benchmark_wire_scalar(output, entity.velocity_x);
				append_benchmark_wire_scalar(output, entity.velocity_y);
				append_benchmark_wire_scalar(output, entity.velocity_z);
				append_uleb128(output, entity.flags);
			}
			output.insert(output.end(), message.blob.begin(), message.blob.end());
			break;
	}
	return output.size() == estimate_encoded_size(message);
}


CandidateDecodeError VarintZigZagFixedFloatCandidate::decode(
		ByteView input,
		BenchmarkMessage &output) {
	if (input.data == nullptr || input.size < 2) {
		return CandidateDecodeError::TRUNCATED;
	}
	Cursor cursor(input);
	BenchmarkMessage decoded;
	CandidateDecodeError error = CandidateDecodeError::OK;
	std::uint8_t kind = 0;
	if (capture_error(cursor.read_u8(kind), error) ||
			capture_error(cursor.read_u8(decoded.subtype), error)) {
		return error;
	}
	if (kind < static_cast<std::uint8_t>(BenchmarkMessageKind::CONTROL) ||
			kind > static_cast<std::uint8_t>(BenchmarkMessageKind::SNAPSHOT)) {
		return CandidateDecodeError::UNKNOWN_KIND;
	}
	decoded.kind = static_cast<BenchmarkMessageKind>(kind);
	if (capture_error(cursor.read_unsigned(decoded.flags), error) ||
			capture_error(cursor.read_unsigned(decoded.sequence), error) ||
			capture_error(cursor.read_uleb128(decoded.tick), error)) {
		return error;
	}

	switch (decoded.kind) {
		case BenchmarkMessageKind::CONTROL:
			if (capture_error(cursor.read_uleb128(decoded.reference_tick), error) ||
					capture_error(cursor.read_uleb128(decoded.buttons), error)) {
				return error;
			}
			break;
		case BenchmarkMessageKind::PLAYER_INPUT: {
			std::uint32_t blob_size = 0;
			if (capture_error(cursor.read_uleb128(decoded.reference_tick), error) ||
					capture_error(cursor.read_uleb128(decoded.buttons), error)) {
				return error;
			}
			for (std::int16_t &axis : decoded.axes) {
				if (capture_error(cursor.read_i16(axis), error)) {
					return error;
				}
			}
			if (capture_error(cursor.read_unsigned(blob_size), error)) {
				return error;
			}
			if (blob_size > MAX_BLOB_SIZE) {
				return CandidateDecodeError::LIMIT_EXCEEDED;
			}
			if (capture_error(cursor.read_bytes(decoded.blob, blob_size), error)) {
				return error;
			}
			break;
		}
		case BenchmarkMessageKind::SNAPSHOT: {
			std::uint32_t entity_count = 0;
			std::uint32_t blob_size = 0;
			if (capture_error(cursor.read_uleb128(decoded.reference_tick), error) ||
					capture_error(cursor.read_unsigned(entity_count), error) ||
					capture_error(cursor.read_unsigned(blob_size), error)) {
				return error;
			}
			if (entity_count > MAX_ENTITIES || blob_size > MAX_BLOB_SIZE) {
				return CandidateDecodeError::LIMIT_EXCEEDED;
			}
			const std::size_t minimum_entity_size = 5 + 6 * benchmark_wire_scalar_size();
			if (cursor.remaining() < static_cast<std::size_t>(entity_count) * minimum_entity_size + blob_size) {
				return CandidateDecodeError::TRUNCATED;
			}
			decoded.entities.resize(entity_count);
			for (BenchmarkEntityState &entity : decoded.entities) {
				if (capture_error(cursor.read_unsigned(entity.entity_id), error) ||
						capture_error(cursor.read_unsigned(entity.change_mask), error) ||
						capture_error(cursor.read_i64(entity.signed_value), error) ||
						capture_error(cursor.read_uleb128(entity.unsigned_value), error) ||
						capture_error(cursor.read_scalar(entity.position_x), error) ||
						capture_error(cursor.read_scalar(entity.position_y), error) ||
						capture_error(cursor.read_scalar(entity.position_z), error) ||
						capture_error(cursor.read_scalar(entity.velocity_x), error) ||
						capture_error(cursor.read_scalar(entity.velocity_y), error) ||
						capture_error(cursor.read_scalar(entity.velocity_z), error) ||
						capture_error(cursor.read_unsigned(entity.flags), error)) {
					return error;
				}
			}
			if (capture_error(cursor.read_bytes(decoded.blob, blob_size), error)) {
				return error;
			}
			break;
		}
	}
	if (cursor.remaining() != 0) {
		return CandidateDecodeError::TRAILING_DATA;
	}
	output = std::move(decoded);
	return CandidateDecodeError::OK;
}


bool VarintZigZagFixedFloatCandidate::equivalent_for_wire(
		const BenchmarkMessage &expected,
		const BenchmarkMessage &actual) noexcept {
	return benchmark_messages_equivalent_for_wire(expected, actual);
}


std::uint64_t VarintZigZagFixedFloatCandidate::semantic_hash_for_wire(
		const BenchmarkMessage &message) noexcept {
	return benchmark_semantic_hash_for_wire(message);
}


void VarintZigZagFixedFloatCandidate::append_invalid_packets(
		std::vector<CandidateInvalidPacket> &packets) {
	packets.push_back(CandidateInvalidPacket{
		"varint-noncanonical-zero",
		{ static_cast<std::uint8_t>(BenchmarkMessageKind::CONTROL), 0, 0x80, 0x00, 0, 0, 0, 0 },
		CandidateDecodeError::MALFORMED,
	});
	packets.push_back(CandidateInvalidPacket{
		"varint-truncated-continuation",
		{ static_cast<std::uint8_t>(BenchmarkMessageKind::CONTROL), 0, 0x80 },
		CandidateDecodeError::TRUNCATED,
	});

	std::vector<std::uint8_t> u64_overflow;
	append_minimal_common(u64_overflow);
	// Replaces the canonical tick and following fields with a ten-byte overflow.
	u64_overflow.resize(4);
	for (unsigned index = 0; index < 9; ++index) {
		u64_overflow.push_back(0x80);
	}
	u64_overflow.push_back(0x02);
	packets.push_back(CandidateInvalidPacket{
		"varint-u64-overflow",
		std::move(u64_overflow),
		CandidateDecodeError::MALFORMED,
	});

	std::vector<std::uint8_t> u16_overflow{
		static_cast<std::uint8_t>(BenchmarkMessageKind::CONTROL), 0,
	};
	append_uleb128(u16_overflow, UINT64_C(65536));
	packets.push_back(CandidateInvalidPacket{
		"varint-u16-overflow",
		std::move(u16_overflow),
		CandidateDecodeError::MALFORMED,
	});

	std::vector<std::uint8_t> excessive_entities;
	append_minimal_common(excessive_entities, BenchmarkMessageKind::SNAPSHOT);
	append_uleb128(excessive_entities, 0);
	append_uleb128(excessive_entities, MAX_ENTITIES + 1U);
	append_uleb128(excessive_entities, 0);
	packets.push_back(CandidateInvalidPacket{
		"varint-excessive-entity-count",
		std::move(excessive_entities),
		CandidateDecodeError::LIMIT_EXCEEDED,
	});

	std::vector<std::uint8_t> excessive_blob;
	append_minimal_common(excessive_blob, BenchmarkMessageKind::PLAYER_INPUT);
	append_uleb128(excessive_blob, 0);
	append_uleb128(excessive_blob, 0);
	for (unsigned axis = 0; axis < 4; ++axis) {
		append_zigzag(excessive_blob, 0);
	}
	append_uleb128(excessive_blob, MAX_BLOB_SIZE + 1U);
	packets.push_back(CandidateInvalidPacket{
		"varint-excessive-player-blob",
		std::move(excessive_blob),
		CandidateDecodeError::LIMIT_EXCEEDED,
	});

	std::vector<std::uint8_t> noncanonical_nan;
	append_minimal_common(noncanonical_nan, BenchmarkMessageKind::SNAPSHOT);
	append_uleb128(noncanonical_nan, 0);
	append_uleb128(noncanonical_nan, 1);
	append_uleb128(noncanonical_nan, 0);
	append_uleb128(noncanonical_nan, 0);
	append_uleb128(noncanonical_nan, 0);
	append_zigzag(noncanonical_nan, 0);
	append_uleb128(noncanonical_nan, 0);
	if (benchmark_wire_scalar_size() == 4) {
		noncanonical_nan.insert(noncanonical_nan.end(), { 0x01, 0x00, 0xC0, 0x7F });
	} else {
		noncanonical_nan.insert(
				noncanonical_nan.end(), { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x7F });
	}
	for (unsigned scalar = 1; scalar < 6; ++scalar) {
		append_benchmark_wire_scalar(noncanonical_nan, 0.0);
	}
	append_uleb128(noncanonical_nan, 0);
	packets.push_back(CandidateInvalidPacket{
		"varint-noncanonical-nan",
		std::move(noncanonical_nan),
		CandidateDecodeError::MALFORMED,
	});
}

} // namespace tick_synchronizer::benchmarks

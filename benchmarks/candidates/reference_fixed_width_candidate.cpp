// Implements the fixed-width reference protocol candidate.
// Provides a transparent baseline for wire size, CPU cost, and validation.

#include "reference_fixed_width_candidate.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace tick_synchronizer::benchmarks {
namespace {

constexpr std::size_t COMMON_SIZE = 16;
constexpr std::size_t CONTROL_SIZE = COMMON_SIZE + 16;
constexpr std::size_t PLAYER_FIXED_SIZE = COMMON_SIZE + 8 + 8 + 8 + 4;
constexpr std::size_t SNAPSHOT_FIXED_SIZE = COMMON_SIZE + 8 + 4 + 4;
#if defined(TICKSYNC_BENCHMARK_PRECISION_DOUBLE)
constexpr std::size_t WIRE_SCALAR_SIZE = 8;
#else
constexpr std::size_t WIRE_SCALAR_SIZE = 4;
#endif
constexpr std::size_t ENTITY_SIZE = 4 + 4 + 8 + 8 + (6 * WIRE_SCALAR_SIZE) + 4;


// Appends one unsigned byte to the reference wire buffer.
void append_u8(std::vector<std::uint8_t> &output, std::uint8_t value) {
	output.push_back(value);
}


// Appends one little-endian unsigned 16-bit value.
void append_u16(std::vector<std::uint8_t> &output, std::uint16_t value) {
	output.push_back(static_cast<std::uint8_t>(value));
	output.push_back(static_cast<std::uint8_t>(value >> 8));
}


// Appends one little-endian unsigned 32-bit value.
void append_u32(std::vector<std::uint8_t> &output, std::uint32_t value) {
	for (unsigned shift = 0; shift < 32; shift += 8) {
		output.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}


// Appends one little-endian unsigned 64-bit value.
void append_u64(std::vector<std::uint8_t> &output, std::uint64_t value) {
	for (unsigned shift = 0; shift < 64; shift += 8) {
		output.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}


// Appends one signed 16-bit bit pattern.
void append_i16(std::vector<std::uint8_t> &output, std::int16_t value) {
	// Appends one little-endian unsigned 16-bit value.
	append_u16(output, static_cast<std::uint16_t>(value));
}


// Appends one signed 64-bit bit pattern.
void append_i64(std::vector<std::uint8_t> &output, std::int64_t value) {
	// Appends one little-endian unsigned 64-bit value.
	append_u64(output, static_cast<std::uint64_t>(value));
}


// Appends the explicit float width selected by the benchmark build.
void append_scalar(std::vector<std::uint8_t> &output, double value) {
	// Appends one selected-precision scalar in canonical little-endian order.
	append_benchmark_wire_scalar(output, value);
}

class Cursor {
	ByteView input;
	std::size_t position = 0;

public:
	explicit Cursor(ByteView view) : input(view) {}

	bool read_u8(std::uint8_t &value) noexcept {
		if (remaining() < 1) {
			return false;
		}
		value = input.data[position++];
		return true;
	}

	bool read_u16(std::uint16_t &value) noexcept {
		if (remaining() < 2) {
			return false;
		}
		value = static_cast<std::uint16_t>(input.data[position]) |
				(static_cast<std::uint16_t>(input.data[position + 1]) << 8);
		position += 2;
		return true;
	}

	bool read_u32(std::uint32_t &value) noexcept {
		if (remaining() < 4) {
			return false;
		}
		value = 0;
		for (unsigned shift = 0; shift < 32; shift += 8) {
			value |= static_cast<std::uint32_t>(input.data[position++]) << shift;
		}
		return true;
	}

	bool read_u64(std::uint64_t &value) noexcept {
		if (remaining() < 8) {
			return false;
		}
		value = 0;
		for (unsigned shift = 0; shift < 64; shift += 8) {
			value |= static_cast<std::uint64_t>(input.data[position++]) << shift;
		}
		return true;
	}

	bool read_i16(std::int16_t &value) noexcept {
		std::uint16_t raw = 0;
		if (!read_u16(raw)) {
			return false;
		}
		std::memcpy(&value, &raw, sizeof(value));
		return true;
	}

	bool read_i64(std::int64_t &value) noexcept {
		std::uint64_t raw = 0;
		if (!read_u64(raw)) {
			return false;
		}
		std::memcpy(&value, &raw, sizeof(value));
		return true;
	}

	CandidateDecodeError read_scalar(double &value) noexcept {
		// Reads one canonical selected-precision scalar and advances only on success.
		return read_benchmark_wire_scalar(input, position, value);
	}

	bool read_bytes(std::vector<std::uint8_t> &output, std::size_t count) {
		if (remaining() < count) {
			return false;
		}
		output.assign(input.data + position, input.data + position + count);
		position += count;
		return true;
	}

	std::size_t remaining() const noexcept {
		return input.size - position;
	}
};


// Decodes one fixed-width entity while preserving scalar error categories.
CandidateDecodeError read_entity(Cursor &cursor, BenchmarkEntityState &entity) {
	if (!cursor.read_u32(entity.entity_id) || !cursor.read_u32(entity.change_mask) ||
			!cursor.read_i64(entity.signed_value) || !cursor.read_u64(entity.unsigned_value)) {
		return CandidateDecodeError::TRUNCATED;
	}
	CandidateDecodeError error = cursor.read_scalar(entity.position_x);
	if (error != CandidateDecodeError::OK) {
		return error;
	}
	error = cursor.read_scalar(entity.position_y);
	if (error != CandidateDecodeError::OK) {
		return error;
	}
	error = cursor.read_scalar(entity.position_z);
	if (error != CandidateDecodeError::OK) {
		return error;
	}
	error = cursor.read_scalar(entity.velocity_x);
	if (error != CandidateDecodeError::OK) {
		return error;
	}
	error = cursor.read_scalar(entity.velocity_y);
	if (error != CandidateDecodeError::OK) {
		return error;
	}
	error = cursor.read_scalar(entity.velocity_z);
	if (error != CandidateDecodeError::OK) {
		return error;
	}
	return cursor.read_u32(entity.flags) ? CandidateDecodeError::OK : CandidateDecodeError::TRUNCATED;
}

} // namespace

ProtocolCandidateInfo ReferenceFixedWidthCandidate::info() noexcept {
	return ProtocolCandidateInfo{
		CANDIDATE_ID,
		"reference_fixed_width",
		"Canonical little-endian fixed-width reference used to validate the benchmark harness.",
	};
}

const char *ReferenceFixedWidthCandidate::wire_precision_name() noexcept {
	// Returns the explicit floating-point width selected for this benchmark build.
	return benchmark_wire_precision_name();
}


std::size_t ReferenceFixedWidthCandidate::estimate_encoded_size(const BenchmarkMessage &message) noexcept {
	switch (message.kind) {
		case BenchmarkMessageKind::CONTROL:
			return CONTROL_SIZE;
		case BenchmarkMessageKind::PLAYER_INPUT:
			return PLAYER_FIXED_SIZE + message.blob.size();
		case BenchmarkMessageKind::SNAPSHOT:
			return SNAPSHOT_FIXED_SIZE + message.entities.size() * ENTITY_SIZE + message.blob.size();
	}
	return 0;
}


bool ReferenceFixedWidthCandidate::encode(
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
	// Appends one unsigned byte to the reference wire buffer.
	append_u8(output, static_cast<std::uint8_t>(message.kind));
	// Appends one unsigned byte to the reference wire buffer.
	append_u8(output, message.subtype);
	// Appends one little-endian unsigned 16-bit value.
	append_u16(output, message.flags);
	// Appends one little-endian unsigned 32-bit value.
	append_u32(output, message.sequence);
	// Appends one little-endian unsigned 64-bit value.
	append_u64(output, message.tick);

	switch (message.kind) {
		case BenchmarkMessageKind::CONTROL:
			// Appends one little-endian unsigned 64-bit value.
			append_u64(output, message.reference_tick);
			// Appends one little-endian unsigned 64-bit value.
			append_u64(output, message.buttons);
			break;
		case BenchmarkMessageKind::PLAYER_INPUT:
			// Appends one little-endian unsigned 64-bit value.
			append_u64(output, message.reference_tick);
			// Appends one little-endian unsigned 64-bit value.
			append_u64(output, message.buttons);
			for (const std::int16_t axis : message.axes) {
				// Appends one signed 16-bit bit pattern.
				append_i16(output, axis);
			}
			// Appends one little-endian unsigned 32-bit value.
			append_u32(output, static_cast<std::uint32_t>(message.blob.size()));
			output.insert(output.end(), message.blob.begin(), message.blob.end());
			break;
		case BenchmarkMessageKind::SNAPSHOT:
			// Appends one little-endian unsigned 64-bit value.
			append_u64(output, message.reference_tick);
			// Appends one little-endian unsigned 32-bit value.
			append_u32(output, static_cast<std::uint32_t>(message.entities.size()));
			// Appends one little-endian unsigned 32-bit value.
			append_u32(output, static_cast<std::uint32_t>(message.blob.size()));
			for (const BenchmarkEntityState &entity : message.entities) {
				// Appends one little-endian unsigned 32-bit value.
				append_u32(output, entity.entity_id);
				// Appends one little-endian unsigned 32-bit value.
				append_u32(output, entity.change_mask);
				// Appends one signed 64-bit bit pattern.
				append_i64(output, entity.signed_value);
				// Appends one little-endian unsigned 64-bit value.
				append_u64(output, entity.unsigned_value);
				// Appends the explicit float width selected by the benchmark build.
				append_scalar(output, entity.position_x);
				// Appends the explicit float width selected by the benchmark build.
				append_scalar(output, entity.position_y);
				// Appends the explicit float width selected by the benchmark build.
				append_scalar(output, entity.position_z);
				// Appends the explicit float width selected by the benchmark build.
				append_scalar(output, entity.velocity_x);
				// Appends the explicit float width selected by the benchmark build.
				append_scalar(output, entity.velocity_y);
				// Appends the explicit float width selected by the benchmark build.
				append_scalar(output, entity.velocity_z);
				// Appends one little-endian unsigned 32-bit value.
				append_u32(output, entity.flags);
			}
			output.insert(output.end(), message.blob.begin(), message.blob.end());
			break;
		default:
			output.clear();
			return false;
	}
	return output.size() == estimate_encoded_size(message);
}


CandidateDecodeError ReferenceFixedWidthCandidate::decode(ByteView input, BenchmarkMessage &output) {
	if (input.data == nullptr || input.size < COMMON_SIZE) {
		return CandidateDecodeError::TRUNCATED;
	}
	Cursor cursor(input);
	BenchmarkMessage decoded;
	std::uint8_t kind = 0;
	if (!cursor.read_u8(kind) || !cursor.read_u8(decoded.subtype) ||
			!cursor.read_u16(decoded.flags) || !cursor.read_u32(decoded.sequence) ||
			!cursor.read_u64(decoded.tick)) {
		return CandidateDecodeError::TRUNCATED;
	}
	if (kind < static_cast<std::uint8_t>(BenchmarkMessageKind::CONTROL) ||
			kind > static_cast<std::uint8_t>(BenchmarkMessageKind::SNAPSHOT)) {
		return CandidateDecodeError::UNKNOWN_KIND;
	}
	decoded.kind = static_cast<BenchmarkMessageKind>(kind);

	switch (decoded.kind) {
		case BenchmarkMessageKind::CONTROL:
			if (!cursor.read_u64(decoded.reference_tick) || !cursor.read_u64(decoded.buttons)) {
				return CandidateDecodeError::TRUNCATED;
			}
			break;
		case BenchmarkMessageKind::PLAYER_INPUT: {
			std::uint32_t blob_size = 0;
			if (!cursor.read_u64(decoded.reference_tick) || !cursor.read_u64(decoded.buttons)) {
				return CandidateDecodeError::TRUNCATED;
			}
			for (std::int16_t &axis : decoded.axes) {
				if (!cursor.read_i16(axis)) {
					return CandidateDecodeError::TRUNCATED;
				}
			}
			if (!cursor.read_u32(blob_size)) {
				return CandidateDecodeError::TRUNCATED;
			}
			if (blob_size > MAX_BLOB_SIZE) {
				return CandidateDecodeError::LIMIT_EXCEEDED;
			}
			if (!cursor.read_bytes(decoded.blob, blob_size)) {
				return CandidateDecodeError::TRUNCATED;
			}
			break;
		}
		case BenchmarkMessageKind::SNAPSHOT: {
			std::uint32_t entity_count = 0;
			std::uint32_t blob_size = 0;
			if (!cursor.read_u64(decoded.reference_tick) || !cursor.read_u32(entity_count) ||
					!cursor.read_u32(blob_size)) {
				return CandidateDecodeError::TRUNCATED;
			}
			if (entity_count > MAX_ENTITIES || blob_size > MAX_BLOB_SIZE) {
				return CandidateDecodeError::LIMIT_EXCEEDED;
			}
			if (cursor.remaining() < static_cast<std::size_t>(entity_count) * ENTITY_SIZE + blob_size) {
				return CandidateDecodeError::TRUNCATED;
			}
			decoded.entities.resize(entity_count);
			for (BenchmarkEntityState &entity : decoded.entities) {
				const CandidateDecodeError entity_error = read_entity(cursor, entity);
				if (entity_error != CandidateDecodeError::OK) {
					return entity_error;
				}
			}
			if (!cursor.read_bytes(decoded.blob, blob_size)) {
				return CandidateDecodeError::TRUNCATED;
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


bool ReferenceFixedWidthCandidate::equivalent_for_wire(
		const BenchmarkMessage &expected,
		const BenchmarkMessage &actual) noexcept {
	// Compares the complete semantic contract after wire-precision conversion.
	return benchmark_messages_equivalent_for_wire(expected, actual);
}


std::uint64_t ReferenceFixedWidthCandidate::semantic_hash_for_wire(const BenchmarkMessage &message) noexcept {
	// Hashes canonical semantics independently of host byte order and candidate encoding.
	return benchmark_semantic_hash_for_wire(message);
}


void ReferenceFixedWidthCandidate::append_invalid_packets(
		std::vector<CandidateInvalidPacket> &packets) {
	std::vector<std::uint8_t> excessive_entities;
	append_u8(excessive_entities, static_cast<std::uint8_t>(BenchmarkMessageKind::SNAPSHOT));
	append_u8(excessive_entities, 0);
	append_u16(excessive_entities, 0);
	append_u32(excessive_entities, 0);
	append_u64(excessive_entities, 0);
	append_u64(excessive_entities, 0);
	append_u32(excessive_entities, MAX_ENTITIES + 1U);
	append_u32(excessive_entities, 0);
	packets.push_back(CandidateInvalidPacket{
		"fixed-width-excessive-entity-count",
		std::move(excessive_entities),
		CandidateDecodeError::LIMIT_EXCEEDED,
	});

	std::vector<std::uint8_t> excessive_blob;
	append_u8(excessive_blob, static_cast<std::uint8_t>(BenchmarkMessageKind::PLAYER_INPUT));
	append_u8(excessive_blob, 0);
	append_u16(excessive_blob, 0);
	append_u32(excessive_blob, 0);
	append_u64(excessive_blob, 0);
	append_u64(excessive_blob, 0);
	append_u64(excessive_blob, 0);
	for (unsigned axis = 0; axis < 4; ++axis) {
		append_i16(excessive_blob, 0);
	}
	append_u32(excessive_blob, MAX_BLOB_SIZE + 1U);
	packets.push_back(CandidateInvalidPacket{
		"fixed-width-excessive-player-blob",
		std::move(excessive_blob),
		CandidateDecodeError::LIMIT_EXCEEDED,
	});

	BenchmarkMessage noncanonical_nan_message;
	noncanonical_nan_message.kind = BenchmarkMessageKind::SNAPSHOT;
	noncanonical_nan_message.entities.resize(1);
	std::vector<std::uint8_t> noncanonical_nan;
	if (encode(noncanonical_nan_message, noncanonical_nan)) {
		constexpr std::size_t FIRST_SCALAR_OFFSET = SNAPSHOT_FIXED_SIZE + 24;
		noncanonical_nan[FIRST_SCALAR_OFFSET] = 1;
		if (benchmark_wire_scalar_size() == 4) {
			noncanonical_nan[FIRST_SCALAR_OFFSET + 1] = 0;
			noncanonical_nan[FIRST_SCALAR_OFFSET + 2] = 0xC0;
			noncanonical_nan[FIRST_SCALAR_OFFSET + 3] = 0x7F;
		} else {
			for (std::size_t index = 1; index < 6; ++index) {
				noncanonical_nan[FIRST_SCALAR_OFFSET + index] = 0;
			}
			noncanonical_nan[FIRST_SCALAR_OFFSET + 6] = 0xF8;
			noncanonical_nan[FIRST_SCALAR_OFFSET + 7] = 0x7F;
		}
		packets.push_back(CandidateInvalidPacket{
			"fixed-width-noncanonical-nan",
			std::move(noncanonical_nan),
			CandidateDecodeError::MALFORMED,
		});
	}
}

} // namespace tick_synchronizer::benchmarks

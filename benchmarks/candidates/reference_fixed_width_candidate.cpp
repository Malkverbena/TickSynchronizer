// Implements the fixed-width reference protocol candidate.
// Provides a transparent baseline for wire size, CPU cost, and validation.

#include "reference_fixed_width_candidate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace tick_synchronizer::benchmarks {
namespace {

#if defined(TICKSYNC_BENCHMARK_PRECISION_DOUBLE)
using WireScalar = double;
#else
using WireScalar = float;
#endif

constexpr std::size_t COMMON_SIZE = 16;
constexpr std::size_t CONTROL_SIZE = COMMON_SIZE + 16;
constexpr std::size_t PLAYER_FIXED_SIZE = COMMON_SIZE + 8 + 8 + 8 + 4;
constexpr std::size_t SNAPSHOT_FIXED_SIZE = COMMON_SIZE + 8 + 4 + 4;
constexpr std::size_t ENTITY_SIZE = 4 + 4 + 8 + 8 + (6 * sizeof(WireScalar)) + 4;
constexpr std::uint64_t FNV_OFFSET = UINT64_C(1469598103934665603);
constexpr std::uint64_t FNV_PRIME = UINT64_C(1099511628211);

// Updates a deterministic non-cryptographic diagnostic hash.
void hash_bytes(std::uint64_t &hash, const void *data, std::size_t size) noexcept {
	const auto *bytes = static_cast<const std::uint8_t *>(data);
	for (std::size_t index = 0; index < size; ++index) {
		hash ^= bytes[index];
		hash *= FNV_PRIME;
	}
}

// Feeds one trivially copyable value into the candidate semantic hash.
template <typename T>
void hash_value(std::uint64_t &hash, const T &value) noexcept {
	// Updates a deterministic non-cryptographic diagnostic hash.
	hash_bytes(hash, &value, sizeof(value));
}


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
	const WireScalar scalar = static_cast<WireScalar>(value);
	if constexpr (sizeof(WireScalar) == sizeof(std::uint64_t)) {
		std::uint64_t bits = 0;
		std::memcpy(&bits, &scalar, sizeof(bits));
		// Appends one little-endian unsigned 64-bit value.
		append_u64(output, bits);
	} else {
		std::uint32_t bits = 0;
		std::memcpy(&bits, &scalar, sizeof(bits));
		// Appends one little-endian unsigned 32-bit value.
		append_u32(output, bits);
	}
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
		value = static_cast<std::int16_t>(raw);
		return true;
	}

	bool read_i64(std::int64_t &value) noexcept {
		std::uint64_t raw = 0;
		if (!read_u64(raw)) {
			return false;
		}
		value = static_cast<std::int64_t>(raw);
		return true;
	}

	bool read_scalar(double &value) noexcept {
		WireScalar scalar = 0;
		if constexpr (sizeof(WireScalar) == sizeof(std::uint64_t)) {
			std::uint64_t bits = 0;
			if (!read_u64(bits)) {
				return false;
			}
			std::memcpy(&scalar, &bits, sizeof(bits));
		} else {
			std::uint32_t bits = 0;
			if (!read_u32(bits)) {
				return false;
			}
			std::memcpy(&scalar, &bits, sizeof(bits));
		}
		value = static_cast<double>(scalar);
		return true;
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

// Rounds semantic doubles to the selected wire precision when required.
WireScalar canonical_scalar(double value) noexcept {
	return static_cast<WireScalar>(value);
}


// Compares scalar semantics after canonical wire-precision conversion.
bool scalar_equal(double expected, double actual) noexcept {
	// Rounds semantic doubles to the selected wire precision when required.
	const WireScalar canonical_expected = canonical_scalar(expected);
	// Rounds semantic doubles to the selected wire precision when required.
	const WireScalar canonical_actual = canonical_scalar(actual);
	using Bits = std::conditional_t<sizeof(WireScalar) == 8, std::uint64_t, std::uint32_t>;
	Bits expected_bits = 0;
	Bits actual_bits = 0;
	std::memcpy(&expected_bits, &canonical_expected, sizeof(Bits));
	std::memcpy(&actual_bits, &canonical_actual, sizeof(Bits));
	return expected_bits == actual_bits;
}


// Compares entity fields preserved by the reference wire format.
bool entities_equal(const BenchmarkEntityState &expected, const BenchmarkEntityState &actual) noexcept {
	return expected.entity_id == actual.entity_id &&
			expected.change_mask == actual.change_mask &&
			expected.signed_value == actual.signed_value &&
			expected.unsigned_value == actual.unsigned_value &&
			// Compares scalar semantics after canonical wire-precision conversion.
			scalar_equal(expected.position_x, actual.position_x) &&
			// Compares scalar semantics after canonical wire-precision conversion.
			scalar_equal(expected.position_y, actual.position_y) &&
			// Compares scalar semantics after canonical wire-precision conversion.
			scalar_equal(expected.position_z, actual.position_z) &&
			// Compares scalar semantics after canonical wire-precision conversion.
			scalar_equal(expected.velocity_x, actual.velocity_x) &&
			// Compares scalar semantics after canonical wire-precision conversion.
			scalar_equal(expected.velocity_y, actual.velocity_y) &&
			// Compares scalar semantics after canonical wire-precision conversion.
			scalar_equal(expected.velocity_z, actual.velocity_z) &&
			expected.flags == actual.flags;
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
#if defined(TICKSYNC_BENCHMARK_PRECISION_DOUBLE)
	return "double";
#else
	return "single";
#endif
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
	if (message.kind != BenchmarkMessageKind::SNAPSHOT && !message.entities.empty()) {
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
	std::uint8_t kind = 0;
	if (!cursor.read_u8(kind) || !cursor.read_u8(output.subtype) ||
			!cursor.read_u16(output.flags) || !cursor.read_u32(output.sequence) ||
			!cursor.read_u64(output.tick)) {
		return CandidateDecodeError::TRUNCATED;
	}
	if (kind < static_cast<std::uint8_t>(BenchmarkMessageKind::CONTROL) ||
			kind > static_cast<std::uint8_t>(BenchmarkMessageKind::SNAPSHOT)) {
		return CandidateDecodeError::UNKNOWN_KIND;
	}
	output.kind = static_cast<BenchmarkMessageKind>(kind);
	output.entities.clear();
	output.blob.clear();
	output.reference_tick = 0;
	output.buttons = 0;
	output.axes = {};

	switch (output.kind) {
		case BenchmarkMessageKind::CONTROL:
			if (!cursor.read_u64(output.reference_tick) || !cursor.read_u64(output.buttons)) {
				return CandidateDecodeError::TRUNCATED;
			}
			break;
		case BenchmarkMessageKind::PLAYER_INPUT: {
			std::uint32_t blob_size = 0;
			if (!cursor.read_u64(output.reference_tick) || !cursor.read_u64(output.buttons)) {
				return CandidateDecodeError::TRUNCATED;
			}
			for (std::int16_t &axis : output.axes) {
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
			if (!cursor.read_bytes(output.blob, blob_size)) {
				return CandidateDecodeError::TRUNCATED;
			}
			break;
		}
		case BenchmarkMessageKind::SNAPSHOT: {
			std::uint32_t entity_count = 0;
			std::uint32_t blob_size = 0;
			if (!cursor.read_u64(output.reference_tick) || !cursor.read_u32(entity_count) ||
					!cursor.read_u32(blob_size)) {
				return CandidateDecodeError::TRUNCATED;
			}
			if (entity_count > MAX_ENTITIES || blob_size > MAX_BLOB_SIZE) {
				return CandidateDecodeError::LIMIT_EXCEEDED;
			}
			if (cursor.remaining() < static_cast<std::size_t>(entity_count) * ENTITY_SIZE + blob_size) {
				return CandidateDecodeError::TRUNCATED;
			}
			output.entities.resize(entity_count);
			for (BenchmarkEntityState &entity : output.entities) {
				if (!cursor.read_u32(entity.entity_id) || !cursor.read_u32(entity.change_mask) ||
						!cursor.read_i64(entity.signed_value) || !cursor.read_u64(entity.unsigned_value) ||
						!cursor.read_scalar(entity.position_x) || !cursor.read_scalar(entity.position_y) ||
						!cursor.read_scalar(entity.position_z) || !cursor.read_scalar(entity.velocity_x) ||
						!cursor.read_scalar(entity.velocity_y) || !cursor.read_scalar(entity.velocity_z) ||
						!cursor.read_u32(entity.flags)) {
					return CandidateDecodeError::TRUNCATED;
				}
			}
			if (!cursor.read_bytes(output.blob, blob_size)) {
				return CandidateDecodeError::TRUNCATED;
			}
			break;
		}
	}
	return cursor.remaining() == 0 ? CandidateDecodeError::OK : CandidateDecodeError::TRAILING_DATA;
}


bool ReferenceFixedWidthCandidate::equivalent_for_wire(
		const BenchmarkMessage &expected,
		const BenchmarkMessage &actual) noexcept {
	if (expected.kind != actual.kind || expected.subtype != actual.subtype ||
			expected.flags != actual.flags || expected.sequence != actual.sequence ||
			expected.tick != actual.tick || expected.reference_tick != actual.reference_tick ||
			expected.buttons != actual.buttons || expected.axes != actual.axes ||
			expected.blob != actual.blob || expected.entities.size() != actual.entities.size()) {
		return false;
	}
	for (std::size_t index = 0; index < expected.entities.size(); ++index) {
		if (!entities_equal(expected.entities[index], actual.entities[index])) {
			return false;
		}
	}
	return true;
}


std::uint64_t ReferenceFixedWidthCandidate::semantic_hash_for_wire(const BenchmarkMessage &message) noexcept {
	std::uint64_t hash = FNV_OFFSET;
	const std::uint8_t kind = static_cast<std::uint8_t>(message.kind);
	// Feeds one trivially copyable value into the candidate semantic hash.
	hash_value(hash, kind);
	// Feeds one trivially copyable value into the candidate semantic hash.
	hash_value(hash, message.subtype);
	// Feeds one trivially copyable value into the candidate semantic hash.
	hash_value(hash, message.flags);
	// Feeds one trivially copyable value into the candidate semantic hash.
	hash_value(hash, message.sequence);
	// Feeds one trivially copyable value into the candidate semantic hash.
	hash_value(hash, message.tick);
	// Feeds one trivially copyable value into the candidate semantic hash.
	hash_value(hash, message.reference_tick);
	// Feeds one trivially copyable value into the candidate semantic hash.
	hash_value(hash, message.buttons);
	for (const std::int16_t axis : message.axes) {
		// Feeds one trivially copyable value into the candidate semantic hash.
		hash_value(hash, axis);
	}
	for (const BenchmarkEntityState &entity : message.entities) {
		// Feeds one trivially copyable value into the candidate semantic hash.
		hash_value(hash, entity.entity_id);
		// Feeds one trivially copyable value into the candidate semantic hash.
		hash_value(hash, entity.change_mask);
		// Feeds one trivially copyable value into the candidate semantic hash.
		hash_value(hash, entity.signed_value);
		// Feeds one trivially copyable value into the candidate semantic hash.
		hash_value(hash, entity.unsigned_value);
		const WireScalar values[] = {
			// Rounds semantic doubles to the selected wire precision when required.
			canonical_scalar(entity.position_x), canonical_scalar(entity.position_y),
			// Rounds semantic doubles to the selected wire precision when required.
			canonical_scalar(entity.position_z), canonical_scalar(entity.velocity_x),
			// Rounds semantic doubles to the selected wire precision when required.
			canonical_scalar(entity.velocity_y), canonical_scalar(entity.velocity_z),
		};
		for (const WireScalar value : values) {
			// Feeds one trivially copyable value into the candidate semantic hash.
			hash_value(hash, value);
		}
		// Feeds one trivially copyable value into the candidate semantic hash.
		hash_value(hash, entity.flags);
	}
	if (!message.blob.empty()) {
		// Updates a deterministic non-cryptographic diagnostic hash.
		hash_bytes(hash, message.blob.data(), message.blob.size());
	}
	return hash;
}

} // namespace tick_synchronizer::benchmarks

// Implements shared protocol-candidate precision, equivalence, and hash rules.
// Keeps semantic diagnostics independent of host byte order and wire candidate.

#include "protocol_candidate.h"

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
using WireScalarBits = std::conditional_t<sizeof(WireScalar) == 8, std::uint64_t, std::uint32_t>;

static_assert(std::numeric_limits<WireScalar>::is_iec559);
static_assert(std::numeric_limits<WireScalar>::radix == 2);
static_assert(std::numeric_limits<float>::digits == 24);
static_assert(std::numeric_limits<double>::is_iec559);
static_assert(std::numeric_limits<double>::radix == 2);
static_assert(std::numeric_limits<double>::digits == 53);
static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(sizeof(double) == sizeof(std::uint64_t));

constexpr std::uint64_t FNV_OFFSET = UINT64_C(1469598103934665603);
constexpr std::uint64_t FNV_PRIME = UINT64_C(1099511628211);
#if defined(TICKSYNC_BENCHMARK_PRECISION_DOUBLE)
constexpr WireScalarBits EXPONENT_MASK = UINT64_C(0x7FF0000000000000);
constexpr WireScalarBits MANTISSA_MASK = UINT64_C(0x000FFFFFFFFFFFFF);
constexpr WireScalarBits CANONICAL_NAN_BITS = UINT64_C(0x7FF8000000000000);
constexpr WireScalarBits POSITIVE_INFINITY_BITS = UINT64_C(0x7FF0000000000000);
constexpr WireScalarBits NEGATIVE_INFINITY_BITS = UINT64_C(0xFFF0000000000000);
#else
constexpr WireScalarBits EXPONENT_MASK = UINT32_C(0x7F800000);
constexpr WireScalarBits MANTISSA_MASK = UINT32_C(0x007FFFFF);
constexpr WireScalarBits CANONICAL_NAN_BITS = UINT32_C(0x7FC00000);
constexpr WireScalarBits POSITIVE_INFINITY_BITS = UINT32_C(0x7F800000);
constexpr WireScalarBits NEGATIVE_INFINITY_BITS = UINT32_C(0xFF800000);
#endif

// Feeds one byte into the canonical non-cryptographic diagnostic hash.
void hash_u8(std::uint64_t &hash, std::uint8_t value) noexcept {
	hash ^= value;
	hash *= FNV_PRIME;
}


// Feeds one unsigned integer into the hash in canonical little-endian order.
template <typename T>
void hash_unsigned(std::uint64_t &hash, T value) noexcept {
	static_assert(std::is_unsigned_v<T>);
	for (unsigned shift = 0; shift < sizeof(T) * 8U; shift += 8U) {
		// Feeds one byte into the canonical non-cryptographic diagnostic hash.
		hash_u8(hash, static_cast<std::uint8_t>(value >> shift));
	}
}


#if !defined(TICKSYNC_BENCHMARK_PRECISION_DOUBLE)
// Rounds an unsigned significand right with IEEE ties-to-even behavior.
std::uint64_t round_right_to_even(std::uint64_t value, unsigned shift) noexcept {
	if (shift == 0) {
		return value;
	}
	if (shift >= 64) {
		return 0;
	}
	const std::uint64_t truncated = value >> shift;
	const std::uint64_t remainder_mask = (UINT64_C(1) << shift) - 1U;
	const std::uint64_t remainder = value & remainder_mask;
	const std::uint64_t halfway = UINT64_C(1) << (shift - 1U);
	return truncated + static_cast<std::uint64_t>(
			remainder > halfway || (remainder == halfway && (truncated & 1U) != 0));
}


// Converts binary64 to canonical binary32 with deterministic ties-to-even rounding.
std::uint32_t binary32_bits_from_double(double value) noexcept {
	std::uint64_t input_bits = 0;
	std::memcpy(&input_bits, &value, sizeof(input_bits));
	const std::uint32_t sign = static_cast<std::uint32_t>(input_bits >> 32U) & UINT32_C(0x80000000);
	const std::uint32_t exponent_bits = static_cast<std::uint32_t>(
			(input_bits >> 52U) & UINT64_C(0x7FF));
	const std::uint64_t fraction = input_bits & UINT64_C(0x000FFFFFFFFFFFFF);
	if (exponent_bits == UINT32_C(0x7FF)) {
		return fraction == 0 ? sign | UINT32_C(0x7F800000) : UINT32_C(0x7FC00000);
	}
	if (exponent_bits == 0) {
		return sign;
	}

	int exponent = static_cast<int>(exponent_bits) - 1023;
	const std::uint64_t significand = (UINT64_C(1) << 52U) | fraction;
	if (exponent >= -126) {
		std::uint64_t rounded = round_right_to_even(significand, 29);
		if (rounded == (UINT64_C(1) << 24U)) {
			rounded >>= 1U;
			++exponent;
		}
		if (exponent > 127) {
			return sign | UINT32_C(0x7F800000);
		}
		return sign |
				(static_cast<std::uint32_t>(exponent + 127) << 23U) |
				(static_cast<std::uint32_t>(rounded) & UINT32_C(0x007FFFFF));
	}

	const unsigned shift = static_cast<unsigned>(-exponent - 97);
	const std::uint64_t rounded = round_right_to_even(significand, shift);
	return sign | static_cast<std::uint32_t>(rounded);
}
#endif


// Returns the selected-precision canonical bit pattern for exact comparison.
WireScalarBits scalar_bits(double value) noexcept {
#if defined(TICKSYNC_BENCHMARK_PRECISION_DOUBLE)
	WireScalarBits bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	if ((bits & EXPONENT_MASK) == EXPONENT_MASK && (bits & MANTISSA_MASK) != 0) {
		return CANONICAL_NAN_BITS;
	}
	return bits;
#else
	return binary32_bits_from_double(value);
#endif
}


// Reports whether one selected-precision bit pattern is a NaN payload.
bool is_nan_bits(WireScalarBits bits) noexcept {
	return (bits & EXPONENT_MASK) == EXPONENT_MASK && (bits & MANTISSA_MASK) != 0;
}


// Verifies one scalar against an exact selected-precision golden bit pattern.
bool scalar_matches(double value, WireScalarBits expected) {
	std::vector<std::uint8_t> encoded;
	append_benchmark_wire_scalar(encoded, value);
	if (encoded.size() != sizeof(WireScalarBits)) {
		return false;
	}
	WireScalarBits actual = 0;
	for (unsigned shift = 0; shift < sizeof(actual) * 8U; shift += 8U) {
		actual |= static_cast<WireScalarBits>(encoded[shift / 8U]) << shift;
	}
	return actual == expected;
}


// Reports whether every message axis has its canonical irrelevant value.
bool axes_are_zero(const BenchmarkMessage &message) noexcept {
	for (const std::int16_t axis : message.axes) {
		if (axis != 0) {
			return false;
		}
	}
	return true;
}


// Compares all entity semantics after selected-precision conversion.
bool entities_equivalent(
		const BenchmarkEntityState &expected,
		const BenchmarkEntityState &actual) noexcept {
	return expected.entity_id == actual.entity_id &&
			expected.change_mask == actual.change_mask &&
			expected.signed_value == actual.signed_value &&
			expected.unsigned_value == actual.unsigned_value &&
			// Returns the selected-precision bit pattern for exact scalar comparison.
			scalar_bits(expected.position_x) == scalar_bits(actual.position_x) &&
			scalar_bits(expected.position_y) == scalar_bits(actual.position_y) &&
			scalar_bits(expected.position_z) == scalar_bits(actual.position_z) &&
			scalar_bits(expected.velocity_x) == scalar_bits(actual.velocity_x) &&
			scalar_bits(expected.velocity_y) == scalar_bits(actual.velocity_y) &&
			scalar_bits(expected.velocity_z) == scalar_bits(actual.velocity_z) &&
			expected.flags == actual.flags;
}

} // namespace

const char *benchmark_wire_precision_name() noexcept {
#if defined(TICKSYNC_BENCHMARK_PRECISION_DOUBLE)
	return "double";
#else
	return "single";
#endif
}


std::size_t benchmark_wire_scalar_size() noexcept {
	return sizeof(WireScalar);
}


void append_benchmark_wire_scalar(std::vector<std::uint8_t> &output, double value) {
	// Returns the selected-precision bit pattern for exact scalar comparison.
	const auto bits = scalar_bits(value);
	for (unsigned shift = 0; shift < sizeof(bits) * 8U; shift += 8U) {
		output.push_back(static_cast<std::uint8_t>(bits >> shift));
	}
}


CandidateDecodeError read_benchmark_wire_scalar(
		ByteView input,
		std::size_t &position,
		double &value) noexcept {
	if (position > input.size || input.size - position < sizeof(WireScalar)) {
		return CandidateDecodeError::TRUNCATED;
	}
	WireScalarBits bits = 0;
	for (unsigned shift = 0; shift < sizeof(bits) * 8U; shift += 8U) {
		bits |= static_cast<WireScalarBits>(input.data[position + shift / 8U]) << shift;
	}
	if (is_nan_bits(bits) && bits != CANONICAL_NAN_BITS) {
		return CandidateDecodeError::MALFORMED;
	}
	WireScalar scalar = 0;
	std::memcpy(&scalar, &bits, sizeof(scalar));
	value = static_cast<double>(scalar);
	position += sizeof(WireScalar);
	return CandidateDecodeError::OK;
}


bool benchmark_wire_scalar_contract_self_test() {
	if (!scalar_matches(0.0, 0) ||
			!scalar_matches(-0.0, static_cast<WireScalarBits>(UINT64_C(1) <<
					(sizeof(WireScalarBits) * 8U - 1U))) ||
			!scalar_matches(std::numeric_limits<double>::infinity(), POSITIVE_INFINITY_BITS) ||
			!scalar_matches(-std::numeric_limits<double>::infinity(), NEGATIVE_INFINITY_BITS) ||
			!scalar_matches(std::numeric_limits<double>::quiet_NaN(), CANONICAL_NAN_BITS)) {
		return false;
	}
#if !defined(TICKSYNC_BENCHMARK_PRECISION_DOUBLE)
	if (!scalar_matches(0x1p-149, UINT32_C(0x00000001)) ||
			!scalar_matches(0x1p-150, UINT32_C(0x00000000)) ||
			!scalar_matches(0x1.8p-149, UINT32_C(0x00000002)) ||
			!scalar_matches(0x1.000001p0, UINT32_C(0x3F800000)) ||
			!scalar_matches(0x1.000003p0, UINT32_C(0x3F800002)) ||
			!scalar_matches(0x1.fffffep127, UINT32_C(0x7F7FFFFF)) ||
			!scalar_matches(0x1.ffffffp127, UINT32_C(0x7F800000)) ||
			!scalar_matches(std::numeric_limits<double>::max(), UINT32_C(0x7F800000))) {
		return false;
	}
#endif
	std::vector<std::uint8_t> encoded;
	append_benchmark_wire_scalar(encoded, std::numeric_limits<double>::quiet_NaN());
	double decoded = 123.0;
	std::size_t position = 0;
	if (read_benchmark_wire_scalar(make_byte_view(encoded), position, decoded) != CandidateDecodeError::OK ||
			position != encoded.size() || !std::isnan(decoded)) {
		return false;
	}
	encoded[0] ^= UINT8_C(1);
	position = 0;
	decoded = 123.0;
	return read_benchmark_wire_scalar(make_byte_view(encoded), position, decoded) ==
				CandidateDecodeError::MALFORMED &&
			position == 0 && decoded == 123.0;
}


bool benchmark_message_kind_semantics_are_valid(const BenchmarkMessage &message) noexcept {
	switch (message.kind) {
		case BenchmarkMessageKind::CONTROL:
			return message.entities.empty() && message.blob.empty() && axes_are_zero(message);
		case BenchmarkMessageKind::PLAYER_INPUT:
			return message.entities.empty();
		case BenchmarkMessageKind::SNAPSHOT:
			return message.buttons == 0 && axes_are_zero(message);
	}
	return false;
}


bool benchmark_messages_equivalent_for_wire(
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
		// Compares all entity semantics after selected-precision conversion.
		if (!entities_equivalent(expected.entities[index], actual.entities[index])) {
			return false;
		}
	}
	return true;
}


std::uint64_t benchmark_semantic_hash_for_wire(const BenchmarkMessage &message) noexcept {
	std::uint64_t hash = FNV_OFFSET;
	// Feeds one byte into the canonical non-cryptographic diagnostic hash.
	hash_u8(hash, static_cast<std::uint8_t>(message.kind));
	hash_u8(hash, message.subtype);
	// Feeds one unsigned integer into the hash in canonical little-endian order.
	hash_unsigned(hash, message.flags);
	hash_unsigned(hash, message.sequence);
	hash_unsigned(hash, message.tick);
	hash_unsigned(hash, message.reference_tick);
	hash_unsigned(hash, message.buttons);
	for (const std::int16_t axis : message.axes) {
		hash_unsigned(hash, static_cast<std::uint16_t>(axis));
	}
	hash_unsigned(hash, static_cast<std::uint64_t>(message.entities.size()));
	for (const BenchmarkEntityState &entity : message.entities) {
		hash_unsigned(hash, entity.entity_id);
		hash_unsigned(hash, entity.change_mask);
		hash_unsigned(hash, static_cast<std::uint64_t>(entity.signed_value));
		hash_unsigned(hash, entity.unsigned_value);
		hash_unsigned(hash, scalar_bits(entity.position_x));
		hash_unsigned(hash, scalar_bits(entity.position_y));
		hash_unsigned(hash, scalar_bits(entity.position_z));
		hash_unsigned(hash, scalar_bits(entity.velocity_x));
		hash_unsigned(hash, scalar_bits(entity.velocity_y));
		hash_unsigned(hash, scalar_bits(entity.velocity_z));
		hash_unsigned(hash, entity.flags);
	}
	hash_unsigned(hash, static_cast<std::uint64_t>(message.blob.size()));
	for (const std::uint8_t byte : message.blob) {
		hash_u8(hash, byte);
	}
	return hash;
}

} // namespace tick_synchronizer::benchmarks

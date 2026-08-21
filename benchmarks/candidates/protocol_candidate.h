// Defines shared protocol-candidate metadata and correctness utilities.
// Establishes candidate-independent wire semantics for fair comparisons.

#pragma once

#include "../benchmark_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace tick_synchronizer::benchmarks {

enum class CandidateDecodeError : std::uint8_t {
	OK = 0,
	TRUNCATED,
	UNKNOWN_KIND,
	INVALID_LENGTH,
	LIMIT_EXCEEDED,
	TRAILING_DATA,
	MALFORMED,
};

struct ProtocolCandidateInfo {
	std::uint32_t id = 0;
	std::string_view name;
	std::string_view description;
};

struct CandidateInvalidPacket {
	std::string_view name;
	std::vector<std::uint8_t> bytes;
	CandidateDecodeError expected_error = CandidateDecodeError::MALFORMED;
};

// Returns the explicit floating-point width selected for this benchmark build.
const char *benchmark_wire_precision_name() noexcept;

// Returns the encoded byte width of one selected-precision scalar.
std::size_t benchmark_wire_scalar_size() noexcept;

// Appends one selected-precision scalar in canonical little-endian order.
void append_benchmark_wire_scalar(std::vector<std::uint8_t> &output, double value);

// Reads one canonical selected-precision scalar and advances only on success.
CandidateDecodeError read_benchmark_wire_scalar(
		ByteView input,
		std::size_t &position,
		double &value) noexcept;

// Verifies exact IEEE conversion, canonical NaN, and atomic scalar decoding.
bool benchmark_wire_scalar_contract_self_test();

// Rejects fields that have no canonical meaning for the selected message kind.
bool benchmark_message_kind_semantics_are_valid(const BenchmarkMessage &message) noexcept;

// Compares the complete semantic contract after wire-precision conversion.
bool benchmark_messages_equivalent_for_wire(
		const BenchmarkMessage &expected,
		const BenchmarkMessage &actual) noexcept;

// Hashes canonical semantics independently of host byte order and candidate encoding.
std::uint64_t benchmark_semantic_hash_for_wire(const BenchmarkMessage &message) noexcept;

} // namespace tick_synchronizer::benchmarks

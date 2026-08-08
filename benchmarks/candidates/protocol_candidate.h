// Defines shared protocol-candidate metadata and decode error categories.
// Establishes candidate-independent correctness semantics for comparisons.

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

} // namespace tick_synchronizer::benchmarks

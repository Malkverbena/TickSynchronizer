// Declares the fixed-width reference protocol candidate.
// Exposes encode, decode, equivalence, and semantic hashing to the harness.

#pragma once

#include "protocol_candidate.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tick_synchronizer::benchmarks {

class ReferenceFixedWidthCandidate {
public:
	static constexpr std::uint32_t CANDIDATE_ID = 1;
	static constexpr std::uint32_t MAX_ENTITIES = 1024;
	static constexpr std::uint32_t MAX_BLOB_SIZE = 4096;

	// Returns stable identity and description metadata for this candidate.
	static ProtocolCandidateInfo info() noexcept;
	// Returns the explicit float width selected for this benchmark build.
	static const char *wire_precision_name() noexcept;

	// Computes a reservation estimate without changing semantic output.
	static std::size_t estimate_encoded_size(const BenchmarkMessage &message) noexcept;

	// Encodes one semantic message into the fixed-width reference format.
	static bool encode(const BenchmarkMessage &message, std::vector<std::uint8_t> &output);

	// Decodes one packet atomically and returns a candidate-level error.
	static CandidateDecodeError decode(ByteView input, BenchmarkMessage &output);

	// Compares only semantics preserved by the selected wire precision.
	static bool equivalent_for_wire(const BenchmarkMessage &expected, const BenchmarkMessage &actual) noexcept;

	// Hashes the canonical semantics used by round-trip validation.
	static std::uint64_t semantic_hash_for_wire(const BenchmarkMessage &message) noexcept;
};

} // namespace tick_synchronizer::benchmarks

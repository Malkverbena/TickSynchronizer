// Declares the isolated canonical-varint protocol candidate.
// Changes integer coding while preserving framing, floats, limits, and semantics.

#pragma once

#include "protocol_candidate.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tick_synchronizer::benchmarks {

class VarintZigZagFixedFloatCandidate {
public:
	static constexpr std::uint32_t CANDIDATE_ID = 2;
	static constexpr std::uint32_t MAX_ENTITIES = 1024;
	static constexpr std::uint32_t MAX_BLOB_SIZE = 4096;

	// Returns stable identity and description metadata for this candidate.
	static ProtocolCandidateInfo info() noexcept;
	// Returns the explicit float width selected for this benchmark build.
	static const char *wire_precision_name() noexcept;

	// Computes the exact canonical encoded size without modifying output.
	static std::size_t estimate_encoded_size(const BenchmarkMessage &message) noexcept;

	// Encodes unsigned integers as ULEB128 and signed integers with ZigZag.
	static bool encode(const BenchmarkMessage &message, std::vector<std::uint8_t> &output);

	// Decodes atomically and rejects overflow and noncanonical integer encodings.
	static CandidateDecodeError decode(ByteView input, BenchmarkMessage &output);

	// Compares only semantics preserved by the selected wire precision.
	static bool equivalent_for_wire(const BenchmarkMessage &expected, const BenchmarkMessage &actual) noexcept;

	// Hashes the shared canonical semantics rather than candidate-specific bytes.
	static std::uint64_t semantic_hash_for_wire(const BenchmarkMessage &message) noexcept;

	// Adds varint-specific malformed packets with exact expected errors.
	static void append_invalid_packets(std::vector<CandidateInvalidPacket> &packets);
};

} // namespace tick_synchronizer::benchmarks

// Declares deterministic protocol benchmark dataset generation.
// Exposes the shared semantic corpus used by every candidate.

#pragma once

#include "benchmark_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace tick_synchronizer::benchmarks {

// Builds the complete deterministic semantic corpus from a fixed seed.
std::vector<BenchmarkDataset> make_protocol_benchmark_datasets(std::uint64_t seed);
// Finds a dataset by its stable report name.
const BenchmarkDataset *find_benchmark_dataset(
		const std::vector<BenchmarkDataset> &datasets,
		std::string_view name) noexcept;

} // namespace tick_synchronizer::benchmarks

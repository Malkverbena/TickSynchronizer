// Declares allocation snapshots and the benchmark allocation counter.
// Provides per-operation allocation metrics without affecting module runtime code.

#pragma once

#include <cstddef>
#include <cstdint>

namespace tick_synchronizer::benchmarks {

struct AllocationSnapshot {
	std::uint64_t allocation_count = 0;
	std::uint64_t allocated_bytes = 0;
};

class AllocationCounter {
public:
	// Starts allocation accounting for the current benchmark operation.
	static void begin() noexcept;

	// Stops accounting and returns allocations recorded since begin().
	static AllocationSnapshot end() noexcept;

	// Records one allocation only while a measurement window is active.
	static void record(std::size_t bytes) noexcept;
};

} // namespace tick_synchronizer::benchmarks

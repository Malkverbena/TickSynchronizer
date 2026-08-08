// Provides cross-platform CPU affinity and runtime processor queries.
// Keeps platform-specific scheduling APIs out of the benchmark runner.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tick_synchronizer::benchmarks {

struct BenchmarkLogicalCpuInfo {
	std::uint32_t logical_cpu = 0;
	std::string processor_group = "unknown";
	std::string processor_number = "unknown";
	std::string cpu_core = "unknown";
	std::string cpu_package = "unknown";
	std::string numa_node = "unknown";
	std::string l3_cache_id = "unknown";
	std::string l3_cache_size = "unknown";
	std::string thread_siblings = "unknown";
};

struct BenchmarkAffinityResult {
	bool requested = false;
	bool applied = false;
	std::uint32_t requested_cpu = 0;
	std::string processor_group = "unknown";
	std::string actual_cpu = "unknown";
	std::string cpu_core = "unknown";
	std::string cpu_package = "unknown";
	std::string numa_node = "unknown";
	std::string l3_cache_id = "unknown";
	std::string thread_siblings = "unknown";
	std::string error;
};

// Enumerates active logical CPUs and the topology needed for directed runs.
std::vector<BenchmarkLogicalCpuInfo> list_benchmark_logical_cpus();

// Pins the current benchmark thread to one logical CPU when requested.
BenchmarkAffinityResult apply_benchmark_thread_affinity(std::uint32_t logical_cpu);

// Reports the current logical CPU after affinity has been applied.
std::string current_benchmark_logical_cpu();

} // namespace tick_synchronizer::benchmarks

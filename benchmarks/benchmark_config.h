// Defines the versioned benchmark methodology and quick diagnostic profile.
// Keeps timing and sampling parameters consistent across protocol candidates.

#pragma once

#include <cstdint>

namespace tick_synchronizer::benchmarks {

inline constexpr char QUALIFICATION_GODOT_COMMIT[] =
		"a13da4feb8d8aefc283c3763d33a2f170a18d541";

struct BenchmarkConfig {
	std::uint32_t suite_version = 1;
	std::uint32_t warmup_rounds = 5;
	std::uint32_t measured_rounds = 30;
	std::uint64_t minimum_iterations = 10'000;
	std::uint64_t minimum_sample_duration_ns = 100'000'000;
	std::uint64_t maximum_iterations = 100'000'000;
	std::uint64_t random_seed = UINT64_C(0x5449434B53594E43);
	bool quick_mode = false;
};

// Reports whether every methodology field matches benchmark suite 1 exactly.
inline bool is_official_benchmark_config(const BenchmarkConfig &config) {
	const BenchmarkConfig official;
	return config.suite_version == official.suite_version &&
			config.warmup_rounds == official.warmup_rounds &&
			config.measured_rounds == official.measured_rounds &&
			config.minimum_iterations == official.minimum_iterations &&
			config.minimum_sample_duration_ns == official.minimum_sample_duration_ns &&
			config.maximum_iterations == official.maximum_iterations &&
			config.random_seed == official.random_seed &&
			config.quick_mode == official.quick_mode;
}

// Returns a short profile for infrastructure validation, not decisions.
inline BenchmarkConfig make_quick_benchmark_config() {
	BenchmarkConfig config;
	config.warmup_rounds = 3;
	config.measured_rounds = 7;
	config.minimum_iterations = 1'000;
	config.minimum_sample_duration_ns = 10'000'000;
	config.maximum_iterations = 10'000'000;
	config.quick_mode = true;
	return config;
}

} // namespace tick_synchronizer::benchmarks

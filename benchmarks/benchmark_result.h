// Defines benchmark result and provenance data structures.
// Carries correctness, timing, size, allocation, build, and platform metadata.

#pragma once

#include "benchmark_config.h"
#include "benchmark_statistics.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tick_synchronizer::benchmarks {

struct OperationBenchmarkResult {
	DistributionStatistics nanoseconds_per_message;
	DistributionStatistics messages_per_second;
	DistributionStatistics mebibytes_per_second;
	DistributionStatistics allocations_per_message;
	DistributionStatistics allocated_bytes_per_message;
	std::uint64_t calibrated_iterations = 0;
	std::uint64_t checksum = 0;
};

struct SizeBenchmarkResult {
	std::uint64_t total_bytes = 0;
	DistributionStatistics bytes_per_message;
};

struct IntegrityBenchmarkResult {
	std::uint64_t encoded_hash = 0;
	std::uint64_t semantic_hash = 0;
	std::uint64_t round_trip_failures = 0;
	std::uint64_t determinism_failures = 0;
};

struct DatasetBenchmarkResult {
	std::string name;
	std::string description;
	std::uint64_t source_message_count = 0;
	SizeBenchmarkResult size;
	OperationBenchmarkResult encode;
	OperationBenchmarkResult decode;
	IntegrityBenchmarkResult integrity;
};

struct InvalidPacketBenchmarkResult {
	std::uint64_t packet_count = 0;
	std::uint64_t rejected = 0;
	std::uint64_t accepted = 0;
	OperationBenchmarkResult decode;
};

struct BenchmarkBuildMetadata {
	std::string generated_utc;
	std::string platform;
	std::string architecture;
	std::string runtime_backend;
	std::string device_manufacturer;
	std::string device_model;
	std::string os_version;
	std::string os_build;
	std::string soc_model;
	std::string compiler;
	std::string compiler_version;
	std::string compiler_command;
	std::string compiler_path;
	std::string compiler_flags;
	std::string optimize;
	std::string lto;
	std::string precision;
	std::string module_commit;
	std::string godot_commit;
	std::string source_state;
	std::string executable_path;
	std::string binary_sha256;
	std::string cpu_model;
	std::string logical_cpu;
	std::string cpu_class;
	std::string processor_group;
	std::string affinity_requested;
	std::string affinity_applied;
	std::string affinity_actual_cpu;
	std::string affinity_error;
	std::string cpu_core;
	std::string cpu_package;
	std::string numa_node;
	std::string l3_cache_id;
	std::string thread_siblings;
	std::string scaling_driver;
	std::string scaling_governor;
	std::string cpu_min_frequency_khz;
	std::string cpu_max_frequency_khz;
};

struct ProtocolBenchmarkReport {
	std::uint32_t schema_version = 3;
	std::uint32_t benchmark_suite_version = 1;
	std::uint32_t api_version = 0;
	std::uint32_t wire_protocol_version = 0;
	std::uint32_t wire_protocol_revision = 0;
	std::uint32_t candidate_id = 0;
	std::string candidate_name;
	std::string candidate_description;
	BenchmarkBuildMetadata build;
	BenchmarkConfig config;
	std::vector<DatasetBenchmarkResult> datasets;
	InvalidPacketBenchmarkResult invalid_packets;
	bool official_eligible = false;
};

} // namespace tick_synchronizer::benchmarks

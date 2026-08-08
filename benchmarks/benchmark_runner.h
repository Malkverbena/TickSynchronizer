// Implements candidate-independent benchmark execution and correctness gates.
// Measures encode/decode behavior over shared datasets with robust statistics.

#pragma once

#include "benchmark_allocation_counter.h"
#include "benchmark_config.h"
#include "benchmark_result.h"
#include "benchmark_statistics.h"
#include "benchmark_types.h"
#include "candidates/protocol_candidate.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tick_synchronizer::benchmarks {
namespace detail {

inline volatile std::uint64_t benchmark_sink = 0;

// Computes a stable FNV-1a digest for encoded benchmark bytes.
inline std::uint64_t hash_bytes(ByteView bytes) noexcept {
	std::uint64_t hash = UINT64_C(1469598103934665603);
	for (std::size_t index = 0; index < bytes.size; ++index) {
		hash ^= bytes.data[index];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

// Combines deterministic sample checksums without even-round cancellation.
inline std::uint64_t combine_diagnostic_checksum(
		std::uint64_t state,
		std::uint64_t value,
		std::uint64_t sequence) noexcept {
	// This combination runs outside the timed region. Unlike XOR across an even
	// number of identical rounds, it preserves diagnostic information.
	value += UINT64_C(0x9E3779B97F4A7C15) +
			sequence * UINT64_C(0xD6E8FEB86659FD93);
	value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
	value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
	value ^= value >> 31U;
	return state ^ (value + UINT64_C(0x9E3779B97F4A7C15) +
			(state << 6U) + (state >> 2U));
}

struct TimedSample {
	double elapsed_ns = 0.0;
	std::uint64_t iterations = 0;
	std::uint64_t bytes = 0;
	AllocationSnapshot allocations;
	std::uint64_t checksum = 0;
};

// Converts one timed sample into normalized metric series.
inline void append_sample_metrics(
		const TimedSample &sample,
		std::vector<double> &nanoseconds_per_message,
		std::vector<double> &messages_per_second,
		std::vector<double> &mebibytes_per_second,
		std::vector<double> &allocations_per_message,
		std::vector<double> &allocated_bytes_per_message) {
	const double iterations = static_cast<double>(sample.iterations);
	const double seconds = sample.elapsed_ns / 1'000'000'000.0;
	nanoseconds_per_message.push_back(sample.elapsed_ns / iterations);
	messages_per_second.push_back(seconds > 0.0 ? iterations / seconds : 0.0);
	mebibytes_per_second.push_back(
			seconds > 0.0 ? (static_cast<double>(sample.bytes) / (1024.0 * 1024.0)) / seconds : 0.0);
	allocations_per_message.push_back(
			static_cast<double>(sample.allocations.allocation_count) / iterations);
	allocated_bytes_per_message.push_back(
			static_cast<double>(sample.allocations.allocated_bytes) / iterations);
}

// Builds one operation result from calibrated iterations and sample distributions.
inline OperationBenchmarkResult make_operation_result(
		std::uint64_t iterations,
		std::uint64_t checksum,
		std::vector<double> ns,
		std::vector<double> messages,
		std::vector<double> mib,
		std::vector<double> allocations,
		std::vector<double> allocated_bytes) {
	OperationBenchmarkResult result;
	result.calibrated_iterations = iterations;
	result.checksum = checksum;
	result.nanoseconds_per_message = calculate_distribution_statistics(std::move(ns));
	result.messages_per_second = calculate_distribution_statistics(std::move(messages));
	result.mebibytes_per_second = calculate_distribution_statistics(std::move(mib));
	result.allocations_per_message = calculate_distribution_statistics(std::move(allocations));
	result.allocated_bytes_per_message = calculate_distribution_statistics(std::move(allocated_bytes));
	return result;
}

template <typename Candidate>
// Measures candidate encoding for one dataset and optional allocation accounting.
TimedSample run_encode_sample(
		const BenchmarkDataset &dataset,
		std::uint64_t iterations,
		bool count_allocations) {
	std::vector<std::uint8_t> output;
	std::size_t maximum_size = 0;
	for (const BenchmarkMessage &message : dataset.messages) {
		maximum_size = std::max(maximum_size, Candidate::estimate_encoded_size(message));
	}
	output.reserve(maximum_size);
	if (count_allocations) {
		// Starts allocation accounting for the current benchmark operation.
		AllocationCounter::begin();
	}
	const auto start = std::chrono::steady_clock::now();
	std::uint64_t bytes = 0;
	std::uint64_t checksum = 0;
	for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
		const BenchmarkMessage &message = dataset.messages[iteration % dataset.messages.size()];
		if (!Candidate::encode(message, output)) {
			throw std::runtime_error("candidate encode failed during benchmark");
		}
		bytes += output.size();
		checksum ^= hash_bytes(make_byte_view(output)) + iteration;
	}
	const auto end = std::chrono::steady_clock::now();
	AllocationSnapshot allocations;
	if (count_allocations) {
		allocations = AllocationCounter::end();
	}
	benchmark_sink ^= checksum;
	return TimedSample{
		std::chrono::duration<double, std::nano>(end - start).count(),
		iterations,
		bytes,
		allocations,
		checksum,
	};
}

template <typename Candidate>
// Measures candidate decoding for a pre-encoded corpus.
TimedSample run_decode_sample(
		const std::vector<std::vector<std::uint8_t>> &encoded,
		std::uint64_t iterations,
		bool count_allocations) {
	BenchmarkMessage output;
	if (count_allocations) {
		// Starts allocation accounting for the current benchmark operation.
		AllocationCounter::begin();
	}
	const auto start = std::chrono::steady_clock::now();
	std::uint64_t bytes = 0;
	std::uint64_t checksum = 0;
	for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
		const std::vector<std::uint8_t> &packet = encoded[iteration % encoded.size()];
		if (Candidate::decode(make_byte_view(packet), output) != CandidateDecodeError::OK) {
			throw std::runtime_error("candidate decode failed during benchmark");
		}
		bytes += packet.size();
		checksum ^= Candidate::semantic_hash_for_wire(output) + iteration;
	}
	const auto end = std::chrono::steady_clock::now();
	AllocationSnapshot allocations;
	if (count_allocations) {
		allocations = AllocationCounter::end();
	}
	benchmark_sink ^= checksum;
	return TimedSample{
		std::chrono::duration<double, std::nano>(end - start).count(),
		iterations,
		bytes,
		allocations,
		checksum,
	};
}

template <typename SampleFunction>
// Calibrates iterations until the configured minimum sample duration is reached.
std::uint64_t calibrate_iterations(const BenchmarkConfig &config, SampleFunction sample_function) {
	std::uint64_t iterations = std::max<std::uint64_t>(config.minimum_iterations, 1);
	while (iterations < config.maximum_iterations) {
		const TimedSample sample = sample_function(iterations, false);
		if (sample.elapsed_ns >= static_cast<double>(config.minimum_sample_duration_ns)) {
			break;
		}
		if (iterations > config.maximum_iterations / 2) {
			iterations = config.maximum_iterations;
			break;
		}
		iterations *= 2;
	}
	return iterations;
}

template <typename SampleFunction>
// Executes warmup and measured rounds for one benchmark operation.
OperationBenchmarkResult benchmark_operation(
		const BenchmarkConfig &config,
		SampleFunction sample_function) {
	const std::uint64_t iterations = calibrate_iterations(config, sample_function);
	for (std::uint32_t round = 0; round < config.warmup_rounds; ++round) {
		(void)sample_function(iterations, false);
	}
	std::vector<double> ns;
	std::vector<double> messages;
	std::vector<double> mib;
	std::vector<double> allocations;
	std::vector<double> allocated_bytes;
	ns.reserve(config.measured_rounds);
	messages.reserve(config.measured_rounds);
	mib.reserve(config.measured_rounds);
	allocations.reserve(config.measured_rounds);
	allocated_bytes.reserve(config.measured_rounds);
	std::uint64_t checksum = UINT64_C(1469598103934665603);
	for (std::uint32_t round = 0; round < config.measured_rounds; ++round) {
		const TimedSample sample = sample_function(iterations, true);
		checksum = combine_diagnostic_checksum(checksum, sample.checksum, round);
		append_sample_metrics(sample, ns, messages, mib, allocations, allocated_bytes);
	}
	return make_operation_result(
			iterations,
			checksum,
			std::move(ns),
			std::move(messages),
			std::move(mib),
			std::move(allocations),
			std::move(allocated_bytes));
}

template <typename Candidate>
// Encodes a deterministic dataset once for decode and integrity measurements.
std::vector<std::vector<std::uint8_t>> encode_corpus(const BenchmarkDataset &dataset) {
	std::vector<std::vector<std::uint8_t>> encoded;
	encoded.reserve(dataset.messages.size());
	for (const BenchmarkMessage &message : dataset.messages) {
		std::vector<std::uint8_t> packet;
		if (!Candidate::encode(message, packet)) {
			throw std::runtime_error("candidate failed to encode deterministic corpus");
		}
		encoded.push_back(std::move(packet));
	}
	return encoded;
}

template <typename Candidate>
// Verifies repeatable encoding and semantic round trips for one candidate.
IntegrityBenchmarkResult verify_integrity(
		const BenchmarkDataset &dataset,
		const std::vector<std::vector<std::uint8_t>> &encoded) {
	IntegrityBenchmarkResult integrity;
	for (std::size_t index = 0; index < dataset.messages.size(); ++index) {
		const BenchmarkMessage &message = dataset.messages[index];
		const std::vector<std::uint8_t> &packet = encoded[index];
		integrity.encoded_hash = combine_diagnostic_checksum(
				integrity.encoded_hash, hash_bytes(make_byte_view(packet)), index);
		std::vector<std::uint8_t> repeated;
		if (!Candidate::encode(message, repeated) || repeated != packet) {
			++integrity.determinism_failures;
		}
		BenchmarkMessage decoded;
		// Round-trip comparison is limited to semantics preserved by wire precision.
		if (Candidate::decode(make_byte_view(packet), decoded) != CandidateDecodeError::OK ||
				!Candidate::equivalent_for_wire(message, decoded)) {
			++integrity.round_trip_failures;
		} else {
			integrity.semantic_hash = combine_diagnostic_checksum(
					integrity.semantic_hash, Candidate::semantic_hash_for_wire(decoded), index);
		}
	}
	return integrity;
}

// Summarizes encoded packet sizes for one dataset.
inline SizeBenchmarkResult calculate_size_result(
		const std::vector<std::vector<std::uint8_t>> &encoded) {
	SizeBenchmarkResult result;
	std::vector<double> sizes;
	sizes.reserve(encoded.size());
	for (const std::vector<std::uint8_t> &packet : encoded) {
		result.total_bytes += packet.size();
		sizes.push_back(static_cast<double>(packet.size()));
	}
	result.bytes_per_message = calculate_distribution_statistics(std::move(sizes));
	return result;
}

// Derives a deterministic malformed-packet corpus from valid packets.
inline std::vector<std::vector<std::uint8_t>> make_invalid_packets(
		const std::vector<std::vector<std::uint8_t>> &valid_packets) {
	std::vector<std::vector<std::uint8_t>> invalid;
	invalid.push_back({});
	for (std::size_t index = 0; index < std::min<std::size_t>(valid_packets.size(), 12); ++index) {
		const std::vector<std::uint8_t> &valid = valid_packets[index];
		if (!valid.empty()) {
			invalid.emplace_back(valid.begin(), valid.end() - 1);
			std::vector<std::uint8_t> trailing = valid;
			trailing.push_back(0xA5);
			invalid.push_back(std::move(trailing));
		}
	}
	if (!valid_packets.empty() && !valid_packets.front().empty()) {
		std::vector<std::uint8_t> unknown_kind = valid_packets.front();
		unknown_kind[0] = 0xFF;
		invalid.push_back(std::move(unknown_kind));
	}
	// Snapshot prefix with entity count and blob length above the fixed limits.
	std::vector<std::uint8_t> excessive(32, 0);
	excessive[0] = static_cast<std::uint8_t>(BenchmarkMessageKind::SNAPSHOT);
	excessive[24] = 0xFF;
	excessive[25] = 0xFF;
	excessive[26] = 0xFF;
	excessive[27] = 0x7F;
	invalid.push_back(std::move(excessive));
	return invalid;
}

} // namespace detail

template <typename Candidate>
// Runs size, integrity, encode, and decode measurements for one dataset.
DatasetBenchmarkResult run_dataset_benchmark(
		const BenchmarkDataset &dataset,
		const BenchmarkConfig &config) {
	if (dataset.messages.empty()) {
		throw std::runtime_error("benchmark dataset is empty: " + dataset.name);
	}
	const std::vector<std::vector<std::uint8_t>> encoded = detail::encode_corpus<Candidate>(dataset);
	DatasetBenchmarkResult result;
	result.name = dataset.name;
	result.description = dataset.description;
	result.source_message_count = dataset.messages.size();
	result.size = detail::calculate_size_result(encoded);
	result.integrity = detail::verify_integrity<Candidate>(dataset, encoded);
	result.encode = detail::benchmark_operation(
			config,
			[&dataset](std::uint64_t iterations, bool allocations) {
				return detail::run_encode_sample<Candidate>(dataset, iterations, allocations);
			});
	result.decode = detail::benchmark_operation(
			config,
			[&encoded](std::uint64_t iterations, bool allocations) {
				return detail::run_decode_sample<Candidate>(encoded, iterations, allocations);
			});
	return result;
}

template <typename Candidate>
// Measures malformed-packet rejection and decode cost for one candidate.
InvalidPacketBenchmarkResult run_invalid_packet_benchmark(
		const std::vector<BenchmarkDataset> &datasets,
		const BenchmarkConfig &config) {
	std::vector<std::vector<std::uint8_t>> valid_packets;
	for (const BenchmarkDataset &dataset : datasets) {
		const auto encoded = detail::encode_corpus<Candidate>(dataset);
		valid_packets.insert(valid_packets.end(), encoded.begin(), encoded.end());
		if (valid_packets.size() >= 32) {
			break;
		}
	}
	const std::vector<std::vector<std::uint8_t>> invalid = detail::make_invalid_packets(valid_packets);
	InvalidPacketBenchmarkResult result;
	result.packet_count = invalid.size();
	BenchmarkMessage output;
	for (const std::vector<std::uint8_t> &packet : invalid) {
		if (Candidate::decode(make_byte_view(packet), output) == CandidateDecodeError::OK) {
			++result.accepted;
		} else {
			++result.rejected;
		}
	}
	result.decode = detail::benchmark_operation(
			config,
			[&invalid](std::uint64_t iterations, bool count_allocations) {
				BenchmarkMessage decoded;
				if (count_allocations) {
					// Starts allocation accounting for the current benchmark operation.
					AllocationCounter::begin();
				}
				const auto start = std::chrono::steady_clock::now();
				std::uint64_t checksum = 0;
				std::uint64_t bytes = 0;
				for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
					const std::vector<std::uint8_t> &packet = invalid[iteration % invalid.size()];
					const CandidateDecodeError error = Candidate::decode(make_byte_view(packet), decoded);
					checksum ^= static_cast<std::uint64_t>(error) + iteration;
					bytes += packet.size();
				}
				const auto end = std::chrono::steady_clock::now();
				AllocationSnapshot allocation_snapshot;
				if (count_allocations) {
					allocation_snapshot = AllocationCounter::end();
				}
				detail::benchmark_sink ^= checksum;
				return detail::TimedSample{
					std::chrono::duration<double, std::nano>(end - start).count(),
					iterations,
					bytes,
					allocation_snapshot,
					checksum,
				};
			});
	return result;
}

} // namespace tick_synchronizer::benchmarks

// Provides the standalone protocol benchmark command-line executable.
// Collects provenance, runs self-tests and measurements, and writes reports.

#include "benchmark_config.h"
#include "benchmark_dataset.h"
#include "benchmark_result.h"
#include "benchmark_result_writer.h"
#include "benchmark_platform.h"
#include "benchmark_runner.h"
#include "candidates/reference_fixed_width_candidate.h"
#include "src/internal/tick_synchronizer_version.h"

#include "benchmark_build_info.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <sys/utsname.h>
#endif

namespace tick_synchronizer::benchmarks {
namespace {

struct CommandLineOptions {
	BenchmarkConfig config;
	bool self_test = false;
	bool list_cpus = false;
	bool list_datasets = false;
	std::string json_path;
	std::string csv_path;
	std::string only_dataset;
	std::optional<std::uint32_t> logical_cpu;
};

// Parses a strict unsigned command-line integer or terminates with context.
std::uint64_t parse_u64(const char *text, const char *option) {
	const std::string value_text = text != nullptr ? text : "<null>";
	try {
		if (text == nullptr || text[0] == '\0' || text[0] == '-') {
			throw std::invalid_argument("missing or negative value");
		}
		const bool hexadecimal = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
		const char *digits = hexadecimal ? text + 2 : text;
		if (digits[0] == '\0') {
			throw std::invalid_argument("missing digits");
		}
		for (const char *cursor = digits; *cursor != '\0'; cursor++) {
			const bool decimal_digit = *cursor >= '0' && *cursor <= '9';
			const bool lower_hex_digit = *cursor >= 'a' && *cursor <= 'f';
			const bool upper_hex_digit = *cursor >= 'A' && *cursor <= 'F';
			if ((!hexadecimal && !decimal_digit) ||
					(hexadecimal && !decimal_digit && !lower_hex_digit && !upper_hex_digit)) {
				throw std::invalid_argument("invalid digit");
			}
		}
		std::size_t consumed = 0;
		const std::uint64_t value = std::stoull(text, &consumed, hexadecimal ? 16 : 10);
		if (text[consumed] != '\0') {
			throw std::invalid_argument("trailing characters");
		}
		return value;
	} catch (const std::exception &) {
		throw std::runtime_error(std::string("invalid value for ") + option + ": " + value_text);
	}
}


// Checks a lowercase full Git object identifier used as report provenance.
bool is_lower_hex_sha1(std::string_view value) {
	if (value.size() != 40) {
		return false;
	}
	return std::all_of(value.begin(), value.end(), [](char character) {
		return (character >= '0' && character <= '9') ||
				(character >= 'a' && character <= 'f');
	});
}


// Prints standalone benchmark command-line options.
void print_usage(const char *program) {
	std::cout
			<< "Usage: " << program << " [options]\n\n"
			<< "  --quick                    Development profile: 3 warmups, 7 samples, 10 ms minimum.\n"
			<< "  --self-test                Validate datasets, round-trip, determinism and invalid rejection.\n"
			<< "  --list-cpus                List active logical CPUs and native topology.\n"
			<< "  --list-datasets            List deterministic dataset names.\n"
			<< "  --only NAME                Run only one dataset.\n"
			<< "  --json PATH                Write the canonical JSON report.\n"
			<< "  --csv PATH                 Write the summary CSV report.\n"
			<< "  --warmup N                 Override warmup rounds.\n"
			<< "  --rounds N                 Override measured rounds.\n"
			<< "  --min-iterations N         Override minimum message operations per sample.\n"
			<< "  --min-sample-ms N          Override minimum sample duration in milliseconds.\n"
			<< "  --seed N                   Override the deterministic dataset seed.\n"
			<< "  -h, --help                 Show this help.\n";
}


// Parses benchmark options while rejecting unknown or incomplete arguments.
CommandLineOptions parse_options(int argc, char **argv) {
	CommandLineOptions options;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument(argv[index]);
		auto require_value = [&](const char *option) -> const char * {
			if (index + 1 >= argc) {
				throw std::runtime_error(std::string(option) + " requires a value");
			}
			return argv[++index];
		};
		if (argument == "--quick") {
			options.config = make_quick_benchmark_config();
		} else if (argument == "--self-test") {
			options.self_test = true;
		} else if (argument == "--list-cpus") {
			options.list_cpus = true;
		} else if (argument == "--list-datasets") {
			options.list_datasets = true;
		} else if (argument == "--only") {
			options.only_dataset = require_value("--only");
		} else if (argument == "--json" || argument == "--output-json") {
			options.json_path = require_value("--json");
		} else if (argument == "--csv" || argument == "--output-csv") {
			options.csv_path = require_value("--csv");
		} else if (argument == "--warmup") {
			const std::uint64_t value = parse_u64(require_value("--warmup"), "--warmup");
			if (value > std::numeric_limits<std::uint32_t>::max()) {
				throw std::runtime_error("--warmup exceeds uint32 range");
			}
			options.config.warmup_rounds = static_cast<std::uint32_t>(value);
		} else if (argument == "--rounds") {
			const std::uint64_t value = parse_u64(require_value("--rounds"), "--rounds");
			if (value > std::numeric_limits<std::uint32_t>::max()) {
				throw std::runtime_error("--rounds exceeds uint32 range");
			}
			options.config.measured_rounds = static_cast<std::uint32_t>(value);
		} else if (argument == "--min-iterations") {
			// Parses a strict unsigned command-line integer or terminates with context.
			options.config.minimum_iterations = parse_u64(require_value("--min-iterations"), "--min-iterations");
		} else if (argument == "--min-sample-ms") {
			const std::uint64_t value = parse_u64(require_value("--min-sample-ms"), "--min-sample-ms");
			if (value > std::numeric_limits<std::uint64_t>::max() / UINT64_C(1'000'000)) {
				throw std::runtime_error("--min-sample-ms exceeds nanosecond range");
			}
			options.config.minimum_sample_duration_ns = value * UINT64_C(1'000'000);
		} else if (argument == "--seed") {
			// Parses a strict unsigned command-line integer or terminates with context.
			options.config.random_seed = parse_u64(require_value("--seed"), "--seed");
		} else if (argument == "--cpu") {
			const std::uint64_t value = parse_u64(require_value("--cpu"), "--cpu");
			if (value > std::numeric_limits<std::uint32_t>::max()) {
				throw std::runtime_error("--cpu exceeds uint32 range");
			}
			options.logical_cpu = static_cast<std::uint32_t>(value);
		} else if (argument == "-h" || argument == "--help") {
			// Prints standalone benchmark command-line options.
			print_usage(argv[0]);
			std::exit(0);
		} else {
			throw std::runtime_error("unknown option: " + std::string(argument));
		}
	}
	if (options.config.warmup_rounds == 0 || options.config.measured_rounds == 0 ||
			options.config.minimum_iterations == 0 || options.config.minimum_sample_duration_ns == 0 ||
			options.config.random_seed == 0) {
		throw std::runtime_error("benchmark counts, durations, and seed must be greater than zero");
	}
	if (options.config.minimum_iterations > options.config.maximum_iterations) {
		throw std::runtime_error("--min-iterations exceeds the benchmark iteration limit");
	}
	return options;
}


// Returns an ISO-like UTC timestamp for report provenance.
std::string utc_timestamp() {
	const std::time_t now = std::time(nullptr);
	std::tm utc = {};
#if defined(_WIN32)
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	std::ostringstream stream;
	stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
	return stream.str();
}


// Reads one environment-provided provenance value with a fallback.
std::string environment_value(const char *name, const char *fallback = "unknown") {
	const char *value = std::getenv(name);
	return value != nullptr && value[0] != '\0' ? value : fallback;
}


// Collects compile, source, binary, platform, and CPU provenance.
BenchmarkBuildMetadata collect_build_metadata(
		const char *program_path,
		const BenchmarkAffinityResult &affinity) {
	BenchmarkBuildMetadata metadata;
	metadata.generated_utc = utc_timestamp();
	metadata.precision = ReferenceFixedWidthCandidate::wire_precision_name();
	metadata.runtime_backend = environment_value("TICKSYNC_BENCHMARK_RUNTIME_BACKEND", "native");
	metadata.device_manufacturer = environment_value("TICKSYNC_BENCHMARK_DEVICE_MANUFACTURER");
	metadata.device_model = environment_value("TICKSYNC_BENCHMARK_DEVICE_MODEL");
	metadata.os_version = environment_value("TICKSYNC_BENCHMARK_OS_VERSION");
	metadata.os_build = environment_value("TICKSYNC_BENCHMARK_OS_BUILD");
	metadata.soc_model = environment_value("TICKSYNC_BENCHMARK_SOC_MODEL");
	metadata.module_commit = TICKSYNC_BENCHMARK_MODULE_COMMIT;
	metadata.godot_commit = TICKSYNC_BENCHMARK_GODOT_COMMIT;
	metadata.source_state = TICKSYNC_BENCHMARK_SOURCE_STATE;
	metadata.compiler_command = TICKSYNC_BENCHMARK_COMPILER_COMMAND;
	metadata.compiler_path = TICKSYNC_BENCHMARK_COMPILER_PATH;
	metadata.compiler_flags = TICKSYNC_BENCHMARK_COMPILER_FLAGS;
	metadata.optimize = TICKSYNC_BENCHMARK_OPTIMIZE;
	metadata.lto = TICKSYNC_BENCHMARK_LTO;
	metadata.executable_path = environment_value("TICKSYNC_BENCHMARK_EXECUTABLE_PATH", program_path);
	metadata.binary_sha256 = environment_value("TICKSYNC_BENCHMARK_BINARY_SHA256");
	metadata.cpu_model = environment_value("TICKSYNC_BENCHMARK_CPU_MODEL");
	metadata.logical_cpu = affinity.requested ? std::to_string(affinity.requested_cpu) :
			environment_value("TICKSYNC_BENCHMARK_LOGICAL_CPU", "unbound");
	metadata.cpu_class = environment_value("TICKSYNC_BENCHMARK_CPU_CLASS", "unspecified");
	metadata.processor_group = affinity.processor_group;
	metadata.affinity_requested = affinity.requested ? "yes" : "no";
	metadata.affinity_applied = affinity.applied ? "yes" : "no";
	metadata.affinity_actual_cpu = affinity.actual_cpu;
	metadata.affinity_error = affinity.error.empty() ? "none" : affinity.error;
	metadata.cpu_core = environment_value("TICKSYNC_BENCHMARK_CPU_CORE", affinity.cpu_core.c_str());
	metadata.cpu_package = environment_value("TICKSYNC_BENCHMARK_CPU_PACKAGE", affinity.cpu_package.c_str());
	metadata.numa_node = environment_value("TICKSYNC_BENCHMARK_NUMA_NODE", affinity.numa_node.c_str());
	metadata.l3_cache_id = environment_value("TICKSYNC_BENCHMARK_L3_CACHE_ID", affinity.l3_cache_id.c_str());
	metadata.thread_siblings = environment_value(
			"TICKSYNC_BENCHMARK_THREAD_SIBLINGS",
			affinity.thread_siblings.c_str());
	metadata.scaling_driver = environment_value("TICKSYNC_BENCHMARK_SCALING_DRIVER");
	metadata.scaling_governor = environment_value("TICKSYNC_BENCHMARK_SCALING_GOVERNOR");
	metadata.cpu_min_frequency_khz = environment_value("TICKSYNC_BENCHMARK_CPU_MIN_FREQUENCY_KHZ");
	metadata.cpu_max_frequency_khz = environment_value("TICKSYNC_BENCHMARK_CPU_MAX_FREQUENCY_KHZ");
#if defined(__clang__)
	metadata.compiler = "clang";
	metadata.compiler_version = __clang_version__;
#elif defined(__GNUC__)
	metadata.compiler = "gcc";
	metadata.compiler_version = __VERSION__;
#elif defined(_MSC_VER)
	metadata.compiler = "msvc";
	metadata.compiler_version = std::to_string(_MSC_VER);
#else
	metadata.compiler = "unknown";
	metadata.compiler_version = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
	metadata.architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
	metadata.architecture = "aarch64";
#else
	metadata.architecture = "unknown";
#endif
#if defined(__ANDROID__)
	metadata.platform = "Android";
#elif defined(__linux__)
	struct utsname information = {};
	if (uname(&information) == 0) {
		metadata.platform = std::string(information.sysname) + " " + information.release;
	} else {
		metadata.platform = "Linux";
	}
#elif defined(_WIN32)
	metadata.platform = "Windows";
#elif defined(__APPLE__)
	metadata.platform = "Apple";
#else
	metadata.platform = "unknown";
#endif
	return metadata;
}


// Runs correctness gates without measuring performance.
bool run_self_test(const std::vector<BenchmarkDataset> &datasets) {
	std::uint64_t message_count = 0;
	std::vector<std::vector<std::uint8_t>> all_valid;
	for (const BenchmarkDataset &dataset : datasets) {
		if (dataset.messages.empty()) {
			std::cerr << "empty dataset: " << dataset.name << '\n';
			return false;
		}
		message_count += dataset.messages.size();
		for (const BenchmarkMessage &message : dataset.messages) {
			std::vector<std::uint8_t> first;
			std::vector<std::uint8_t> second;
			BenchmarkMessage decoded;
			if (!ReferenceFixedWidthCandidate::encode(message, first) ||
					!ReferenceFixedWidthCandidate::encode(message, second) || first != second ||
					ReferenceFixedWidthCandidate::decode(make_byte_view(first), decoded) != CandidateDecodeError::OK ||
					!ReferenceFixedWidthCandidate::equivalent_for_wire(message, decoded)) {
				std::cerr << "self-test failed in dataset: " << dataset.name << '\n';
				return false;
			}
			if (all_valid.size() < 32) {
				all_valid.push_back(std::move(first));
			}
		}
	}
	const std::vector<std::vector<std::uint8_t>> invalid = detail::make_invalid_packets(all_valid);
	BenchmarkMessage decoded;
	for (const std::vector<std::uint8_t> &packet : invalid) {
		if (ReferenceFixedWidthCandidate::decode(make_byte_view(packet), decoded) == CandidateDecodeError::OK) {
			std::cerr << "self-test accepted an invalid packet\n";
			return false;
		}
	}
	std::cout << "TICKSYNCHRONIZER_BENCHMARK_SELF_TEST_OK suite="
			<< version::BENCHMARK_SUITE_VERSION
			<< " candidate=" << ReferenceFixedWidthCandidate::info().name
			<< " precision=" << ReferenceFixedWidthCandidate::wire_precision_name()
			<< " datasets=" << datasets.size()
			<< " messages=" << message_count
			<< " invalid=" << invalid.size() << '\n';
	return true;
}


// Prints one stable, tab-separated topology row per active logical CPU.
void print_cpu_topology() {
	const std::vector<BenchmarkLogicalCpuInfo> cpus = list_benchmark_logical_cpus();
	if (cpus.empty()) {
		throw std::runtime_error("native logical CPU topology is unavailable");
	}
	std::cout << "logical_cpu\tprocessor_group\tprocessor_number\tcore\tpackage\tnuma"
			  "\tl3_id\tl3_size\tthread_siblings\n";
	for (const BenchmarkLogicalCpuInfo &cpu : cpus) {
		std::cout << cpu.logical_cpu << '\t'
				<< cpu.processor_group << '\t'
				<< cpu.processor_number << '\t'
				<< cpu.cpu_core << '\t'
				<< cpu.cpu_package << '\t'
				<< cpu.numa_node << '\t'
				<< cpu.l3_cache_id << '\t'
				<< cpu.l3_cache_size << '\t'
				<< cpu.thread_siblings << '\n';
	}
}


// Prints a compact human-readable summary after report generation.
void print_console_summary(const ProtocolBenchmarkReport &report) {
	std::cout << "TickSynchronizer protocol benchmark suite " << report.benchmark_suite_version
			<< " — " << report.candidate_name << " — precision=" << report.build.precision << '\n';
	std::cout << std::left << std::setw(22) << "dataset"
			<< std::right << std::setw(12) << "bytes"
			<< std::setw(16) << "encode ns"
			<< std::setw(16) << "decode ns"
			<< std::setw(14) << "enc MiB/s"
			<< std::setw(14) << "dec MiB/s" << '\n';
	for (const DatasetBenchmarkResult &dataset : report.datasets) {
		std::cout << std::left << std::setw(22) << dataset.name
				<< std::right << std::setw(12) << std::fixed << std::setprecision(1) << dataset.size.bytes_per_message.median
				<< std::setw(16) << dataset.encode.nanoseconds_per_message.median
				<< std::setw(16) << dataset.decode.nanoseconds_per_message.median
				<< std::setw(14) << dataset.encode.mebibytes_per_second.median
				<< std::setw(14) << dataset.decode.mebibytes_per_second.median << '\n';
	}
	std::cout << "invalid packets: rejected=" << report.invalid_packets.rejected
			<< " accepted=" << report.invalid_packets.accepted << '\n';
}

} // namespace
} // namespace tick_synchronizer::benchmarks

int main(int argc, char **argv) {
	using namespace tick_synchronizer::benchmarks;
	try {
		// Parses benchmark options while rejecting unknown or incomplete arguments.
		const CommandLineOptions options = parse_options(argc, argv);
		if (options.config.suite_version != tick_synchronizer::version::BENCHMARK_SUITE_VERSION) {
			throw std::runtime_error("benchmark suite version mismatch between config and version contract");
		}
		if (options.list_cpus) {
			// Prints one stable, tab-separated topology row per active logical CPU.
			print_cpu_topology();
			return 0;
		}
		std::vector<BenchmarkDataset> datasets = make_protocol_benchmark_datasets(options.config.random_seed);
		if (options.list_datasets) {
			for (const BenchmarkDataset &dataset : datasets) {
				std::cout << dataset.name << "\t" << dataset.description << '\n';
			}
			return 0;
		}
		if (!options.only_dataset.empty()) {
			const BenchmarkDataset *selected = find_benchmark_dataset(datasets, options.only_dataset);
			if (selected == nullptr) {
				throw std::runtime_error("unknown dataset: " + options.only_dataset);
			}
			datasets = { *selected };
		}
		if (options.self_test) {
			// Runs correctness gates without measuring performance.
			return run_self_test(datasets) ? 0 : 1;
		}

		BenchmarkAffinityResult affinity;
		if (options.logical_cpu.has_value()) {
			affinity = apply_benchmark_thread_affinity(*options.logical_cpu);
			if (!affinity.applied) {
				throw std::runtime_error("failed to apply CPU affinity: " + affinity.error);
			}
		}

		const ProtocolCandidateInfo candidate = ReferenceFixedWidthCandidate::info();
		ProtocolBenchmarkReport report;
		report.benchmark_suite_version = tick_synchronizer::version::BENCHMARK_SUITE_VERSION;
		report.api_version = tick_synchronizer::version::API_VERSION;
		report.wire_protocol_version = tick_synchronizer::version::WIRE_PROTOCOL_VERSION;
		report.wire_protocol_revision = tick_synchronizer::version::WIRE_PROTOCOL_REVISION;
		report.candidate_id = candidate.id;
		report.candidate_name = std::string(candidate.name);
		report.candidate_description = std::string(candidate.description);
		// Collects compile, source, binary, platform, and CPU provenance.
		report.build = collect_build_metadata(argv[0], affinity);
		report.config = options.config;
		report.official_eligible = is_official_benchmark_config(report.config) &&
				options.only_dataset.empty() &&
				report.build.source_state == "clean" &&
				is_lower_hex_sha1(report.build.module_commit) &&
				report.build.godot_commit == QUALIFICATION_GODOT_COMMIT &&
				report.build.affinity_requested == "yes" &&
				report.build.affinity_applied == "yes" &&
				report.build.affinity_actual_cpu != "unbound" &&
				report.build.affinity_actual_cpu != "unknown" &&
				report.build.affinity_error == "none";
		report.datasets.reserve(datasets.size());
		for (const BenchmarkDataset &dataset : datasets) {
			DatasetBenchmarkResult result = run_dataset_benchmark<ReferenceFixedWidthCandidate>(dataset, options.config);
			if (result.integrity.round_trip_failures != 0 || result.integrity.determinism_failures != 0) {
				throw std::runtime_error("integrity gate failed for dataset: " + dataset.name);
			}
			report.datasets.push_back(std::move(result));
		}
		report.invalid_packets = run_invalid_packet_benchmark<ReferenceFixedWidthCandidate>(datasets, options.config);
		if (report.invalid_packets.accepted != 0) {
			throw std::runtime_error("candidate accepted invalid packets");
		}
		// Prints a compact human-readable summary after report generation.
		print_console_summary(report);
		if (!options.json_path.empty() && !write_json_report_file(report, options.json_path)) {
			throw std::runtime_error("failed to write JSON report: " + options.json_path);
		}
		if (!options.csv_path.empty() && !write_csv_report_file(report, options.csv_path)) {
			throw std::runtime_error("failed to write CSV report: " + options.csv_path);
		}
		std::cout << "TICKSYNCHRONIZER_PROTOCOL_BENCHMARK_OK suite="
				<< report.benchmark_suite_version
				<< " candidate=" << report.candidate_name
				<< " precision=" << report.build.precision
				<< " datasets=" << report.datasets.size() << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "ERROR: " << error.what() << '\n';
		return 1;
	}
}

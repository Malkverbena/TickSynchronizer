// Provides the standalone protocol benchmark command-line executable.
// Collects provenance, runs self-tests and measurements, and writes reports.

#include "benchmark_config.h"
#include "benchmark_dataset.h"
#include "benchmark_result.h"
#include "benchmark_result_writer.h"
#include "benchmark_platform.h"
#include "benchmark_runner.h"
#include "candidates/reference_fixed_width_candidate.h"
#include "candidates/varint_zigzag_fixed_float_candidate.h"
#include "src/internal/tick_synchronizer_version.h"

#include "benchmark_build_info.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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
	bool list_candidates = false;
	bool help = false;
	std::string json_path;
	std::string csv_path;
	std::string only_dataset;
	std::string candidate_name = "reference_fixed_width";
	std::optional<std::uint32_t> logical_cpu;
};

// Parses one strict unsigned command-line integer with overflow detection.
bool parse_u64(
		const char *p_text,
		const char *p_option,
		std::uint64_t &r_value,
		std::string &r_error) {
	if (p_text == nullptr || p_text[0] == '\0' || p_text[0] == '-') {
		r_error = std::string("invalid value for ") + p_option + ": " +
				(p_text != nullptr ? p_text : "<null>");
		return false;
	}
	const bool hexadecimal = p_text[0] == '0' && (p_text[1] == 'x' || p_text[1] == 'X');
	const char *digits = hexadecimal ? p_text + 2 : p_text;
	if (digits[0] == '\0') {
		r_error = std::string("invalid value for ") + p_option + ": " + p_text;
		return false;
	}

	const std::uint64_t base = hexadecimal ? 16 : 10;
	std::uint64_t value = 0;
	for (const char *cursor = digits; *cursor != '\0'; cursor++) {
		std::uint64_t digit = 0;
		if (*cursor >= '0' && *cursor <= '9') {
			digit = static_cast<std::uint64_t>(*cursor - '0');
		} else if (hexadecimal && *cursor >= 'a' && *cursor <= 'f') {
			digit = static_cast<std::uint64_t>(*cursor - 'a' + 10);
		} else if (hexadecimal && *cursor >= 'A' && *cursor <= 'F') {
			digit = static_cast<std::uint64_t>(*cursor - 'A' + 10);
		} else {
			r_error = std::string("invalid value for ") + p_option + ": " + p_text;
			return false;
		}
		if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / base) {
			r_error = std::string("value exceeds uint64 range for ") + p_option + ": " + p_text;
			return false;
		}
		value = value * base + digit;
	}
	r_value = value;
	return true;
}


// Advances to and parses one required unsigned option value.
bool parse_required_u64(
		int p_argument_count,
		char **p_arguments,
		int &r_index,
		const char *p_option,
		std::uint64_t &r_value,
		std::string &r_error) {
	if (r_index + 1 >= p_argument_count) {
		r_error = std::string(p_option) + " requires a value";
		return false;
	}
	r_index++;
	return parse_u64(p_arguments[r_index], p_option, r_value, r_error);
}


// Advances to one required string option value.
bool read_required_value(
		int p_argument_count,
		char **p_arguments,
		int &r_index,
		const char *p_option,
		const char *&r_value,
		std::string &r_error) {
	if (r_index + 1 >= p_argument_count) {
		r_error = std::string(p_option) + " requires a value";
		return false;
	}
	r_value = p_arguments[++r_index];
	return true;
}


// Checks a lowercase full Git object identifier used as report provenance.
bool is_lower_hex_sha1(std::string_view value) {
	if (value.size() != 40) {
		return false;
	}
	for (char character : value) {
		if (!((character >= '0' && character <= '9') ||
				(character >= 'a' && character <= 'f'))) {
			return false;
		}
	}
	return true;
}


// Accepts either verified hard affinity or the exact macOS scheduler policy.
bool has_official_cpu_execution_policy(const BenchmarkBuildMetadata &build) {
#if defined(__APPLE__)
	const bool scheduler_managed_macos =
			build.runtime_backend == "macos-native" &&
			build.logical_cpu == "unbound" &&
			build.cpu_class == "representative" &&
			build.processor_group == "unsupported" &&
			build.affinity_requested == "no" &&
			build.affinity_applied == "no" &&
			build.affinity_actual_cpu == "unknown" &&
			build.affinity_error == "unsupported-by-platform-policy";
	return scheduler_managed_macos;
#else
	const bool verified_hard_affinity =
			build.logical_cpu != "unbound" &&
			build.logical_cpu != "unknown" &&
			build.affinity_requested == "yes" &&
			build.affinity_applied == "yes" &&
			build.affinity_actual_cpu != "unbound" &&
			build.affinity_actual_cpu != "unknown" &&
			build.affinity_error == "none";
	return verified_hard_affinity;
#endif
}


// Prints standalone benchmark command-line options.
void print_usage(const char *program) {
	std::cout
			<< "Usage: " << program << " [options]\n\n"
			<< "  --quick                    Development profile: 3 warmups, 7 samples, 10 ms minimum.\n"
			<< "  --self-test                Validate datasets, round-trip, determinism and invalid rejection.\n"
			<< "  --list-cpus                List active logical CPUs and native topology.\n"
			<< "  --list-datasets            List deterministic dataset names.\n"
			<< "  --list-candidates          List compiled protocol candidates.\n"
			<< "  --candidate NAME           Select one candidate; default: reference_fixed_width.\n"
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
bool parse_options(
		int argc,
		char **argv,
		CommandLineOptions &r_options,
		std::string &r_error) {
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument(argv[index]);
		if (argument == "--quick") {
			r_options.config = make_quick_benchmark_config();
		} else if (argument == "--self-test") {
			r_options.self_test = true;
		} else if (argument == "--list-cpus") {
			r_options.list_cpus = true;
		} else if (argument == "--list-datasets") {
			r_options.list_datasets = true;
		} else if (argument == "--list-candidates") {
			r_options.list_candidates = true;
		} else if (argument == "--candidate") {
			const char *value = nullptr;
			if (!read_required_value(argc, argv, index, "--candidate", value, r_error)) {
				return false;
			}
			r_options.candidate_name = value;
		} else if (argument == "--only") {
			const char *value = nullptr;
			if (!read_required_value(argc, argv, index, "--only", value, r_error)) {
				return false;
			}
			r_options.only_dataset = value;
		} else if (argument == "--json" || argument == "--output-json") {
			const char *value = nullptr;
			if (!read_required_value(argc, argv, index, "--json", value, r_error)) {
				return false;
			}
			r_options.json_path = value;
		} else if (argument == "--csv" || argument == "--output-csv") {
			const char *value = nullptr;
			if (!read_required_value(argc, argv, index, "--csv", value, r_error)) {
				return false;
			}
			r_options.csv_path = value;
		} else if (argument == "--warmup") {
			std::uint64_t value = 0;
			if (!parse_required_u64(argc, argv, index, "--warmup", value, r_error)) {
				return false;
			}
			if (value > std::numeric_limits<std::uint32_t>::max()) {
				r_error = "--warmup exceeds uint32 range";
				return false;
			}
			r_options.config.warmup_rounds = static_cast<std::uint32_t>(value);
		} else if (argument == "--rounds") {
			std::uint64_t value = 0;
			if (!parse_required_u64(argc, argv, index, "--rounds", value, r_error)) {
				return false;
			}
			if (value > std::numeric_limits<std::uint32_t>::max()) {
				r_error = "--rounds exceeds uint32 range";
				return false;
			}
			r_options.config.measured_rounds = static_cast<std::uint32_t>(value);
		} else if (argument == "--min-iterations") {
			if (!parse_required_u64(
					argc, argv, index, "--min-iterations",
					r_options.config.minimum_iterations, r_error)) {
				return false;
			}
		} else if (argument == "--min-sample-ms") {
			std::uint64_t value = 0;
			if (!parse_required_u64(argc, argv, index, "--min-sample-ms", value, r_error)) {
				return false;
			}
			if (value > std::numeric_limits<std::uint64_t>::max() / UINT64_C(1'000'000)) {
				r_error = "--min-sample-ms exceeds nanosecond range";
				return false;
			}
			r_options.config.minimum_sample_duration_ns = value * UINT64_C(1'000'000);
		} else if (argument == "--seed") {
			if (!parse_required_u64(
					argc, argv, index, "--seed",
					r_options.config.random_seed, r_error)) {
				return false;
			}
		} else if (argument == "--cpu") {
			std::uint64_t value = 0;
			if (!parse_required_u64(argc, argv, index, "--cpu", value, r_error)) {
				return false;
			}
			if (value > std::numeric_limits<std::uint32_t>::max()) {
				r_error = "--cpu exceeds uint32 range";
				return false;
			}
			r_options.logical_cpu = static_cast<std::uint32_t>(value);
		} else if (argument == "-h" || argument == "--help") {
			r_options.help = true;
		} else {
			r_error = "unknown option: " + std::string(argument);
			return false;
		}
	}
	if (r_options.config.warmup_rounds == 0 || r_options.config.measured_rounds == 0 ||
			r_options.config.minimum_iterations == 0 || r_options.config.minimum_sample_duration_ns == 0 ||
			r_options.config.random_seed == 0) {
		r_error = "benchmark counts, durations, and seed must be greater than zero";
		return false;
	}
	if (r_options.config.minimum_iterations > r_options.config.maximum_iterations) {
		r_error = "--min-iterations exceeds the benchmark iteration limit";
		return false;
	}
	return true;
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
		const BenchmarkAffinityResult &affinity,
		const char *precision) {
	BenchmarkBuildMetadata metadata;
	metadata.generated_utc = utc_timestamp();
	metadata.precision = precision;
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
	metadata.platform = "macOS";
#else
	metadata.platform = "unknown";
#endif
	return metadata;
}


template <typename Candidate>
// Runs correctness, canonical-size, bounds, and atomic-failure gates.
bool run_self_test(const std::vector<BenchmarkDataset> &datasets) {
	if (!benchmark_wire_scalar_contract_self_test()) {
		std::cerr << "self-test failed selected-precision scalar contract\n";
		return false;
	}
	std::uint64_t message_count = 0;
	std::vector<std::vector<std::uint8_t>> all_valid;
	for (const BenchmarkDataset &dataset : datasets) {
		if (dataset.messages.empty()) {
			std::cerr << "empty dataset: " << dataset.name << '\n';
			return false;
		}
		message_count += dataset.messages.size();
		bool retained_dataset_packet = false;
		for (const BenchmarkMessage &message : dataset.messages) {
			std::vector<std::uint8_t> first;
			std::vector<std::uint8_t> second;
			BenchmarkMessage decoded;
			if (!Candidate::encode(message, first) ||
					!Candidate::encode(message, second) || first != second ||
					first.size() != Candidate::estimate_encoded_size(message) ||
					Candidate::decode(make_byte_view(first), decoded) != CandidateDecodeError::OK ||
					!Candidate::equivalent_for_wire(message, decoded) ||
					Candidate::semantic_hash_for_wire(message) != Candidate::semantic_hash_for_wire(decoded)) {
				std::cerr << "self-test failed in dataset: " << dataset.name << '\n';
				return false;
			}
			if (!retained_dataset_packet) {
				all_valid.push_back(std::move(first));
				retained_dataset_packet = true;
			}
		}
	}
	const std::vector<CandidateInvalidPacket> invalid = detail::make_invalid_packets<Candidate>(all_valid);
	const BenchmarkMessage sentinel = datasets.front().messages.front();
	for (const CandidateInvalidPacket &packet : invalid) {
		BenchmarkMessage decoded = sentinel;
		const CandidateDecodeError error = Candidate::decode(make_byte_view(packet.bytes), decoded);
		if (error != packet.expected_error) {
			std::cerr << "self-test malformed error mismatch: " << packet.name << '\n';
			return false;
		}
		if (!Candidate::equivalent_for_wire(sentinel, decoded)) {
			std::cerr << "self-test observed partial decode mutation: " << packet.name << '\n';
			return false;
		}
	}
	const std::vector<std::uint8_t> output_sentinel = { 0xA5 };
	std::vector<std::uint8_t> rejected_output = output_sentinel;
	BenchmarkMessage invalid_semantics = sentinel;
	invalid_semantics.entities.push_back(BenchmarkEntityState{});
	if (Candidate::encode(invalid_semantics, rejected_output) || rejected_output != output_sentinel) {
		std::cerr << "self-test encoded entities in a control message or mutated output\n";
		return false;
	}
	invalid_semantics = sentinel;
	invalid_semantics.blob.push_back(0x01);
	if (Candidate::encode(invalid_semantics, rejected_output) || rejected_output != output_sentinel) {
		std::cerr << "self-test encoded a blob in a control message or mutated output\n";
		return false;
	}
	invalid_semantics = sentinel;
	invalid_semantics.axes[0] = 1;
	if (Candidate::encode(invalid_semantics, rejected_output) || rejected_output != output_sentinel) {
		std::cerr << "self-test encoded axes in a control message or mutated output\n";
		return false;
	}
	invalid_semantics = BenchmarkMessage{};
	invalid_semantics.kind = BenchmarkMessageKind::SNAPSHOT;
	invalid_semantics.buttons = 1;
	if (Candidate::encode(invalid_semantics, rejected_output) || rejected_output != output_sentinel) {
		std::cerr << "self-test encoded buttons in a snapshot or mutated output\n";
		return false;
	}
	invalid_semantics.buttons = 0;
	invalid_semantics.axes[0] = 1;
	if (Candidate::encode(invalid_semantics, rejected_output) || rejected_output != output_sentinel) {
		std::cerr << "self-test encoded axes in a snapshot or mutated output\n";
		return false;
	}
	invalid_semantics = BenchmarkMessage{};
	invalid_semantics.kind = static_cast<BenchmarkMessageKind>(0xFF);
	if (Candidate::encode(invalid_semantics, rejected_output) || rejected_output != output_sentinel) {
		std::cerr << "self-test encoded an unknown kind or mutated output\n";
		return false;
	}
	BenchmarkMessage excessive_blob;
	excessive_blob.kind = BenchmarkMessageKind::PLAYER_INPUT;
	excessive_blob.blob.resize(static_cast<std::size_t>(Candidate::MAX_BLOB_SIZE) + 1U);
	if (Candidate::encode(excessive_blob, rejected_output) || rejected_output != output_sentinel) {
		std::cerr << "self-test encoded an excessive blob or mutated output\n";
		return false;
	}
	BenchmarkMessage excessive_entities;
	excessive_entities.kind = BenchmarkMessageKind::SNAPSHOT;
	excessive_entities.entities.resize(static_cast<std::size_t>(Candidate::MAX_ENTITIES) + 1U);
	if (Candidate::encode(excessive_entities, rejected_output) || rejected_output != output_sentinel) {
		std::cerr << "self-test encoded an excessive entity count or mutated output\n";
		return false;
	}
	std::cout << "TICKSYNCHRONIZER_BENCHMARK_SELF_TEST_OK suite="
			<< version::BENCHMARK_SUITE_VERSION
			<< " candidate=" << Candidate::info().name
			<< " precision=" << Candidate::wire_precision_name()
			<< " datasets=" << datasets.size()
			<< " messages=" << message_count
			<< " invalid=" << invalid.size() << '\n';
	return true;
}


// Prints one stable, tab-separated topology row per active logical CPU.
bool print_cpu_topology() {
	const std::vector<BenchmarkLogicalCpuInfo> cpus = list_benchmark_logical_cpus();
	if (cpus.empty()) {
		return false;
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
	return true;
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


// Prints one stable command failure and returns the process failure status.
int report_error(const std::string &p_error) {
	std::cerr << "ERROR: " << p_error << '\n';
	return 1;
}


template <typename Candidate>
// Executes one selected candidate and writes at most one JSON/CSV report pair.
int run_candidate_benchmark(
		const CommandLineOptions &options,
		const std::vector<BenchmarkDataset> &datasets,
		const BenchmarkAffinityResult &affinity,
		const char *program_path) {
	const ProtocolCandidateInfo candidate = Candidate::info();
	ProtocolBenchmarkReport report;
	report.benchmark_suite_version = tick_synchronizer::version::BENCHMARK_SUITE_VERSION;
	report.api_version = tick_synchronizer::version::API_VERSION;
	report.wire_protocol_version = tick_synchronizer::version::WIRE_PROTOCOL_VERSION;
	report.wire_protocol_revision = tick_synchronizer::version::WIRE_PROTOCOL_REVISION;
	report.candidate_id = candidate.id;
	report.candidate_name = std::string(candidate.name);
	report.candidate_description = std::string(candidate.description);
	// Collects compile, source, binary, platform, and CPU provenance.
	report.build = collect_build_metadata(program_path, affinity, Candidate::wire_precision_name());
	report.config = options.config;
	report.official_eligible = is_official_benchmark_config(report.config) &&
			options.only_dataset.empty() &&
			report.build.source_state == "clean" &&
			is_lower_hex_sha1(report.build.module_commit) &&
			report.build.godot_commit == QUALIFICATION_GODOT_COMMIT &&
			has_official_cpu_execution_policy(report.build);
	report.datasets.reserve(datasets.size());
	for (const BenchmarkDataset &dataset : datasets) {
		DatasetBenchmarkResult result;
		const BenchmarkRunError run_error = run_dataset_benchmark<Candidate>(
				dataset, options.config, result);
		if (run_error != BenchmarkRunError::OK) {
			return report_error(
					std::string(benchmark_run_error_message(run_error)) + ": " + dataset.name);
		}
		if (result.integrity.round_trip_failures != 0 || result.integrity.determinism_failures != 0) {
			return report_error("integrity gate failed for dataset: " + dataset.name);
		}
		report.datasets.push_back(std::move(result));
	}
	const BenchmarkRunError invalid_error = run_invalid_packet_benchmark<Candidate>(
			datasets, options.config, report.invalid_packets);
	if (invalid_error != BenchmarkRunError::OK) {
		return report_error(benchmark_run_error_message(invalid_error));
	}
	if (report.invalid_packets.accepted != 0) {
		return report_error("candidate accepted invalid packets");
	}
	// Prints a compact human-readable summary after report generation.
	print_console_summary(report);
	if (!options.json_path.empty() && !write_json_report_file(report, options.json_path)) {
		return report_error("failed to write JSON report: " + options.json_path);
	}
	if (!options.csv_path.empty() && !write_csv_report_file(report, options.csv_path)) {
		return report_error("failed to write CSV report: " + options.csv_path);
	}
	std::cout << "TICKSYNCHRONIZER_PROTOCOL_BENCHMARK_OK suite="
			<< report.benchmark_suite_version
			<< " candidate=" << report.candidate_name
			<< " precision=" << report.build.precision
			<< " datasets=" << report.datasets.size() << '\n';
	return 0;
}

} // namespace
} // namespace tick_synchronizer::benchmarks

int main(int argc, char **argv) {
	using namespace tick_synchronizer::benchmarks;
	CommandLineOptions options;
	std::string error;
	if (!parse_options(argc, argv, options, error)) {
		return report_error(error);
	}
	if (options.help) {
		// Prints standalone benchmark command-line options.
		print_usage(argv[0]);
		return 0;
	}
	if (options.config.suite_version != tick_synchronizer::version::BENCHMARK_SUITE_VERSION) {
		return report_error("benchmark suite version mismatch between config and version contract");
	}
	if (options.list_candidates) {
		const ProtocolCandidateInfo candidates[] = {
			ReferenceFixedWidthCandidate::info(),
			VarintZigZagFixedFloatCandidate::info(),
		};
		for (const ProtocolCandidateInfo &candidate : candidates) {
			std::cout << candidate.id << "\t" << candidate.name << "\t" << candidate.description << '\n';
		}
		return 0;
	}
	const std::string_view selected_candidate(options.candidate_name);
	const bool reference_selected =
			selected_candidate == ReferenceFixedWidthCandidate::info().name;
	const bool varint_selected =
			selected_candidate == VarintZigZagFixedFloatCandidate::info().name;
	if (!reference_selected && !varint_selected) {
		return report_error("unknown candidate: " + options.candidate_name);
	}
	if (options.list_cpus) {
		// Prints one stable, tab-separated topology row per active logical CPU.
		return print_cpu_topology() ? 0 : report_error("native logical CPU topology is unavailable");
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
			return report_error("unknown dataset: " + options.only_dataset);
		}
		datasets = { *selected };
	}
	if (options.self_test) {
		if (reference_selected) {
			return run_self_test<ReferenceFixedWidthCandidate>(datasets) ? 0 : 1;
		}
		return run_self_test<VarintZigZagFixedFloatCandidate>(datasets) ? 0 : 1;
	}

	BenchmarkAffinityResult affinity = make_benchmark_platform_affinity_state();
	if (options.logical_cpu.has_value()) {
		affinity = apply_benchmark_thread_affinity(*options.logical_cpu);
		if (!affinity.applied) {
			return report_error("failed to apply CPU affinity: " + affinity.error);
		}
	}

	if (reference_selected) {
		return run_candidate_benchmark<ReferenceFixedWidthCandidate>(
				options, datasets, affinity, argv[0]);
	}
	return run_candidate_benchmark<VarintZigZagFixedFloatCandidate>(
			options, datasets, affinity, argv[0]);
}

// Serializes benchmark reports to stable JSON and CSV formats.
// Keeps report output separate from measurement and candidate logic.

#include "benchmark_result_writer.h"

#include <fstream>
#include <iomanip>
#include <ostream>
#include <string>

namespace tick_synchronizer::benchmarks {
namespace {

// Escapes report strings without relying on an external JSON library.
std::string json_escape(const std::string &value) {
	std::string result;
	result.reserve(value.size());
	for (const char character : value) {
		switch (character) {
			case '"': result += "\\\""; break;
			case '\\': result += "\\\\"; break;
			case '\b': result += "\\b"; break;
			case '\f': result += "\\f"; break;
			case '\n': result += "\\n"; break;
			case '\r': result += "\\r"; break;
			case '\t': result += "\\t"; break;
			default:
				if (static_cast<unsigned char>(character) < 0x20U) {
					result += "?";
				} else {
					result += character;
				}
		}
	}
	return result;
}


// Quotes one CSV field when it contains separators, quotes, or line breaks.
std::string csv_escape(const std::string &value) {
	if (value.find_first_of(",\"\r\n") == std::string::npos) {
		return value;
	}
	std::string result = "\"";
	for (const char character : value) {
		if (character == '\"') {
			result += "\"\"";
		} else {
			result += character;
		}
	}
	result += "\"";
	return result;
}


void write_distribution_json(
		std::ostream &output,
		const DistributionStatistics &statistics,
		unsigned indent) {
	const std::string padding(indent, ' ');
	const std::string inner(indent + 2, ' ');
	output << "{\n"
			<< inner << "\"minimum\": " << statistics.minimum << ",\n"
			<< inner << "\"median\": " << statistics.median << ",\n"
			<< inner << "\"p95\": " << statistics.p95 << ",\n"
			<< inner << "\"maximum\": " << statistics.maximum << ",\n"
			<< inner << "\"mean\": " << statistics.mean << ",\n"
			<< inner << "\"mad\": " << statistics.median_absolute_deviation << ",\n"
			<< inner << "\"sample_count\": " << statistics.sample_count << "\n"
			<< padding << "}";
}


void write_operation_json(
		std::ostream &output,
		const OperationBenchmarkResult &operation,
		unsigned indent) {
	const std::string padding(indent, ' ');
	const std::string inner(indent + 2, ' ');
	output << "{\n";
	output << inner << "\"calibrated_iterations\": " << operation.calibrated_iterations << ",\n";
	output << inner << "\"checksum\": " << operation.checksum << ",\n";
	output << inner << "\"nanoseconds_per_message\": ";
	write_distribution_json(output, operation.nanoseconds_per_message, indent + 2);
	output << ",\n" << inner << "\"messages_per_second\": ";
	write_distribution_json(output, operation.messages_per_second, indent + 2);
	output << ",\n" << inner << "\"mebibytes_per_second\": ";
	write_distribution_json(output, operation.mebibytes_per_second, indent + 2);
	output << ",\n" << inner << "\"allocations_per_message\": ";
	write_distribution_json(output, operation.allocations_per_message, indent + 2);
	output << ",\n" << inner << "\"allocated_bytes_per_message\": ";
	write_distribution_json(output, operation.allocated_bytes_per_message, indent + 2);
	output << "\n" << padding << "}";
}


void write_csv_row(
		std::ostream &output,
		const ProtocolBenchmarkReport &report,
		const DatasetBenchmarkResult &dataset,
		const char *operation_name,
		const OperationBenchmarkResult &operation) {
	output << report.benchmark_suite_version << ','
			<< report.candidate_id << ','
			<< csv_escape(report.candidate_name) << ','
			<< report.build.precision << ','
			<< csv_escape(report.build.platform) << ','
			<< csv_escape(report.build.architecture) << ','
			<< csv_escape(report.build.runtime_backend) << ','
			<< csv_escape(report.build.device_model) << ','
			<< csv_escape(report.build.cpu_class) << ','
			<< csv_escape(report.build.logical_cpu) << ','
			<< csv_escape(dataset.name) << ','
			<< csv_escape(operation_name) << ','
			<< dataset.source_message_count << ','
			<< dataset.size.bytes_per_message.median << ','
			<< operation.nanoseconds_per_message.median << ','
			<< operation.nanoseconds_per_message.p95 << ','
			<< operation.messages_per_second.median << ','
			<< operation.mebibytes_per_second.median << ','
			<< operation.allocations_per_message.median << ','
			<< operation.allocated_bytes_per_message.median << ','
			<< dataset.integrity.round_trip_failures << ','
			<< dataset.integrity.determinism_failures << '\n';
}

} // namespace

void write_json_report(const ProtocolBenchmarkReport &report, std::ostream &output) {
	output << std::setprecision(17);
	output << "{\n"
			<< "  \"schema_version\": " << report.schema_version << ",\n"
			<< "  \"benchmark_suite_version\": " << report.benchmark_suite_version << ",\n"
			<< "  \"api_version\": " << report.api_version << ",\n"
			<< "  \"wire_protocol_version\": " << report.wire_protocol_version << ",\n"
			<< "  \"wire_protocol_revision\": " << report.wire_protocol_revision << ",\n"
			<< "  \"candidate\": {\n"
			<< "    \"id\": " << report.candidate_id << ",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"name\": \"" << json_escape(report.candidate_name) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"description\": \"" << json_escape(report.candidate_description) << "\"\n"
			<< "  },\n"
			<< "  \"build\": {\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"generated_utc\": \"" << json_escape(report.build.generated_utc) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"platform\": \"" << json_escape(report.build.platform) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"architecture\": \"" << json_escape(report.build.architecture) << "\",\n"
			<< "    \"runtime_backend\": \"" << json_escape(report.build.runtime_backend) << "\",\n"
			<< "    \"device_manufacturer\": \"" << json_escape(report.build.device_manufacturer) << "\",\n"
			<< "    \"device_model\": \"" << json_escape(report.build.device_model) << "\",\n"
			<< "    \"os_version\": \"" << json_escape(report.build.os_version) << "\",\n"
			<< "    \"os_build\": \"" << json_escape(report.build.os_build) << "\",\n"
			<< "    \"soc_model\": \"" << json_escape(report.build.soc_model) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"compiler\": \"" << json_escape(report.build.compiler) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"compiler_version\": \"" << json_escape(report.build.compiler_version) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"compiler_command\": \"" << json_escape(report.build.compiler_command) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"compiler_path\": \"" << json_escape(report.build.compiler_path) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"compiler_flags\": \"" << json_escape(report.build.compiler_flags) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"optimize\": \"" << json_escape(report.build.optimize) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"lto\": \"" << json_escape(report.build.lto) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"precision\": \"" << json_escape(report.build.precision) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"module_commit\": \"" << json_escape(report.build.module_commit) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"godot_commit\": \"" << json_escape(report.build.godot_commit) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"source_state\": \"" << json_escape(report.build.source_state) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"executable_path\": \"" << json_escape(report.build.executable_path) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"binary_sha256\": \"" << json_escape(report.build.binary_sha256) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"cpu_model\": \"" << json_escape(report.build.cpu_model) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"logical_cpu\": \"" << json_escape(report.build.logical_cpu) << "\",\n"
			<< "    \"cpu_class\": \"" << json_escape(report.build.cpu_class) << "\",\n"
			<< "    \"processor_group\": \"" << json_escape(report.build.processor_group) << "\",\n"
			<< "    \"affinity_requested\": \"" << json_escape(report.build.affinity_requested) << "\",\n"
			<< "    \"affinity_applied\": \"" << json_escape(report.build.affinity_applied) << "\",\n"
			<< "    \"affinity_actual_cpu\": \"" << json_escape(report.build.affinity_actual_cpu) << "\",\n"
			<< "    \"affinity_error\": \"" << json_escape(report.build.affinity_error) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"cpu_core\": \"" << json_escape(report.build.cpu_core) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"cpu_package\": \"" << json_escape(report.build.cpu_package) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"numa_node\": \"" << json_escape(report.build.numa_node) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"l3_cache_id\": \"" << json_escape(report.build.l3_cache_id) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"thread_siblings\": \"" << json_escape(report.build.thread_siblings) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"scaling_driver\": \"" << json_escape(report.build.scaling_driver) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"scaling_governor\": \"" << json_escape(report.build.scaling_governor) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"cpu_min_frequency_khz\": \"" << json_escape(report.build.cpu_min_frequency_khz) << "\",\n"
			// Escapes report strings without relying on an external JSON library.
			<< "    \"cpu_max_frequency_khz\": \"" << json_escape(report.build.cpu_max_frequency_khz) << "\"\n"
			<< "  },\n"
			<< "  \"official_eligible\": " << (report.official_eligible ? "true" : "false") << ",\n"
			<< "  \"config\": {\n"
			<< "    \"warmup_rounds\": " << report.config.warmup_rounds << ",\n"
			<< "    \"measured_rounds\": " << report.config.measured_rounds << ",\n"
			<< "    \"minimum_iterations\": " << report.config.minimum_iterations << ",\n"
			<< "    \"minimum_sample_duration_ns\": " << report.config.minimum_sample_duration_ns << ",\n"
			<< "    \"maximum_iterations\": " << report.config.maximum_iterations << ",\n"
			<< "    \"random_seed\": " << report.config.random_seed << ",\n"
			<< "    \"quick_mode\": " << (report.config.quick_mode ? "true" : "false") << "\n"
			<< "  },\n"
			<< "  \"datasets\": [\n";
	for (std::size_t index = 0; index < report.datasets.size(); ++index) {
		const DatasetBenchmarkResult &dataset = report.datasets[index];
		output << "    {\n"
				// Escapes report strings without relying on an external JSON library.
				<< "      \"name\": \"" << json_escape(dataset.name) << "\",\n"
				// Escapes report strings without relying on an external JSON library.
				<< "      \"description\": \"" << json_escape(dataset.description) << "\",\n"
				<< "      \"source_message_count\": " << dataset.source_message_count << ",\n"
				<< "      \"size\": {\n"
				<< "        \"total_bytes\": " << dataset.size.total_bytes << ",\n"
				<< "        \"bytes_per_message\": ";
		write_distribution_json(output, dataset.size.bytes_per_message, 8);
		output << "\n      },\n"
				<< "      \"integrity\": {\n"
				<< "        \"encoded_hash\": " << dataset.integrity.encoded_hash << ",\n"
				<< "        \"semantic_hash\": " << dataset.integrity.semantic_hash << ",\n"
				<< "        \"round_trip_failures\": " << dataset.integrity.round_trip_failures << ",\n"
				<< "        \"determinism_failures\": " << dataset.integrity.determinism_failures << "\n"
				<< "      },\n"
				<< "      \"encode\": ";
		write_operation_json(output, dataset.encode, 6);
		output << ",\n      \"decode\": ";
		write_operation_json(output, dataset.decode, 6);
		output << "\n    }" << (index + 1 == report.datasets.size() ? "\n" : ",\n");
	}
	output << "  ],\n"
			<< "  \"invalid_packets\": {\n"
			<< "    \"packet_count\": " << report.invalid_packets.packet_count << ",\n"
			<< "    \"rejected\": " << report.invalid_packets.rejected << ",\n"
			<< "    \"accepted\": " << report.invalid_packets.accepted << ",\n"
			<< "    \"decode\": ";
	write_operation_json(output, report.invalid_packets.decode, 4);
	output << "\n  }\n}\n";
}


void write_csv_report(const ProtocolBenchmarkReport &report, std::ostream &output) {
	output << std::setprecision(17);
	output << "benchmark_suite_version,candidate_id,candidate_name,precision,platform,architecture,"
			  "runtime_backend,device_model,cpu_class,logical_cpu,dataset,operation,"
			  "source_message_count,median_bytes_per_message,median_ns_per_message,p95_ns_per_message,"
			  "median_messages_per_second,median_mib_per_second,median_allocations_per_message,"
			  "median_allocated_bytes_per_message,round_trip_failures,determinism_failures\n";
	for (const DatasetBenchmarkResult &dataset : report.datasets) {
		write_csv_row(output, report, dataset, "encode", dataset.encode);
		write_csv_row(output, report, dataset, "decode", dataset.decode);
	}
}


bool write_json_report_file(const ProtocolBenchmarkReport &report, const std::string &path) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		return false;
	}
	write_json_report(report, file);
	return static_cast<bool>(file);
}


bool write_csv_report_file(const ProtocolBenchmarkReport &report, const std::string &path) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		return false;
	}
	write_csv_report(report, file);
	return static_cast<bool>(file);
}

} // namespace tick_synchronizer::benchmarks

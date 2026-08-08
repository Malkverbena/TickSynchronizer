// Implements CPU affinity for Linux, Android, and Windows benchmark hosts.
// Verifies the selected processor before any measured benchmark operation begins.

#include "benchmark_platform.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#include <unistd.h>
#endif

namespace tick_synchronizer::benchmarks {
namespace {

#if defined(_WIN32)

template <typename Visitor>
bool visit_windows_relationships(
		LOGICAL_PROCESSOR_RELATIONSHIP relationship,
		Visitor visitor) {
	DWORD byte_count = 0;
	if (GetLogicalProcessorInformationEx(relationship, nullptr, &byte_count) != FALSE ||
			GetLastError() != ERROR_INSUFFICIENT_BUFFER || byte_count == 0) {
		return false;
	}
	const std::size_t unit_count =
			(byte_count + sizeof(std::max_align_t) - 1U) / sizeof(std::max_align_t);
	std::vector<std::max_align_t> storage(unit_count);
	auto *buffer = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
	if (GetLogicalProcessorInformationEx(relationship, buffer, &byte_count) == FALSE) {
		return false;
	}
	std::size_t offset = 0;
	while (offset < byte_count) {
		auto *entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
				reinterpret_cast<unsigned char *>(buffer) + offset);
		if (entry->Size < sizeof(entry->Relationship) + sizeof(entry->Size) ||
				offset + entry->Size > byte_count) {
			return false;
		}
		visitor(*entry);
		offset += entry->Size;
	}
	return offset == byte_count;
}


bool affinity_contains_processor(
		const GROUP_AFFINITY &affinity,
		WORD group,
		std::uint32_t processor_number) {
	if (affinity.Group != group || processor_number >= sizeof(KAFFINITY) * 8U) {
		return false;
	}
	const KAFFINITY bit = static_cast<KAFFINITY>(1) << processor_number;
	return (affinity.Mask & bit) != 0;
}

BenchmarkLogicalCpuInfo *find_windows_cpu(
		std::vector<BenchmarkLogicalCpuInfo> &cpus,
		WORD group,
		std::uint32_t processor_number) {
	for (BenchmarkLogicalCpuInfo &cpu : cpus) {
		if (cpu.processor_group == std::to_string(group) &&
				cpu.processor_number == std::to_string(processor_number)) {
			return &cpu;
		}
	}
	return nullptr;
}


std::string format_windows_processor_list(
		const GROUP_AFFINITY *affinities,
		WORD affinity_count) {
	std::ostringstream stream;
	bool first = true;
	for (WORD index = 0; index < affinity_count; ++index) {
		const GROUP_AFFINITY &affinity = affinities[index];
		for (std::uint32_t bit = 0; bit < sizeof(KAFFINITY) * 8U; ++bit) {
			if (!affinity_contains_processor(affinity, affinity.Group, bit)) {
				continue;
			}
			if (!first) {
				stream << ',';
			}
			stream << affinity.Group << ':' << bit;
			first = false;
		}
	}
	return first ? "unknown" : stream.str();
}


std::string format_windows_cache_id(
		const GROUP_AFFINITY *affinities,
		WORD affinity_count) {
	std::ostringstream stream;
	for (WORD index = 0; index < affinity_count; ++index) {
		if (index != 0) {
			stream << ',';
		}
		stream << 'g' << affinities[index].Group << "-0x"
				<< std::hex << std::setw(static_cast<int>(sizeof(KAFFINITY) * 2U))
				<< std::setfill('0') << static_cast<unsigned long long>(affinities[index].Mask)
				<< std::dec;
	}
	return stream.str();
}

template <typename Callback>
void for_each_windows_processor(
		const GROUP_AFFINITY *affinities,
		WORD affinity_count,
		std::vector<BenchmarkLogicalCpuInfo> &cpus,
		Callback callback) {
	for (WORD index = 0; index < affinity_count; ++index) {
		const GROUP_AFFINITY &affinity = affinities[index];
		for (std::uint32_t bit = 0; bit < sizeof(KAFFINITY) * 8U; ++bit) {
			if (!affinity_contains_processor(affinity, affinity.Group, bit)) {
				continue;
			}
			if (BenchmarkLogicalCpuInfo *cpu = find_windows_cpu(cpus, affinity.Group, bit)) {
				callback(*cpu);
			}
		}
	}
}


std::vector<BenchmarkLogicalCpuInfo> list_windows_logical_cpus() {
	std::vector<BenchmarkLogicalCpuInfo> cpus;
	std::uint32_t flat_index = 0;
	const WORD group_count = GetActiveProcessorGroupCount();
	for (WORD group = 0; group < group_count; ++group) {
		const DWORD processor_count = GetActiveProcessorCount(group);
		if (processor_count == 0) {
			continue;
		}
		for (DWORD number = 0; number < processor_count; ++number) {
			BenchmarkLogicalCpuInfo cpu;
			cpu.logical_cpu = flat_index++;
			cpu.processor_group = std::to_string(group);
			cpu.processor_number = std::to_string(number);
			cpus.push_back(std::move(cpu));
		}
	}

	std::uint32_t core_index = 0;
	visit_windows_relationships(RelationProcessorCore, [&](const auto &entry) {
		const PROCESSOR_RELATIONSHIP &processor = entry.Processor;
		const std::string core = std::to_string(core_index++);
		const std::string siblings =
				format_windows_processor_list(processor.GroupMask, processor.GroupCount);
		for_each_windows_processor(
				processor.GroupMask,
				processor.GroupCount,
				cpus,
				[&](BenchmarkLogicalCpuInfo &cpu) {
					cpu.cpu_core = core;
					cpu.thread_siblings = siblings;
				});
	});

	std::uint32_t package_index = 0;
	visit_windows_relationships(RelationProcessorPackage, [&](const auto &entry) {
		const PROCESSOR_RELATIONSHIP &processor = entry.Processor;
		const std::string package = std::to_string(package_index++);
		for_each_windows_processor(
				processor.GroupMask,
				processor.GroupCount,
				cpus,
				[&](BenchmarkLogicalCpuInfo &cpu) { cpu.cpu_package = package; });
	});

	visit_windows_relationships(RelationNumaNode, [&](const auto &entry) {
		const NUMA_NODE_RELATIONSHIP &node = entry.NumaNode;
		const std::string node_id = std::to_string(node.NodeNumber);
		for_each_windows_processor(
				&node.GroupMask,
				1,
				cpus,
				[&](BenchmarkLogicalCpuInfo &cpu) { cpu.numa_node = node_id; });
	});

	visit_windows_relationships(RelationCache, [&](const auto &entry) {
		const CACHE_RELATIONSHIP &cache = entry.Cache;
		if (cache.Level != 3) {
			return;
		}
		// GroupMask is present in both legacy and current MinGW-w64 headers.
		const GROUP_AFFINITY *group_masks = &cache.GroupMask;
		const std::string cache_id = format_windows_cache_id(group_masks, 1);
		const std::string cache_size = std::to_string(cache.CacheSize / 1024U) + "K";
		for_each_windows_processor(
				group_masks,
				1,
				cpus,
				[&](BenchmarkLogicalCpuInfo &cpu) {
					cpu.l3_cache_id = cache_id;
					cpu.l3_cache_size = cache_size;
				});
	});
	return cpus;
}

#elif defined(__linux__) || defined(__ANDROID__)

std::string trim_text(std::string value) {
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
		value.pop_back();
	}
	std::size_t first = 0;
	while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
		++first;
	}
	return value.substr(first);
}


std::string read_linux_text_file(const std::string &path) {
	std::ifstream input(path);
	std::string value;
	if (!input || !std::getline(input, value)) {
		return "unknown";
	}
	value = trim_text(value);
	return value.empty() ? "unknown" : value;
}


std::string find_linux_numa_node(std::uint32_t logical_cpu) {
	for (std::uint32_t node = 0; node < 1024; ++node) {
		const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(logical_cpu) +
				"/node" + std::to_string(node);
		if (access(path.c_str(), F_OK) == 0) {
			return std::to_string(node);
		}
	}
	return "unknown";
}


void populate_linux_l3(BenchmarkLogicalCpuInfo &cpu, const std::string &cpu_path) {
	for (std::uint32_t index = 0; index < 16; ++index) {
		const std::string cache_path = cpu_path + "/cache/index" + std::to_string(index);
		if (read_linux_text_file(cache_path + "/level") != "3") {
			continue;
		}
		cpu.l3_cache_id = read_linux_text_file(cache_path + "/id");
		cpu.l3_cache_size = read_linux_text_file(cache_path + "/size");
		if (cpu.l3_cache_id == "unknown") {
			cpu.l3_cache_id = read_linux_text_file(cache_path + "/shared_cpu_list");
		}
		return;
	}
}


std::vector<BenchmarkLogicalCpuInfo> list_linux_logical_cpus() {
	std::vector<BenchmarkLogicalCpuInfo> cpus;
	const long configured_cpu_count = sysconf(_SC_NPROCESSORS_CONF);
	if (configured_cpu_count <= 0) {
		return cpus;
	}
	for (long number = 0; number < configured_cpu_count; ++number) {
		const std::string cpu_path = "/sys/devices/system/cpu/cpu" + std::to_string(number);
		if (access(cpu_path.c_str(), F_OK) != 0) {
			continue;
		}
		const std::string online = read_linux_text_file(cpu_path + "/online");
		if (online == "0") {
			continue;
		}
		BenchmarkLogicalCpuInfo cpu;
		cpu.logical_cpu = static_cast<std::uint32_t>(number);
		cpu.processor_group = "0";
		cpu.processor_number = std::to_string(number);
		cpu.cpu_core = read_linux_text_file(cpu_path + "/topology/core_id");
		cpu.cpu_package = read_linux_text_file(cpu_path + "/topology/physical_package_id");
		cpu.numa_node = find_linux_numa_node(cpu.logical_cpu);
		cpu.thread_siblings = read_linux_text_file(cpu_path + "/topology/thread_siblings_list");
		populate_linux_l3(cpu, cpu_path);
		cpus.push_back(std::move(cpu));
	}
	return cpus;
}


std::string errno_message(const char *operation) {
	std::ostringstream stream;
	stream << operation << " failed: " << std::strerror(errno);
	return stream.str();
}
#endif

void copy_topology_to_affinity(
		BenchmarkAffinityResult &affinity,
		const BenchmarkLogicalCpuInfo &cpu) {
	affinity.cpu_core = cpu.cpu_core;
	affinity.cpu_package = cpu.cpu_package;
	affinity.numa_node = cpu.numa_node;
	affinity.l3_cache_id = cpu.l3_cache_id;
	affinity.thread_siblings = cpu.thread_siblings;
}

} // namespace

std::vector<BenchmarkLogicalCpuInfo> list_benchmark_logical_cpus() {
#if defined(_WIN32)
	return list_windows_logical_cpus();
#elif defined(__linux__) || defined(__ANDROID__)
	return list_linux_logical_cpus();
#else
	return {};
#endif
}


std::string current_benchmark_logical_cpu() {
#if defined(_WIN32)
	PROCESSOR_NUMBER processor = {};
	GetCurrentProcessorNumberEx(&processor);
	std::ostringstream stream;
	stream << static_cast<unsigned>(processor.Group) << ':'
			<< static_cast<unsigned>(processor.Number);
	return stream.str();
#elif defined(__linux__) || defined(__ANDROID__)
	const int cpu = sched_getcpu();
	return cpu >= 0 ? std::to_string(cpu) : "unknown";
#else
	return "unknown";
#endif
}


BenchmarkAffinityResult apply_benchmark_thread_affinity(std::uint32_t logical_cpu) {
	BenchmarkAffinityResult result;
	result.requested = true;
	result.requested_cpu = logical_cpu;

#if defined(_WIN32)
	const WORD group_count = GetActiveProcessorGroupCount();
	std::uint32_t remaining = logical_cpu;
	for (WORD group = 0; group < group_count; ++group) {
		const DWORD count = GetActiveProcessorCount(group);
		if (remaining >= count) {
			remaining -= count;
			continue;
		}
		if (remaining >= sizeof(KAFFINITY) * 8U) {
			result.error = "logical CPU exceeds the affinity mask width";
			return result;
		}
		GROUP_AFFINITY affinity = {};
		affinity.Group = group;
		affinity.Mask = static_cast<KAFFINITY>(1) << remaining;
		if (SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) == 0) {
			result.error = "SetThreadGroupAffinity failed with error " + std::to_string(GetLastError());
			return result;
		}
		result.processor_group = std::to_string(group);
		result.actual_cpu = current_benchmark_logical_cpu();
		const std::string expected_cpu = std::to_string(group) + ':' + std::to_string(remaining);
		if (result.actual_cpu != expected_cpu) {
			result.error = "affinity verification expected CPU " + expected_cpu +
					" but observed " + result.actual_cpu;
			return result;
		}
		const std::vector<BenchmarkLogicalCpuInfo> topology = list_benchmark_logical_cpus();
		const auto cpu = std::find_if(topology.begin(), topology.end(), [&](const auto &entry) {
			return entry.logical_cpu == logical_cpu;
		});
		if (cpu != topology.end()) {
			copy_topology_to_affinity(result, *cpu);
		}
		result.applied = true;
		return result;
	}
	result.error = "logical CPU is outside all active Windows processor groups";
	return result;
#elif defined(__linux__) || defined(__ANDROID__)
	if (logical_cpu >= CPU_SETSIZE) {
		result.error = "logical CPU exceeds CPU_SETSIZE";
		return result;
	}
	cpu_set_t cpu_set;
	CPU_ZERO(&cpu_set);
	CPU_SET(static_cast<int>(logical_cpu), &cpu_set);
	if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0) {
		result.error = errno_message("sched_setaffinity");
		return result;
	}
	result.processor_group = "0";
	result.actual_cpu = current_benchmark_logical_cpu();
	const std::string expected_cpu = std::to_string(logical_cpu);
	if (result.actual_cpu != expected_cpu) {
		result.error = "affinity verification expected CPU " + expected_cpu +
				" but observed " + result.actual_cpu;
		return result;
	}
	const std::vector<BenchmarkLogicalCpuInfo> topology = list_benchmark_logical_cpus();
	const auto cpu = std::find_if(topology.begin(), topology.end(), [&](const auto &entry) {
		return entry.logical_cpu == logical_cpu;
	});
	if (cpu != topology.end()) {
		copy_topology_to_affinity(result, *cpu);
	}
	result.applied = true;
	return result;
#else
	result.error = "CPU affinity is not implemented for this platform";
	return result;
#endif
}

} // namespace tick_synchronizer::benchmarks

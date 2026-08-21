#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def require_number(value: object, path: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        fail(f"{path} is not numeric")
    number = float(value)
    if not math.isfinite(number) or number < 0:
        fail(f"{path} must be finite and nonnegative")
    return number


def require_text(mapping: dict, key: str, path: str) -> str:
    value = mapping.get(key)
    if not isinstance(value, str) or not value:
        fail(f"{path}.{key} missing or invalid")
    return value


def require_nonnegative_integer(value: object, path: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        fail(f"{path} must be a nonnegative integer")
    return value


def require_positive_integer(value: object, path: str) -> int:
    result = require_nonnegative_integer(value, path)
    if result == 0:
        fail(f"{path} must be greater than zero")
    return result


OFFICIAL_CONFIG = {
    "warmup_rounds": 5,
    "measured_rounds": 30,
    "minimum_iterations": 10_000,
    "minimum_sample_duration_ns": 100_000_000,
    "maximum_iterations": 100_000_000,
    "random_seed": 0x5449434B53594E43,
    "quick_mode": False,
}

EXPECTED_DATASETS = {
    "control_minimal",
    "player_input",
    "snapshot_sparse",
    "snapshot_medium",
    "snapshot_dense",
    "numeric_extremes",
    "integer_boundaries",
    "sequential_flow",
}
EXPECTED_CANDIDATES = {
    "reference_fixed_width": (
        1,
        "Canonical little-endian fixed-width reference used to validate the benchmark harness.",
    ),
    "varint_zigzag_fixed_float": (
        2,
        "Canonical ULEB128 unsigned integers and ZigZag signed integers with unchanged fixed-width floats.",
    ),
}

QUALIFICATION_GODOT_COMMIT = "a13da4feb8d8aefc283c3763d33a2f170a18d541"
HOST_SPECIFIC_PATH_PATTERNS = (
    re.compile(r"(?:^|[\s\"'])/(?:home|Users|Volumes|mnt|media|run/media)/"),
    re.compile(r"/private/var/folders/"),
    re.compile(r"(?:^|[\s\"'])[A-Za-z]:[\\/]"),
)


def contains_host_specific_path(value: str) -> bool:
    return any(pattern.search(value) is not None for pattern in HOST_SPECIFIC_PATH_PATTERNS)


def is_known_candidate(candidate: object) -> bool:
    candidate_name = candidate.get("name") if isinstance(candidate, dict) else None
    expected = EXPECTED_CANDIDATES.get(candidate_name)
    return (
        isinstance(candidate, dict)
        and expected is not None
        and candidate.get("id") == expected[0]
        and candidate.get("description") == expected[1]
    )


def verify_provenance_privacy(build: dict) -> None:
    for key in ("compiler_command", "compiler_path", "compiler_flags", "executable_path"):
        if contains_host_specific_path(build[key]):
            fail(f"build.{key} contains a host-specific path")


def is_verified_hard_affinity(build: dict) -> bool:
    return (
        build["logical_cpu"] not in ("unbound", "unknown")
        and build["affinity_requested"] == "yes"
        and build["affinity_applied"] == "yes"
        and build["affinity_actual_cpu"] not in ("unbound", "unknown")
        and build["affinity_error"] == "none"
    )


def is_scheduler_managed_macos(build: dict) -> bool:
    return (
        build["platform"] == "macOS"
        and build["runtime_backend"] == "macos-native"
        and build["logical_cpu"] == "unbound"
        and build["cpu_class"] == "representative"
        and build["processor_group"] == "unsupported"
        and build["affinity_requested"] == "no"
        and build["affinity_applied"] == "no"
        and build["affinity_actual_cpu"] == "unknown"
        and build["affinity_error"] == "unsupported-by-platform-policy"
    )


def run_self_test() -> None:
    hard_affinity = {
        "platform": "Linux",
        "runtime_backend": "linux-native",
        "logical_cpu": "2",
        "cpu_class": "test",
        "processor_group": "0",
        "affinity_requested": "yes",
        "affinity_applied": "yes",
        "affinity_actual_cpu": "2",
        "affinity_error": "none",
    }
    macos_policy = {
        "platform": "macOS",
        "runtime_backend": "macos-native",
        "logical_cpu": "unbound",
        "cpu_class": "representative",
        "processor_group": "unsupported",
        "affinity_requested": "no",
        "affinity_applied": "no",
        "affinity_actual_cpu": "unknown",
        "affinity_error": "unsupported-by-platform-policy",
    }
    if not is_verified_hard_affinity(hard_affinity):
        fail("hard-affinity policy self-test failed")
    if not is_scheduler_managed_macos(macos_policy):
        fail("macOS scheduler-managed policy self-test failed")
    invalid_macos_affinity = dict(hard_affinity)
    invalid_macos_affinity["platform"] = "macOS"
    invalid_macos_affinity["runtime_backend"] = "macos-native"
    if is_scheduler_managed_macos(invalid_macos_affinity):
        fail("macOS scheduler policy accepted a hard-affinity claim")
    if not is_known_candidate(
        {
            "id": 2,
            "name": "varint_zigzag_fixed_float",
            "description": EXPECTED_CANDIDATES["varint_zigzag_fixed_float"][1],
        }
    ):
        fail("known-candidate self-test failed")
    if is_known_candidate(
        {
            "id": 1,
            "name": "varint_zigzag_fixed_float",
            "description": EXPECTED_CANDIDATES["varint_zigzag_fixed_float"][1],
        }
    ):
        fail("candidate identity self-test accepted a mismatched ID")
    if is_known_candidate(
        {
            "id": 2,
            "name": "varint_zigzag_fixed_float",
            "description": "mismatched description",
        }
    ):
        fail("candidate identity self-test accepted a mismatched description")
    for key, invalid_value in (
        ("runtime_backend", "native"),
        ("cpu_class", "desktop"),
        ("processor_group", "unknown"),
        ("affinity_error", "none"),
    ):
        invalid_policy = dict(macos_policy)
        invalid_policy[key] = invalid_value
        if is_scheduler_managed_macos(invalid_policy):
            fail(f"macOS scheduler policy accepted invalid {key}")
    for safe_value in (
        "android-ndk/aarch64-linux-android24-clang++",
        "tick_synchronizer_protocol_benchmark.double",
        "/data/local/tmp/ticksynchronizer-benchmark/tick_synchronizer_protocol_benchmark.double",
    ):
        if contains_host_specific_path(safe_value):
            fail(f"privacy guard rejected safe provenance: {safe_value}")
    for unsafe_value in (
        "/" + "home/example/toolchain/clang++",
        "/" + "mnt/build-volume/benchmark",
        "/" + "Users/example/Library/Developer/clang++",
        "C:" + "\\Users\\example\\benchmark.exe",
    ):
        if not contains_host_specific_path(unsafe_value):
            fail(f"privacy guard accepted host-specific provenance: {unsafe_value}")
    print(
        "TICKSYNCHRONIZER_BENCHMARK_VERIFIER_SELF_TEST_OK "
        "policies=hard-affinity,macos-scheduler-managed,privacy-safe-provenance"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", nargs="?", type=Path)
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--allow-unpinned", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        if args.report is not None or args.allow_dirty or args.allow_unpinned:
            fail("--self-test cannot be combined with report validation options")
        run_self_test()
        return
    if args.report is None:
        fail("a report path is required")
    data = json.loads(args.report.read_text(encoding="utf-8"))

    expected = {
        "schema_version": 3,
        "benchmark_suite_version": 2,
        "api_version": 4,
        "wire_protocol_version": 0,
        "wire_protocol_revision": 2,
    }
    for key, value in expected.items():
        if data.get(key) != value:
            fail(f"{key}={data.get(key)!r}; expected {value}")

    candidate = data.get("candidate")
    if not is_known_candidate(candidate):
        fail("candidate missing or invalid")

    build = data.get("build")
    if not isinstance(build, dict):
        fail("build missing or invalid")
    for key in (
        "generated_utc", "platform", "architecture", "runtime_backend",
        "device_manufacturer", "device_model", "os_version", "os_build",
        "soc_model", "compiler",
        "compiler_version", "compiler_command", "compiler_path",
        "compiler_flags", "optimize", "lto", "precision",
        "module_commit", "godot_commit", "source_state",
        "executable_path", "binary_sha256", "cpu_model", "logical_cpu",
        "cpu_class", "processor_group", "affinity_requested", "affinity_applied",
        "affinity_actual_cpu", "affinity_error", "cpu_core", "cpu_package", "numa_node", "l3_cache_id",
        "thread_siblings", "scaling_driver", "scaling_governor",
        "cpu_min_frequency_khz", "cpu_max_frequency_khz",
    ):
        require_text(build, key, "build")
    verify_provenance_privacy(build)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", build["binary_sha256"]):
        fail("build.binary_sha256 must contain 64 hexadecimal digits")
    if build["source_state"] not in ("clean", "dirty"):
        fail("build.source_state must be clean or dirty")
    if build["affinity_requested"] not in ("yes", "no"):
        fail("build.affinity_requested must be yes or no")
    if build["affinity_applied"] not in ("yes", "no"):
        fail("build.affinity_applied must be yes or no")
    if build["affinity_applied"] == "yes":
        if build["affinity_requested"] != "yes":
            fail("build.affinity_applied cannot be yes when affinity was not requested")
        if build["affinity_actual_cpu"] in ("unbound", "unknown"):
            fail("build.affinity_actual_cpu must identify the verified processor")
        if build["affinity_error"] != "none":
            fail("build.affinity_error must be none when affinity was applied")

    config = data.get("config")
    if not isinstance(config, dict):
        fail("config missing or invalid")
    quick_mode = config.get("quick_mode") is True
    if not isinstance(config.get("quick_mode"), bool):
        fail("config.quick_mode must be boolean")
    for key in (
        "warmup_rounds", "measured_rounds", "minimum_iterations",
        "minimum_sample_duration_ns", "maximum_iterations", "random_seed",
    ):
        require_positive_integer(config.get(key), f"config.{key}")
    if config["minimum_iterations"] > config["maximum_iterations"]:
        fail("config.minimum_iterations exceeds config.maximum_iterations")
    official_config = all(config.get(key) == value for key, value in OFFICIAL_CONFIG.items())
    pinned = build["platform"] != "macOS" and is_verified_hard_affinity(build)
    scheduler_managed_macos = is_scheduler_managed_macos(build)
    execution_policy_valid = pinned or scheduler_managed_macos
    clean = build["source_state"] == "clean"
    provenance_valid = (
        re.fullmatch(r"[0-9a-f]{40}", build["module_commit"]) is not None
        and build["godot_commit"] == QUALIFICATION_GODOT_COMMIT
    )

    datasets = data.get("datasets")
    if not isinstance(datasets, list) or not datasets:
        fail("datasets missing")
    names: set[str] = set()
    for index, dataset in enumerate(datasets):
        if not isinstance(dataset, dict):
            fail(f"datasets[{index}] invalid")
        name = dataset.get("name")
        if not isinstance(name, str) or not name or name in names:
            fail(f"invalid or duplicate dataset name: {name!r}")
        names.add(name)
        require_positive_integer(dataset.get("source_message_count"), f"{name}.source_message_count")
        integrity = dataset.get("integrity", {})
        if integrity.get("round_trip_failures") != 0:
            fail(f"{name}: round-trip failures")
        if integrity.get("determinism_failures") != 0:
            fail(f"{name}: determinism failures")
        for hash_name in ("encoded_hash", "semantic_hash"):
            hash_value = integrity.get(hash_name)
            if not isinstance(hash_value, int) or isinstance(hash_value, bool):
                fail(f"{name}.integrity.{hash_name} must be an integer")
        require_positive_integer(dataset.get("size", {}).get("total_bytes"), f"{name}.size.total_bytes")
        require_number(dataset.get("size", {}).get("bytes_per_message", {}).get("median"), f"{name}.size.median")
        for operation_name in ("encode", "decode"):
            operation = dataset.get(operation_name, {})
            require_positive_integer(
                operation.get("calibrated_iterations"),
                f"{name}.{operation_name}.calibrated_iterations",
            )
            checksum = operation.get("checksum")
            if not isinstance(checksum, int) or isinstance(checksum, bool) or checksum == 0:
                fail(f"{name}.{operation_name}.checksum must be a nonzero integer")
            require_number(operation.get("nanoseconds_per_message", {}).get("median"), f"{name}.{operation_name}.median_ns")
            require_number(operation.get("mebibytes_per_second", {}).get("median"), f"{name}.{operation_name}.median_mib")

    invalid = data.get("invalid_packets")
    if not isinstance(invalid, dict):
        fail("invalid_packets missing or invalid")
    if require_nonnegative_integer(invalid.get("accepted"), "invalid_packets.accepted") != 0:
        fail("one or more invalid packets were accepted")
    packet_count = require_positive_integer(invalid.get("packet_count"), "invalid_packets.packet_count")
    rejected = require_positive_integer(invalid.get("rejected"), "invalid_packets.rejected")
    if rejected != packet_count:
        fail("invalid packet accounting mismatch")
    invalid_checksum = invalid.get("decode", {}).get("checksum")
    if not isinstance(invalid_checksum, int) or isinstance(invalid_checksum, bool) or invalid_checksum == 0:
        fail("invalid_packets.decode.checksum must be a nonzero integer")

    full_dataset_set = names == EXPECTED_DATASETS
    expected_eligible = (
        official_config
        and full_dataset_set
        and clean
        and provenance_valid
        and execution_policy_valid
    )
    if data.get("official_eligible") is not expected_eligible:
        fail(
            f"official_eligible={data.get('official_eligible')!r}; "
            f"expected {expected_eligible}"
        )
    if not quick_mode and not clean and not args.allow_dirty:
        fail("official run uses a dirty tree; use --allow-dirty for diagnostics only")
    if not quick_mode and not execution_policy_valid and not args.allow_unpinned:
        fail(
            "official run does not satisfy its platform CPU execution policy; "
            "use --allow-unpinned for diagnostics only"
        )

    print(
        "TICKSYNCHRONIZER_BENCHMARK_RESULT_OK "
        f"suite={data['benchmark_suite_version']} schema={data['schema_version']} "
        f"candidate={candidate['name']} datasets={len(datasets)} "
        f"official={'yes' if data['official_eligible'] else 'no'}"
    )


if __name__ == "__main__":
    main()

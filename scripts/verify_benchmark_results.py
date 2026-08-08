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
    "sequential_flow",
}

QUALIFICATION_GODOT_COMMIT = "a13da4feb8d8aefc283c3763d33a2f170a18d541"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--allow-unpinned", action="store_true")
    args = parser.parse_args()
    data = json.loads(args.report.read_text(encoding="utf-8"))

    expected = {
        "schema_version": 3,
        "benchmark_suite_version": 1,
        "api_version": 4,
        "wire_protocol_version": 0,
        "wire_protocol_revision": 2,
    }
    for key, value in expected.items():
        if data.get(key) != value:
            fail(f"{key}={data.get(key)!r}; expected {value}")

    candidate = data.get("candidate")
    if not isinstance(candidate, dict) or not candidate.get("name"):
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
    pinned = (
        build["logical_cpu"] not in ("unbound", "unknown")
        and build["affinity_requested"] == "yes"
        and build["affinity_applied"] == "yes"
        and build["affinity_actual_cpu"] not in ("unbound", "unknown")
        and build["affinity_error"] == "none"
    )
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
        integrity = dataset.get("integrity", {})
        if integrity.get("round_trip_failures") != 0:
            fail(f"{name}: round-trip failures")
        if integrity.get("determinism_failures") != 0:
            fail(f"{name}: determinism failures")
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
    require_positive_integer(invalid.get("rejected"), "invalid_packets.rejected")
    invalid_checksum = invalid.get("decode", {}).get("checksum")
    if not isinstance(invalid_checksum, int) or isinstance(invalid_checksum, bool) or invalid_checksum == 0:
        fail("invalid_packets.decode.checksum must be a nonzero integer")

    full_dataset_set = names == EXPECTED_DATASETS
    expected_eligible = official_config and full_dataset_set and clean and provenance_valid and pinned
    if data.get("official_eligible") is not expected_eligible:
        fail(
            f"official_eligible={data.get('official_eligible')!r}; "
            f"expected {expected_eligible}"
        )
    if not quick_mode and not clean and not args.allow_dirty:
        fail("official run uses a dirty tree; use --allow-dirty for diagnostics only")
    if not quick_mode and not pinned and not args.allow_unpinned:
        fail("official run is not pinned to a CPU; use --allow-unpinned for diagnostics only")

    print(
        "TICKSYNCHRONIZER_BENCHMARK_RESULT_OK "
        f"suite={data['benchmark_suite_version']} schema={data['schema_version']} "
        f"candidate={candidate['name']} datasets={len(datasets)} "
        f"official={'yes' if data['official_eligible'] else 'no'}"
    )


if __name__ == "__main__":
    main()

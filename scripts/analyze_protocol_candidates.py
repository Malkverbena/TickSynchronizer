#!/usr/bin/env python3

"""Calculates paired suite-2 protocol-candidate tradeoffs and screening gates."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
from dataclasses import dataclass
from pathlib import Path


REFERENCE_CANDIDATE = "reference_fixed_width"
VARINT_CANDIDATE = "varint_zigzag_fixed_float"
EXPECTED_CANDIDATES = {
    REFERENCE_CANDIDATE: (
        1,
        "Canonical little-endian fixed-width reference used to validate the benchmark harness.",
    ),
    VARINT_CANDIDATE: (
        2,
        "Canonical ULEB128 unsigned integers and ZigZag signed integers with unchanged fixed-width floats.",
    ),
}
EXPECTED_DATASETS = (
    "control_minimal",
    "player_input",
    "snapshot_sparse",
    "snapshot_medium",
    "snapshot_dense",
    "numeric_extremes",
    "integer_boundaries",
    "sequential_flow",
)
COVERAGE_ONLY_DATASETS = {"numeric_extremes", "integer_boundaries"}
MAX_WORKLOAD_SIZE_RATIO = 0.95
MAX_WEIGHTED_LATENCY_RATIO = 1.50
MAX_DATASET_LATENCY_RATIO = 2.00
MAX_PAIR_TIME_DELTA_SECONDS = 3_600
PAIR_BUILD_FIELDS = (
    "platform",
    "architecture",
    "runtime_backend",
    "device_manufacturer",
    "device_model",
    "os_version",
    "os_build",
    "soc_model",
    "compiler",
    "compiler_version",
    "compiler_command",
    "compiler_path",
    "compiler_flags",
    "optimize",
    "lto",
    "precision",
    "module_commit",
    "godot_commit",
    "source_state",
    "executable_path",
    "binary_sha256",
    "cpu_model",
    "logical_cpu",
    "cpu_class",
    "processor_group",
    "affinity_requested",
    "affinity_applied",
    "affinity_actual_cpu",
    "affinity_error",
    "cpu_core",
    "cpu_package",
    "numa_node",
    "l3_cache_id",
    "thread_siblings",
    "scaling_driver",
    "scaling_governor",
    "cpu_min_frequency_khz",
    "cpu_max_frequency_khz",
)


class AnalysisError(ValueError):
    """Reports an invalid report set or failed comparison contract."""


@dataclass(frozen=True)
class LoadedReport:
    """Keeps validated report content together with its source path."""

    path: Path
    data: dict


@dataclass(frozen=True)
class DatasetTradeoff:
    """Stores one paired dataset comparison."""

    name: str
    messages: int
    reference_bytes: int
    candidate_bytes: int
    size_ratio: float
    encode_ratio: float
    decode_ratio: float


@dataclass(frozen=True)
class PairTradeoff:
    """Stores one execution-identity comparison and its screening result."""

    identity: str
    datasets: tuple[DatasetTradeoff, ...]
    workload_reference_bytes: int
    workload_candidate_bytes: int
    workload_size_ratio: float
    weighted_encode_ratio: float
    weighted_decode_ratio: float
    encode_ns_per_byte_saved: float
    decode_ns_per_byte_saved: float
    qualifies: bool
    reasons: tuple[str, ...]


def require_number(value: object, location: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise AnalysisError(f"{location} must be numeric")
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise AnalysisError(f"{location} must be finite and nonnegative")
    return number


def load_report(path: Path, allow_preliminary: bool) -> LoadedReport:
    data = json.loads(path.read_text(encoding="utf-8"))
    expected_contract = {
        "schema_version": 3,
        "benchmark_suite_version": 2,
        "api_version": 4,
        "wire_protocol_version": 0,
        "wire_protocol_revision": 2,
    }
    for key, expected in expected_contract.items():
        if data.get(key) != expected:
            raise AnalysisError(f"{path}: {key}={data.get(key)!r}; expected {expected}")
    if not allow_preliminary and data.get("official_eligible") is not True:
        raise AnalysisError(f"{path}: report is preliminary; use --allow-preliminary")
    candidate = data.get("candidate")
    build = data.get("build")
    candidate_name = candidate.get("name") if isinstance(candidate, dict) else None
    expected_candidate = EXPECTED_CANDIDATES.get(candidate_name)
    if (
        not isinstance(candidate, dict)
        or expected_candidate is None
        or candidate.get("id") != expected_candidate[0]
        or candidate.get("description") != expected_candidate[1]
    ):
        raise AnalysisError(f"{path}: unsupported candidate")
    if not isinstance(build, dict) or build.get("precision") not in {"single", "double"}:
        raise AnalysisError(f"{path}: missing build identity")
    parse_generated_utc(build.get("generated_utc"), path)
    invalid = data.get("invalid_packets")
    if not isinstance(invalid, dict):
        raise AnalysisError(f"{path}: invalid-packet gate missing")
    packet_count = invalid.get("packet_count")
    rejected = invalid.get("rejected")
    accepted = invalid.get("accepted")
    if (
        not isinstance(packet_count, int)
        or isinstance(packet_count, bool)
        or packet_count <= 0
        or not isinstance(rejected, int)
        or isinstance(rejected, bool)
        or rejected != packet_count
        or accepted != 0
    ):
        raise AnalysisError(f"{path}: invalid-packet gate failed")
    dataset_map(data, path)
    return LoadedReport(path, data)


def parse_generated_utc(value: object, path: Path) -> dt.datetime:
    if not isinstance(value, str):
        raise AnalysisError(f"{path}: build.generated_utc missing")
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise AnalysisError(f"{path}: invalid build.generated_utc") from error
    if parsed.tzinfo is None:
        raise AnalysisError(f"{path}: build.generated_utc must include a timezone")
    return parsed


def dataset_map(report: dict, path: Path) -> dict[str, dict]:
    datasets = report.get("datasets")
    if not isinstance(datasets, list):
        raise AnalysisError(f"{path}: datasets missing")
    mapped: dict[str, dict] = {}
    for dataset in datasets:
        if not isinstance(dataset, dict) or not isinstance(dataset.get("name"), str):
            raise AnalysisError(f"{path}: invalid dataset entry")
        name = dataset["name"]
        if name in mapped:
            raise AnalysisError(f"{path}: duplicate dataset {name}")
        mapped[name] = dataset
    if tuple(mapped) != EXPECTED_DATASETS:
        raise AnalysisError(
            f"{path}: dataset order differs; expected {', '.join(EXPECTED_DATASETS)}"
        )
    return mapped


def pairing_key(report: LoadedReport) -> tuple[str, ...]:
    build = report.data["build"]
    fields = (
        "precision",
        "runtime_backend",
        "device_model",
        "os_build",
        "cpu_class",
        "logical_cpu",
        "affinity_actual_cpu",
        "module_commit",
        "binary_sha256",
    )
    return tuple(str(build.get(field, "unknown")) for field in fields)


def identity_text(report: LoadedReport) -> str:
    build = report.data["build"]
    return (
        f"{build.get('runtime_backend', 'unknown')} / {build.get('device_model', 'unknown')} / "
        f"{build.get('cpu_class', 'unknown')} CPU {build.get('logical_cpu', 'unknown')} / "
        f"{build.get('precision', 'unknown')}"
    )


def operation_median(dataset: dict, operation: str, path: Path) -> float:
    return require_number(
        dataset.get(operation, {}).get("nanoseconds_per_message", {}).get("median"),
        f"{path}:{dataset.get('name')}.{operation}.median",
    )


def analyze_pair(reference: LoadedReport, candidate: LoadedReport) -> PairTradeoff:
    if reference.data.get("config") != candidate.data.get("config"):
        raise AnalysisError(f"{candidate.path}: paired benchmark configuration differs")
    reference_build = reference.data["build"]
    candidate_build = candidate.data["build"]
    for field in PAIR_BUILD_FIELDS:
        if reference_build.get(field) != candidate_build.get(field):
            raise AnalysisError(f"{candidate.path}: paired build.{field} differs")
    reference_time = parse_generated_utc(reference_build.get("generated_utc"), reference.path)
    candidate_time = parse_generated_utc(candidate_build.get("generated_utc"), candidate.path)
    elapsed = abs((candidate_time - reference_time).total_seconds())
    if elapsed > MAX_PAIR_TIME_DELTA_SECONDS:
        raise AnalysisError(
            f"{candidate.path}: paired reports are {elapsed:.0f} seconds apart; "
            f"maximum is {MAX_PAIR_TIME_DELTA_SECONDS}"
        )
    reference_datasets = dataset_map(reference.data, reference.path)
    candidate_datasets = dataset_map(candidate.data, candidate.path)
    tradeoffs: list[DatasetTradeoff] = []
    workload_reference_bytes = 0
    workload_candidate_bytes = 0
    reference_encode_ns = 0.0
    candidate_encode_ns = 0.0
    reference_decode_ns = 0.0
    candidate_decode_ns = 0.0
    reasons: list[str] = []

    for name in EXPECTED_DATASETS:
        baseline = reference_datasets[name]
        contender = candidate_datasets[name]
        message_count = baseline.get("source_message_count")
        if not isinstance(message_count, int) or message_count <= 0:
            raise AnalysisError(f"{reference.path}:{name}: invalid source_message_count")
        if contender.get("source_message_count") != message_count:
            raise AnalysisError(f"{candidate.path}:{name}: source message count mismatch")
        for loaded, dataset in ((reference, baseline), (candidate, contender)):
            integrity = dataset.get("integrity", {})
            if integrity.get("round_trip_failures") != 0 or integrity.get("determinism_failures") != 0:
                raise AnalysisError(f"{loaded.path}:{name}: integrity gate failed")
        if baseline.get("integrity", {}).get("semantic_hash") != contender.get("integrity", {}).get(
            "semantic_hash"
        ):
            raise AnalysisError(f"{candidate.path}:{name}: semantic corpus hash mismatch")
        reference_bytes = baseline.get("size", {}).get("total_bytes")
        candidate_bytes = contender.get("size", {}).get("total_bytes")
        if not isinstance(reference_bytes, int) or reference_bytes <= 0:
            raise AnalysisError(f"{reference.path}:{name}: invalid total_bytes")
        if not isinstance(candidate_bytes, int) or candidate_bytes <= 0:
            raise AnalysisError(f"{candidate.path}:{name}: invalid total_bytes")
        reference_encode = operation_median(baseline, "encode", reference.path)
        candidate_encode = operation_median(contender, "encode", candidate.path)
        reference_decode = operation_median(baseline, "decode", reference.path)
        candidate_decode = operation_median(contender, "decode", candidate.path)
        tradeoff = DatasetTradeoff(
            name=name,
            messages=message_count,
            reference_bytes=reference_bytes,
            candidate_bytes=candidate_bytes,
            size_ratio=candidate_bytes / reference_bytes,
            encode_ratio=candidate_encode / reference_encode,
            decode_ratio=candidate_decode / reference_decode,
        )
        tradeoffs.append(tradeoff)
        if name not in COVERAGE_ONLY_DATASETS:
            workload_reference_bytes += reference_bytes
            workload_candidate_bytes += candidate_bytes
            reference_encode_ns += reference_encode * message_count
            candidate_encode_ns += candidate_encode * message_count
            reference_decode_ns += reference_decode * message_count
            candidate_decode_ns += candidate_decode * message_count
            if tradeoff.size_ratio > 1.0:
                reasons.append(f"{name} increases encoded workload size")
            if tradeoff.encode_ratio > MAX_DATASET_LATENCY_RATIO:
                reasons.append(f"{name} encode latency exceeds {MAX_DATASET_LATENCY_RATIO:.2f}x")
            if tradeoff.decode_ratio > MAX_DATASET_LATENCY_RATIO:
                reasons.append(f"{name} decode latency exceeds {MAX_DATASET_LATENCY_RATIO:.2f}x")

    workload_size_ratio = workload_candidate_bytes / workload_reference_bytes
    weighted_encode_ratio = candidate_encode_ns / reference_encode_ns
    weighted_decode_ratio = candidate_decode_ns / reference_decode_ns
    if workload_size_ratio > MAX_WORKLOAD_SIZE_RATIO:
        reasons.append(f"workload size ratio exceeds {MAX_WORKLOAD_SIZE_RATIO:.2f}x")
    if weighted_encode_ratio > MAX_WEIGHTED_LATENCY_RATIO:
        reasons.append(f"weighted encode ratio exceeds {MAX_WEIGHTED_LATENCY_RATIO:.2f}x")
    if weighted_decode_ratio > MAX_WEIGHTED_LATENCY_RATIO:
        reasons.append(f"weighted decode ratio exceeds {MAX_WEIGHTED_LATENCY_RATIO:.2f}x")
    bytes_saved = workload_reference_bytes - workload_candidate_bytes
    encode_ns_per_byte = (
        (candidate_encode_ns - reference_encode_ns) / bytes_saved if bytes_saved > 0 else math.inf
    )
    decode_ns_per_byte = (
        (candidate_decode_ns - reference_decode_ns) / bytes_saved if bytes_saved > 0 else math.inf
    )
    return PairTradeoff(
        identity=identity_text(reference),
        datasets=tuple(tradeoffs),
        workload_reference_bytes=workload_reference_bytes,
        workload_candidate_bytes=workload_candidate_bytes,
        workload_size_ratio=workload_size_ratio,
        weighted_encode_ratio=weighted_encode_ratio,
        weighted_decode_ratio=weighted_decode_ratio,
        encode_ns_per_byte_saved=encode_ns_per_byte,
        decode_ns_per_byte_saved=decode_ns_per_byte,
        qualifies=not reasons,
        reasons=tuple(reasons),
    )


def group_pairs(reports: list[LoadedReport]) -> list[PairTradeoff]:
    grouped: dict[tuple[str, ...], dict[str, LoadedReport]] = {}
    for report in reports:
        group = grouped.setdefault(pairing_key(report), {})
        candidate = report.data["candidate"]["name"]
        if candidate in group:
            raise AnalysisError(f"{report.path}: duplicate {candidate} report for one execution identity")
        group[candidate] = report
    pairs: list[PairTradeoff] = []
    for group in grouped.values():
        missing = {REFERENCE_CANDIDATE, VARINT_CANDIDATE} - set(group)
        if missing:
            sample = next(iter(group.values()))
            raise AnalysisError(f"{sample.path}: unpaired execution identity; missing {sorted(missing)}")
        pairs.append(analyze_pair(group[REFERENCE_CANDIDATE], group[VARINT_CANDIDATE]))
    return pairs


def ratio_text(value: float) -> str:
    return "n/a" if not math.isfinite(value) else f"{value:.3f}x"


def cost_text(value: float) -> str:
    return "n/a" if not math.isfinite(value) else f"{value:.3f} ns/B"


def geometric_mean(values: list[float]) -> float:
    return math.exp(sum(math.log(value) for value in values) / len(values))


def render(pairs: list[PairTradeoff], preliminary: bool) -> str:
    lines = [
        "# TickSynchronizer suite 2 candidate analysis",
        "",
        f"Evidence class: {'preliminary' if preliminary else 'official'} paired reports.",
        "Coverage-only datasets validate boundaries but are excluded from workload-weighted gates.",
        "Each pair has identical build identity and sampling configuration, with reports no more than one hour apart.",
        "",
        "Screening gates were fixed before reading results: at least 5% workload byte reduction, "
        "no workload dataset byte increase, no weighted encode/decode ratio above 1.50x, and no "
        "individual workload latency ratio above 2.00x. Passing screens an integer primitive for "
        "continued evaluation; it does not stabilize the production wire protocol.",
        "",
    ]
    for index, pair in enumerate(pairs, start=1):
        lines.extend(
            [
                f"## Pair {index}: {pair.identity}",
                "",
                "| Dataset | Reference bytes | Varint bytes | Size | Encode | Decode |",
                "|---|---:|---:|---:|---:|---:|",
            ]
        )
        for dataset in pair.datasets:
            suffix = " (coverage)" if dataset.name in COVERAGE_ONLY_DATASETS else ""
            lines.append(
                f"| {dataset.name}{suffix} | {dataset.reference_bytes} | {dataset.candidate_bytes} | "
                f"{ratio_text(dataset.size_ratio)} | {ratio_text(dataset.encode_ratio)} | "
                f"{ratio_text(dataset.decode_ratio)} |"
            )
        verdict = "PASS" if pair.qualifies else "FAIL"
        lines.extend(
            [
                "",
                f"Workload bytes: {pair.workload_reference_bytes} -> "
                f"{pair.workload_candidate_bytes} ({ratio_text(pair.workload_size_ratio)}).",
                f"Weighted encode/decode: {ratio_text(pair.weighted_encode_ratio)} / "
                f"{ratio_text(pair.weighted_decode_ratio)}.",
                f"Incremental CPU cost per byte saved: encode {cost_text(pair.encode_ns_per_byte_saved)}, "
                f"decode {cost_text(pair.decode_ns_per_byte_saved)}.",
                f"Screening verdict: {verdict}.",
            ]
        )
        if pair.reasons:
            lines.append("Reasons: " + "; ".join(pair.reasons) + ".")
        lines.append("")
    lines.extend(
        [
            "## Cross-pair aggregate",
            "",
            f"Pairs: {len(pairs)}; passed: {sum(pair.qualifies for pair in pairs)}; "
            f"failed: {sum(not pair.qualifies for pair in pairs)}.",
            f"Geometric-mean workload size ratio: "
            f"{ratio_text(geometric_mean([pair.workload_size_ratio for pair in pairs]))}.",
            f"Geometric-mean weighted encode ratio: "
            f"{ratio_text(geometric_mean([pair.weighted_encode_ratio for pair in pairs]))}.",
            f"Geometric-mean weighted decode ratio: "
            f"{ratio_text(geometric_mean([pair.weighted_decode_ratio for pair in pairs]))}.",
            "",
        ]
    )
    return "\n".join(lines)


def self_test() -> None:
    def make_report(candidate: str, size_factor: float, latency_factor: float) -> dict:
        datasets = []
        for name in EXPECTED_DATASETS:
            reference_bytes = 1_000
            datasets.append(
                {
                    "name": name,
                    "source_message_count": 10,
                    "size": {"total_bytes": int(reference_bytes * size_factor)},
                    "integrity": {
                        "semantic_hash": 1234,
                        "round_trip_failures": 0,
                        "determinism_failures": 0,
                    },
                    "encode": {"nanoseconds_per_message": {"median": 10 * latency_factor}},
                    "decode": {"nanoseconds_per_message": {"median": 8 * latency_factor}},
                }
            )
        return {
            "schema_version": 3,
            "benchmark_suite_version": 2,
            "api_version": 4,
            "wire_protocol_version": 0,
            "wire_protocol_revision": 2,
            "official_eligible": True,
            "candidate": {
                "id": 1 if candidate == REFERENCE_CANDIDATE else 2,
                "name": candidate,
                "description": EXPECTED_CANDIDATES[candidate][1],
            },
            "build": {
                "generated_utc": "2026-08-20T12:00:00Z",
                "precision": "double",
                "runtime_backend": "self-test",
                "device_model": "self-test",
                "os_build": "self-test",
                "cpu_class": "self-test",
                "logical_cpu": "0",
                "affinity_actual_cpu": "0",
                "module_commit": "0" * 40,
                "binary_sha256": "0" * 64,
            },
            "datasets": datasets,
            "invalid_packets": {"packet_count": 1, "rejected": 1, "accepted": 0},
        }

    reference = LoadedReport(Path("reference.json"), make_report(REFERENCE_CANDIDATE, 1.0, 1.0))
    varint = LoadedReport(Path("varint.json"), make_report(VARINT_CANDIDATE, 0.8, 1.1))
    result = analyze_pair(reference, varint)
    if not result.qualifies or not math.isclose(result.workload_size_ratio, 0.8):
        raise AnalysisError("self-test failed to accept the qualifying pair")
    failing = LoadedReport(Path("slow.json"), make_report(VARINT_CANDIDATE, 0.98, 1.6))
    if analyze_pair(reference, failing).qualifies:
        raise AnalysisError("self-test failed to reject the nonqualifying pair")
    mismatched = make_report(VARINT_CANDIDATE, 0.8, 1.1)
    mismatched["config"] = {"measured_rounds": 99}
    try:
        analyze_pair(reference, LoadedReport(Path("mismatched.json"), mismatched))
    except AnalysisError:
        pass
    else:
        raise AnalysisError("self-test accepted different paired configurations")
    print("TICKSYNCHRONIZER_PROTOCOL_ANALYZER_SELF_TEST_OK schema=3 suite=2")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("reports", nargs="*", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--allow-preliminary", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            if args.reports or args.output or args.allow_preliminary:
                raise AnalysisError("--self-test cannot be combined with report options")
            self_test()
            return
        if len(args.reports) < 2:
            raise AnalysisError("provide at least one paired reference/varint report set")
        reports = [load_report(path, args.allow_preliminary) for path in args.reports]
        pairs = group_pairs(reports)
        rendered = render(pairs, args.allow_preliminary)
        if args.output:
            args.output.write_text(rendered, encoding="utf-8")
        else:
            print(rendered, end="")
    except (AnalysisError, OSError, json.JSONDecodeError, KeyError, ZeroDivisionError) as error:
        raise SystemExit(f"ERROR: {error}") from error


if __name__ == "__main__":
    main()

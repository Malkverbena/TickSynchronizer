#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


class ComparisonError(ValueError):
    """Reports an incompatible or ambiguous comparison input."""


@dataclass(frozen=True)
class LoadedReport:
    """Keeps a report together with its source path and display identifier."""

    path: Path
    data: dict
    identifier: str


def load(path: Path, allow_preliminary: bool) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    expected_contract = {
        "schema_version": 3,
        "benchmark_suite_version": 1,
        "api_version": 4,
        "wire_protocol_version": 0,
        "wire_protocol_revision": 2,
    }
    for key, expected in expected_contract.items():
        if data.get(key) != expected:
            raise ComparisonError(f"{path}: incompatible {key}; expected {expected}")
    if not allow_preliminary and data.get("official_eligible") is not True:
        raise ComparisonError(
            f"{path}: report is not eligible for official comparison; "
            "use --allow-preliminary for diagnostics only"
        )
    build = data.get("build")
    candidate = data.get("candidate")
    if not isinstance(build, dict) or build.get("precision") not in ("single", "double"):
        raise ComparisonError(f"{path}: missing or invalid build.precision")
    if not isinstance(candidate, dict) or not candidate.get("name"):
        raise ComparisonError(f"{path}: missing or invalid candidate.name")
    metric_map(data, path)
    return data


def metric_map(report: dict, path: Path | None = None) -> dict[str, dict]:
    datasets = report.get("datasets")
    location = f"{path}: " if path is not None else ""
    if not isinstance(datasets, list) or not datasets:
        raise ComparisonError(f"{location}datasets missing or invalid")
    mapped: dict[str, dict] = {}
    for dataset in datasets:
        if not isinstance(dataset, dict) or not isinstance(dataset.get("name"), str):
            raise ComparisonError(f"{location}dataset entry missing a name")
        name = dataset["name"]
        if name in mapped:
            raise ComparisonError(f"{location}duplicate dataset: {name}")
        mapped[name] = dataset
    return mapped


def ratio(value: float, baseline: float) -> str:
    if baseline == 0:
        return "n/a"
    return f"{value / baseline:.3f}x"


def markdown_text(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def report_folder(path: Path) -> str:
    if path.name.lower() == "results.json":
        return path.parent.name or path.name
    return path.stem


def report_description(loaded: LoadedReport) -> str:
    report = loaded.data
    build = report["build"]
    candidate = report["candidate"]
    binary_hash = str(build.get("binary_sha256", "unknown"))
    if len(binary_hash) > 12:
        binary_hash = binary_hash[:12]
    return (
        f"`{markdown_text(report_folder(loaded.path))}` — "
        f"{markdown_text(candidate['name'])} / {markdown_text(build['precision'])} / "
        f"{markdown_text(build.get('runtime_backend', build.get('platform', 'unknown')))} / "
        f"{markdown_text(build.get('device_model', 'unknown'))} / "
        f"{markdown_text(build.get('os_version', 'unknown'))} build "
        f"{markdown_text(build.get('os_build', 'unknown'))} / "
        f"{markdown_text(build.get('cpu_class', 'unknown'))} CPU "
        f"{markdown_text(build.get('logical_cpu', 'unknown'))} / L3 "
        f"{markdown_text(build.get('l3_cache_id', 'unknown'))} / binary `{binary_hash}`"
    )


def group_by_precision(reports: list[LoadedReport]) -> dict[str, list[LoadedReport]]:
    grouped: dict[str, list[LoadedReport]] = defaultdict(list)
    for report in reports:
        grouped[report.data["build"]["precision"]].append(report)
    return dict(grouped)


def validate_dataset_contract(group: list[LoadedReport]) -> list[str]:
    baseline = metric_map(group[0].data, group[0].path)
    baseline_names = list(baseline)
    baseline_set = set(baseline_names)
    for loaded in group[1:]:
        names = set(metric_map(loaded.data, loaded.path))
        if names != baseline_set:
            missing = sorted(baseline_set - names)
            extra = sorted(names - baseline_set)
            raise ComparisonError(
                f"{loaded.path}: dataset set differs from the {group[0].path} baseline; "
                f"missing={missing}, extra={extra}"
            )
    return baseline_names


def render_comparison(reports: list[LoadedReport]) -> str:
    groups = group_by_precision(reports)
    lines = [
        "# TickSynchronizer protocol benchmark comparison",
        "",
        "Ratios are computed only against the first report with the same precision. "
        "Latency and size ratios below 1.0 are lower than that precision's baseline.",
        "",
    ]
    for precision in ("double", "single"):
        group = groups.get(precision)
        if not group:
            continue
        dataset_names = validate_dataset_contract(group)
        baseline = group[0]
        baseline_map = metric_map(baseline.data)
        lines.extend(
            [
                f"## {precision} precision",
                "",
                f"Baseline: {baseline.identifier} — {report_description(baseline)}",
                "",
                "| ID | Report identity |",
                "|---|---|",
            ]
        )
        for loaded in group:
            lines.append(f"| {loaded.identifier} | {report_description(loaded)} |")
        lines.extend(
            [
                "",
                "| Report | Dataset | Bytes | Encode ns | Decode ns | Size ratio | Encode ratio | Decode ratio |",
                "|---|---|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for loaded in group:
            datasets = metric_map(loaded.data)
            for name in dataset_names:
                dataset = datasets[name]
                base = baseline_map[name]
                size = dataset["size"]["bytes_per_message"]["median"]
                encode = dataset["encode"]["nanoseconds_per_message"]["median"]
                decode = dataset["decode"]["nanoseconds_per_message"]["median"]
                lines.append(
                    f"| {loaded.identifier} | {markdown_text(name)} | "
                    f"{size:.2f} | {encode:.2f} | {decode:.2f} | "
                    f"{ratio(size, base['size']['bytes_per_message']['median'])} | "
                    f"{ratio(encode, base['encode']['nanoseconds_per_message']['median'])} | "
                    f"{ratio(decode, base['decode']['nanoseconds_per_message']['median'])} |"
                )
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def self_test() -> None:
    def make_report(precision: str, os_build: str, logical_cpu: str) -> dict:
        return {
            "schema_version": 3,
            "benchmark_suite_version": 1,
            "api_version": 4,
            "wire_protocol_version": 0,
            "wire_protocol_revision": 2,
            "official_eligible": True,
            "candidate": {"name": "reference_fixed_width"},
            "build": {
                "precision": precision,
                "runtime_backend": "windows-native",
                "platform": "Windows",
                "device_model": "test-host",
                "os_version": "Windows test",
                "os_build": os_build,
                "cpu_class": "test-ccd",
                "logical_cpu": logical_cpu,
                "l3_cache_id": "l3-test",
                "binary_sha256": "0123456789abcdef",
            },
            "datasets": [
                {
                    "name": "control_minimal",
                    "size": {"bytes_per_message": {"median": 32.0}},
                    "encode": {"nanoseconds_per_message": {"median": 10.0}},
                    "decode": {"nanoseconds_per_message": {"median": 5.0}},
                }
            ],
        }

    loaded = [
        LoadedReport(Path("win11-double/results.json"), make_report("double", "11", "2"), "R1"),
        LoadedReport(Path("win10-double/results.json"), make_report("double", "10", "2"), "R2"),
        LoadedReport(Path("win11-single/results.json"), make_report("single", "11", "2"), "R3"),
        LoadedReport(Path("win10-single/results.json"), make_report("single", "10", "2"), "R4"),
    ]
    rendered = render_comparison(loaded)
    if rendered.count("Baseline:") != 2:
        raise ComparisonError("self-test did not create one baseline per precision")
    for folder in ("win11-double", "win10-double", "win11-single", "win10-single"):
        if folder not in rendered:
            raise ComparisonError(f"self-test lost report identity: {folder}")
    print("TICKSYNCHRONIZER_BENCHMARK_COMPARATOR_SELF_TEST_OK schema=3 suite=1")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("reports", nargs="*", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--allow-preliminary", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            if args.reports or args.output:
                raise ComparisonError("--self-test cannot be combined with report paths or --output")
            self_test()
            return
        if len(args.reports) < 2:
            raise ComparisonError("provide at least two reports")
        reports = [
            LoadedReport(path, load(path, args.allow_preliminary), f"R{index}")
            for index, path in enumerate(args.reports, start=1)
        ]
        output = render_comparison(reports)
        if args.output:
            args.output.write_text(output, encoding="utf-8")
        else:
            print(output, end="")
    except (ComparisonError, KeyError, TypeError, OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"ERROR: {error}") from error


if __name__ == "__main__":
    main()

#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

command -v python3 >/dev/null 2>&1 || fail "python3 not found"

python3 - "$MODULE_DIR" <<'PY'
from __future__ import annotations

import pathlib
import re
import sys
import xml.etree.ElementTree as ET

module = pathlib.Path(sys.argv[1]).resolve()
errors: list[str] = []

required_files = {
    "AGENTS.md",
    "README.md",
    "FILE_MANIFEST.txt",
    "GODOT_MINIMUM_VERSION",
    "SCsub",
    "config.py",
    "src/README.md",
    "src/internal/tick_synchronizer_build_config.h",
    "src/internal/tick_synchronizer_version.h",
    "src/public/tick_synchronizer.h",
    "src/public/tick_synchronizer.cpp",
    "src/public/tick_synchronizer_buffer.h",
    "src/public/tick_synchronizer_buffer.cpp",
    "src/protocol/tick_synchronizer_packet_codec.h",
    "src/protocol/tick_synchronizer_packet_codec.cpp",
    "src/protocol/tick_synchronizer_handshake.h",
    "src/protocol/tick_synchronizer_handshake.cpp",
    "src/protocol/tick_synchronizer_handshake_state_machine.h",
    "src/protocol/tick_synchronizer_handshake_state_machine.cpp",
    "doc_classes/TickSynchronizer.xml",
    "doc_classes/TickSynchronizerBuffer.xml",
    "tests/smoke_project/smoke_test.gd",
    "tests/smoke_project/smoke_test.gd.uid",
    "tests/golden/control_hello_v4.bin",
    "scripts/build_and_validate.sh",
    "scripts/run_sanitized_tests.sh",
    "scripts/verify_godot_baseline.sh",
    "scripts/verify_godot_version.py",
    "scripts/verify_lsan_suppressions.sh",
    "scripts/verify_sanitizer_suppressions.sh",
    "scripts/compute_module_build_id.py",
    "scripts/verify_mermaid_diagrams.py",
    "scripts/build_protocol_benchmarks.sh",
    "scripts/run_protocol_benchmarks.sh",
    "scripts/build_protocol_benchmarks_android.sh",
    "scripts/run_protocol_benchmarks_android.sh",
    "scripts/build_protocol_benchmarks_windows_cross.sh",
    "scripts/run_protocol_benchmarks_windows.ps1",
    "scripts/build_protocol_benchmarks_macos.sh",
    "scripts/run_protocol_benchmarks_macos.sh",
    "scripts/export_protocol_benchmarks.sh",
    "scripts/verify_benchmark_results.py",
    "scripts/compare_protocol_benchmarks.py",
    "scripts/analyze_protocol_candidates.py",
    "scripts/sanitizer_suppressions/godot-4.7.1-lsan.supp",
    "scripts/sanitizer_suppressions/godot-4.7.1-ubsan.supp",
    "benchmarks/SConstruct",
    "benchmarks/benchmark_compiler_contract.h",
    "benchmarks/benchmark_platform.h",
    "benchmarks/benchmark_platform.cpp",
    "benchmarks/protocol_benchmark_main.cpp",
    "benchmarks/benchmark_config.h",
    "benchmarks/benchmark_dataset.cpp",
    "benchmarks/benchmark_runner.h",
    "benchmarks/benchmark_result.h",
    "benchmarks/benchmark_result_writer.cpp",
    "benchmarks/candidates/protocol_candidate.h",
    "benchmarks/candidates/protocol_candidate.cpp",
    "benchmarks/candidates/reference_fixed_width_candidate.h",
    "benchmarks/candidates/reference_fixed_width_candidate.cpp",
    "benchmarks/candidates/varint_zigzag_fixed_float_candidate.h",
    "benchmarks/candidates/varint_zigzag_fixed_float_candidate.cpp",
    "benchmark_reports/.gitignore",
    "documentation/README.md",
    "documentation/ARCHITECTURE.md",
    "documentation/BENCHMARKS.md",
    "documentation/BENCHMARK_DECISIONS.md",
    "documentation/BUILD.md",
    "documentation/PROTOCOL.md",
    "documentation/PRIVACY.md",
    "documentation/TESTING.md",
    "documentation/development/README.md",
    "documentation/development/PROJECT_STATE.md",
    "documentation/development/ROADMAP.md",
    "documentation/development/VALIDATION.md",
    "documentation/adr/0002-external-module-layout.md",
    "documentation/adr/0028-deterministic-protocol-benchmark-suite.md",
    "documentation/adr/0029-cross-platform-benchmark-backends.md",
    "documentation/adr/0030-godot-version-handshake-compatibility.md",
    "documentation/adr/0031-restore-sanitized-smoke-with-source-scoped-godot-suppressions.md",
    "documentation/adr/0032-add-local-macos-universal2-benchmark-backend.md",
    "documentation/adr/0033-enforce-godot-cpp-policy-boundary.md",
    "documentation/adr/0034-publish-macos-source-with-local-apple-toolchain.md",
    "documentation/adr/0035-separate-repository-project-and-wiki-responsibilities.md",
    "documentation/adr/0036-separate-candidate-selection-from-final-macos-qualification.md",
    "documentation/adr/0037-adopt-suite2-and-screen-canonical-varint-integers.md",
    "documentation/adr/0038-keep-lsan-enabled-with-a-godot-joypad-suppression.md",
    "documentation/adr/0039-adopt-varint-zigzag-fixed-float-default.md",
    "documentation/adr/0040-support-godot-4-4-and-newer.md",
}

for relative in sorted(required_files):
    if not (module / relative).is_file():
        errors.append(f"missing required file: {relative}")

def read(relative: str) -> str:
    path = module / relative
    return path.read_text(encoding="utf-8") if path.is_file() else ""


manifest_path = module / "FILE_MANIFEST.txt"
manifest_entries = set(read("FILE_MANIFEST.txt").splitlines())
for relative in sorted(required_files):
    if relative not in manifest_entries:
        errors.append(f"FILE_MANIFEST.txt does not contain: {relative}")

# Ensure the manifest describes the complete source tree and excludes only
# repository-owned generated locations.
generated_prefixes = (
    ".git/",
    ".godot/",
    "build/",
    "build_reports/",
    "benchmarks/.build/",
    "benchmarks/bin/",
    "benchmark_reports/",
    "benchmark_dist/",
)


def is_generated_artifact(path: pathlib.Path) -> bool:
    relative_path = path.relative_to(module)
    relative = relative_path.as_posix()
    if relative == "benchmark_reports/.gitignore":
        return False
    if ".godot" in relative_path.parts[:-1]:
        return True
    if "__pycache__" in relative_path.parts:
        return True
    return relative.startswith(generated_prefixes)


actual_files: set[str] = set()
for path in module.rglob("*"):
    if not path.is_file():
        continue
    relative = path.relative_to(module).as_posix()
    if is_generated_artifact(path) or relative == "FILE_MANIFEST.txt":
        continue
    actual_files.add(relative)

manifest_without_self = manifest_entries - {"FILE_MANIFEST.txt"}
missing_from_manifest = sorted(actual_files - manifest_without_self)
stale_manifest_entries = sorted(manifest_without_self - actual_files)
if missing_from_manifest:
    errors.append("files missing from FILE_MANIFEST.txt: " + ", ".join(missing_from_manifest))
if stale_manifest_entries:
    errors.append("stale FILE_MANIFEST.txt entries: " + ", ".join(stale_manifest_entries))

# The project has one compilation graph. Alternative build-system descriptors,
# generated directories, and source references are policy violations.
forbidden_build_terms = (("c" + "make"), ("nin" + "ja"))
forbidden_descriptor_names = {"Makefile", "meson.build", "BUILD.bazel"}
for path in sorted(module.rglob("*")):
    if not path.is_file() or is_generated_artifact(path):
        continue
    relative = path.relative_to(module).as_posix()
    lower_relative = relative.lower()
    if path.name in forbidden_descriptor_names or any(term in lower_relative for term in forbidden_build_terms):
        errors.append(f"alternative build-system residue: {relative}")

sconstructs = sorted(
    path.relative_to(module).as_posix()
    for path in module.rglob("SConstruct")
    if path.is_file() and not is_generated_artifact(path)
)
if sconstructs != ["benchmarks/SConstruct"]:
    errors.append(
        "expected exactly one benchmark SConstruct at benchmarks/SConstruct; found: "
        + (", ".join(sconstructs) if sconstructs else "none")
    )

# Public buffer headers, bindings, and class-reference XML must describe one revision.
buffer_header = read("src/public/tick_synchronizer_buffer.h")
buffer_source = read("src/public/tick_synchronizer_buffer.cpp")
buffer_xml_path = module / "doc_classes/TickSynchronizerBuffer.xml"
if buffer_xml_path.is_file():
    xml_root = ET.parse(buffer_xml_path).getroot()
    documented = {method.attrib["name"] for method in xml_root.findall("./methods/method")}
else:
    documented = set()
bound = set(re.findall(r'D_METHOD\("([A-Za-z_][A-Za-z0-9_]*)"', buffer_source))
declared = set(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*(?:const\s*)?;', buffer_header))

missing_bindings = sorted(documented - bound)
undocumented_bindings = sorted(bound - documented)
missing_declarations = sorted(documented - declared)
if missing_bindings:
    errors.append("documented methods without bindings: " + ", ".join(missing_bindings))
if undocumented_bindings:
    errors.append("bindings without XML documentation: " + ", ".join(undocumented_bindings))
if missing_declarations:
    errors.append("documented methods missing from the header: " + ", ".join(missing_declarations))

required_buffer_api = {
    "write_bits", "read_bits", "align_write_to_byte", "align_read_to_byte",
    "write_u8", "write_u16", "write_u32", "write_u64",
    "read_u8", "read_u16", "read_u32", "read_u64",
    "write_varuint", "read_varuint", "write_varint", "read_varint",
    "write_float32", "read_float32", "write_float64", "read_float64",
    "set_max_size_bytes", "get_max_size_bytes", "get_max_size_bits",
    "get_remaining_write_bits", "can_write_bits", "is_equal_to", "get_content_hash",
}
missing_required = sorted(required_buffer_api - declared)
if missing_required:
    errors.append("required buffer API missing from the header: " + ", ".join(missing_required))
for constant in ("DEFAULT_MAX_SIZE_BYTES", "MAX_CONFIGURABLE_SIZE_BYTES"):
    if constant not in buffer_header or constant not in buffer_source:
        errors.append(f"required constant missing or not bound: {constant}")

# Root diagnostic API must also match header, binding, and XML.
sync_header = read("src/public/tick_synchronizer.h")
sync_source = read("src/public/tick_synchronizer.cpp")
sync_xml_path = module / "doc_classes/TickSynchronizer.xml"
if sync_xml_path.is_file():
    sync_xml = ET.parse(sync_xml_path).getroot()
    sync_documented = {method.attrib["name"] for method in sync_xml.findall("./methods/method")}
else:
    sync_documented = set()
sync_bound = set(re.findall(r'D_METHOD\("([A-Za-z_][A-Za-z0-9_]*)"', sync_source))
required_diagnostics = {
    "get_build_precision", "is_double_precision", "get_protocol_magic",
    "get_protocol_major", "get_protocol_minor", "get_protocol_precision_mode",
}
for method in sorted(required_diagnostics):
    if method not in sync_header:
        errors.append(f"public TickSynchronizer method missing from header: {method}")
    if method not in sync_bound:
        errors.append(f"public TickSynchronizer binding missing: {method}")
    if method not in sync_documented:
        errors.append(f"public TickSynchronizer XML documentation missing: {method}")

# Smoke integration markers and minimum test count.
smoke = read("tests/smoke_project/smoke_test.gd")
required_smoke_markers = {
    "TICKSYNCHRONIZER_BUFFER_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_INTEGER_CODEC_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_FLOAT_CODEC_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_RESOURCE_LIMIT_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_SMOKE_TEST_OK",
}
missing_markers = sorted(marker for marker in required_smoke_markers if marker not in smoke)
if missing_markers:
    errors.append("missing smoke-test markers: " + ", ".join(missing_markers))

test_cases = sum(
    path.read_text(encoding="utf-8").count("TEST_CASE(")
    for path in sorted((module / "tests").glob("test_tick_synchronizer*.h"))
)
if test_cases < 140:
    errors.append(f"only {test_cases} TEST_CASE entries found; minimum expected: 140")

# Script interface and sanitizer contracts.
build_script = read("scripts/build_and_validate.sh")
sanitizer_script = read("scripts/run_sanitized_tests.sh")
godot_compatibility_script = read("scripts/verify_godot_baseline.sh")
godot_version_script = read("scripts/verify_godot_version.py")
lsan_guard = read("scripts/verify_lsan_suppressions.sh")
sanitizer_guard = read("scripts/verify_sanitizer_suppressions.sh")
mermaid_script = read("scripts/verify_mermaid_diagrams.py")
export_script = read("scripts/export_protocol_benchmarks.sh")
lsan_suppression = read("scripts/sanitizer_suppressions/godot-4.7.1-lsan.supp")
ubsan_suppression = read("scripts/sanitizer_suppressions/godot-4.7.1-ubsan.supp")
lsan_adr = read("documentation/adr/0038-keep-lsan-enabled-with-a-godot-joypad-suppression.md")
build_api_match = re.search(r'readonly SCRIPT_API_VERSION="([0-9]+)"', build_script)
wrapper_api_match = re.search(r'readonly EXPECTED_BUILD_SCRIPT_API="([0-9]+)"', sanitizer_script)
if not build_api_match or not wrapper_api_match:
    errors.append("script API contract not found")
elif build_api_match.group(1) != wrapper_api_match.group(1):
    errors.append(
        f"incompatible scripts: build API {build_api_match.group(1)} "
        f"!= wrapper API {wrapper_api_match.group(1)}"
    )
for token in (
    "GODOT_MINIMUM_VERSION",
    "verify_godot_version.py",
    "verify_godot_compatibility",
    "QUALIFIED_BASELINE_MATCH",
    "TICKSYNCHRONIZER_BUILD_PREFLIGHT_OK",
):
    if token not in build_script:
        errors.append(f"Godot 4.4+ build policy is missing: {token}")
for token in (
    "TICKSYNCHRONIZER_GODOT_COMPATIBILITY_OK",
    "Qualified baseline match",
    "Supported Godot series: 4.x",
):
    if token not in godot_compatibility_script:
        errors.append(f"Godot compatibility verifier is incomplete: {token}")
for token in (
    "TICKSYNCHRONIZER_GODOT_VERSION_POLICY_SELF_TEST_OK",
    "unsupported Godot major version",
    "minimum_godot_version",
):
    if token not in godot_version_script:
        errors.append(f"Godot version policy verifier is incomplete: {token}")
if "verify_qualified_sanitizer_baseline" not in sanitizer_script:
    errors.append("sanitizer policy is not locked to the qualified Godot baseline")
if "Skipped for sanitized editor" not in build_script:
    errors.append("sanitized artifact structural validation is missing")
if 'ASAN_OPTIONS+=\":detect_invalid_pointer_pairs=0\"' not in sanitizer_script:
    errors.append("ASAN profile does not disable engine-level invalid-pointer-pair checking by default")
if "--invalid-pointer-pairs" not in sanitizer_script:
    errors.append("diagnostic option --invalid-pointer-pairs is missing")
expected_lsan_rules = ["leak:JoypadSDL::initialize"]
actual_lsan_rules = [
    line.strip()
    for line in lsan_suppression.splitlines()
    if line.strip() and not line.lstrip().startswith("#")
]
if actual_lsan_rules != expected_lsan_rules:
    errors.append("Godot 4.7.1 LSAN suppressions differ from the reviewed exact policy")
if 'ASAN_OPTIONS+=\":detect_leaks=1\"' not in sanitizer_script:
    errors.append("ASAN profile does not force LeakSanitizer to remain enabled")
if "suppressions=${LSAN_SUPPRESSION_FILE}" not in sanitizer_script:
    errors.append("sanitizer wrapper does not apply the LSAN suppression file")
if "--no-godot-lsan-suppressions" not in sanitizer_script:
    errors.append("diagnostic option --no-godot-lsan-suppressions is missing")
if "verify_lsan_suppressions.sh" not in sanitizer_script:
    errors.append("sanitizer wrapper does not invoke the LSAN suppression guard")
for token in (
    "single|double|all",
    'PRECISIONS=("double" "single")',
    "LSAN_NEGATIVE_CONTROL_COMPLETED",
    "--policy-only",
):
    if token not in sanitizer_script:
        errors.append(f"sanitizer batch contract is missing: {token}")
for token in (
    "TICKSYNCHRONIZER_LSAN_SUPPRESSION_POLICY_OK",
    "TICKSYNCHRONIZER_LSAN_SUPPRESSION_POLICY_ONLY_OK",
    "TICKSYNCHRONIZER_LSAN_UNRELATED_LEAK_NEGATIVE_CONTROL_OK",
    "TICKSYNCHRONIZER_LSAN_SUPPRESSION_GUARD_OK",
    "lsan_unrelated_leak_probe.cpp",
    "-fsanitize=address",
    "exitcode=23",
    "--kill-after=5s",
    "--policy-only",
):
    if token not in lsan_guard:
        errors.append(f"LSAN suppression guard contract is missing: {token}")
for token in (
    "6,991 leaked bytes in 172 allocations",
    "leak:JoypadSDL::initialize",
    "detect_leaks=1",
    "status 23",
    "uninterrupted sanitizer acceptance batch",
    "every later ASAN or combined pass",
    "Do not patch Godot or SDL",
):
    if token not in lsan_adr:
        errors.append(f"incomplete LSAN policy decision: {token}")
expected_ubsan_rules = [
    "nonnull-attribute:core/string/ustring.cpp",
    "bounds:thirdparty/sdl/thread/SDL_thread.c",
    "alignment:modules/gdscript/gdscript_vm.cpp",
]
actual_ubsan_rules = [
    line.strip()
    for line in ubsan_suppression.splitlines()
    if line.strip() and not line.lstrip().startswith("#")
]
if actual_ubsan_rules != expected_ubsan_rules:
    errors.append("Godot 4.7.1 UBSAN suppressions differ from the reviewed exact policy")
if "suppressions=${UBSAN_SUPPRESSION_FILE}" not in sanitizer_script:
    errors.append("sanitizer wrapper does not apply the UBSAN suppression file")
if "--no-godot-ubsan-suppressions" not in sanitizer_script:
    errors.append("diagnostic option --no-godot-ubsan-suppressions is missing")
if "verify_sanitizer_suppressions.sh" not in sanitizer_script:
    errors.append("sanitizer wrapper does not invoke the UBSAN suppression guard")
for token in (
    "TICKSYNCHRONIZER_UBSAN_SUPPRESSION_POLICY_OK",
    "TICKSYNCHRONIZER_UBSAN_SUPPRESSION_GUARD_OK",
    "ubsan_bounds_probe.cpp",
    "ubsan_alignment_probe.cpp",
    "-fsanitize=undefined",
):
    if token not in sanitizer_guard:
        errors.append(f"UBSAN suppression guard contract is missing: {token}")
for relative, script_text in (
    ("scripts/run_sanitized_tests.sh", sanitizer_script),
    ("scripts/verify_lsan_suppressions.sh", lsan_guard),
    ("scripts/verify_sanitizer_suppressions.sh", sanitizer_guard),
    ("scripts/verify_mermaid_diagrams.py", mermaid_script),
    ("scripts/export_protocol_benchmarks.sh", export_script),
):
    for token in ("TICKSYNC_TEMP_DIR", "tick_synchronizer_tmp"):
        if token not in script_text:
            errors.append(f"host temporary-directory policy missing from {relative}: {token}")
for line_number, line in enumerate(sanitizer_script.splitlines(), start=1):
    if "mktemp" in line and "-d" in line and "$TEMP_ROOT/" not in line:
        errors.append(f"unscoped host temporary directory in scripts/run_sanitized_tests.sh:{line_number}")
for line_number, line in enumerate(lsan_guard.splitlines(), start=1):
    if "mktemp" in line and "-d" in line and "$temp_root/" not in line:
        errors.append(f"unscoped host temporary directory in scripts/verify_lsan_suppressions.sh:{line_number}")
for line_number, line in enumerate(sanitizer_guard.splitlines(), start=1):
    if "mktemp" in line and "-d" in line and "$temp_root/" not in line:
        errors.append(f"unscoped host temporary directory in scripts/verify_sanitizer_suppressions.sh:{line_number}")
for line_number, line in enumerate(export_script.splitlines(), start=1):
    if "mktemp" in line and "-d" in line and "$TEMP_ROOT/" not in line:
        errors.append(f"unscoped host temporary directory in scripts/export_protocol_benchmarks.sh:{line_number}")
if "TemporaryDirectory(prefix=" not in mermaid_script or "dir=temp_root" not in mermaid_script:
    errors.append("Mermaid rendering does not use the controlled host temporary directory")

# Central version contract.
version_header = read("src/internal/tick_synchronizer_version.h")
godot_version = read("GODOT_VERSION").strip()
godot_commit = read("GODOT_COMMIT").strip()
godot_minimum_version = read("GODOT_MINIMUM_VERSION").strip()
if godot_minimum_version != "4.4.0":
    errors.append(f"unexpected minimum Godot version: {godot_minimum_version!r}")
if godot_version != "4.7.1-stable":
    errors.append(f"unexpected Godot version baseline: {godot_version!r}")
if godot_commit != "a13da4feb8d8aefc283c3763d33a2f170a18d541":
    errors.append(f"unexpected Godot commit baseline: {godot_commit!r}")

def read_integer_version(name: str) -> int | None:
    matches = re.findall(
        rf"\b{name}\b\s*=\s*(0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*\s*;",
        version_header,
    )
    if len(matches) != 1:
        errors.append(f"invalid version contract: {name} has {len(matches)} declarations")
        return None
    return int(matches[0], 0)

api_version = read_integer_version("API_VERSION")
wire_version = read_integer_version("WIRE_PROTOCOL_VERSION")
wire_revision = read_integer_version("WIRE_PROTOCOL_REVISION")
benchmark_version = read_integer_version("BENCHMARK_SUITE_VERSION")
exact_match_values = re.findall(
    r"\bEXACT_BUILD_MATCH_REQUIRED\b\s*=\s*(true|false)\s*;",
    version_header,
)
if len(exact_match_values) != 1:
    errors.append("invalid version contract: EXACT_BUILD_MATCH_REQUIRED must have one declaration")
    exact_match = None
else:
    exact_match = exact_match_values[0] == "true"

expected_versions = {
    "API_VERSION": (api_version, 4),
    "WIRE_PROTOCOL_VERSION": (wire_version, 0),
    "WIRE_PROTOCOL_REVISION": (wire_revision, 2),
    "BENCHMARK_SUITE_VERSION": (benchmark_version, 2),
}
for name, (actual, expected) in expected_versions.items():
    if actual is not None and actual != expected:
        errors.append(f"unexpected version contract: {name}={actual}, expected {expected}")
if exact_match is not None and not exact_match:
    errors.append("EXACT_BUILD_MATCH_REQUIRED must remain true during the experimental wire period")
if wire_version == 0 and wire_revision == 0:
    errors.append("experimental wire protocol requires a nonzero revision")

# Benchmark suite contract.
benchmark_config = read("benchmarks/benchmark_config.h")
benchmark_dataset = read("benchmarks/benchmark_dataset.cpp")
benchmark_runner = read("benchmarks/benchmark_runner.h")
benchmark_main = read("benchmarks/protocol_benchmark_main.cpp")
benchmark_result = read("benchmarks/benchmark_result.h")
benchmark_writer = read("benchmarks/benchmark_result_writer.cpp")
benchmark_candidate_shared = read("benchmarks/candidates/protocol_candidate.h") + read(
    "benchmarks/candidates/protocol_candidate.cpp"
)
benchmark_reference_candidate = read("benchmarks/candidates/reference_fixed_width_candidate.h") + read(
    "benchmarks/candidates/reference_fixed_width_candidate.cpp"
)
benchmark_varint_candidate = read("benchmarks/candidates/varint_zigzag_fixed_float_candidate.h") + read(
    "benchmarks/candidates/varint_zigzag_fixed_float_candidate.cpp"
)
benchmark_sconstruct = read("benchmarks/SConstruct")
benchmark_compiler_contract = read("benchmarks/benchmark_compiler_contract.h")
benchmark_platform = read("benchmarks/benchmark_platform.h") + read("benchmarks/benchmark_platform.cpp")
benchmark_allocation = read("benchmarks/benchmark_allocation_counter.cpp")
benchmark_build_script = read("scripts/build_protocol_benchmarks.sh")
benchmark_run_script = read("scripts/run_protocol_benchmarks.sh")
benchmark_android_build = read("scripts/build_protocol_benchmarks_android.sh")
benchmark_android_run = read("scripts/run_protocol_benchmarks_android.sh")
benchmark_windows_build = read("scripts/build_protocol_benchmarks_windows_cross.sh")
benchmark_windows_run = read("scripts/run_protocol_benchmarks_windows.ps1")
benchmark_macos_build = read("scripts/build_protocol_benchmarks_macos.sh")
benchmark_macos_run = read("scripts/run_protocol_benchmarks_macos.sh")
benchmark_export_script = read("scripts/export_protocol_benchmarks.sh")
benchmark_verify_script = read("scripts/verify_benchmark_results.py")
benchmark_compare_script = read("scripts/compare_protocol_benchmarks.py")
benchmark_analyze_script = read("scripts/analyze_protocol_candidates.py")
benchmark_doc = read("documentation/BENCHMARKS.md")
benchmark_decisions = read("documentation/BENCHMARK_DECISIONS.md")
benchmark_adr = read("documentation/adr/0028-deterministic-protocol-benchmark-suite.md") + read(
    "documentation/adr/0029-cross-platform-benchmark-backends.md"
) + read("documentation/adr/0032-add-local-macos-universal2-benchmark-backend.md") + read(
    "documentation/adr/0036-separate-candidate-selection-from-final-macos-qualification.md"
) + read("documentation/adr/0037-adopt-suite2-and-screen-canonical-varint-integers.md")
cpp_policy_adr = read("documentation/adr/0033-enforce-godot-cpp-policy-boundary.md")
macos_distribution_adr = read(
    "documentation/adr/0034-publish-macos-source-with-local-apple-toolchain.md"
)
documentation_governance_adr = read(
    "documentation/adr/0035-separate-repository-project-and-wiki-responsibilities.md"
)
candidate_gate_adr = read(
    "documentation/adr/0036-separate-candidate-selection-from-final-macos-qualification.md"
)
suite2_adr = read(
    "documentation/adr/0037-adopt-suite2-and-screen-canonical-varint-integers.md"
)

if re.findall(r"suite_version\s*=\s*([0-9]+)", benchmark_config) != ["2"]:
    errors.append("benchmark_config.h must declare exactly suite_version=2")
required_datasets = {
    "control_minimal", "player_input", "snapshot_sparse", "snapshot_medium",
    "snapshot_dense", "numeric_extremes", "integer_boundaries", "sequential_flow",
}
missing_datasets = sorted(name for name in required_datasets if f'"{name}"' not in benchmark_dataset)
if missing_datasets:
    errors.append("missing benchmark datasets: " + ", ".join(missing_datasets))
for token in ("reference_fixed_width", "CANDIDATE_ID = 1", "MAX_ENTITIES = 1024", "MAX_BLOB_SIZE = 4096"):
    if token not in benchmark_reference_candidate:
        errors.append(f"incomplete reference candidate: {token}")
for token in (
    "varint_zigzag_fixed_float", "CANDIDATE_ID = 2", "append_uleb128",
    "zigzag_encode", "read_uleb128", "MAX_ENTITIES = 1024", "MAX_BLOB_SIZE = 4096",
):
    if token not in benchmark_varint_candidate:
        errors.append(f"incomplete canonical-varint candidate: {token}")
for token in (
    "CANONICAL_NAN_BITS", "binary32_bits_from_double", "round_right_to_even",
    "benchmark_wire_scalar_contract_self_test", "CandidateDecodeError read_benchmark_wire_scalar",
    "benchmark_message_kind_semantics_are_valid", "std::numeric_limits<double>::is_iec559",
):
    if token not in benchmark_candidate_shared:
        errors.append(f"incomplete canonical scalar contract: {token}")
for token in ("round_trip_failures", "determinism_failures", "AllocationCounter", "minimum_sample_duration_ns", "measured_rounds", "combine_diagnostic_checksum", "expected_error", "append_invalid_packets"):
    if token not in benchmark_runner:
        errors.append(f"incomplete benchmark runner: {token}")
for token in ("version::BENCHMARK_SUITE_VERSION", "--self-test", "--list-cpus", "--list-candidates", "--candidate", "--quick", "--json", "--csv", "--cpu", "TICKSYNCHRONIZER_PROTOCOL_BENCHMARK_OK"):
    if token not in benchmark_main:
        errors.append(f"incomplete benchmark executable: {token}")
if "is_official_benchmark_config" not in benchmark_config + benchmark_main or "options.only_dataset.empty()" not in benchmark_main:
    errors.append("official benchmark eligibility must require the exact methodology and complete dataset suite")
for token in ("QUALIFICATION_GODOT_COMMIT", "is_lower_hex_sha1"):
    if token not in benchmark_config + benchmark_main:
        errors.append(f"official benchmark provenance guard is missing: {token}")
for token in (
    "platform", "linuxbsd", "windows", "android", "macos", "precision", "-std=c++17",
    "protocol_candidate.cpp", "reference_fixed_width_candidate.cpp",
    "varint_zigzag_fixed_float_candidate.cpp", "benchmark_platform.cpp", "SConsignFile",
    "mingw-gcc", "llvm-mingw", "android-ndk", "aarch64-linux-android",
    "-static-libstdc++", "apple-clang", "-mmacosx-version-min",
    "-fno-exceptions", "-fno-rtti", "benchmark_compiler_contract.h",
):
    if token not in benchmark_sconstruct:
        errors.append(f"incomplete benchmark SConstruct: {token}")
for token in (
    "reported_compiler_command", "reported_compiler_path", "macos_sdk",
    "os.path.abspath(os.path.expanduser(command))", '"LINK": compiler_path',
):
    if token not in benchmark_sconstruct:
        errors.append(f"incomplete privacy-safe benchmark build provenance: {token}")
if "path.expanduser().resolve()" in benchmark_sconstruct:
    errors.append(
        "benchmark compiler resolution must preserve the C++ driver symlink basename"
    )
for token in (
    "__cpp_exceptions", "__EXCEPTIONS", "_CPPUNWIND",
    "__GXX_RTTI", "_CPPRTTI",
):
    if token not in benchmark_compiler_contract:
        errors.append(f"incomplete benchmark compiler-feature guard: {token}")
for token in (
    "sched_setaffinity", "SetThreadGroupAffinity", "affinity verification expected CPU",
    "affinity_actual_cpu", "GetLogicalProcessorInformationEx", "RelationCache",
    "list_benchmark_logical_cpus",
    "unsupported-by-platform-policy",
):
    if token not in benchmark_platform + benchmark_result:
        errors.append(f"incomplete native benchmark affinity support: {token}")
for token in ("#if defined(_WIN32)", "<malloc.h>", "_aligned_malloc", "_aligned_free"):
    if token not in benchmark_allocation:
        errors.append(f"incomplete Windows aligned-allocation support: {token}")
guarded_nominmax = "#if defined(_WIN32)\n#ifndef NOMINMAX\n#define NOMINMAX\n#endif"
if "_MSC_VER" in benchmark_allocation or guarded_nominmax not in benchmark_platform:
    errors.append("Windows benchmark portability guards must support MinGW without macro redefinition")
if "--print-benchmark-suite-version" not in benchmark_build_script or "--self-test" not in benchmark_build_script:
    errors.append("benchmark build script does not enforce the central suite contract and self-test")
for token in ("verify_benchmark_results.py", "results.json", "results.csv", "--allow-dirty", "--allow-unpinned", "binary_sha256", "--cpu", "--cpu-class", "--l3-cache-id", "--list-cpus"):
    if token not in benchmark_run_script:
        errors.append(f"incomplete Linux benchmark run script: {token}")
for token in ("ANDROID_NDK_HOME", "platform=android", "android-ndk", "arm64-v8a", "llvm-readelf"):
    if token not in benchmark_android_build:
        errors.append(f"incomplete Android benchmark build backend: {token}")
for token in ("adb", "/data/local/tmp", "--list-cpus", "--cpu-class", "verify_benchmark_results.py"):
    if token not in benchmark_android_run:
        errors.append(f"incomplete Android benchmark run backend: {token}")
for token in ("CPU_EXPLICIT", "could not compute the adb binary SHA-256"):
    if token not in benchmark_android_run:
        errors.append(f"incomplete Android official execution guard: {token}")
for token in ("platform=windows", "x86_64-w64-mingw32", "windows/x86_64", "mingw-gcc", "llvm-mingw", "Wine", "deployment"):
    if token not in benchmark_windows_build:
        errors.append(f"incomplete Linux-to-Windows cross-build backend: {token}")
for token in (
    'binary_dir="$(cd -- "$(dirname -- "$binary")" && pwd -P)"',
    'binary_name="$(basename -- "$binary")"',
    'cd -- "$binary_dir"',
    '"$OBJDUMP_BIN" --private-headers "$binary_name"',
    '"$OBJDUMP_BIN" -p "$binary_name"',
):
    if token not in benchmark_windows_build:
        errors.append(f"Windows PE inspection must use a privacy-safe binary basename: {token}")
for forbidden in (
    '"$OBJDUMP_BIN" --private-headers "$binary"',
    '"$OBJDUMP_BIN" -p "$binary"',
):
    if forbidden in benchmark_windows_build:
        errors.append(f"Windows PE inspection exposes the build-host binary path: {forbidden}")
for token in (
    "Get-CimInstance", "--cpu", "ConvertFrom-Json", "Get-FileHash",
    "official_eligible", "--self-test", "--list-cpus", "CpuClass",
    "l3_cache_id", "PACKAGE_INTEGRITY_OK",
):
    if token not in benchmark_windows_run:
        errors.append(f"incomplete execution-only Windows benchmark backend: {token}")
for forbidden in ("scons", "build_protocol_benchmarks_windows.ps1", "Python-Command", "git -C"):
    if forbidden in benchmark_windows_run:
        errors.append(f"Windows execution host must not require development tooling: {forbidden}")
for token in (
    "platform=macos", "apple-clang", "x86_64", "arm64", "lipo", "otool",
    "Universal 2", "--deployment-target", "LC_BUILD_VERSION",
    "verify_source_consistency.sh", "source state changed during the Universal 2 build",
    "Apple clang version", "unless both --cxx and --sdk are supplied",
    "normalize_version",
):
    if token not in benchmark_macos_build:
        errors.append(f"incomplete local macOS Universal 2 build backend: {token}")
for required_command in (
    '/usr/bin/lipo "$binary" -verify_arch "$selected_architecture"',
    '/usr/bin/lipo "$universal_binary" -verify_arch x86_64 arm64',
    '/usr/bin/lipo "$binary" -verify_arch "$host_architecture"',
):
    if required_command not in benchmark_macos_build:
        errors.append(
            "macOS lipo architecture verification must place the input file "
            f"before -verify_arch: {required_command}"
        )
if "/usr/bin/lipo -verify_arch" in benchmark_macos_build:
    errors.append("macOS lipo architecture verification uses an incompatible argument order")
for token in (
    "macos-native", "unsupported-by-platform-policy", "scheduler-managed",
    "PACKAGE_INTEGRITY_OK", "shasum", "plutil", "--quick", "--allow-dirty",
    "sysctl.proc_translated", "com.apple.quarantine",
    '"source_state": "dirty"', "--quick and --allow-dirty are mutually exclusive",
):
    if token not in benchmark_macos_run:
        errors.append(f"incomplete execution-only macOS benchmark backend: {token}")
for forbidden in ("scons ", "xcrun", "clang++", "python3", "git -C", "lipo", "otool"):
    if forbidden.lower() in benchmark_macos_run.lower():
        errors.append(f"macOS execution host must not require development tooling: {forbidden}")
for token in (
    "--platform", "linuxbsd", "windows", "android", "macos", "execution-only",
    "TICKSYNC_TEMP_DIR", "tick_synchronizer_tmp", "SHA256SUMS.txt",
    "verify_benchmark_package_privacy", "write_archive_hash_sidecar", '${archive##*/}',
):
    if token not in benchmark_export_script:
        errors.append(f"incomplete benchmark deployment exporter: {token}")
for runner_name, runner_text in (
    ("Linux", benchmark_run_script),
    ("Android", benchmark_android_run),
):
    for token in ("--execution-only", "--binary-dir", "PACKAGE_INTEGRITY_OK"):
        if token not in runner_text:
            errors.append(f"incomplete {runner_name} execution-only benchmark runner: {token}")
for token in ("PACKAGE_INTEGRITY_OK", "--binary-dir", "macos-native"):
    if token not in benchmark_macos_run:
        errors.append(f"incomplete macOS execution-only benchmark runner: {token}")
for token in ("benchmark_suite_version", "round_trip_failures", "accepted", "schema_version", "official_eligible", "binary_sha256", "EXPECTED_CANDIDATES", "invalid packet accounting mismatch"):
    if token not in benchmark_verify_script:
        errors.append(f"incomplete benchmark report verifier: {token}")
for token in ("OFFICIAL_CONFIG", "EXPECTED_DATASETS", "QUALIFICATION_GODOT_COMMIT"):
    if token not in benchmark_verify_script:
        errors.append(f"incomplete official benchmark report verification: {token}")
if "is_scheduler_managed_macos" not in benchmark_verify_script:
    errors.append("benchmark report verifier does not enforce the exact macOS scheduling policy")
if "verify_provenance_privacy" not in benchmark_verify_script:
    errors.append("benchmark report verifier does not reject host-specific provenance paths")
if 'TICKSYNC_BENCHMARK_EXECUTABLE_PATH=${binary_name}' not in benchmark_run_script:
    errors.append("Linux benchmark reports must use a privacy-safe executable basename")
if "[System.IO.Path]::GetFileName($Binary)" not in benchmark_windows_run:
    errors.append("Windows benchmark reports must use a privacy-safe executable basename")
for token in (
    "official_eligible", "benchmark_suite_version", "group_by_precision",
    "report_folder", "--self-test",
):
    if token not in benchmark_compare_script:
        errors.append(f"incomplete benchmark comparator: {token}")
for token in (
    "REFERENCE_CANDIDATE", "VARINT_CANDIDATE", "COVERAGE_ONLY_DATASETS",
    "MAX_WORKLOAD_SIZE_RATIO = 0.95", "MAX_WEIGHTED_LATENCY_RATIO = 1.50",
    "MAX_DATASET_LATENCY_RATIO = 2.00", "MAX_PAIR_TIME_DELTA_SECONDS = 3_600",
    "paired benchmark configuration differs", "semantic_hash", "--self-test",
):
    if token not in benchmark_analyze_script:
        errors.append(f"incomplete paired candidate analyzer: {token}")
if "std::uint32_t schema_version = 3;" not in benchmark_result:
    errors.append("benchmark report schema must be 3")
for token in ("binary_sha256", "compiler_flags", "logical_cpu", "runtime_backend", "device_model", "affinity_applied", "official_eligible"):
    if token not in benchmark_result + benchmark_writer:
        errors.append(f"incomplete benchmark provenance: {token}")
benchmark_documentation_text = (benchmark_doc + benchmark_decisions).lower()
for token in (
    "clean Git tree", "Linux x86_64", "Windows x86_64", "Android ARM64",
    "macOS Universal 2", "schema 3", "16 matched", "36 reports", "suite 2",
):
    if token.lower() not in benchmark_documentation_text:
        errors.append(f"benchmark documentation is missing required decision context: {token}")
if "reference_fixed_width" not in benchmark_adr:
    errors.append("ADR 0028 does not record the reference candidate")
for token in (
    "Godot policy", "ProtocolFixedBytes", "-fno-exceptions", "-fno-rtti",
    "explicit status values", "benchmark suite version remains 1",
):
    if token not in cpp_policy_adr:
        errors.append(f"incomplete Godot C++ policy decision: {token}")
for token in (
    "source-only", "Apple-branded hardware", "copied Apple SDK",
    "must remain outside Git and public releases",
):
    if token not in macos_distribution_adr:
        errors.append(f"incomplete macOS source-distribution decision: {token}")
for token in (
    "versioned repository remains authoritative",
    "GitHub Project is the canonical operational planning surface",
    "GitHub Wiki publication begins only after",
    "18 matched candidate pairs",
    "Wiki must never be the sole source of truth",
):
    if token not in documentation_governance_adr:
        errors.append(f"incomplete documentation-governance decision: {token}")
for token in (
    "16 matched pairs", "32 reports", "final development portability gate",
    "18 matched pairs", "36 candidate reports", "reopens the affected decision",
):
    if token not in candidate_gate_adr:
        errors.append(f"incomplete candidate-selection gate decision: {token}")
for token in (
    "benchmark suite 2", "varint_zigzag_fixed_float", "round-to-nearest, ties-to-even",
    "0.718x", "0.602x", "selected as the integer primitive", "wire version 0 revision 2",
):
    if token not in suite2_adr:
        errors.append(f"incomplete suite 2 candidate decision: {token}")
if godot_commit not in benchmark_config:
    errors.append("benchmark qualification commit does not match GODOT_COMMIT")
for script_name, script_text in (
    ("Linux", benchmark_build_script),
    ("Android", benchmark_android_build),
    ("Windows", benchmark_windows_build),
    ("macOS", benchmark_macos_build),
):
    if "verify_source_consistency.sh" not in script_text:
        errors.append(f"{script_name} benchmark build must run source consistency before SCons")

# Protocol contract and golden packet.
protocol_header = read("src/protocol/tick_synchronizer_packet_codec.h")
protocol_source = read("src/protocol/tick_synchronizer_packet_codec.cpp")
handshake_header = read("src/protocol/tick_synchronizer_handshake.h")
handshake_source = read("src/protocol/tick_synchronizer_handshake.cpp")
state_header = read("src/protocol/tick_synchronizer_handshake_state_machine.h")
module_id_script = read("scripts/compute_module_build_id.py")
scsub = read("SCsub")
for token in (
    "PROTOCOL_MAGIC", "PROTOCOL_MAJOR", "PROTOCOL_MINOR", "CONTROL_HEADER_SIZE",
    "ProtocolCompatibilityProfile", "PUBLIC_API_VERSION", "WIRE_PROTOCOL_VERSION",
    "WIRE_PROTOCOL_REVISION", "EXACT_BUILD_MATCH_REQUIRED", "encode_packet",
    "decode_packet", "inspect_control_header", "encode_hello_payload", "decode_hello_payload",
):
    if token not in protocol_header:
        errors.append(f"missing protocol contract token: {token}")
for token in (
    "ProtocolHandshakeEvaluator", "API_VERSION_MISMATCH", "WIRE_PROTOCOL_VERSION_MISMATCH",
    "WIRE_PROTOCOL_REVISION_MISMATCH", "BUILD_COMPATIBILITY_MISMATCH",
    "GODOT_VERSION_MISMATCH", "ProtocolHandshakeWarning", "GODOT_COMMIT_MISMATCH",
    "evaluate_profiles", "evaluate_hello", "validate_hello_ack",
):
    if token not in handshake_header:
        errors.append(f"missing handshake contract token: {token}")
for token in ("ProtocolHandshakeStateMachine", "start", "receive_packet", "cancel", "close", "build_outbound_packet"):
    if token not in state_header:
        errors.append(f"missing handshake state-machine token: {token}")
for token in ("hashlib.sha256", "git", "module_build_id", "--format"):
    if token not in module_id_script:
        errors.append(f"incomplete module_build_id generator: {token}")
for protocol_text, relative in (
    (protocol_header, "src/protocol/tick_synchronizer_packet_codec.h"),
    (protocol_source, "src/protocol/tick_synchronizer_packet_codec.cpp"),
    (handshake_header, "src/protocol/tick_synchronizer_handshake.h"),
    (handshake_source, "src/protocol/tick_synchronizer_handshake.cpp"),
):
    if "BENCHMARK_SUITE_VERSION" in protocol_text:
        errors.append(f"benchmark suite version must not enter the wire format: {relative}")
if 'add_source_files(module_obj, "src/protocol/*.cpp")' not in scsub:
    errors.append("SCsub does not compile src/protocol/*.cpp")
if 'add_source_files(module_obj, "src/public/*.cpp")' not in scsub:
    errors.append("SCsub does not compile src/public/*.cpp")
if "TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK" not in build_script:
    errors.append("the build pipeline does not require the protocol smoke marker")

expected_hello_golden = bytes.fromhex(
    "54 53 59 4E 01 01 01 28 00 00 00 00 "
    "EF CD AB 89 67 45 23 01 40 30 20 10 "
    "08 07 06 05 04 03 02 01 90 00 00 00 "
    "80 04 00 00 04 02 00 00 11 22 33 44 "
    "55 66 77 88 04 00 00 00 00 00 00 00 "
    "02 00 00 00 34 2E 37 2E 31 2D 73 74 "
    "61 62 6C 65 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 00 00 00 00 "
    "01 02 03 04 05 06 07 08 "
    "09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 "
    "21 22 23 24 25 26 27 28 29 2A 2B 2C "
    "2D 2E 2F 30 31 32 33 34 41 42 43 44 "
    "45 46 47 48 49 4A 4B 4C 4D 4E 4F 50 "
    "61 62 63 64 65 66 67 68 69 6A 6B 6C "
    "6D 6E 6F 70 07 00 00 00 00 00 00 00 "
    "03 00 00 00 00 00 00 00"
)
golden_path = module / "tests/golden/control_hello_v4.bin"
if golden_path.is_file() and golden_path.read_bytes() != expected_hello_golden:
    errors.append("control_hello_v4.bin differs from the experimental wire 0 revision 2 golden packet")

protocol_tests = read("tests/test_tick_synchronizer_packet_codec.h").count("TEST_CASE(")
handshake_tests = read("tests/test_tick_synchronizer_handshake.h").count("TEST_CASE(")
state_tests = read("tests/test_tick_synchronizer_handshake_state_machine.h").count("TEST_CASE(")
if protocol_tests < 29:
    errors.append(f"only {protocol_tests} packet codec tests; minimum expected: 29")
if handshake_tests < 30:
    errors.append(f"only {handshake_tests} handshake evaluator tests; minimum expected: 30")
if state_tests < 34:
    errors.append(f"only {state_tests} handshake state-machine tests; minimum expected: 34")

# Documentation must explicitly preserve both supported module layouts.
layout_text = "\n".join(
    read(relative)
    for relative in (
        "README.md", "AGENTS.md", "documentation/BUILD.md",
        "documentation/ARCHITECTURE.md", "documentation/GODOT_COMPATIBILITY.md",
        "documentation/adr/0002-external-module-layout.md",
    )
)
for token in ("custom_modules", "godot/modules/tick_synchronizer"):
    if token not in layout_text:
        errors.append(f"documentation does not describe both supported module layouts: {token}")

build_script_layout = read("scripts/build_and_validate.sh")
for token in (
    'MODULE_LAYOUT="unknown"',
    "modules/tick_synchronizer",
    'target_args+=("custom_modules=${CUSTOM_MODULES}")',
    "get_godot_dirty_status",
):
    if token not in build_script_layout:
        errors.append(f"build script lacks dual-layout support token: {token}")
if 'if [[ -n "$CUSTOM_MODULES" ]]' not in build_script_layout:
    errors.append("build script does not omit custom_modules for in-tree builds")

# Public documentation must retain the repository privacy boundary.
privacy_documentation = read("documentation/PRIVACY.md")
for token in (
    "credentials",
    "private local paths",
    "operator identity",
    "serial identifiers",
    "unpublished qualification artifacts",
    "Git remote URLs",
    "Private handoff packages",
):
    if token not in privacy_documentation:
        errors.append(f"privacy documentation is incomplete: {token}")

public_documentation = "\n".join(
    read(path.relative_to(module).as_posix())
    for path in sorted(module.rglob("*.md"))
    if path.is_file() and not is_generated_artifact(path)
)
for pattern, label in (
    (r"/home/[^\s`]+", "private Linux home path"),
    (r"/mnt/[^\s`]+", "private mounted-volume path"),
    (r"/Users/[^\s`]+", "private macOS home path"),
    (r"[A-Za-z]:[\\/]Users[\\/][^\s`]+", "private Windows home path"),
):
    if re.search(pattern, public_documentation):
        errors.append(f"public documentation contains a {label}")

# All project-owned C++ files require two leading purpose lines.
cpp_files = sorted(
    path for path in module.rglob("*")
    if path.is_file() and path.suffix in {".h", ".cpp"}
    and not is_generated_artifact(path)
)
for path in cpp_files:
    lines = path.read_text(encoding="utf-8").splitlines()
    if len(lines) < 2 or not lines[0].startswith("// ") or not lines[1].startswith("// "):
        errors.append(f"missing two-line file purpose comment: {path.relative_to(module)}")


def strip_cpp_comments_and_literals(text: str) -> str:
    pattern = re.compile(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )
    return pattern.sub(lambda match: "\n" * match.group(0).count("\n"), text)


def find_cpp_policy_violations(
    paths: list[pathlib.Path],
    rules: tuple[tuple[str, re.Pattern[str]], ...],
) -> None:
    for path in paths:
        source = strip_cpp_comments_and_literals(path.read_text(encoding="utf-8"))
        for feature, pattern in rules:
            match = pattern.search(source)
            if match is None:
                continue
            line = source.count("\n", 0, match.start()) + 1
            errors.append(
                f"C++ policy violation ({feature}) in {path.relative_to(module)}:{line}"
            )


exception_and_rtti_rules = (
    ("try block", re.compile(r"\btry\b")),
    ("catch block", re.compile(r"\bcatch\s*\(")),
    ("throw expression", re.compile(r"\bthrow\b")),
    ("dynamic cast", re.compile(r"\bdynamic_cast\s*<")),
    ("type identification", re.compile(r"\btypeid\s*\(")),
)
module_cpp_files = sorted(
    path
    for root in (module / "src", module / "tests")
    for path in root.rglob("*")
    if path.is_file() and path.suffix in {".h", ".cpp"}
)
module_cpp_files.extend(
    path
    for path in (module / "register_types.h", module / "register_types.cpp")
    if path.is_file()
)
module_only_rules = exception_and_rtti_rules + (
    ("auto type inference", re.compile(r"\bauto\b")),
    (
        "avoidable lambda",
        re.compile(
            r"(?<![A-Za-z0-9_\]])\[[^\]\n]*\]\s*(?:\([^;{}]*\))?\s*(?:mutable\s*)?"
            r"(?:noexcept\s*)?(?:->\s*[^{}]+)?\s*\{"
        ),
    ),
    (
        "STL container",
        re.compile(
            r"\bstd::(?:any|array|basic_string|deque|forward_list|function|list|map|"
            r"optional|pair|set|shared_ptr|span|string|string_view|tuple|unique_ptr|"
            r"unordered_map|unordered_set|variant|vector|weak_ptr)\b"
        ),
    ),
)
find_cpp_policy_violations(module_cpp_files, module_only_rules)

benchmark_cpp_files = sorted(
    path
    for path in (module / "benchmarks").rglob("*")
    if path.is_file() and path.suffix in {".h", ".cpp"}
    and not is_generated_artifact(path)
)
find_cpp_policy_violations(benchmark_cpp_files, exception_and_rtti_rules)

# Public/protocol/benchmark method declarations require an immediately preceding concise comment.
comment_headers = sorted(
    list((module / "src/public").glob("*.h"))
    + list((module / "src/protocol").glob("*.h"))
    + list((module / "benchmarks").glob("*.h"))
    + list((module / "benchmarks/candidates").glob("*.h"))
    + [module / "register_types.h"]
)
method_line = re.compile(
    r"^\s*(?:explicit\s+)?(?:static\s+)?(?:inline\s+)?(?:virtual\s+)?"
    r"[^#/;{}]*\b[A-Za-z_~][A-Za-z0-9_]*\s*\([^;{}]*$"
)
for path in comment_headers:
    if not path.is_file():
        continue
    lines = path.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not method_line.match(line):
            continue
        if stripped.startswith(("if (", "for (", "while (", "switch (", "return ", "throw ")):
            continue
        # Ignore call expressions, enum initializers, and continuation arguments.
        declaration_prefix = line.split("(", 1)[0]
        if any(token in declaration_prefix for token in (".", "->", "=", ",")):
            continue
        if stripped.startswith(("static_cast", "reinterpret_cast", "const_cast", "dynamic_cast")):
            continue
        if stripped.endswith(",") or stripped.endswith("));"):
            continue
        previous = index - 1
        while previous >= 0 and not lines[previous].strip():
            previous -= 1
        if previous < 0 or not lines[previous].lstrip().startswith("//"):
            errors.append(f"method declaration lacks a purpose comment: {path.relative_to(module)}:{index + 1}")

# English-only policy. Identifiers are ignored; encoded legacy terms avoid
# embedding non-English prose in this validator itself.
text_suffixes = {".md", ".sh", ".ps1", ".py", ".gd", ".xml", ".txt", ".h", ".cpp", ".json"}
text_names = {".gitignore", "SConstruct", "SCsub"}
legacy_word_hex = (
    "6e616f", "6172717569766f", "6172717569766f73", "6d6f64756c6f",
    "64697265746f72696f", "707265636973616f", "76616c69646163616f",
    "646f63756d656e746163616f", "636f6d70696c6163616f", "657865637563616f",
    "6f7063616f", "6f70636f6573", "6172766f7265", "66616c686f75",
    "617573656e7465", "736f6d656e7465", "6170656e6173", "70726f78696d6f",
    "70726f78696d61", "6c656974757261", "65736372697461",
)
legacy_words = [bytes.fromhex(value).decode("ascii") for value in legacy_word_hex]
accented_pattern = re.compile(r"[\u00c0-\u00ff]")
legacy_word_pattern = re.compile(
    r"\b(?:" + "|".join(re.escape(word) for word in legacy_words) + r")\b",
    re.IGNORECASE,
)
for path in sorted(module.rglob("*")):
    if not path.is_file() or (
        path.suffix.lower() not in text_suffixes and path.name not in text_names
    ):
        continue
    if is_generated_artifact(path):
        continue
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        continue
    lower_text = text.lower()
    for forbidden_term in forbidden_build_terms:
        if forbidden_term in lower_text:
            line = lower_text.count("\n", 0, lower_text.index(forbidden_term)) + 1
            errors.append(
                f"alternative build-system reference in {path.relative_to(module)}:{line}"
            )
    match = accented_pattern.search(text) or legacy_word_pattern.search(text)
    if match:
        line = text.count("\n", 0, match.start()) + 1
        errors.append(
            f"non-English repository text in {path.relative_to(module)}:{line}: {match.group(0)!r}"
        )

if errors:
    print("TickSynchronizer source consistency check failed:", file=sys.stderr)
    for error in errors:
        print(f"- {error}", file=sys.stderr)
    raise SystemExit(1)

print(
    "TICKSYNCHRONIZER_VERSION_CONTRACT_OK "
    f"api={api_version} wire={wire_version} revision={wire_revision} "
    f"benchmark={benchmark_version} stable={'yes' if wire_version else 'no'} "
    f"exact_build_match={'yes' if exact_match else 'no'}"
)
print(
    "TICKSYNCHRONIZER_BENCHMARK_SUITE_OK "
    f"suite={benchmark_version} datasets={len(required_datasets)} "
    "candidates=reference_fixed_width,varint_zigzag_fixed_float "
    "backends=linux,windows,android,macos schema=3"
)
print(
    "TICKSYNCHRONIZER_DOCUMENTATION_POLICY_OK "
    f"language=english cpp_file_headers={len(cpp_files)} benchmark_decisions=yes "
    "module_layouts=external,in-tree records=separated"
)
print(
    "TICKSYNCHRONIZER_CPP_POLICY_OK "
    "module=godot-subset benchmark=exception-free,rtti-free"
)
print(f"TICKSYNCHRONIZER_SOURCE_CONSISTENCY_OK methods={len(documented)} tests={test_cases}")
PY

"$MODULE_DIR/scripts/compare_protocol_benchmarks.py" --self-test
"$MODULE_DIR/scripts/analyze_protocol_candidates.py" --self-test
"$MODULE_DIR/scripts/verify_benchmark_results.py" --self-test
"$MODULE_DIR/scripts/verify_mermaid_diagrams.py" --root "$MODULE_DIR" --min-diagrams 19

#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_API_VERSION="5"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
VERSION_HEADER="${TICKSYNC_VERSION_HEADER:-${MODULE_DIR}/src/internal/tick_synchronizer_version.h}"
PRINT_ACTION=""
MODULE_API_VERSION=""
WIRE_PROTOCOL_VERSION=""
WIRE_PROTOCOL_REVISION=""
BENCHMARK_SUITE_VERSION=""
EXACT_BUILD_MATCH_REQUIRED=""

default_godot_dir() {
    local in_tree_root
    in_tree_root="$(realpath -m -- "${MODULE_DIR}/../..")"
    if [[ "$(basename -- "$(dirname -- "$MODULE_DIR")")" == "modules" && -f "$in_tree_root/SConstruct" ]]; then
        printf '%s\n' "$in_tree_root"
    else
        realpath -m -- "${MODULE_DIR}/../godot"
    fi
}

GODOT_DIR="${TICKSYNC_GODOT_DIR:-$(default_godot_dir)}"
CUSTOM_MODULES="${TICKSYNC_CUSTOM_MODULES-}"
CUSTOM_MODULES_EXPLICIT="$([[ -v TICKSYNC_CUSTOM_MODULES ]] && printf 1 || printf 0)"
MODULE_LAYOUT="unknown"
PLATFORM="${TICKSYNC_PLATFORM:-linuxbsd}"
PRECISION="${TICKSYNC_PRECISION:-double}"
JOBS="${TICKSYNC_JOBS:-}"
SCONS_BIN="${TICKSYNC_SCONS_BIN:-scons}"
MODE="quick"
EDITOR_DEV_BUILD="yes"
TEST_FILTER="*TickSynchronizer*"
MIN_TEST_CASES=140
RUN_SMOKE=1
CLEAN_FIRST=0
ALLOW_GODOT_MISMATCH=0
ALLOW_DIRTY_GODOT=0
GODOT_BASELINE="${TICKSYNC_GODOT_BASELINE_COMMIT:-}"
EXTRA_SCONS_ARGS=()

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/build_and_validate.sh [options]

Modes:
  quick       Builds the editor and runs C++ tests and the smoke test. Default.
  editor      Same as quick; explicit name for editor tasks.
  templates   Builds and validates template_debug and template_release.
  all         Runs editor/tests/smoke and both templates.

Options:
  --mode MODE                 quick, editor, templates, or all.
  --precision MODE            double or single. Default: double.
  --godot-dir PATH            Godot source tree. Auto-detected for external and in-tree layouts.
  --custom-modules PATH       Explicit custom_modules value. Omitted for an in-tree module.
  --platform NAME             SCons platform. Default: linuxbsd.
  --jobs N                    Number of parallel jobs.
  --scons-bin COMMAND         SCons executable. Default: scons.
  --editor-dev-build yes|no   Sets editor dev_build. Default: yes.
  --test-filter FILTER        Doctest filter. Default: *TickSynchronizer*.
  --min-test-cases N          Minimum accepted count. Default: 140.
  --clean-first               Cleans each configuration before building.
  --no-smoke                  Does not run the GDScript smoke test.
  --scons-arg ARG             Additional SCons argument; may be repeated.
  --godot-baseline REF        Overrides GODOT_COMMIT for diagnostics.
  --allow-godot-mismatch      Allows HEAD to differ from the baseline.
  --allow-dirty-godot         Allows local engine changes.
  --version-header PATH       Central version-contract header.
  --print-script-api-version  Prints this script interface version.
  --print-api-version         Prints the module public API version.
  --print-wire-protocol-version
                              Prints the stable wire protocol version.
  --print-wire-protocol-revision
                              Prints the experimental wire revision.
  --print-benchmark-suite-version
                              Prints the benchmark suite version.
  --print-version-contract    Prints the complete version contract.
  -h, --help                  Shows this help.

Examples:
  ./scripts/build_and_validate.sh --mode quick --precision double
  ./scripts/build_and_validate.sh --mode all --precision single
  ./scripts/build_and_validate.sh --mode templates --clean-first
  ./scripts/build_and_validate.sh --scons-arg use_llvm=yes
USAGE
}

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '[TickSynchronizer] %s\n' "$*" >&2
}

set_print_action() {
    local requested="$1"
    if [[ -n "$PRINT_ACTION" ]]; then
        fail "use only one --print-* option per invocation"
    fi
    PRINT_ACTION="$requested"
}

resolve_version_header() {
    if [[ "$VERSION_HEADER" != /* ]]; then
        VERSION_HEADER="$(realpath -m -- "${MODULE_DIR}/${VERSION_HEADER}")"
    else
        VERSION_HEADER="$(realpath -m -- "$VERSION_HEADER")"
    fi
}

load_version_contract() {
    resolve_version_header
    [[ -f "$VERSION_HEADER" ]] || \
        fail "central version header not found: $VERSION_HEADER"
    command -v python3 >/dev/null 2>&1 || \
        fail "python3 is required to read the version contract"

    local parsed
    parsed="$(
        python3 - "$VERSION_HEADER" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

# Remove comments before matching constants, avoiding false positives in
# documentation blocks and commented-out declarations.
text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
text = re.sub(r"//[^\n]*", "", text)

integer_names = (
    "API_VERSION",
    "WIRE_PROTOCOL_VERSION",
    "WIRE_PROTOCOL_REVISION",
    "BENCHMARK_SUITE_VERSION",
)

values: dict[str, int] = {}
for name in integer_names:
    matches = re.findall(
        rf"\b{name}\b\s*=\s*(0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*\s*;",
        text,
    )
    if len(matches) != 1:
        print(
            f"{path}: expected exactly one value for {name}; found {len(matches)}",
            file=sys.stderr,
        )
        raise SystemExit(1)
    values[name] = int(matches[0], 0)

bool_matches = re.findall(
    r"\bEXACT_BUILD_MATCH_REQUIRED\b\s*=\s*(true|false)\s*;",
    text,
)
if len(bool_matches) != 1:
    print(
        f"{path}: expected exactly one value for EXACT_BUILD_MATCH_REQUIRED; "
        f"found {len(bool_matches)}",
        file=sys.stderr,
    )
    raise SystemExit(1)

if values["API_VERSION"] <= 0:
    print(f"{path}: API_VERSION must be greater than zero", file=sys.stderr)
    raise SystemExit(1)

if (
    values["WIRE_PROTOCOL_VERSION"] == 0
    and values["WIRE_PROTOCOL_REVISION"] == 0
):
    print(
        f"{path}: experimental wire protocol requires a nonzero revision",
        file=sys.stderr,
    )
    raise SystemExit(1)

print(values["API_VERSION"])
print(values["WIRE_PROTOCOL_VERSION"])
print(values["WIRE_PROTOCOL_REVISION"])
print(values["BENCHMARK_SUITE_VERSION"])
print("yes" if bool_matches[0] == "true" else "no")
PY
    )" || fail "could not read the version contract from $VERSION_HEADER"

    local -a version_values=()
    mapfile -t version_values <<< "$parsed"
    [[ "${#version_values[@]}" -eq 5 ]] || \
        fail "incomplete version contract in $VERSION_HEADER"

    MODULE_API_VERSION="${version_values[0]}"
    WIRE_PROTOCOL_VERSION="${version_values[1]}"
    WIRE_PROTOCOL_REVISION="${version_values[2]}"
    BENCHMARK_SUITE_VERSION="${version_values[3]}"
    EXACT_BUILD_MATCH_REQUIRED="${version_values[4]}"
}

print_version_contract() {
    local wire_stable="yes"
    if [[ "$WIRE_PROTOCOL_VERSION" == "0" ]]; then
        wire_stable="no"
    fi

    printf 'script_api=%s\n' "$SCRIPT_API_VERSION"
    printf 'api=%s\n' "$MODULE_API_VERSION"
    printf 'wire=%s\n' "$WIRE_PROTOCOL_VERSION"
    printf 'wire_revision=%s\n' "$WIRE_PROTOCOL_REVISION"
    printf 'benchmark_suite=%s\n' "$BENCHMARK_SUITE_VERSION"
    printf 'wire_stable=%s\n' "$wire_stable"
    printf 'exact_build_match=%s\n' "$EXACT_BUILD_MATCH_REQUIRED"
    printf 'version_header=%s\n' "$VERSION_HEADER"
}

command_string() {
    printf '%q ' "$@"
    printf '\n'
}

get_default_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.logicalcpu 2>/dev/null || printf '1\n'
    else
        printf '1\n'
    fi
}

resolve_path() {
    local base="$1"
    local path="$2"
    (
        cd -- "$base"
        realpath -m -- "$path"
    )
}

configure_module_layout() {
    local expected_in_tree
    expected_in_tree="$(realpath -m -- "${GODOT_DIR}/modules/tick_synchronizer")"

    if [[ "$CUSTOM_MODULES_EXPLICIT" == "0" && "$MODULE_DIR" == "$expected_in_tree" ]]; then
        MODULE_LAYOUT="in-tree"
        CUSTOM_MODULES=""
        return
    fi

    MODULE_LAYOUT="external"
    if [[ -z "$CUSTOM_MODULES" ]]; then
        CUSTOM_MODULES="$(python3 - "$GODOT_DIR" "$MODULE_DIR" <<'PY_REL'
import os
import sys
print(os.path.relpath(sys.argv[2], sys.argv[1]))
PY_REL
)"
    fi
}

get_godot_dirty_status() {
    if [[ "$MODULE_LAYOUT" == "in-tree" ]]; then
        git -C "$GODOT_DIR" status --porcelain --untracked-files=all -- \
            . ':(exclude)modules/tick_synchronizer'
    else
        git -C "$GODOT_DIR" status --porcelain --untracked-files=all
    fi
}

get_scons_arg_value() {
    local key="$1"
    local default_value="$2"
    local arg
    local value="$default_value"
    for arg in "${EXTRA_SCONS_ARGS[@]}"; do
        if [[ "$arg" == "${key}="* ]]; then
            value="${arg#*=}"
        fi
    done
    printf '%s\n' "$value"
}

is_sanitized_build() {
    local key
    for key in use_asan use_ubsan use_lsan use_tsan use_msan; do
        if [[ "$(get_scons_arg_value "$key" no)" == "yes" ]]; then
            return 0
        fi
    done
    return 1
}

verify_inputs() {
    [[ "$PRECISION" == "double" || "$PRECISION" == "single" ]] || \
        fail "invalid precision: $PRECISION"
    [[ "$MODE" == "quick" || "$MODE" == "editor" || "$MODE" == "templates" || "$MODE" == "all" ]] || \
        fail "invalid mode: $MODE"
    [[ "$EDITOR_DEV_BUILD" == "yes" || "$EDITOR_DEV_BUILD" == "no" ]] || \
        fail "--editor-dev-build accepts only yes or no"
    [[ "$MIN_TEST_CASES" =~ ^[0-9]+$ ]] || fail "--min-test-cases must be a nonnegative integer"
    [[ -d "$GODOT_DIR" ]] || fail "Godot directory not found: $GODOT_DIR"
    [[ -f "$GODOT_DIR/SConstruct" ]] || fail "SConstruct not found in: $GODOT_DIR"
    [[ -f "$MODULE_DIR/SCsub" ]] || fail "module SCsub not found"
    [[ -f "$MODULE_DIR/config.py" ]] || fail "module config.py not found"
    [[ -x "$SCRIPT_DIR/build_and_validate.sh" ]] || fail "script not executable"
    [[ -x "$SCRIPT_DIR/verify_source_consistency.sh" ]] || fail "verify_source_consistency.sh missing or not executable"
    command -v "$SCONS_BIN" >/dev/null 2>&1 || fail "SCons not found: $SCONS_BIN"
    command -v git >/dev/null 2>&1 || fail "git not found"
    command -v python3 >/dev/null 2>&1 || fail "python3 not found"

    "$SCRIPT_DIR/verify_source_consistency.sh" >/dev/null || \
        fail "module files belong to incompatible revisions; extract the complete package again"

    if [[ "$MODULE_LAYOUT" == "external" ]]; then
        local custom_modules_resolved
        custom_modules_resolved="$(resolve_path "$GODOT_DIR" "$CUSTOM_MODULES")"
        [[ "$custom_modules_resolved" == "$MODULE_DIR" ]] || \
            fail "custom_modules resolves to '$custom_modules_resolved', but the module is at '$MODULE_DIR'"
    else
        local expected_in_tree
        expected_in_tree="$(realpath -m -- "${GODOT_DIR}/modules/tick_synchronizer")"
        [[ "$MODULE_DIR" == "$expected_in_tree" ]] || \
            fail "in-tree module must be located at '$expected_in_tree'"
    fi

    if [[ -z "$JOBS" ]]; then
        JOBS="$(get_default_jobs)"
    fi
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "invalid jobs value: $JOBS"
}

verify_godot_baseline() {
    git -C "$GODOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
        fail "the Godot tree is not a Git repository"

    local configured_version="unknown"
    if [[ -f "$MODULE_DIR/GODOT_VERSION" ]]; then
        configured_version="$(tr -d '[:space:]' < "$MODULE_DIR/GODOT_VERSION")"
    fi

    if [[ -z "$GODOT_BASELINE" ]]; then
        [[ -f "$MODULE_DIR/GODOT_COMMIT" ]] || fail "GODOT_COMMIT not found"
        GODOT_BASELINE="$(tr -d '[:space:]' < "$MODULE_DIR/GODOT_COMMIT")"
    fi
    [[ -n "$GODOT_BASELINE" ]] || fail "empty baseline"

    local baseline_full
    local head_full
    baseline_full="$(git -C "$GODOT_DIR" rev-parse "${GODOT_BASELINE}^{commit}" 2>/dev/null)" || \
        fail "could not resolve baseline '$GODOT_BASELINE' in the Godot tree"
    head_full="$(git -C "$GODOT_DIR" rev-parse HEAD)"

    if [[ "$head_full" != "$baseline_full" ]]; then
        if (( ALLOW_GODOT_MISMATCH )); then
            printf 'WARNING: Godot HEAD (%s) differs from baseline (%s).\n' "$head_full" "$baseline_full" >&2
        else
            fail "Godot HEAD differs from baseline $configured_version ($baseline_full)"
        fi
    fi

    local dirty_status
    dirty_status="$(get_godot_dirty_status)"
    if [[ -n "$dirty_status" ]]; then
        if (( ALLOW_DIRTY_GODOT )); then
            printf 'WARNING: Godot tree has local changes:\n%s\n' "$dirty_status" >&2
        else
            fail "The Godot tree has local changes; the project forbids engine modifications"
        fi
    fi

    RESOLVED_GODOT_BASELINE="$baseline_full"
    GODOT_HEAD="$head_full"
    GODOT_VERSION_LABEL="$configured_version"
}

artifact_matches_configuration() {
    local artifact="$1"
    local target="$2"
    local filename
    filename="$(basename -- "$artifact")"

    if [[ "$PRECISION" == "double" ]]; then
        [[ "$filename" == *.double.* || "$filename" == *.double ]] || return 1
    else
        [[ "$filename" != *.double.* && "$filename" != *.double ]] || return 1
    fi

    local llvm
    llvm="$(get_scons_arg_value use_llvm no)"
    if [[ "$llvm" == "yes" ]]; then
        [[ "$filename" == *.llvm* ]] || return 1
    else
        [[ "$filename" != *.llvm* ]] || return 1
    fi

    if is_sanitized_build; then
        [[ "$filename" == *.san* ]] || return 1
    else
        [[ "$filename" != *.san* ]] || return 1
    fi

    if [[ "$target" == "editor" ]]; then
        if [[ "$EDITOR_DEV_BUILD" == "yes" ]]; then
            [[ "$filename" == *.editor.dev.* ]] || return 1
        else
            [[ "$filename" != *.editor.dev.* ]] || return 1
        fi
    fi

    return 0
}

find_artifact() {
    local target="$1"
    local candidate
    local newest=""
    local newest_mtime=0

    while IFS= read -r -d '' candidate; do
        artifact_matches_configuration "$candidate" "$target" || continue
        local mtime
        mtime="$(stat -c '%Y' "$candidate" 2>/dev/null || stat -f '%m' "$candidate")"
        if (( mtime >= newest_mtime )); then
            newest="$candidate"
            newest_mtime="$mtime"
        fi
    done < <(
        find "$GODOT_DIR/bin" -maxdepth 1 -type f \
            -name "godot.${PLATFORM}.${target}*" -print0 2>/dev/null
    )

    [[ -n "$newest" ]] || return 1
    printf '%s\n' "$newest"
}

write_environment_report() {
    local report="$1"
    {
        printf 'TickSynchronizer build environment\n'
        printf 'Generated UTC: %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
        printf 'Script API: %s\n' "$SCRIPT_API_VERSION"
        printf 'Module API: %s\n' "$MODULE_API_VERSION"
        printf 'Wire protocol version: %s\n' "$WIRE_PROTOCOL_VERSION"
        printf 'Wire protocol revision: %s\n' "$WIRE_PROTOCOL_REVISION"
        printf 'Benchmark suite version: %s\n' "$BENCHMARK_SUITE_VERSION"
        printf 'Exact build match required: %s\n' "$EXACT_BUILD_MATCH_REQUIRED"
        printf 'Version header: %s\n' "$VERSION_HEADER"
        printf 'Module dir: %s\n' "$MODULE_DIR"
        printf 'Godot dir: %s\n' "$GODOT_DIR"
        printf 'Godot version: %s\n' "$GODOT_VERSION_LABEL"
        printf 'Godot baseline: %s\n' "$RESOLVED_GODOT_BASELINE"
        printf 'Godot HEAD: %s\n' "$GODOT_HEAD"
        printf 'Godot branch: %s\n' "$(git -C "$GODOT_DIR" branch --show-current 2>/dev/null || true)"
        printf 'Godot describe: %s\n' "$(git -C "$GODOT_DIR" describe --always --dirty --tags 2>/dev/null || true)"
        printf 'Godot origin: %s\n' "$(git -C "$GODOT_DIR" remote get-url origin 2>/dev/null || true)"
        printf 'Module layout: %s\n' "$MODULE_LAYOUT"
        printf 'custom_modules: %s\n' "${CUSTOM_MODULES:-<omitted>}"
        printf 'Mode: %s\n' "$MODE"
        printf 'Platform: %s\n' "$PLATFORM"
        printf 'Precision: %s\n' "$PRECISION"
        printf 'Editor dev_build: %s\n' "$EDITOR_DEV_BUILD"
        printf 'Test filter: %s\n' "$TEST_FILTER"
        printf 'Minimum test cases: %s\n' "$MIN_TEST_CASES"
        printf 'Jobs: %s\n' "$JOBS"
        printf 'SCons command: %s\n' "$SCONS_BIN"
        printf 'Extra SCons args:'
        printf ' %q' "${EXTRA_SCONS_ARGS[@]}"
        printf '\n\nSystem:\n'
        uname -a 2>/dev/null || true
        command -v lsb_release >/dev/null 2>&1 && lsb_release -a 2>/dev/null || true
        command -v lscpu >/dev/null 2>&1 && lscpu 2>/dev/null || true
        command -v free >/dev/null 2>&1 && free -h 2>/dev/null || true
        printf '\nTools:\n'
        "$SCONS_BIN" --version 2>/dev/null || true
        python3 --version 2>/dev/null || true
        c++ --version 2>/dev/null || true
        clang++ --version 2>/dev/null || true
        ld.lld --version 2>/dev/null || true
    } > "$report"
}

build_target() {
    local target="$1"
    local -a target_args=(
        "platform=${PLATFORM}"
        "target=${target}"
        "precision=${PRECISION}"
        "module_tick_synchronizer_enabled=yes"
    )
    if [[ -n "$CUSTOM_MODULES" ]]; then
        target_args+=("custom_modules=${CUSTOM_MODULES}")
    fi

    if [[ "$target" == "editor" ]]; then
        target_args+=("tests=yes" "dev_build=${EDITOR_DEV_BUILD}")
    fi

    local -a full_args=("${target_args[@]}" "${EXTRA_SCONS_ARGS[@]}")
    local clean_log="$REPORT_DIR/${target}.clean.log"
    local build_log="$REPORT_DIR/${target}.build.log"
    local command_file="$REPORT_DIR/${target}.command.txt"
    local time_file="$REPORT_DIR/${target}.time.txt"

    if (( CLEAN_FIRST )); then
        log "Cleaning ${target}..."
        (
            cd -- "$GODOT_DIR"
            "$SCONS_BIN" --clean "${full_args[@]}"
        ) > "$clean_log" 2>&1 || fail "Clean failed for ${target}. See $clean_log"
    fi

    local -a command=("$SCONS_BIN" "${full_args[@]}" "-j${JOBS}")
    command_string "${command[@]}" > "$command_file"
    log "Building ${target} with precision=${PRECISION}..."

    local status
    set +e
    if [[ -x /usr/bin/time ]]; then
        (
            cd -- "$GODOT_DIR"
            /usr/bin/time -v -o "$time_file" "${command[@]}"
        ) 2>&1 | tee "$build_log"
        status=${PIPESTATUS[0]}
    else
        (
            cd -- "$GODOT_DIR"
            "${command[@]}"
        ) 2>&1 | tee "$build_log"
        status=${PIPESTATUS[0]}
    fi
    set -e

    (( status == 0 )) || fail "Build ${target} failed. See $build_log"

    local artifact
    artifact="$(find_artifact "$target")" || \
        fail "could not locate the ${target} artifact matching this configuration"
    validate_artifact "$target" "$artifact"
    LAST_ARTIFACT="$artifact"
}

validate_artifact() {
    local target="$1"
    local artifact="$2"
    local log_file="$REPORT_DIR/${target}.validation.log"

    [[ -s "$artifact" ]] || fail "empty artifact: $artifact"
    if [[ "$PLATFORM" == "linuxbsd" ]]; then
        [[ -x "$artifact" ]] || fail "artifact is not executable: $artifact"
    fi

    local status
    set +e
    (
        printf 'Target: %s\n' "$target"
        printf 'Precision: %s\n' "$PRECISION"
        printf 'Artifact: %s\n' "$artifact"
        printf 'Size bytes: %s\n' "$(stat -c '%s' "$artifact" 2>/dev/null || stat -f '%z' "$artifact")"
        printf 'Modified: %s\n' "$(stat -c '%y' "$artifact" 2>/dev/null || stat -f '%Sm' "$artifact")"
        file "$artifact" 2>/dev/null || true

        if [[ "$PLATFORM" == "linuxbsd" ]]; then
            if command -v readelf >/dev/null 2>&1; then
                printf '\nELF header:\n'
                readelf -h "$artifact"
            fi

            if command -v ldd >/dev/null 2>&1; then
                printf '\nDynamic dependencies:\n'
                ldd_output="$(ldd "$artifact" 2>&1)"
                printf '%s\n' "$ldd_output"
                if grep -Eq '=>[[:space:]]+not found([[:space:]]|$)' <<<"$ldd_output"; then
                    printf '\nERROR: one or more dynamic dependencies were not found.\n'
                    exit 97
                fi
            fi

            if is_sanitized_build; then
                printf '\nExecution probe:\n'
                printf '%s\n' \
                    'Skipped for sanitized editor. The mandatory filtered C++ tests' \
                    ' and smoke test are the runtime validation.'
            else
                printf '\n--version:\n'
                timeout 30s "$artifact" --version
            fi
        fi
    ) > "$log_file" 2>&1
    status=$?
    set -e

    if (( status != 0 )); then
        printf '--- %s ---\n' "$log_file" >&2
        tail -n 160 "$log_file" >&2 || true
        fail "artifact validation failed: $artifact"
    fi

    log "${target} validated: ${artifact}"
}

run_cpp_tests() {
    local editor="$1"
    local log_file="$REPORT_DIR/editor.tests.log"
    log "Running C++ tests with filter ${TEST_FILTER}..."

    local status
    set +e
    timeout 900s "$editor" --test --test-case="$TEST_FILTER" 2>&1 | tee "$log_file"
    status=${PIPESTATUS[0]}
    set -e
    (( status == 0 )) || fail "C++ tests failed. See $log_file"

    local found
    found="$(sed -nE 's/.*test cases:[[:space:]]*([0-9]+).*/\1/p' "$log_file" | tail -n 1)"
    [[ -n "$found" ]] || fail "could not determine the test count in $log_file"
    (( found >= MIN_TEST_CASES )) || \
        fail "only ${found} tests found; minimum expected: ${MIN_TEST_CASES}"
    TEST_CASES_FOUND="$found"
}

run_smoke_test() {
    local editor="$1"
    local log_file="$REPORT_DIR/editor.smoke.log"
    log "Running the GDScript smoke test through the editor..."

    local status
    set +e
    TICKSYNC_EXPECTED_PRECISION="$PRECISION" \
        timeout 120s "$editor" \
        --headless \
        --path "$MODULE_DIR/tests/smoke_project" \
        --quit-after 600 2>&1 | tee "$log_file"
    status=${PIPESTATUS[0]}
    set -e
    (( status == 0 )) || fail "smoke test failed. See $log_file"

    local marker
    for marker in \
        "TICKSYNCHRONIZER_BUILD_PRECISION=${PRECISION}" \
        "TICKSYNCHRONIZER_BUFFER_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_INTEGER_CODEC_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_FLOAT_CODEC_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_RESOURCE_LIMIT_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_SMOKE_TEST_OK"; do
        grep -Fq "$marker" "$log_file" || fail "missing smoke-test marker: $marker"
    done
}

write_artifact_record() {
    local label="$1"
    local artifact="$2"
    local size
    local hash
    size="$(stat -c '%s' "$artifact" 2>/dev/null || stat -f '%z' "$artifact")"
    hash="$(sha256sum "$artifact" | awk '{print $1}')"
    printf '%s\t%s\t%s\t%s\n' "$label" "$artifact" "$size" "$hash" >> "$REPORT_DIR/artifacts.tsv"
    printf '%s  %s\n' "$hash" "$artifact" >> "$REPORT_DIR/sha256sums.txt"
}

write_summary() {
    {
        printf 'TickSynchronizer validation summary\n'
        printf 'Status: SUCCESS\n'
        printf 'Generated UTC: %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
        printf 'Script API: %s\n' "$SCRIPT_API_VERSION"
        printf 'Module API: %s\n' "$MODULE_API_VERSION"
        printf 'Wire protocol version: %s\n' "$WIRE_PROTOCOL_VERSION"
        printf 'Wire protocol revision: %s\n' "$WIRE_PROTOCOL_REVISION"
        printf 'Benchmark suite version: %s\n' "$BENCHMARK_SUITE_VERSION"
        printf 'Exact build match required: %s\n' "$EXACT_BUILD_MATCH_REQUIRED"
        printf 'Module layout: %s\n' "$MODULE_LAYOUT"
        printf 'Mode: %s\n' "$MODE"
        printf 'Platform: %s\n' "$PLATFORM"
        printf 'Precision: %s\n' "$PRECISION"
        printf 'Godot version: %s\n' "$GODOT_VERSION_LABEL"
        printf 'Godot baseline: %s\n' "$RESOLVED_GODOT_BASELINE"
        printf 'Godot HEAD: %s\n' "$GODOT_HEAD"
        printf 'Test filter: %s\n' "$TEST_FILTER"
        printf 'Minimum test cases: %s\n' "$MIN_TEST_CASES"
        printf 'Test cases found: %s\n' "${TEST_CASES_FOUND:-not-run}"
        printf 'Smoke test: %s\n' "$([[ $RUN_SMOKE -eq 1 && ( "$MODE" == quick || "$MODE" == editor || "$MODE" == all ) ]] && printf passed || printf not-run)"
        printf '\nArtifacts:\n'
        cat "$REPORT_DIR/artifacts.tsv" 2>/dev/null || true
    } > "$REPORT_DIR/summary.txt"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --print-script-api-version)
            set_print_action "script-api"
            shift
            ;;
        --print-api-version)
            set_print_action "module-api"
            shift
            ;;
        --print-wire-protocol-version)
            set_print_action "wire-version"
            shift
            ;;
        --print-wire-protocol-revision)
            set_print_action "wire-revision"
            shift
            ;;
        --print-benchmark-suite-version)
            set_print_action "benchmark-suite"
            shift
            ;;
        --print-version-contract)
            set_print_action "version-contract"
            shift
            ;;
        --version-header)
            [[ $# -ge 2 ]] || fail "--version-header requires a value"
            VERSION_HEADER="$2"
            shift 2
            ;;
        --mode)
            [[ $# -ge 2 ]] || fail "--mode requires a value"
            MODE="$2"
            shift 2
            ;;
        --precision)
            [[ $# -ge 2 ]] || fail "--precision requires a value"
            PRECISION="$2"
            shift 2
            ;;
        --godot-dir)
            [[ $# -ge 2 ]] || fail "--godot-dir requires a value"
            GODOT_DIR="$2"
            shift 2
            ;;
        --custom-modules)
            [[ $# -ge 2 ]] || fail "--custom-modules requires a value"
            CUSTOM_MODULES="$2"
            CUSTOM_MODULES_EXPLICIT=1
            shift 2
            ;;
        --platform)
            [[ $# -ge 2 ]] || fail "--platform requires a value"
            PLATFORM="$2"
            shift 2
            ;;
        --jobs)
            [[ $# -ge 2 ]] || fail "--jobs requires a value"
            JOBS="$2"
            shift 2
            ;;
        --scons-bin)
            [[ $# -ge 2 ]] || fail "--scons-bin requires a value"
            SCONS_BIN="$2"
            shift 2
            ;;
        --editor-dev-build)
            [[ $# -ge 2 ]] || fail "--editor-dev-build requires a value"
            EDITOR_DEV_BUILD="$2"
            shift 2
            ;;
        --test-filter)
            [[ $# -ge 2 ]] || fail "--test-filter requires a value"
            TEST_FILTER="$2"
            shift 2
            ;;
        --min-test-cases)
            [[ $# -ge 2 ]] || fail "--min-test-cases requires a value"
            MIN_TEST_CASES="$2"
            shift 2
            ;;
        --clean-first)
            CLEAN_FIRST=1
            shift
            ;;
        --no-smoke)
            RUN_SMOKE=0
            shift
            ;;
        --scons-arg)
            [[ $# -ge 2 ]] || fail "--scons-arg requires a value"
            EXTRA_SCONS_ARGS+=("$2")
            shift 2
            ;;
        --godot-baseline)
            [[ $# -ge 2 ]] || fail "--godot-baseline requires a value"
            GODOT_BASELINE="$2"
            shift 2
            ;;
        --allow-godot-mismatch)
            ALLOW_GODOT_MISMATCH=1
            shift
            ;;
        --allow-dirty-godot)
            ALLOW_DIRTY_GODOT=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1"
            ;;
    esac
done

GODOT_DIR="$(realpath -m -- "$GODOT_DIR")"

if [[ "$PRINT_ACTION" == "script-api" ]]; then
    printf '%s\n' "$SCRIPT_API_VERSION"
    exit 0
fi

load_version_contract

case "$PRINT_ACTION" in
    "")
        ;;
    module-api)
        printf '%s\n' "$MODULE_API_VERSION"
        exit 0
        ;;
    wire-version)
        printf '%s\n' "$WIRE_PROTOCOL_VERSION"
        exit 0
        ;;
    wire-revision)
        printf '%s\n' "$WIRE_PROTOCOL_REVISION"
        exit 0
        ;;
    benchmark-suite)
        printf '%s\n' "$BENCHMARK_SUITE_VERSION"
        exit 0
        ;;
    version-contract)
        print_version_contract
        exit 0
        ;;
    *)
        fail "invalid internal print action: $PRINT_ACTION"
        ;;
esac

configure_module_layout
verify_inputs
verify_godot_baseline

REPORT_TAG="$(date -u +'%Y%m%dT%H%M%SZ')-${PLATFORM}-${PRECISION}-${MODE}"
REPORT_DIR="$MODULE_DIR/build_reports/$REPORT_TAG"
mkdir -p "$REPORT_DIR"
ln -sfn "$REPORT_TAG" "$MODULE_DIR/build_reports/latest"
: > "$REPORT_DIR/artifacts.tsv"
: > "$REPORT_DIR/sha256sums.txt"
write_environment_report "$REPORT_DIR/environment.txt"

log "Reports: $REPORT_DIR"
log "Godot: $GODOT_DIR"
log "Baseline: $GODOT_VERSION_LABEL ($RESOLVED_GODOT_BASELINE)"
log "Module: $MODULE_DIR"
log "Module layout: $MODULE_LAYOUT"
log "Mode: $MODE"
log "Precision: $PRECISION"

EDITOR_ARTIFACT=""
if [[ "$MODE" == "quick" || "$MODE" == "editor" || "$MODE" == "all" ]]; then
    build_target editor
    EDITOR_ARTIFACT="$LAST_ARTIFACT"
    write_artifact_record editor "$EDITOR_ARTIFACT"
fi

if [[ "$MODE" == "templates" || "$MODE" == "all" ]]; then
    build_target template_debug
    TEMPLATE_DEBUG_ARTIFACT="$LAST_ARTIFACT"
    write_artifact_record template_debug "$TEMPLATE_DEBUG_ARTIFACT"
    build_target template_release
    TEMPLATE_RELEASE_ARTIFACT="$LAST_ARTIFACT"
    write_artifact_record template_release "$TEMPLATE_RELEASE_ARTIFACT"
fi

if [[ -n "$EDITOR_ARTIFACT" ]]; then
    run_cpp_tests "$EDITOR_ARTIFACT"
    if (( RUN_SMOKE )); then
        run_smoke_test "$EDITOR_ARTIFACT"
    fi
fi

write_summary
log "Build and validation completed."
log "Summary: $REPORT_DIR/summary.txt"
if [[ -n "$EDITOR_ARTIFACT" ]]; then
    log "C++ tests: $REPORT_DIR/editor.tests.log"
    if (( RUN_SMOKE )); then
        log "Smoke test: $REPORT_DIR/editor.smoke.log"
    fi
fi

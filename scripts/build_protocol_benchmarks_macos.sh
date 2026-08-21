#!/usr/bin/env bash
# Builds Universal 2 standalone benchmarks locally on macOS through SCons.
# Validates both slices, system-only runtime dependencies, and native self-tests.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BENCHMARK_DIR="${MODULE_DIR}/benchmarks"
PRECISION="all"
JOBS=""
SCONS_BIN="${TICKSYNC_SCONS_BIN:-scons}"
CXX_BIN=""
SDK_PATH=""
DEPLOYMENT_TARGET="12.0"
CLEAN_FIRST=0
LTO="no"
EXPORT_PACKAGE=1
EXPORT_OUTPUT_DIR=""
SOURCE_HEAD_BEFORE=""
SOURCE_STATUS_BEFORE=""

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '[TickSynchronizer] %s\n' "$*" >&2
}

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/build_protocol_benchmarks_macos.sh [options]

Options:
  --precision single|double|all  Precision. Default: all.
  --jobs N                      Parallel build jobs.
  --scons-bin COMMAND           SCons executable. Default: scons.
  --cxx PATH                    Apple Clang C++ driver. Default: xcrun clang++.
  --sdk PATH                    macOS SDK. Default: xcrun macosx SDK.
  --deployment-target VERSION  Minimum macOS version. Default: 12.0.
  --lto                         Enables LTO.
  --clean-first                 Cleans each thin SCons target before building.
  --no-export-package           Builds and validates without exporting a ZIP.
  --output-dir PATH             Deployment-package output directory.
  -h, --help                    Shows this help.

The build host needs Apple Clang, a macOS SDK, Python, SCons, and Git. The exported
Universal 2 package is execution-only and does not carry or require those tools.
USAGE
}

get_jobs() {
    /usr/sbin/sysctl -n hw.logicalcpu 2>/dev/null || printf '1\n'
}

resolve_build_tools() {
    local compiler_version
    if [[ -z "$CXX_BIN" || -z "$SDK_PATH" ]]; then
        command -v xcrun >/dev/null 2>&1 || \
            fail "xcrun is required unless both --cxx and --sdk are supplied"
    fi
    if [[ -z "$CXX_BIN" ]]; then
        CXX_BIN="$(xcrun --sdk macosx --find clang++)"
    fi
    if [[ -z "$SDK_PATH" ]]; then
        SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)"
    fi
    [[ -x "$CXX_BIN" ]] || fail "Apple Clang C++ driver not found: $CXX_BIN"
    [[ -d "$SDK_PATH" ]] || fail "macOS SDK not found: $SDK_PATH"
    compiler_version="$("$CXX_BIN" --version 2>&1)" || \
        fail "could not query the selected C++ compiler: $CXX_BIN"
    [[ "$compiler_version" == *"Apple clang version"* ]] || \
        fail "selected C++ compiler is not Apple Clang: $CXX_BIN"
}

build_thin_slice() {
    local selected_precision="$1"
    local selected_architecture="$2"
    local binary="${BENCHMARK_DIR}/bin/macos/${selected_architecture}/tick_synchronizer_protocol_benchmark.${selected_precision}"
    local -a scons_args=(
        platform=macos
        "arch=${selected_architecture}"
        toolchain=apple-clang
        "precision=${selected_precision}"
        "cxx=${CXX_BIN}"
        "macos_sdk=${SDK_PATH}"
        "macos_deployment_target=${DEPLOYMENT_TARGET}"
        "lto=${LTO}"
    )

    if (( CLEAN_FIRST )); then
        "$SCONS_BIN" -C "$BENCHMARK_DIR" -c "${scons_args[@]}"
    fi
    log "Building macOS ${selected_architecture} benchmark (${selected_precision})..."
    "$SCONS_BIN" -C "$BENCHMARK_DIR" "${scons_args[@]}" "-j${JOBS}"
    [[ -x "$binary" ]] || fail "thin benchmark binary not found: $binary"
    /usr/bin/lipo "$binary" -verify_arch "$selected_architecture" || \
        fail "thin binary does not contain ${selected_architecture}: $binary"
}

inspect_runtime_dependencies() {
    local binary="$1"
    local dependency_report="$2"
    local dependencies sanitized_dependencies binary_name line trimmed dependency
    dependencies="$(/usr/bin/otool -arch all -L "$binary")"
    binary_name="${binary##*/}"
    sanitized_dependencies="${dependencies//$binary/$binary_name}"
    printf '%s\n' "$sanitized_dependencies" > "$dependency_report"
    while IFS= read -r line; do
        [[ "$line" == $'\t'* ]] || continue
        trimmed="${line#"${line%%[![:space:]]*}"}"
        [[ -n "$trimmed" ]] || continue
        dependency="${trimmed%% *}"
        case "$dependency" in
            /usr/lib/*|/System/Library/*) ;;
            *) fail "Universal 2 binary has a non-system runtime dependency: $dependency" ;;
        esac
    done <<< "$dependencies"
}

create_universal_binary() {
    local selected_precision="$1"
    local x86_binary="${BENCHMARK_DIR}/bin/macos/x86_64/tick_synchronizer_protocol_benchmark.${selected_precision}"
    local arm_binary="${BENCHMARK_DIR}/bin/macos/arm64/tick_synchronizer_protocol_benchmark.${selected_precision}"
    local universal_dir="${BENCHMARK_DIR}/bin/macos/universal2"
    local universal_binary="${universal_dir}/tick_synchronizer_protocol_benchmark.${selected_precision}"
    local architectures architecture_count signing_state load_commands deployment_count

    mkdir -p "$universal_dir"
    /usr/bin/lipo -create "$x86_binary" "$arm_binary" -output "$universal_binary"
    chmod 755 "$universal_binary"
    /usr/bin/lipo "$universal_binary" -verify_arch x86_64 arm64 || \
        fail "Universal 2 binary is missing a required architecture: $universal_binary"
    architectures="$(/usr/bin/lipo -archs "$universal_binary")"
    architecture_count="$(wc -w <<< "$architectures" | tr -d '[:space:]')"
    [[ "$architecture_count" == "2" ]] || fail "unexpected Universal 2 architecture set: $architectures"

    {
        printf 'Binary: %s\n' "${universal_binary##*/}"
        /usr/bin/file -b "$universal_binary"
    } > "${universal_binary}.file.txt"
    load_commands="$(/usr/bin/otool -arch all -l "$universal_binary")"
    deployment_count="$(printf '%s\n' "$load_commands" | /usr/bin/awk -v expected="$DEPLOYMENT_TARGET" '
        function normalize_version(value, parts) {
            split(value, parts, ".")
            return sprintf("%d.%d.%d", parts[1] + 0, parts[2] + 0, parts[3] + 0)
        }
        $1 == "cmd" && ($2 == "LC_BUILD_VERSION" || $2 == "LC_VERSION_MIN_MACOSX") {
            in_version_command = 1
            next
        }
        in_version_command && $1 == "cmd" {
            in_version_command = 0
        }
        in_version_command && ($1 == "minos" || $1 == "version") &&
                normalize_version($2) == normalize_version(expected) {
            matches++
            in_version_command = 0
        }
        END { print matches + 0 }
    ')"
    [[ "$deployment_count" == "2" ]] || \
        fail "expected deployment target ${DEPLOYMENT_TARGET} in both Universal 2 slices; found ${deployment_count}"
    printf '%s\n' "${load_commands//$universal_binary/${universal_binary##*/}}" > \
        "${universal_binary}.load-commands.txt"
    inspect_runtime_dependencies "$universal_binary" "${universal_binary}.dylibs.txt"

    if /usr/bin/codesign -dv "$universal_binary" >/dev/null 2>&1; then
        signing_state="signed"
    else
        signing_state="unsigned"
    fi
    printf 'TICKSYNCHRONIZER_MACOS_UNIVERSAL2_BINARY_OK precision=%s architectures=%s deployment_target=%s signing=%s\n' \
        "$selected_precision" "${architectures// /,}" "$DEPLOYMENT_TARGET" "$signing_state"
}

run_native_self_test() {
    local selected_precision="$1"
    local binary="${BENCHMARK_DIR}/bin/macos/universal2/tick_synchronizer_protocol_benchmark.${selected_precision}"
    local host_architecture translated
    host_architecture="$(uname -m)"
    translated="$(/usr/sbin/sysctl -n sysctl.proc_translated 2>/dev/null || printf '0')"
    [[ "$translated" != "1" ]] || fail "run the build from a native shell instead of Rosetta"
    [[ "$host_architecture" == "x86_64" || "$host_architecture" == "arm64" ]] || \
        fail "unsupported native macOS architecture: $host_architecture"
    /usr/bin/lipo "$binary" -verify_arch "$host_architecture" || \
        fail "Universal 2 binary lacks the native host slice: $host_architecture"
    "$binary" --self-test --candidate reference_fixed_width
    "$binary" --self-test --candidate varint_zigzag_fixed_float
    printf 'TICKSYNCHRONIZER_MACOS_NATIVE_SELF_TEST_OK precision=%s architecture=%s\n' \
        "$selected_precision" "$host_architecture"
}

build_one() {
    local selected_precision="$1"
    build_thin_slice "$selected_precision" x86_64
    build_thin_slice "$selected_precision" arm64
    create_universal_binary "$selected_precision"
    run_native_self_test "$selected_precision"
}

capture_source_state() {
    SOURCE_HEAD_BEFORE="$(git -C "$MODULE_DIR" rev-parse HEAD)"
    SOURCE_STATUS_BEFORE="$(git -C "$MODULE_DIR" status --porcelain --untracked-files=all)"
}

verify_source_unchanged() {
    local source_head_after source_status_after
    source_head_after="$(git -C "$MODULE_DIR" rev-parse HEAD)"
    source_status_after="$(git -C "$MODULE_DIR" status --porcelain --untracked-files=all)"
    [[ "$source_head_after" == "$SOURCE_HEAD_BEFORE" && "$source_status_after" == "$SOURCE_STATUS_BEFORE" ]] || \
        fail "source state changed during the Universal 2 build; discard the artifacts and rebuild"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --precision) [[ $# -ge 2 ]] || fail "--precision requires a value"; PRECISION="$2"; shift 2 ;;
        --jobs) [[ $# -ge 2 ]] || fail "--jobs requires a value"; JOBS="$2"; shift 2 ;;
        --scons-bin) [[ $# -ge 2 ]] || fail "--scons-bin requires a value"; SCONS_BIN="$2"; shift 2 ;;
        --cxx) [[ $# -ge 2 ]] || fail "--cxx requires a value"; CXX_BIN="$2"; shift 2 ;;
        --sdk) [[ $# -ge 2 ]] || fail "--sdk requires a value"; SDK_PATH="$2"; shift 2 ;;
        --deployment-target) [[ $# -ge 2 ]] || fail "--deployment-target requires a value"; DEPLOYMENT_TARGET="$2"; shift 2 ;;
        --lto) LTO="yes"; shift ;;
        --clean-first) CLEAN_FIRST=1; shift ;;
        --no-export-package) EXPORT_PACKAGE=0; shift ;;
        --output-dir) [[ $# -ge 2 ]] || fail "--output-dir requires a value"; EXPORT_OUTPUT_DIR="$2"; EXPORT_PACKAGE=1; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || fail "the macOS Universal 2 build must run locally on macOS"
[[ "$PRECISION" == "single" || "$PRECISION" == "double" || "$PRECISION" == "all" ]] || \
    fail "invalid precision: $PRECISION"
[[ "$DEPLOYMENT_TARGET" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || \
    fail "invalid deployment target: $DEPLOYMENT_TARGET"
[[ -n "$JOBS" ]] || JOBS="$(get_jobs)"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "invalid jobs value: $JOBS"
command -v "$SCONS_BIN" >/dev/null 2>&1 || fail "SCons not found: $SCONS_BIN"
command -v git >/dev/null 2>&1 || fail "Git not found on the macOS build host"
for required_tool in /usr/bin/lipo /usr/bin/otool /usr/bin/file /usr/bin/codesign /usr/sbin/sysctl; do
    [[ -x "$required_tool" ]] || fail "required macOS build tool not found: $required_tool"
done
resolve_build_tools
"$SCRIPT_DIR/verify_source_consistency.sh" >/dev/null || \
    fail "source consistency verification failed"
capture_source_state

if [[ "$PRECISION" == "all" ]]; then
    build_one double
    build_one single
else
    build_one "$PRECISION"
fi
verify_source_unchanged

if (( EXPORT_PACKAGE )); then
    export_args=(
        --platform macos
        --precision "$PRECISION"
        --toolchain apple-clang
        --macos-deployment-target "$DEPLOYMENT_TARGET"
    )
    [[ -z "$EXPORT_OUTPUT_DIR" ]] || export_args+=(--output-dir "$EXPORT_OUTPUT_DIR")
    "$SCRIPT_DIR/export_protocol_benchmarks.sh" "${export_args[@]}"
fi

printf 'TICKSYNCHRONIZER_MACOS_UNIVERSAL2_BUILD_OK precision=%s deployment_target=%s export=%s\n' \
    "$PRECISION" "$DEPLOYMENT_TARGET" "$EXPORT_PACKAGE"

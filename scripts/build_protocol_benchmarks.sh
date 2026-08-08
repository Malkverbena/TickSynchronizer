#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BENCHMARK_DIR="${MODULE_DIR}/benchmarks"
PRECISION="double"
JOBS=""
SCONS_BIN="${TICKSYNC_SCONS_BIN:-scons}"
CXX_BIN="${CXX:-c++}"
CLEAN_FIRST=0
LTO="no"
EXPORT_PACKAGE=0
EXPORT_OUTPUT_DIR=""

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/build_protocol_benchmarks.sh [options]

Options:
  --precision single|double|all  Reference candidate precision. Default: double.
  --jobs N                      Parallel jobs.
  --scons-bin COMMAND           SCons executable. Default: scons.
  --cxx COMMAND                 C++ compiler. Default: the CXX variable or c++.
  --lto                         Enables LTO.
  --clean-first                 Cleans the target before building.
  --export-package              Exports execution-only test binaries after building.
  --export-output-dir PATH      Overrides the deployment-package output directory.
  -h, --help                    Shows this help.
USAGE
}

get_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        printf '1\n'
    fi
}

build_one() {
    local precision="$1"
    local binary="${BENCHMARK_DIR}/bin/tick_synchronizer_protocol_benchmark.${precision}"
    if (( CLEAN_FIRST )); then
        "$SCONS_BIN" -C "$BENCHMARK_DIR" -c \
            platform=linuxbsd arch=x86_64 toolchain=native \
            "precision=${precision}" "cxx=${CXX_BIN}" "lto=${LTO}"
    fi
    "$SCONS_BIN" -C "$BENCHMARK_DIR" \
        platform=linuxbsd arch=x86_64 toolchain=native \
        "precision=${precision}" "cxx=${CXX_BIN}" "lto=${LTO}" "-j${JOBS}"
    [[ -x "$binary" ]] || fail "binary not found after build: $binary"
    "$binary" --self-test
    printf 'TICKSYNCHRONIZER_BENCHMARK_BUILD_OK suite=%s precision=%s binary=%s\n' \
        "$(${MODULE_DIR}/scripts/build_and_validate.sh --print-benchmark-suite-version)" \
        "$precision" "$binary"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --precision)
            [[ $# -ge 2 ]] || fail "--precision requires a value"
            PRECISION="$2"
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
        --cxx)
            [[ $# -ge 2 ]] || fail "--cxx requires a value"
            CXX_BIN="$2"
            shift 2
            ;;
        --lto)
            LTO="yes"
            shift
            ;;
        --clean-first)
            CLEAN_FIRST=1
            shift
            ;;
        --export-package)
            EXPORT_PACKAGE=1
            shift
            ;;
        --export-output-dir)
            [[ $# -ge 2 ]] || fail "--export-output-dir requires a value"
            EXPORT_OUTPUT_DIR="$2"
            EXPORT_PACKAGE=1
            shift 2
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

[[ "$PRECISION" == "single" || "$PRECISION" == "double" || "$PRECISION" == "all" ]] || \
    fail "invalid precision: $PRECISION"
if [[ -z "$JOBS" ]]; then
    JOBS="$(get_jobs)"
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "invalid jobs value: $JOBS"
command -v "$SCONS_BIN" >/dev/null 2>&1 || fail "SCons not found: $SCONS_BIN"
command -v "$CXX_BIN" >/dev/null 2>&1 || fail "compiler not found: $CXX_BIN"
[[ -f "$BENCHMARK_DIR/SConstruct" ]] || fail "benchmarks/SConstruct missing"
"$SCRIPT_DIR/verify_source_consistency.sh" >/dev/null || \
    fail "source consistency verification failed"

if [[ "$PRECISION" == "all" ]]; then
    build_one double
    build_one single
else
    build_one "$PRECISION"
fi

if (( EXPORT_PACKAGE )); then
    export_args=(--platform linuxbsd --precision "$PRECISION" --toolchain native)
    [[ -z "$EXPORT_OUTPUT_DIR" ]] || export_args+=(--output-dir "$EXPORT_OUTPUT_DIR")
    "$SCRIPT_DIR/export_protocol_benchmarks.sh" "${export_args[@]}"
fi

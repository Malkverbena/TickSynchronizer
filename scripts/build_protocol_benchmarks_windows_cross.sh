#!/usr/bin/env bash
# Cross-compiles the standalone benchmark from Linux to native Windows x86_64.
# Produces an execution-only deployment ZIP for a Windows test machine.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BENCHMARK_DIR="${MODULE_DIR}/benchmarks"

PRECISION="all"
JOBS=""
TOOLCHAIN="auto"
TOOLCHAIN_ROOT=""
SCONS_BIN="${TICKSYNC_SCONS_BIN:-scons}"
LTO="no"
CLEAN_FIRST=0
REQUIRE_WINE_SELF_TEST=0
SKIP_WINE_SELF_TEST=0
OUTPUT_DIR="${MODULE_DIR}/benchmark_dist/windows/x86_64"

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/build_protocol_benchmarks_windows_cross.sh [options]

Options:
  --precision MODE          single, double, or all. Default: all.
  --jobs N                 Parallel build jobs. Default: host logical CPUs.
  --toolchain NAME         auto, mingw-gcc, or llvm-mingw. Default: auto.
  --toolchain-root PATH    Root of a standalone LLVM-MinGW/MinGW toolchain.
  --scons-bin COMMAND      SCons executable. Default: scons.
  --lto                    Enable link-time optimization.
  --clean-first            Clean each selected SCons target before building.
  --output-dir PATH        Deployment-package output directory.
  --require-wine-self-test Fail unless Wine executes each self-test successfully.
  --skip-wine-self-test    Do not attempt a Wine self-test.
  -h, --help               Show this help.

The generated package contains native PE x86_64 executables and a PowerShell
runner. The Windows test machine does not need build tools or project sources.
USAGE
}

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '[TickSynchronizer] %s\n' "$*" >&2
}

command_path() {
    local name="$1"
    if [[ -n "$TOOLCHAIN_ROOT" && -x "$TOOLCHAIN_ROOT/bin/$name" ]]; then
        printf '%s\n' "$TOOLCHAIN_ROOT/bin/$name"
        return 0
    fi
    command -v "$name" 2>/dev/null || return 1
}

resolve_toolchain() {
    local llvm_cxx=""
    local gcc_cxx=""

    llvm_cxx="$(command_path x86_64-w64-mingw32-clang++ || true)"
    gcc_cxx="$(command_path x86_64-w64-mingw32-g++-posix || true)"
    if [[ -z "$gcc_cxx" ]]; then
        gcc_cxx="$(command_path x86_64-w64-mingw32-g++ || true)"
    fi

    case "$TOOLCHAIN" in
        auto)
            if [[ -n "$gcc_cxx" ]]; then
                RESOLVED_TOOLCHAIN="mingw-gcc"
                CXX_COMPILER="$gcc_cxx"
            elif [[ -n "$llvm_cxx" ]]; then
                RESOLVED_TOOLCHAIN="llvm-mingw"
                CXX_COMPILER="$llvm_cxx"
            else
                fail "no Windows x86_64 cross-compiler found; install MinGW-w64 or provide --toolchain-root"
            fi
            ;;
        llvm-mingw)
            [[ -n "$llvm_cxx" ]] || fail "x86_64-w64-mingw32-clang++ not found"
            RESOLVED_TOOLCHAIN="llvm-mingw"
            CXX_COMPILER="$llvm_cxx"
            ;;
        mingw-gcc)
            [[ -n "$gcc_cxx" ]] || fail "x86_64-w64-mingw32-g++ not found"
            RESOLVED_TOOLCHAIN="mingw-gcc"
            CXX_COMPILER="$gcc_cxx"
            ;;
        *)
            fail "unsupported toolchain: $TOOLCHAIN"
            ;;
    esac

    OBJDUMP_BIN="$(command_path x86_64-w64-mingw32-objdump || true)"
    if [[ -z "$OBJDUMP_BIN" ]]; then
        OBJDUMP_BIN="$(command_path llvm-objdump || true)"
    fi
}

inspect_pe_binary() {
    local binary="$1"
    [[ -s "$binary" ]] || fail "Windows benchmark binary is missing or empty: $binary"

    if command -v file >/dev/null 2>&1; then
        file "$binary" | grep -Eq 'PE32\+.*x86-64|PE32\+.*Intel 80386' || \
            fail "output is not a Windows x86_64 PE executable: $binary"
    fi

    if [[ -n "$OBJDUMP_BIN" ]]; then
        local headers
        if [[ "$(basename -- "$OBJDUMP_BIN")" == "llvm-objdump" ]]; then
            headers="$($OBJDUMP_BIN --private-headers "$binary" 2>/dev/null || true)"
        else
            headers="$($OBJDUMP_BIN -p "$binary" 2>/dev/null || true)"
        fi
        printf '%s\n' "$headers" > "${binary}.pe-headers.txt"
        if grep -Eiq 'DLL Name:.*(libstdc\+\+|libgcc|libwinpthread|libc\+\+|libunwind).*\.dll' <<<"$headers"; then
            fail "binary depends on a non-system compiler runtime DLL; static Windows runtime is required"
        fi
    fi
}

run_wine_self_test() {
    local binary="$1"
    if (( SKIP_WINE_SELF_TEST )); then
        return 0
    fi

    local wine_bin=""
    wine_bin="$(command -v wine64 2>/dev/null || command -v wine 2>/dev/null || true)"
    if [[ -z "$wine_bin" ]]; then
        if (( REQUIRE_WINE_SELF_TEST )); then
            fail "Wine was required but was not found"
        fi
        log "Wine not found; the self-test will run on the physical Windows machine"
        return 0
    fi

    log "Running Windows self-test through Wine: $(basename -- "$binary")"
    WINEDEBUG=-all "$wine_bin" "$binary" --self-test || \
        fail "Windows benchmark self-test failed under Wine: $binary"
}

build_one() {
    local selected_precision="$1"
    local binary="${BENCHMARK_DIR}/bin/windows/x86_64/tick_synchronizer_protocol_benchmark.${selected_precision}.exe"
    local -a scons_args=(
        platform=windows
        arch=x86_64
        "toolchain=${RESOLVED_TOOLCHAIN}"
        "precision=${selected_precision}"
        "cxx=${CXX_COMPILER}"
        "lto=${LTO}"
    )

    if (( CLEAN_FIRST )); then
        "$SCONS_BIN" -C "$BENCHMARK_DIR" -c "${scons_args[@]}"
    fi
    log "Cross-compiling Windows x86_64 benchmark (${selected_precision}, ${RESOLVED_TOOLCHAIN})..."
    "$SCONS_BIN" -C "$BENCHMARK_DIR" "${scons_args[@]}" "-j${JOBS}"
    inspect_pe_binary "$binary"
    run_wine_self_test "$binary"
    log "Windows cross-build validated: $binary"
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
        --toolchain)
            [[ $# -ge 2 ]] || fail "--toolchain requires a value"
            TOOLCHAIN="$2"
            shift 2
            ;;
        --toolchain-root)
            [[ $# -ge 2 ]] || fail "--toolchain-root requires a value"
            TOOLCHAIN_ROOT="$(realpath -m -- "$2")"
            shift 2
            ;;
        --scons-bin)
            [[ $# -ge 2 ]] || fail "--scons-bin requires a value"
            SCONS_BIN="$2"
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
        --output-dir)
            [[ $# -ge 2 ]] || fail "--output-dir requires a value"
            OUTPUT_DIR="$(realpath -m -- "$2")"
            shift 2
            ;;
        --require-wine-self-test)
            REQUIRE_WINE_SELF_TEST=1
            shift
            ;;
        --skip-wine-self-test)
            SKIP_WINE_SELF_TEST=1
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

[[ "$(uname -s)" == "Linux" ]] || fail "Windows cross-compilation must run on Linux"
[[ "$PRECISION" == "single" || "$PRECISION" == "double" || "$PRECISION" == "all" ]] || \
    fail "invalid precision: $PRECISION"
[[ -z "$JOBS" ]] && JOBS="$(nproc 2>/dev/null || printf 1)"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "invalid jobs value: $JOBS"
command -v "$SCONS_BIN" >/dev/null 2>&1 || fail "SCons not found: $SCONS_BIN"
command -v zip >/dev/null 2>&1 || fail "zip not found"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum not found"
"$SCRIPT_DIR/verify_source_consistency.sh" >/dev/null || \
    fail "source consistency verification failed"

resolve_toolchain
log "Selected cross toolchain: $RESOLVED_TOOLCHAIN ($CXX_COMPILER)"

if [[ "$PRECISION" == "all" ]]; then
    build_one double
    build_one single
else
    build_one "$PRECISION"
fi

"$SCRIPT_DIR/export_protocol_benchmarks.sh" \
    --platform windows \
    --precision "$PRECISION" \
    --toolchain "$RESOLVED_TOOLCHAIN" \
    --output-dir "$OUTPUT_DIR"

printf 'TICKSYNCHRONIZER_WINDOWS_CROSS_BUILD_OK toolchain=%s precision=%s output=%s\n' \
    "$RESOLVED_TOOLCHAIN" "$PRECISION" "$OUTPUT_DIR"

#!/usr/bin/env bash
# Cross-compiles the standalone benchmark for Android ARM64 with SCons.
# Uses the Android NDK Clang driver and validates the exported ELF runtime contract.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BENCHMARK_DIR="${MODULE_DIR}/benchmarks"
PRECISION="double"
ABI="arm64-v8a"
API_LEVEL="24"
JOBS=""
SCONS_BIN="${TICKSYNC_SCONS_BIN:-scons}"
HOST_TAG="linux-x86_64"
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
  ./scripts/build_protocol_benchmarks_android.sh [options]

Options:
  --precision single|double|all  Precision. Default: double.
  --abi ABI                     Android ABI. Default: arm64-v8a.
  --api-level N                 Android API level. Default: 24.
  --jobs N                      Parallel build jobs.
  --scons-bin COMMAND           SCons executable. Default: scons.
  --host-tag TAG                NDK host tag. Default: linux-x86_64.
  --lto                         Enables link-time optimization.
  --clean-first                 Cleans each selected SCons target first.
  --export-package              Exports an execution-only Android test package.
  --export-output-dir PATH      Overrides the deployment-package output directory.
  -h, --help                    Shows this help.

Environment:
  ANDROID_NDK_HOME or ANDROID_NDK_ROOT must point to an installed Android NDK.
USAGE
}

get_jobs() {
    command -v nproc >/dev/null 2>&1 && nproc || printf '1\n'
}

resolve_ndk() {
    local ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
    [[ -n "$ndk" ]] || fail "ANDROID_NDK_HOME or ANDROID_NDK_ROOT is required"
    ndk="$(realpath -m -- "$ndk")"
    [[ -d "$ndk/toolchains/llvm/prebuilt/$HOST_TAG" ]] || \
        fail "Android NDK host toolchain not found under: $ndk/toolchains/llvm/prebuilt/$HOST_TAG"
    [[ -x "$ndk/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android${API_LEVEL}-clang++" ]] || \
        fail "Android NDK compiler not found for API ${API_LEVEL}"
    printf '%s\n' "$ndk"
}

inspect_android_binary() {
    local binary="$1"
    local ndk="$2"
    [[ -s "$binary" ]] || fail "Android benchmark binary is missing or empty: $binary"

    if command -v file >/dev/null 2>&1; then
        file "$binary" | grep -Eq 'ELF 64-bit.*(ARM aarch64|aarch64)' || \
            fail "output is not an Android AArch64 ELF executable: $binary"
    fi

    local readelf_bin="$ndk/toolchains/llvm/prebuilt/$HOST_TAG/bin/llvm-readelf"
    if [[ -x "$readelf_bin" ]]; then
        local dependencies
        dependencies="$($readelf_bin --dynamic "$binary" 2>/dev/null || true)"
        printf '%s\n' "$dependencies" > "${binary}.elf-dynamic.txt"
        if grep -Eq 'Shared library: \[(libc\+\+_shared|libstdc\+\+|libgcc_s|libunwind)\.so\]' <<<"$dependencies"; then
            fail "Android benchmark depends on a shared compiler runtime"
        fi
    fi
}

build_one() {
    local precision="$1"
    local ndk="$2"
    local binary="${BENCHMARK_DIR}/bin/android/${ABI}/tick_synchronizer_protocol_benchmark.${precision}"
    local -a scons_args=(
        platform=android
        arch=arm64
        toolchain=android-ndk
        "precision=${precision}"
        "android_abi=${ABI}"
        "android_api=${API_LEVEL}"
        "android_host_tag=${HOST_TAG}"
        "android_ndk_root=${ndk}"
        "lto=${LTO}"
    )

    if (( CLEAN_FIRST )); then
        "$SCONS_BIN" -C "$BENCHMARK_DIR" -c "${scons_args[@]}"
    fi
    "$SCONS_BIN" -C "$BENCHMARK_DIR" "${scons_args[@]}" "-j${JOBS}"
    inspect_android_binary "$binary" "$ndk"
    printf 'TICKSYNCHRONIZER_ANDROID_BENCHMARK_BUILD_OK suite=%s precision=%s abi=%s api=%s binary=%s\n' \
        "$("${MODULE_DIR}/scripts/build_and_validate.sh" --print-benchmark-suite-version)" \
        "$precision" "$ABI" "$API_LEVEL" "$binary"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --precision) [[ $# -ge 2 ]] || fail "--precision requires a value"; PRECISION="$2"; shift 2 ;;
        --abi) [[ $# -ge 2 ]] || fail "--abi requires a value"; ABI="$2"; shift 2 ;;
        --api-level) [[ $# -ge 2 ]] || fail "--api-level requires a value"; API_LEVEL="$2"; shift 2 ;;
        --jobs) [[ $# -ge 2 ]] || fail "--jobs requires a value"; JOBS="$2"; shift 2 ;;
        --scons-bin) [[ $# -ge 2 ]] || fail "--scons-bin requires a value"; SCONS_BIN="$2"; shift 2 ;;
        --host-tag) [[ $# -ge 2 ]] || fail "--host-tag requires a value"; HOST_TAG="$2"; shift 2 ;;
        --lto) LTO="yes"; shift ;;
        --clean-first) CLEAN_FIRST=1; shift ;;
        --export-package) EXPORT_PACKAGE=1; shift ;;
        --export-output-dir)
            [[ $# -ge 2 ]] || fail "--export-output-dir requires a value"
            EXPORT_OUTPUT_DIR="$2"
            EXPORT_PACKAGE=1
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ "$(uname -s)" == "Linux" ]] || fail "Android cross-compilation must run on Linux"
[[ "$PRECISION" == "single" || "$PRECISION" == "double" || "$PRECISION" == "all" ]] || \
    fail "invalid precision: $PRECISION"
[[ "$ABI" == "arm64-v8a" ]] || fail "only arm64-v8a is currently supported"
[[ "$API_LEVEL" =~ ^[0-9]+$ ]] && (( API_LEVEL >= 21 )) || fail "invalid Android API level: $API_LEVEL"
[[ -n "$JOBS" ]] || JOBS="$(get_jobs)"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "invalid jobs value: $JOBS"
command -v "$SCONS_BIN" >/dev/null 2>&1 || fail "SCons not found: $SCONS_BIN"
"$SCRIPT_DIR/verify_source_consistency.sh" >/dev/null || \
    fail "source consistency verification failed"
NDK="$(resolve_ndk)"

if [[ "$PRECISION" == "all" ]]; then
    build_one double "$NDK"
    build_one single "$NDK"
else
    build_one "$PRECISION" "$NDK"
fi

if (( EXPORT_PACKAGE )); then
    export_args=(
        --platform android
        --precision "$PRECISION"
        --abi "$ABI"
        --toolchain android-ndk
    )
    [[ -z "$EXPORT_OUTPUT_DIR" ]] || export_args+=(--output-dir "$EXPORT_OUTPUT_DIR")
    "$SCRIPT_DIR/export_protocol_benchmarks.sh" "${export_args[@]}"
fi

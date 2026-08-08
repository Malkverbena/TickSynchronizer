#!/usr/bin/env bash
# Exports prebuilt protocol benchmarks as execution-only deployment packages.
# Keeps compilers, SCons, source files, and target SDKs off test machines.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BENCHMARK_DIR="${MODULE_DIR}/benchmarks"
PLATFORM=""
PRECISION="all"
ABI="arm64-v8a"
TOOLCHAIN="unspecified"
OUTPUT_DIR=""
STAGING_DIR=""

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/export_protocol_benchmarks.sh --platform PLATFORM [options]

Options:
  --platform linuxbsd|windows|android  Required target platform.
  --precision single|double|all       Packaged precision. Default: all.
  --abi ABI                           Android ABI. Default: arm64-v8a.
  --toolchain NAME                    Build toolchain recorded in package metadata.
  --output-dir PATH                   Package output directory.
  -h, --help                          Shows this help.

The package contains prebuilt binaries, an execution-only runner, integrity
hashes, and instructions. It never contains a compiler, SDK, or project source.
USAGE
}

cleanup() {
    if [[ -n "$STAGING_DIR" ]]; then
        case "$STAGING_DIR" in
            "$TEMP_ROOT"/ticksync-export.*)
                rm -rf -- "$STAGING_DIR"
                ;;
            *)
                printf 'WARNING: refusing to clean unexpected staging path: %s\n' "$STAGING_DIR" >&2
                ;;
        esac
    fi
}

selected_precisions() {
    if [[ "$PRECISION" == "all" ]]; then
        printf 'double\nsingle\n'
    else
        printf '%s\n' "$PRECISION"
    fi
}

copy_binary() {
    local source="$1"
    local destination_dir="$2"
    [[ -s "$source" ]] || fail "prebuilt benchmark binary not found: $source"
    mkdir -p -- "$destination_dir"
    cp --preserve=mode -- "$source" "$destination_dir/"
}

write_readme() {
    local package_root="$1"
    case "$PLATFORM" in
        linuxbsd)
            cat > "$package_root/README.txt" <<'EOF'
TickSynchronizer Linux x86_64 execution-only benchmark package

Runtime requirements:
- 64-bit Linux on x86_64.
- Bash, Python 3, sha256sum, and standard system utilities.
- No compiler, SCons installation, SDK, Git checkout, or project source.

Inspect logical CPUs, then run a quick qualification:
  ./scripts/run_protocol_benchmarks.sh --execution-only --list-cpus
  ./scripts/run_protocol_benchmarks.sh --execution-only --precision all --cpu 2 --quick

On a multi-CCD processor, select each L3 domain explicitly and label it:
  ./scripts/run_protocol_benchmarks.sh --execution-only --precision all --l3-cache-id ID --cpu-class LABEL --quick

Run an official benchmark only when PACKAGE_METADATA.txt says Source state: clean:
  ./scripts/run_protocol_benchmarks.sh --execution-only --precision all --cpu 2

The runner verifies SHA256SUMS.txt before execution. The native executable then
applies and verifies affinity. Generated reports are written under
benchmark_reports/ and use report schema 3.
EOF
            ;;
        windows)
            cat > "$package_root/README.txt" <<'EOF'
TickSynchronizer Windows x86_64 execution-only benchmark package

Runtime requirements:
- 64-bit Windows.
- Windows PowerShell 5.1 or newer.
- No compiler, SCons installation, SDK, Git checkout, Python, or project source.

List native logical-processor and L3 topology:
  powershell -ExecutionPolicy Bypass -File .\run_protocol_benchmarks_windows.ps1 -ListCpus

Run a quick qualification:
  powershell -ExecutionPolicy Bypass -File .\run_protocol_benchmarks_windows.ps1 -Precision all -Cpu 2 -CpuClass LABEL -Quick

Run an official benchmark only when PACKAGE_METADATA.txt says Source state: clean:
  powershell -ExecutionPolicy Bypass -File .\run_protocol_benchmarks_windows.ps1 -Precision all -Cpu 2 -CpuClass LABEL

The runner verifies SHA256SUMS.txt before execution. The native executable maps
flat CPU indices across processor groups, records the selected L3 domain, and
writes report schema 3 under benchmark_reports/.
EOF
            ;;
        android)
            cat > "$package_root/README.txt" <<'EOF'
TickSynchronizer Android ARM64 execution-only benchmark package

Android device requirements:
- arm64-v8a Android device with API level 24 or newer.
- No compiler, NDK, SCons installation, development environment, or source files.

Controller requirements:
- Linux with Bash, ADB, Python 3, sha256sum, and standard system utilities.
- USB debugging or wireless ADB access to the selected device.

Inspect device CPU topology:
  ./scripts/run_protocol_benchmarks_android.sh --execution-only --serial SERIAL --list-cpus

Run a quick qualification:
  ./scripts/run_protocol_benchmarks_android.sh --execution-only --serial SERIAL --precision all --cpu-class prime --quick

For official evidence, select an explicit logical CPU and omit --quick. The
runner verifies SHA256SUMS.txt, pushes only the native executable and a small
generated shell launcher to /data/local/tmp, then pulls report schema 3 back to
benchmark_reports/.
EOF
            ;;
    esac
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform) [[ $# -ge 2 ]] || fail "--platform requires a value"; PLATFORM="$2"; shift 2 ;;
        --precision) [[ $# -ge 2 ]] || fail "--precision requires a value"; PRECISION="$2"; shift 2 ;;
        --abi) [[ $# -ge 2 ]] || fail "--abi requires a value"; ABI="$2"; shift 2 ;;
        --toolchain) [[ $# -ge 2 ]] || fail "--toolchain requires a value"; TOOLCHAIN="$2"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || fail "--output-dir requires a value"; OUTPUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ "$PLATFORM" == "linuxbsd" || "$PLATFORM" == "windows" || "$PLATFORM" == "android" ]] || \
    fail "--platform must be linuxbsd, windows, or android"
[[ "$PRECISION" == "single" || "$PRECISION" == "double" || "$PRECISION" == "all" ]] || \
    fail "invalid precision: $PRECISION"
[[ "$ABI" == "arm64-v8a" ]] || fail "only arm64-v8a is currently supported"
command -v zip >/dev/null 2>&1 || fail "zip not found"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum not found"
command -v mktemp >/dev/null 2>&1 || fail "mktemp not found"

TEMP_ROOT="${TICKSYNC_TEMP_DIR:-$(realpath -m -- "${MODULE_DIR}/../tick_synchronizer_tmp")}"
TEMP_ROOT="$(realpath -m -- "$TEMP_ROOT")"
case "$TEMP_ROOT" in
    /|/tmp|/tmp/*)
        fail "host temporary files must use ../tick_synchronizer_tmp or a safe TICKSYNC_TEMP_DIR override"
        ;;
esac
mkdir -p -- "$TEMP_ROOT"
STAGING_DIR="$(mktemp -d -- "$TEMP_ROOT/ticksync-export.XXXXXX")"
trap cleanup EXIT

case "$PLATFORM" in
    linuxbsd)
        PLATFORM_TAG="linuxbsd-x86_64"
        DEFAULT_OUTPUT="${MODULE_DIR}/benchmark_dist/linuxbsd/x86_64"
        ;;
    windows)
        PLATFORM_TAG="windows-x86_64"
        DEFAULT_OUTPUT="${MODULE_DIR}/benchmark_dist/windows/x86_64"
        ;;
    android)
        PLATFORM_TAG="android-${ABI}"
        DEFAULT_OUTPUT="${MODULE_DIR}/benchmark_dist/android/${ABI}"
        ;;
esac
OUTPUT_DIR="$(realpath -m -- "${OUTPUT_DIR:-$DEFAULT_OUTPUT}")"
mkdir -p -- "$OUTPUT_DIR"

MODULE_COMMIT="$(git -C "$MODULE_DIR" rev-parse HEAD 2>/dev/null || printf unknown)"
if [[ -n "$(git -C "$MODULE_DIR" status --porcelain --untracked-files=all 2>/dev/null || true)" ]]; then
    SOURCE_STATE="dirty"
elif [[ "$MODULE_COMMIT" == "unknown" ]]; then
    SOURCE_STATE="unknown"
else
    SOURCE_STATE="clean"
fi
COMMIT_TAG="${MODULE_COMMIT:0:12}"
SUITE_VERSION="$("$SCRIPT_DIR/build_and_validate.sh" --print-benchmark-suite-version)"
PACKAGE_NAME="tick_synchronizer-benchmarks-suite${SUITE_VERSION}-${PLATFORM_TAG}-${COMMIT_TAG}"
PACKAGE_ROOT="$STAGING_DIR/$PACKAGE_NAME"
mkdir -p -- "$PACKAGE_ROOT"

while IFS= read -r selected_precision; do
    case "$PLATFORM" in
        linuxbsd)
            copy_binary \
                "$BENCHMARK_DIR/bin/tick_synchronizer_protocol_benchmark.${selected_precision}" \
                "$PACKAGE_ROOT/benchmarks/bin"
            ;;
        windows)
            windows_binary="$BENCHMARK_DIR/bin/windows/x86_64/tick_synchronizer_protocol_benchmark.${selected_precision}.exe"
            copy_binary "$windows_binary" "$PACKAGE_ROOT"
            if [[ -f "${windows_binary}.pe-headers.txt" ]]; then
                cp -- "${windows_binary}.pe-headers.txt" "$PACKAGE_ROOT/"
            fi
            ;;
        android)
            android_binary="$BENCHMARK_DIR/bin/android/${ABI}/tick_synchronizer_protocol_benchmark.${selected_precision}"
            copy_binary "$android_binary" "$PACKAGE_ROOT/benchmarks/bin/android/${ABI}"
            if [[ -f "${android_binary}.elf-dynamic.txt" ]]; then
                mkdir -p -- "$PACKAGE_ROOT/benchmarks/bin/android/${ABI}"
                cp -- "${android_binary}.elf-dynamic.txt" "$PACKAGE_ROOT/benchmarks/bin/android/${ABI}/"
            fi
            ;;
    esac
done < <(selected_precisions)

case "$PLATFORM" in
    linuxbsd)
        mkdir -p -- "$PACKAGE_ROOT/scripts"
        cp --preserve=mode -- "$SCRIPT_DIR/run_protocol_benchmarks.sh" "$PACKAGE_ROOT/scripts/"
        cp --preserve=mode -- "$SCRIPT_DIR/verify_benchmark_results.py" "$PACKAGE_ROOT/scripts/"
        ;;
    windows)
        cp -- "$SCRIPT_DIR/run_protocol_benchmarks_windows.ps1" "$PACKAGE_ROOT/"
        ;;
    android)
        mkdir -p -- "$PACKAGE_ROOT/scripts"
        cp --preserve=mode -- "$SCRIPT_DIR/run_protocol_benchmarks_android.sh" "$PACKAGE_ROOT/scripts/"
        cp --preserve=mode -- "$SCRIPT_DIR/verify_benchmark_results.py" "$PACKAGE_ROOT/scripts/"
        ;;
esac

cp -- "$MODULE_DIR/LICENSE" "$PACKAGE_ROOT/"
cp -- "$MODULE_DIR/GODOT_COMMIT" "$PACKAGE_ROOT/"
cp -- "$MODULE_DIR/GODOT_VERSION" "$PACKAGE_ROOT/"
write_readme "$PACKAGE_ROOT"
{
    printf 'TickSynchronizer protocol benchmark deployment package\n'
    printf 'Generated UTC: %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
    printf 'Platform: %s\n' "$PLATFORM"
    printf 'Architecture: %s\n' "${PLATFORM_TAG#*-}"
    printf 'Precision: %s\n' "$PRECISION"
    printf 'Benchmark suite: %s\n' "$SUITE_VERSION"
    printf 'Module commit: %s\n' "$MODULE_COMMIT"
    printf 'Source state: %s\n' "$SOURCE_STATE"
    printf 'Toolchain: %s\n' "$TOOLCHAIN"
} > "$PACKAGE_ROOT/PACKAGE_METADATA.txt"

(
    cd -- "$PACKAGE_ROOT"
    find . -type f ! -name SHA256SUMS.txt -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS.txt
)

TEMP_ZIP="$STAGING_DIR/${PACKAGE_NAME}.zip"
(
    cd -- "$STAGING_DIR"
    zip -q -r "$TEMP_ZIP" "$PACKAGE_NAME"
)
FINAL_ZIP="$OUTPUT_DIR/${PACKAGE_NAME}.zip"
mv -f -- "$TEMP_ZIP" "$FINAL_ZIP"
sha256sum "$FINAL_ZIP" > "${FINAL_ZIP}.sha256"

printf 'TICKSYNCHRONIZER_BENCHMARK_EXPORT_OK platform=%s precision=%s package=%s\n' \
    "$PLATFORM" "$PRECISION" "$FINAL_ZIP"

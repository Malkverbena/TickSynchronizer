#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BENCHMARK_DIR="${MODULE_DIR}/benchmarks"
PRECISION="double"
ABI="arm64-v8a"
API_LEVEL="24"
JOBS=""
SERIAL=""
CPU=""
CPU_CLASS="explicit"
CPU_EXPLICIT=0
QUICK=0
NO_BUILD=0
ALLOW_DIRTY=0
OUTPUT_ROOT="${MODULE_DIR}/benchmark_reports"
BINARY_DIR="${TICKSYNC_BENCHMARK_BINARY_DIR:-}"
EXECUTION_ONLY=0
LIST_CPUS=0
EXTRA_ARGS=()

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/run_protocol_benchmarks_android.sh [options] [-- benchmark-arguments]

Options:
  --precision single|double|all  Precision. Default: double.
  --serial SERIAL               Selects one adb device.
  --cpu N                       Pins the native benchmark thread to logical CPU N.
  --cpu-class CLASS             Selects efficiency, performance, or prime by max frequency.
  --list-cpus                   Prints detected CPU topology and exits.
  --abi ABI                     Android ABI. Default: arm64-v8a.
  --api-level N                 Android API level used for a build. Default: 24.
  --jobs N                      Parallel build jobs.
  --quick                       Runs the short qualification profile.
  --no-build                    Uses an existing Android binary.
  --allow-dirty                 Allows a full diagnostic run on a dirty tree.
  --execution-only              Runs an exported package without source or build tools.
  --binary-dir PATH             Overrides the Android binary directory.
  --output-dir PATH             Root report directory.
  -h, --help                    Shows this help.
USAGE
}

adb_command() {
    if [[ -n "$SERIAL" ]]; then
        adb -s "$SERIAL" "$@"
    else
        adb "$@"
    fi
}

adb_shell() {
    adb_command shell "$@"
}

prop() {
    local name="$1"
    local value
    value="$(adb_shell getprop "$name" 2>/dev/null | tr -d '\r\n')"
    [[ -n "$value" ]] && printf '%s' "$value" || printf 'unknown'
}

sanitize_slug() {
    tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g; s/^-+|-+$//g'
}

shell_quote() {
    local value="$1"
    printf "'%s'" "${value//\'/\'\\\'\'}"
}

cpu_topology() {
    adb_shell 'for c in /sys/devices/system/cpu/cpu[0-9]*; do n=${c##*cpu}; online=$(cat "$c/online" 2>/dev/null || echo 1); max=$(cat "$c/cpufreq/cpuinfo_max_freq" 2>/dev/null || echo unknown); min=$(cat "$c/cpufreq/cpuinfo_min_freq" 2>/dev/null || echo unknown); core=$(cat "$c/topology/core_id" 2>/dev/null || echo unknown); echo "$n $online $min $max $core"; done' | tr -d '\r'
}

select_cpu_class() {
    local class="$1"
    local topology frequencies selected
    topology="$(cpu_topology)"
    frequencies="$(awk '$2 == 1 && $4 ~ /^[0-9]+$/ {print $4}' <<<"$topology" | sort -n -u)"
    [[ -n "$frequencies" ]] || fail "the device did not expose readable CPU maximum frequencies"
    case "$class" in
        efficiency) selected="$(head -n 1 <<<"$frequencies")" ;;
        prime) selected="$(tail -n 1 <<<"$frequencies")" ;;
        performance)
            local frequency_count
            frequency_count="$(wc -l <<<"$frequencies" | tr -d ' ')"
            if (( frequency_count >= 3 )); then
                selected="$(tail -n 2 <<<"$frequencies" | head -n 1)"
            else
                selected="$(tail -n 1 <<<"$frequencies")"
            fi
            ;;
        *) fail "invalid CPU class: $class" ;;
    esac
    awk -v target="$selected" '$2 == 1 && $4 == target {print $1; exit}' <<<"$topology"
}

thermal_snapshot() {
    local prefix="$1"
    adb_shell 'for z in /sys/class/thermal/thermal_zone*; do [ -d "$z" ] || continue; t=$(cat "$z/type" 2>/dev/null || echo unknown); v=$(cat "$z/temp" 2>/dev/null || echo unknown); echo "$t=$v"; done' | \
        tr -d '\r' | sed "s/^/${prefix} thermal /"
}

verify_preconditions() {
    command -v adb >/dev/null 2>&1 || fail "adb not found"
    command -v python3 >/dev/null 2>&1 || fail "python3 not found"
    command -v sha256sum >/dev/null 2>&1 || fail "sha256sum not found"
    adb_command get-state >/dev/null 2>&1 || fail "selected adb device is not available"
    local device_abi
    device_abi="$(prop ro.product.cpu.abi)"
    [[ "$device_abi" == "$ABI" ]] || fail "device ABI is $device_abi but the selected benchmark ABI is $ABI"
    if (( ! EXECUTION_ONLY )); then
        command -v git >/dev/null 2>&1 || fail "git not found"
        local dirty
        dirty="$(git -C "$MODULE_DIR" status --porcelain --untracked-files=all)"
        if (( ! LIST_CPUS && ! QUICK && ! ALLOW_DIRTY )) && [[ -n "$dirty" ]]; then
            fail "official Android benchmark requires a clean Git tree; commit the work or use --quick"
        fi
    fi
    if (( ! LIST_CPUS && ! QUICK && ! CPU_EXPLICIT )); then
        fail "official Android benchmark requires an explicit --cpu N; --cpu-class is available only for quick diagnostics"
    fi
}

verify_package_integrity() {
    if (( ! EXECUTION_ONLY )) || [[ ! -f "$MODULE_DIR/PACKAGE_METADATA.txt" ]]; then
        return
    fi
    [[ -f "$MODULE_DIR/SHA256SUMS.txt" ]] || fail "exported package is missing SHA256SUMS.txt"
    if ! (cd "$MODULE_DIR" && sha256sum -c SHA256SUMS.txt >/dev/null); then
        fail "exported package integrity verification failed"
    fi
    printf 'TICKSYNCHRONIZER_BENCHMARK_PACKAGE_INTEGRITY_OK platform=android\n'
}

run_one() {
    local precision="$1"
    local selected_binary_dir="${BINARY_DIR:-${BENCHMARK_DIR}/bin/android/${ABI}}"
    local binary="${selected_binary_dir}/tick_synchronizer_protocol_benchmark.${precision}"
    if (( ! NO_BUILD )); then
        local -a build=("${SCRIPT_DIR}/build_protocol_benchmarks_android.sh" --precision "$precision" --abi "$ABI" --api-level "$API_LEVEL")
        [[ -z "$JOBS" ]] || build+=(--jobs "$JOBS")
        "${build[@]}"
    fi
    [[ -f "$binary" ]] || fail "Android benchmark binary not found: $binary"

    local manufacturer model android_version api fingerprint soc hardware device_slug
    manufacturer="$(prop ro.product.manufacturer)"
    model="$(prop ro.product.model)"
    android_version="$(prop ro.build.version.release)"
    api="$(prop ro.build.version.sdk)"
    fingerprint="$(prop ro.build.fingerprint)"
    soc="$(prop ro.soc.model)"
    [[ "$soc" != unknown ]] || soc="$(prop ro.hardware)"
    hardware="$(prop ro.hardware)"
    device_slug="$(printf '%s-%s' "$manufacturer" "$model" | sanitize_slug)"

    local timestamp report_dir remote_dir remote_binary remote_json remote_csv remote_script
    timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
    report_dir="${OUTPUT_ROOT}/${timestamp}-android-${device_slug}-${precision}-suite1"
    remote_dir="/data/local/tmp/ticksynchronizer-benchmark"
    remote_binary="${remote_dir}/tick_synchronizer_protocol_benchmark.${precision}"
    remote_json="${remote_dir}/results.${precision}.json"
    remote_csv="${remote_dir}/results.${precision}.csv"
    remote_script="${remote_dir}/run.${precision}.sh"
    mkdir -p "$report_dir"
    adb_shell mkdir -p "$remote_dir"
    adb_command push "$binary" "$remote_binary" >/dev/null
    adb_shell chmod 755 "$remote_binary"

    local local_hash remote_hash
    local_hash="$(sha256sum "$binary" | awk '{print $1}')"
    remote_hash="$(adb_shell "toybox sha256sum '$remote_binary' 2>/dev/null || sha256sum '$remote_binary' 2>/dev/null" | tr -d '\r' | awk '{print $1}')"
    [[ -n "$remote_hash" ]] || fail "could not compute the adb binary SHA-256"
    [[ "$remote_hash" == "$local_hash" ]] || fail "adb binary hash mismatch"

    local max_freq min_freq governor cpu_model
    max_freq="$(adb_shell "cat /sys/devices/system/cpu/cpu${CPU}/cpufreq/cpuinfo_max_freq 2>/dev/null || echo unknown" | tr -d '\r\n')"
    min_freq="$(adb_shell "cat /sys/devices/system/cpu/cpu${CPU}/cpufreq/cpuinfo_min_freq 2>/dev/null || echo unknown" | tr -d '\r\n')"
    governor="$(adb_shell "cat /sys/devices/system/cpu/cpu${CPU}/cpufreq/scaling_governor 2>/dev/null || echo unknown" | tr -d '\r\n')"
    cpu_model="$soc"

    local host_script="${report_dir}/remote-run.sh"
    {
        printf '#!/system/bin/sh\nset -eu\n'
        printf 'export TICKSYNC_BENCHMARK_RUNTIME_BACKEND=%s\n' "$(shell_quote android-adb)"
        printf 'export TICKSYNC_BENCHMARK_DEVICE_MANUFACTURER=%s\n' "$(shell_quote "$manufacturer")"
        printf 'export TICKSYNC_BENCHMARK_DEVICE_MODEL=%s\n' "$(shell_quote "$model")"
        printf 'export TICKSYNC_BENCHMARK_OS_VERSION=%s\n' "$(shell_quote "Android ${android_version} API ${api}")"
        printf 'export TICKSYNC_BENCHMARK_OS_BUILD=%s\n' "$(shell_quote "$fingerprint")"
        printf 'export TICKSYNC_BENCHMARK_SOC_MODEL=%s\n' "$(shell_quote "$soc")"
        printf 'export TICKSYNC_BENCHMARK_EXECUTABLE_PATH=%s\n' "$(shell_quote "$remote_binary")"
        printf 'export TICKSYNC_BENCHMARK_BINARY_SHA256=%s\n' "$(shell_quote "$local_hash")"
        printf 'export TICKSYNC_BENCHMARK_CPU_MODEL=%s\n' "$(shell_quote "$cpu_model")"
        printf 'export TICKSYNC_BENCHMARK_CPU_CLASS=%s\n' "$(shell_quote "$CPU_CLASS")"
        printf 'export TICKSYNC_BENCHMARK_SCALING_DRIVER=%s\n' "$(shell_quote cpufreq)"
        printf 'export TICKSYNC_BENCHMARK_SCALING_GOVERNOR=%s\n' "$(shell_quote "$governor")"
        printf 'export TICKSYNC_BENCHMARK_CPU_MIN_FREQUENCY_KHZ=%s\n' "$(shell_quote "$min_freq")"
        printf 'export TICKSYNC_BENCHMARK_CPU_MAX_FREQUENCY_KHZ=%s\n' "$(shell_quote "$max_freq")"
        printf '%s --self-test\n' "$(shell_quote "$remote_binary")"
        printf 'exec %s --cpu %s --json %s --csv %s' \
            "$(shell_quote "$remote_binary")" "$CPU" "$(shell_quote "$remote_json")" "$(shell_quote "$remote_csv")"
        (( QUICK )) && printf ' --quick'
        local arg
        for arg in "${EXTRA_ARGS[@]}"; do printf ' %s' "$(shell_quote "$arg")"; done
        printf '\n'
    } > "$host_script"
    adb_command push "$host_script" "$remote_script" >/dev/null
    adb_shell chmod 755 "$remote_script"

    {
        printf 'Generated UTC: %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
        printf 'Device: %s %s\n' "$manufacturer" "$model"
        printf 'Android: %s API %s\n' "$android_version" "$api"
        printf 'Build fingerprint: %s\n' "$fingerprint"
        printf 'SoC: %s\n' "$soc"
        printf 'Hardware: %s\n' "$hardware"
        printf 'ABI: %s\n' "$ABI"
        printf 'Precision: %s\n' "$precision"
        printf 'Logical CPU: %s\n' "$CPU"
        printf 'CPU class: %s\n' "$CPU_CLASS"
        printf 'CPU min/max kHz: %s / %s\n' "$min_freq" "$max_freq"
        printf 'Governor: %s\n' "$governor"
        printf 'Binary SHA-256: %s\n' "$local_hash"
        printf '\nCPU topology:\n%s\n' "$(cpu_topology)"
        printf '\nBattery before:\n'
        adb_shell dumpsys battery 2>/dev/null | tr -d '\r' || true
        printf '\nThermal before:\n'
        thermal_snapshot before || true
    } > "$report_dir/environment.txt"

    adb_shell sh "$remote_script" 2>&1 | tee "$report_dir/benchmark.log"
    adb_command pull "$remote_json" "$report_dir/results.json" >/dev/null
    adb_command pull "$remote_csv" "$report_dir/results.csv" >/dev/null

    {
        printf '\nBattery after:\n'
        adb_shell dumpsys battery 2>/dev/null | tr -d '\r' || true
        printf '\nThermal after:\n'
        thermal_snapshot after || true
    } >> "$report_dir/environment.txt"

    local -a verify_args=("$report_dir/results.json")
    (( QUICK || ALLOW_DIRTY )) && verify_args+=(--allow-dirty)
    python3 "$SCRIPT_DIR/verify_benchmark_results.py" "${verify_args[@]}"
    find "$report_dir" -maxdepth 1 -type f ! -name SHA256SUMS.txt -print0 | sort -z | \
        xargs -0 sha256sum > "$report_dir/SHA256SUMS.txt"
    printf 'TICKSYNCHRONIZER_ANDROID_BENCHMARK_REPORT_OK precision=%s cpu=%s device=%s report=%s\n' \
        "$precision" "$CPU" "$device_slug" "$report_dir"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --precision) [[ $# -ge 2 ]] || fail "--precision requires a value"; PRECISION="$2"; shift 2 ;;
        --serial) [[ $# -ge 2 ]] || fail "--serial requires a value"; SERIAL="$2"; shift 2 ;;
        --cpu) [[ $# -ge 2 ]] || fail "--cpu requires a value"; CPU="$2"; CPU_CLASS="explicit"; CPU_EXPLICIT=1; shift 2 ;;
        --cpu-class) [[ $# -ge 2 ]] || fail "--cpu-class requires a value"; CPU_CLASS="$2"; shift 2 ;;
        --list-cpus) LIST_CPUS=1; shift ;;
        --abi) [[ $# -ge 2 ]] || fail "--abi requires a value"; ABI="$2"; shift 2 ;;
        --api-level) [[ $# -ge 2 ]] || fail "--api-level requires a value"; API_LEVEL="$2"; shift 2 ;;
        --jobs) [[ $# -ge 2 ]] || fail "--jobs requires a value"; JOBS="$2"; shift 2 ;;
        --quick) QUICK=1; shift ;;
        --no-build) NO_BUILD=1; shift ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        --execution-only) EXECUTION_ONLY=1; NO_BUILD=1; shift ;;
        --binary-dir)
            [[ $# -ge 2 ]] || fail "--binary-dir requires a value"
            BINARY_DIR="$(realpath -m -- "$2")"
            shift 2
            ;;
        --output-dir) [[ $# -ge 2 ]] || fail "--output-dir requires a value"; OUTPUT_ROOT="$2"; shift 2 ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ "$PRECISION" == "single" || "$PRECISION" == "double" || "$PRECISION" == "all" ]] || \
    fail "invalid precision: $PRECISION"
[[ -z "$CPU" || "$CPU" =~ ^[0-9]+$ ]] || fail "invalid CPU: $CPU"
verify_package_integrity
verify_preconditions
if (( LIST_CPUS )); then
    printf 'cpu online min_khz max_khz core\n'
    cpu_topology
    exit 0
fi
if [[ -z "$CPU" && "$CPU_CLASS" != explicit ]]; then
    CPU="$(select_cpu_class "$CPU_CLASS")"
    [[ -n "$CPU" ]] || fail "could not select CPU class: $CPU_CLASS"
fi
[[ -n "$CPU" ]] || fail "--cpu or --cpu-class is required"
mkdir -p "$OUTPUT_ROOT"

if [[ "$PRECISION" == all ]]; then
    run_one double
    run_one single
else
    run_one "$PRECISION"
fi

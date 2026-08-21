#!/usr/bin/env bash
# Runs prebuilt Universal 2 benchmarks with only standard macOS runtime tools.
# Records scheduler-managed provenance without pretending to hard-pin a CPU.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
if [[ -f "$SCRIPT_DIR/PACKAGE_METADATA.txt" ]]; then
    PACKAGE_ROOT="$SCRIPT_DIR"
    DEFAULT_BINARY_DIR="$SCRIPT_DIR"
    DEFAULT_OUTPUT_ROOT="$SCRIPT_DIR/benchmark_reports"
else
    PACKAGE_ROOT=""
    MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
    DEFAULT_BINARY_DIR="$MODULE_DIR/benchmarks/bin/macos/universal2"
    DEFAULT_OUTPUT_ROOT="$MODULE_DIR/benchmark_reports"
fi

PRECISION="all"
CANDIDATE="all"
BINARY_DIR="$DEFAULT_BINARY_DIR"
OUTPUT_ROOT="$DEFAULT_OUTPUT_ROOT"
QUICK=0
ALLOW_DIRTY=0
EXTRA_ARGS=()

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
Usage:
  ./run_protocol_benchmarks_macos.sh [options] [-- benchmark-arguments]

Options:
  --precision single|double|all  Precision. Default: all.
  --candidate reference_fixed_width|varint_zigzag_fixed_float|all
                                Candidate. Default: all.
  --quick                       Runs the short qualification profile.
  --allow-dirty                 Allows a full diagnostic run from a dirty build.
  --binary-dir PATH             Overrides the Universal 2 binary directory.
  --output-dir PATH             Root report directory.
  -h, --help                    Shows this help.

macOS uses a documented scheduler-managed representative configuration because
its public affinity policy does not provide hard logical-CPU pinning. Official
reports therefore use the exact unbound macOS policy instead of --cpu.
USAGE
}

sysctl_value() {
    local name="$1"
    local value
    value="$(/usr/sbin/sysctl -n "$name" 2>/dev/null || true)"
    [[ -n "$value" ]] && printf '%s' "$value" || printf 'unknown'
}

hz_to_khz() {
    local value="$1"
    if [[ "$value" =~ ^[0-9]+$ ]]; then
        printf '%s' "$((value / 1000))"
    else
        printf 'unknown'
    fi
}

slugify() {
    tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g; s/^-+|-+$//g'
}

verify_package_integrity() {
    [[ -n "$PACKAGE_ROOT" ]] || return
    [[ -f "$PACKAGE_ROOT/SHA256SUMS.txt" ]] || fail "exported package is missing SHA256SUMS.txt"
    if ! (cd -- "$PACKAGE_ROOT" && /usr/bin/shasum -a 256 -c SHA256SUMS.txt >/dev/null); then
        fail "exported package integrity verification failed"
    fi
    printf 'TICKSYNCHRONIZER_BENCHMARK_PACKAGE_INTEGRITY_OK platform=macos\n'
}

require_report_line() {
    local report="$1"
    local expected="$2"
    /usr/bin/grep -Fq -- "$expected" "$report" || fail "report contract is missing: $expected"
}

verify_report() {
    local report="$1"
    local binary_hash="$2"
    local expected_architecture="$3"
    local expected_candidate="$4"
    local source_message_count round_trip_count determinism_count rejected_count

    /usr/bin/plutil -lint "$report" >/dev/null || fail "macOS report is not valid JSON: $report"
    require_report_line "$report" '"schema_version": 3'
    require_report_line "$report" '"benchmark_suite_version": 2'
    require_report_line "$report" '"api_version": 4'
    require_report_line "$report" '"wire_protocol_version": 0'
    require_report_line "$report" '"wire_protocol_revision": 2'
    require_report_line "$report" '"platform": "macOS"'
    require_report_line "$report" '"runtime_backend": "macos-native"'
    require_report_line "$report" "\"name\": \"${expected_candidate}\""
    require_report_line "$report" "\"architecture\": \"${expected_architecture}\""
    require_report_line "$report" "\"binary_sha256\": \"${binary_hash}\""
    require_report_line "$report" '"logical_cpu": "unbound"'
    require_report_line "$report" '"cpu_class": "representative"'
    require_report_line "$report" '"processor_group": "unsupported"'
    require_report_line "$report" '"affinity_requested": "no"'
    require_report_line "$report" '"affinity_applied": "no"'
    require_report_line "$report" '"affinity_actual_cpu": "unknown"'
    require_report_line "$report" '"affinity_error": "unsupported-by-platform-policy"'
    require_report_line "$report" '"accepted": 0'
    rejected_count="$(/usr/bin/sed -nE 's/.*"rejected": ([0-9]+).*/\1/p' "$report" | /usr/bin/head -n 1)"
    [[ "$rejected_count" =~ ^[1-9][0-9]*$ ]] || fail "macOS invalid-packet corpus was not rejected"

    source_message_count="$(/usr/bin/grep -Fc '"source_message_count":' "$report")"
    round_trip_count="$(/usr/bin/grep -Fc '"round_trip_failures": 0' "$report")"
    determinism_count="$(/usr/bin/grep -Fc '"determinism_failures": 0' "$report")"
    [[ "$source_message_count" == "8" ]] || fail "unexpected dataset count in macOS report: $source_message_count"
    [[ "$round_trip_count" == "8" ]] || fail "one or more macOS round-trip gates are missing"
    [[ "$determinism_count" == "8" ]] || fail "one or more macOS determinism gates are missing"

    if (( QUICK )); then
        require_report_line "$report" '"official_eligible": false'
    elif (( ALLOW_DIRTY )); then
        require_report_line "$report" '"source_state": "dirty"'
        require_report_line "$report" '"official_eligible": false'
    else
        require_report_line "$report" '"source_state": "clean"'
        require_report_line "$report" '"official_eligible": true'
    fi
}

privacy_check_report() {
    local report_dir="$1"
    if LC_ALL=C /usr/bin/grep -ERq '/Users/|/Volumes/|/private/var/folders/' "$report_dir"; then
        fail "report contains a host-specific path and cannot be archived"
    fi
}

write_report_hashes() {
    local report_dir="$1"
    (
        cd -- "$report_dir"
        local file
        for file in *; do
            [[ -f "$file" && "$file" != "SHA256SUMS.txt" ]] || continue
            /usr/bin/shasum -a 256 "$file"
        done
    ) > "$report_dir/SHA256SUMS.txt"
}

run_one() {
    local selected_precision="$1"
    local selected_candidate="$2"
    local binary="$BINARY_DIR/tick_synchronizer_protocol_benchmark.${selected_precision}"
    local host_architecture report_architecture translated
    local device_model cpu_model os_version os_build logical_cpus physical_cpus
    local min_frequency_hz max_frequency_hz min_frequency_khz max_frequency_khz
    local timestamp device_slug report_dir binary_hash quarantine_state binary_name
    local json_path csv_path log_path

    [[ -x "$binary" ]] || fail "Universal 2 benchmark binary not found: $binary"
    host_architecture="$(uname -m)"
    translated="$(/usr/sbin/sysctl -n sysctl.proc_translated 2>/dev/null || printf '0')"
    [[ "$translated" != "1" ]] || fail "run the benchmark from a native shell instead of Rosetta"
    case "$host_architecture" in
        x86_64) report_architecture="x86_64" ;;
        arm64) report_architecture="aarch64" ;;
        *) fail "unsupported native macOS architecture: $host_architecture" ;;
    esac

    device_model="$(sysctl_value hw.model)"
    cpu_model="$(sysctl_value machdep.cpu.brand_string)"
    [[ "$cpu_model" != "unknown" ]] || cpu_model="$device_model"
    os_version="$(/usr/bin/sw_vers -productVersion)"
    os_build="$(/usr/bin/sw_vers -buildVersion)"
    logical_cpus="$(sysctl_value hw.logicalcpu)"
    physical_cpus="$(sysctl_value hw.physicalcpu)"
    min_frequency_hz="$(sysctl_value hw.cpufrequency_min)"
    max_frequency_hz="$(sysctl_value hw.cpufrequency_max)"
    min_frequency_khz="$(hz_to_khz "$min_frequency_hz")"
    max_frequency_khz="$(hz_to_khz "$max_frequency_hz")"
    binary_name="${binary##*/}"
    binary_hash="$(/usr/bin/shasum -a 256 "$binary" | /usr/bin/awk '{print $1}')"
    timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
    device_slug="$(printf '%s' "$device_model" | slugify)"
    [[ -n "$device_slug" ]] || device_slug="unknown-mac"
    report_dir="$OUTPUT_ROOT/${timestamp}-macos-${device_slug}-representative-${selected_candidate}-${selected_precision}-suite2"
    mkdir -p "$report_dir"
    json_path="$report_dir/results.json"
    csv_path="$report_dir/results.csv"
    log_path="$report_dir/benchmark.log"

    if /usr/bin/xattr -p com.apple.quarantine "$binary" >/dev/null 2>&1; then
        quarantine_state="present"
    else
        quarantine_state="absent"
    fi

    local -a metadata_env=(
        "TICKSYNC_BENCHMARK_EXECUTABLE_PATH=${binary_name}"
        "TICKSYNC_BENCHMARK_BINARY_SHA256=${binary_hash}"
        "TICKSYNC_BENCHMARK_RUNTIME_BACKEND=macos-native"
        "TICKSYNC_BENCHMARK_DEVICE_MANUFACTURER=Apple Inc."
        "TICKSYNC_BENCHMARK_DEVICE_MODEL=${device_model}"
        "TICKSYNC_BENCHMARK_OS_VERSION=macOS ${os_version}"
        "TICKSYNC_BENCHMARK_OS_BUILD=${os_build}"
        "TICKSYNC_BENCHMARK_SOC_MODEL=${cpu_model}"
        "TICKSYNC_BENCHMARK_CPU_MODEL=${cpu_model}"
        "TICKSYNC_BENCHMARK_CPU_CLASS=representative"
        "TICKSYNC_BENCHMARK_LOGICAL_CPU=unbound"
        "TICKSYNC_BENCHMARK_CPU_CORE=unsupported-by-platform-policy"
        "TICKSYNC_BENCHMARK_CPU_PACKAGE=unsupported-by-platform-policy"
        "TICKSYNC_BENCHMARK_NUMA_NODE=unsupported-by-platform-policy"
        "TICKSYNC_BENCHMARK_L3_CACHE_ID=unsupported-by-platform-policy"
        "TICKSYNC_BENCHMARK_THREAD_SIBLINGS=unsupported-by-platform-policy"
        "TICKSYNC_BENCHMARK_SCALING_DRIVER=macos-scheduler"
        "TICKSYNC_BENCHMARK_SCALING_GOVERNOR=system-managed"
        "TICKSYNC_BENCHMARK_CPU_MIN_FREQUENCY_KHZ=${min_frequency_khz}"
        "TICKSYNC_BENCHMARK_CPU_MAX_FREQUENCY_KHZ=${max_frequency_khz}"
    )
    local -a command=(/usr/bin/env "${metadata_env[@]}" "$binary" --candidate "$selected_candidate" --json "$json_path" --csv "$csv_path")
    (( QUICK )) && command+=(--quick)
    command+=("${EXTRA_ARGS[@]}")

    {
        printf 'Generated UTC: %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
        printf 'Platform: macOS\n'
        printf 'OS version: %s\n' "$os_version"
        printf 'OS build: %s\n' "$os_build"
        printf 'Device model: %s\n' "$device_model"
        printf 'CPU model: %s\n' "$cpu_model"
        printf 'Native architecture: %s\n' "$host_architecture"
        printf 'Translated process: %s\n' "$translated"
        printf 'Logical CPUs: %s\n' "$logical_cpus"
        printf 'Physical CPUs: %s\n' "$physical_cpus"
        printf 'CPU class: representative\n'
        printf 'CPU execution policy: scheduler-managed-unbound\n'
        printf 'Precision: %s\n' "$selected_precision"
        printf 'Candidate: %s\n' "$selected_candidate"
        printf 'Quick: %s\n' "$QUICK"
        printf 'Allow dirty: %s\n' "$ALLOW_DIRTY"
        printf 'Binary: %s\n' "$binary_name"
        printf 'Binary SHA-256: %s\n' "$binary_hash"
        printf 'Quarantine attribute: %s\n' "$quarantine_state"
        printf '\nThermal state before:\n'
        /usr/bin/pmset -g therm 2>/dev/null || printf 'unavailable\n'
    } > "$report_dir/environment.txt"

    "$binary" --self-test --candidate "$selected_candidate" 2>&1 | /usr/bin/tee "$report_dir/self-test.log"
    "${command[@]}" 2>&1 | /usr/bin/tee "$log_path"
    verify_report "$json_path" "$binary_hash" "$report_architecture" "$selected_candidate"

    {
        printf '\nThermal state after:\n'
        /usr/bin/pmset -g therm 2>/dev/null || printf 'unavailable\n'
    } >> "$report_dir/environment.txt"
    privacy_check_report "$report_dir"
    write_report_hashes "$report_dir"
    local official_state="no"
    (( ! QUICK && ! ALLOW_DIRTY )) && official_state="yes"
    printf 'TICKSYNCHRONIZER_MACOS_BENCHMARK_REPORT_OK candidate=%s precision=%s architecture=%s policy=scheduler-managed official=%s report=%s\n' \
        "$selected_candidate" "$selected_precision" "$host_architecture" "$official_state" "$report_dir"
}

run_precision() {
    local selected_precision="$1"
    if [[ "$CANDIDATE" == all ]]; then
        run_one "$selected_precision" reference_fixed_width
        run_one "$selected_precision" varint_zigzag_fixed_float
    else
        run_one "$selected_precision" "$CANDIDATE"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --precision) [[ $# -ge 2 ]] || fail "--precision requires a value"; PRECISION="$2"; shift 2 ;;
        --candidate) [[ $# -ge 2 ]] || fail "--candidate requires a value"; CANDIDATE="$2"; shift 2 ;;
        --quick) QUICK=1; shift ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        --binary-dir) [[ $# -ge 2 ]] || fail "--binary-dir requires a value"; BINARY_DIR="$2"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || fail "--output-dir requires a value"; OUTPUT_ROOT="$2"; shift 2 ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || fail "the macOS runner must execute on macOS"
[[ "$PRECISION" == "single" || "$PRECISION" == "double" || "$PRECISION" == "all" ]] || \
    fail "invalid precision: $PRECISION"
[[ "$CANDIDATE" == "reference_fixed_width" || "$CANDIDATE" == "varint_zigzag_fixed_float" || "$CANDIDATE" == "all" ]] || \
    fail "invalid candidate: $CANDIDATE"
(( !(QUICK && ALLOW_DIRTY) )) || \
    fail "--quick and --allow-dirty are mutually exclusive; quick runs are already diagnostic"
for required_tool in /usr/bin/shasum /usr/bin/plutil /usr/bin/sw_vers /usr/bin/xattr /usr/bin/pmset /usr/sbin/sysctl; do
    [[ -x "$required_tool" ]] || fail "required macOS runtime tool not found: $required_tool"
done
verify_package_integrity
if [[ -n "$PACKAGE_ROOT" && ! QUICK ]]; then
    if (( ALLOW_DIRTY )); then
        /usr/bin/grep -Fq 'Source state: dirty' "$PACKAGE_ROOT/PACKAGE_METADATA.txt" || \
            fail "--allow-dirty requires a package built from a dirty source tree"
    else
        /usr/bin/grep -Fq 'Source state: clean' "$PACKAGE_ROOT/PACKAGE_METADATA.txt" || \
            fail "official benchmark requires a package built from a clean source tree; use --quick or --allow-dirty for diagnosis"
    fi
fi
mkdir -p "$OUTPUT_ROOT"

if [[ "$PRECISION" == "all" ]]; then
    run_precision double
    run_precision single
else
    run_precision "$PRECISION"
fi

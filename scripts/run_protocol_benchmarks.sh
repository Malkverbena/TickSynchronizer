#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BENCHMARK_DIR="${MODULE_DIR}/benchmarks"
PRECISION="double"
JOBS=""
CPU=""
CPU_CLASS="desktop"
L3_CACHE_ID=""
LIST_CPUS=0
QUICK=0
NO_BUILD=0
USE_PERF=0
ALLOW_DIRTY=0
ALLOW_UNPINNED=0
OUTPUT_ROOT="${MODULE_DIR}/benchmark_reports"
BINARY_DIR="${TICKSYNC_BENCHMARK_BINARY_DIR:-}"
EXECUTION_ONLY=0
EXTRA_ARGS=()

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/run_protocol_benchmarks.sh [options] [-- benchmark-arguments]

Options:
  --precision single|double|all  Precision. Default: double.
  --jobs N                      Parallel jobs used by the build.
  --cpu N                       Pins the benchmark thread to logical CPU N.
  --cpu-class LABEL             Records the selected CPU class or CCD label.
  --l3-cache-id ID              Selects a primary thread from Linux L3 domain ID.
  --list-cpus                   Prints Linux CPU, L3, SMT, and frequency topology.
  --quick                       Short profile; permits a dirty tree and unpinned execution.
  --no-build                    Uses previously built binaries.
  --perf                        Records perf stat when available.
  --allow-dirty                 Allows a full run on a dirty tree; diagnostics only.
  --allow-unpinned              Allows a full run without --cpu; diagnostics only.
  --execution-only              Runs exported binaries without source or build tools.
  --binary-dir PATH             Overrides the directory containing Linux binaries.
  --output-dir PATH             Root report directory.
  -h, --help                    Shows this help.
USAGE
}

read_text_file() {
    local path="$1"
    if [[ -r "$path" ]]; then
        tr -d '\n' < "$path"
    else
        printf 'unknown'
    fi
}

cpu_property() {
    local relative="$1"
    if [[ -z "$CPU" ]]; then
        printf 'unbound'
        return
    fi
    read_text_file "/sys/devices/system/cpu/cpu${CPU}/${relative}"
}

find_numa_node() {
    if [[ -z "$CPU" ]]; then
        printf 'unbound'
        return
    fi
    local node
    node="$(find "/sys/devices/system/cpu/cpu${CPU}" -maxdepth 1 -type l -name 'node[0-9]*' -printf '%f\n' 2>/dev/null | head -n 1)"
    [[ -n "$node" ]] && printf '%s' "${node#node}" || printf 'unknown'
}

find_l3_cache_id() {
    if [[ -z "$CPU" ]]; then
        printf 'unbound'
        return
    fi
    local index
    for index in /sys/devices/system/cpu/cpu"${CPU}"/cache/index*; do
        [[ -d "$index" ]] || continue
        if [[ "$(read_text_file "$index/level")" == "3" ]]; then
            read_text_file "$index/id"
            return
        fi
    done
    printf 'unknown'
}

find_l3_cache_path_for_cpu() {
    local logical_cpu="$1"
    local index
    for index in /sys/devices/system/cpu/cpu"${logical_cpu}"/cache/index*; do
        [[ -d "$index" ]] || continue
        if [[ "$(read_text_file "$index/level")" == "3" ]]; then
            printf '%s' "$index"
            return
        fi
    done
}

l3_property_for_cpu() {
    local logical_cpu="$1"
    local property="$2"
    local cache_path
    cache_path="$(find_l3_cache_path_for_cpu "$logical_cpu")"
    if [[ -n "$cache_path" ]]; then
        read_text_file "$cache_path/$property"
    else
        printf 'unknown'
    fi
}

numa_node_for_cpu() {
    local logical_cpu="$1"
    local node
    node="$(find "/sys/devices/system/cpu/cpu${logical_cpu}" -maxdepth 1 -type l -name 'node[0-9]*' -printf '%f\n' 2>/dev/null | head -n 1)"
    [[ -n "$node" ]] && printf '%s' "${node#node}" || printf 'unknown'
}

logical_cpu_numbers() {
    local path number
    for path in /sys/devices/system/cpu/cpu[0-9]*; do
        [[ -d "$path" ]] || continue
        number="${path##*cpu}"
        [[ "$number" =~ ^[0-9]+$ ]] && printf '%s\n' "$number"
    done | sort -n
}

list_cpu_topology() {
    printf 'cpu\tonline\tcore\tpackage\tnuma\tl3_id\tl3_size\tl3_shared\tthread_siblings\tmin_khz\tmax_khz\n'
    local logical_cpu cpu_path online
    while IFS= read -r logical_cpu; do
        cpu_path="/sys/devices/system/cpu/cpu${logical_cpu}"
        online="$(read_text_file "$cpu_path/online")"
        [[ "$online" != unknown ]] || online=1
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$logical_cpu" \
            "$online" \
            "$(read_text_file "$cpu_path/topology/core_id")" \
            "$(read_text_file "$cpu_path/topology/physical_package_id")" \
            "$(numa_node_for_cpu "$logical_cpu")" \
            "$(l3_property_for_cpu "$logical_cpu" id)" \
            "$(l3_property_for_cpu "$logical_cpu" size)" \
            "$(l3_property_for_cpu "$logical_cpu" shared_cpu_list)" \
            "$(read_text_file "$cpu_path/topology/thread_siblings_list")" \
            "$(read_text_file "$cpu_path/cpufreq/cpuinfo_min_freq")" \
            "$(read_text_file "$cpu_path/cpufreq/cpuinfo_max_freq")"
    done < <(logical_cpu_numbers)
}

select_primary_cpu_from_l3() {
    local requested_l3="$1"
    local logical_cpu cpu_path online siblings first_sibling fallback=""
    while IFS= read -r logical_cpu; do
        cpu_path="/sys/devices/system/cpu/cpu${logical_cpu}"
        online="$(read_text_file "$cpu_path/online")"
        [[ "$online" != 0 ]] || continue
        [[ "$(l3_property_for_cpu "$logical_cpu" id)" == "$requested_l3" ]] || continue
        [[ -n "$fallback" ]] || fallback="$logical_cpu"
        siblings="$(read_text_file "$cpu_path/topology/thread_siblings_list")"
        first_sibling="${siblings%%,*}"
        first_sibling="${first_sibling%%-*}"
        if [[ "$logical_cpu" == "$first_sibling" ]]; then
            printf '%s' "$logical_cpu"
            return
        fi
    done < <(logical_cpu_numbers)
    printf '%s' "$fallback"
}

resolve_cpu_selection() {
    if [[ -n "$CPU" ]]; then
        [[ -d "/sys/devices/system/cpu/cpu${CPU}" ]] || fail "logical CPU does not exist: $CPU"
        local online
        online="$(read_text_file "/sys/devices/system/cpu/cpu${CPU}/online")"
        [[ "$online" != 0 ]] || fail "logical CPU is offline: $CPU"
    fi
    if [[ -n "$L3_CACHE_ID" ]]; then
        if [[ -z "$CPU" ]]; then
            CPU="$(select_primary_cpu_from_l3 "$L3_CACHE_ID")"
            [[ -n "$CPU" ]] || fail "no online logical CPU belongs to L3 cache ID $L3_CACHE_ID"
        elif [[ "$(l3_property_for_cpu "$CPU" id)" != "$L3_CACHE_ID" ]]; then
            fail "logical CPU $CPU does not belong to L3 cache ID $L3_CACHE_ID"
        fi
    fi
}

cpu_model_name() {
    if command -v lscpu >/dev/null 2>&1; then
        LC_ALL=C lscpu | sed -nE 's/^Model name:[[:space:]]*//p' | head -n 1
    else
        printf 'unknown'
    fi
}

linux_os_version() {
    if [[ -r /etc/os-release ]]; then
        . /etc/os-release
        printf '%s' "${PRETTY_NAME:-${NAME:-unknown}}"
    else
        printf 'unknown'
    fi
}

linux_device_model() {
    read_text_file /sys/class/dmi/id/product_name
}

linux_device_manufacturer() {
    read_text_file /sys/class/dmi/id/sys_vendor
}

thermal_snapshot() {
    local prefix="$1"
    local zone type temp
    for zone in /sys/class/thermal/thermal_zone*; do
        [[ -d "$zone" ]] || continue
        type="$(read_text_file "$zone/type")"
        temp="$(read_text_file "$zone/temp")"
        printf '%s thermal %s=%s\n' "$prefix" "$type" "$temp"
    done
}

verify_official_preconditions() {
    if (( ! EXECUTION_ONLY )); then
        git -C "$MODULE_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
            fail "the module is not a Git repository"
        local dirty
        dirty="$(git -C "$MODULE_DIR" status --porcelain --untracked-files=all)"
        if (( ! QUICK && ! ALLOW_DIRTY )) && [[ -n "$dirty" ]]; then
            fail "official benchmark requires a clean Git tree; commit the work or use --quick. --allow-dirty is diagnostic only"
        fi
    fi
    if (( ! QUICK && ! ALLOW_UNPINNED )) && [[ -z "$CPU" ]]; then
        fail "official benchmark requires --cpu N; --allow-unpinned is diagnostic only"
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
    printf 'TICKSYNCHRONIZER_BENCHMARK_PACKAGE_INTEGRITY_OK platform=linuxbsd\n'
}

run_one() {
    local precision="$1"
    local selected_binary_dir="${BINARY_DIR:-${BENCHMARK_DIR}/bin}"
    local binary="${selected_binary_dir}/tick_synchronizer_protocol_benchmark.${precision}"
    local timestamp cpu_class_slug
    timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
    cpu_class_slug="$(printf '%s' "$CPU_CLASS" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g; s/^-+|-+$//g')"
    [[ -n "$cpu_class_slug" ]] || cpu_class_slug=unspecified
    local report_dir="${OUTPUT_ROOT}/${timestamp}-linuxbsd-${cpu_class_slug}-cpu${CPU:-unbound}-${precision}-suite1"
    mkdir -p "$report_dir"

    if (( ! NO_BUILD )); then
        local -a build=("${SCRIPT_DIR}/build_protocol_benchmarks.sh" --precision "$precision")
        [[ -z "$JOBS" ]] || build+=(--jobs "$JOBS")
        "${build[@]}"
    fi
    [[ -x "$binary" ]] || fail "missing binary: $binary"

    local binary_sha256
    binary_sha256="$(sha256sum "$binary" | awk '{print $1}')"
    local logical_cpu="${CPU:-unbound}"
    local cpu_model device_model device_manufacturer os_version os_build
    cpu_model="$(cpu_model_name)"
    device_model="$(linux_device_model)"
    device_manufacturer="$(linux_device_manufacturer)"
    os_version="$(linux_os_version)"
    os_build="$(uname -r)"
    [[ -n "$cpu_model" ]] || cpu_model="unknown"
    local cpu_core cpu_package numa_node l3_cache_id l3_cache_size l3_shared_cpus thread_siblings scaling_driver scaling_governor cpu_min_freq cpu_max_freq
    cpu_core="$(cpu_property topology/core_id)"
    cpu_package="$(cpu_property topology/physical_package_id)"
    numa_node="$(find_numa_node)"
    l3_cache_id="$(find_l3_cache_id)"
    if [[ -n "$CPU" ]]; then
        l3_cache_size="$(l3_property_for_cpu "$CPU" size)"
        l3_shared_cpus="$(l3_property_for_cpu "$CPU" shared_cpu_list)"
    else
        l3_cache_size="unbound"
        l3_shared_cpus="unbound"
    fi
    thread_siblings="$(cpu_property topology/thread_siblings_list)"
    scaling_driver="$(cpu_property cpufreq/scaling_driver)"
    scaling_governor="$(cpu_property cpufreq/scaling_governor)"
    cpu_min_freq="$(cpu_property cpufreq/cpuinfo_min_freq)"
    cpu_max_freq="$(cpu_property cpufreq/cpuinfo_max_freq)"

    local -a metadata_env=(
        "TICKSYNC_BENCHMARK_EXECUTABLE_PATH=$(realpath -m -- "$binary")"
        "TICKSYNC_BENCHMARK_BINARY_SHA256=${binary_sha256}"
        "TICKSYNC_BENCHMARK_RUNTIME_BACKEND=linux-native"
        "TICKSYNC_BENCHMARK_DEVICE_MANUFACTURER=${device_manufacturer}"
        "TICKSYNC_BENCHMARK_DEVICE_MODEL=${device_model}"
        "TICKSYNC_BENCHMARK_OS_VERSION=${os_version}"
        "TICKSYNC_BENCHMARK_OS_BUILD=${os_build}"
        "TICKSYNC_BENCHMARK_SOC_MODEL=${cpu_model}"
        "TICKSYNC_BENCHMARK_CPU_MODEL=${cpu_model}"
        "TICKSYNC_BENCHMARK_CPU_CLASS=${CPU_CLASS}"
        "TICKSYNC_BENCHMARK_LOGICAL_CPU=${logical_cpu}"
        "TICKSYNC_BENCHMARK_CPU_CORE=${cpu_core}"
        "TICKSYNC_BENCHMARK_CPU_PACKAGE=${cpu_package}"
        "TICKSYNC_BENCHMARK_NUMA_NODE=${numa_node}"
        "TICKSYNC_BENCHMARK_L3_CACHE_ID=${l3_cache_id}"
        "TICKSYNC_BENCHMARK_THREAD_SIBLINGS=${thread_siblings}"
        "TICKSYNC_BENCHMARK_SCALING_DRIVER=${scaling_driver}"
        "TICKSYNC_BENCHMARK_SCALING_GOVERNOR=${scaling_governor}"
        "TICKSYNC_BENCHMARK_CPU_MIN_FREQUENCY_KHZ=${cpu_min_freq}"
        "TICKSYNC_BENCHMARK_CPU_MAX_FREQUENCY_KHZ=${cpu_max_freq}"
    )
    local -a command=(env "${metadata_env[@]}" "$binary" --json "$report_dir/results.json" --csv "$report_dir/results.csv")
    [[ -z "$CPU" ]] || command+=(--cpu "$CPU")
    (( QUICK )) && command+=(--quick)
    command+=("${EXTRA_ARGS[@]}")

    {
        printf 'Generated UTC: %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
        printf 'Precision: %s\n' "$precision"
        printf 'Quick: %s\n' "$QUICK"
        printf 'Allow dirty: %s\n' "$ALLOW_DIRTY"
        printf 'Allow unpinned: %s\n' "$ALLOW_UNPINNED"
        printf 'Binary: %s\n' "$binary"
        printf 'Binary SHA-256: %s\n' "$binary_sha256"
        if (( EXECUTION_ONLY )); then
            printf 'Module commit: embedded in the benchmark executable\n'
            printf 'Execution mode: exported package\n'
        else
            printf 'Module commit: %s\n' "$(git -C "$MODULE_DIR" rev-parse HEAD)"
            printf 'Execution mode: source tree\n'
        fi
        printf 'Godot commit: %s\n' "$(tr -d '[:space:]' < "$MODULE_DIR/GODOT_COMMIT" 2>/dev/null || printf unknown)"
        printf 'Logical CPU: %s\n' "$logical_cpu"
        printf 'CPU class: %s\n' "$CPU_CLASS"
        printf 'Core: %s\n' "$cpu_core"
        printf 'Package: %s\n' "$cpu_package"
        printf 'NUMA node: %s\n' "$numa_node"
        printf 'L3 cache id: %s\n' "$l3_cache_id"
        printf 'L3 cache size: %s\n' "$l3_cache_size"
        printf 'L3 shared CPUs: %s\n' "$l3_shared_cpus"
        printf 'Thread siblings: %s\n' "$thread_siblings"
        printf 'Scaling driver: %s\n' "$scaling_driver"
        printf 'Scaling governor: %s\n' "$scaling_governor"
        printf 'CPU min frequency kHz: %s\n' "$cpu_min_freq"
        printf 'CPU max frequency kHz: %s\n' "$cpu_max_freq"
        printf 'Command:'
        printf ' %q' "${command[@]}"
        printf '\n\nSystem:\n'
        uname -a || true
        command -v lscpu >/dev/null 2>&1 && LC_ALL=C lscpu || true
        command -v lscpu >/dev/null 2>&1 && LC_ALL=C lscpu -e=CPU,CORE,SOCKET,NODE,CACHE,ONLINE,MAXMHZ,MINMHZ || true
        if (( ! EXECUTION_ONLY )); then
            printf '\nGit status before:\n'
            git -C "$MODULE_DIR" status --short || true
        fi
        printf '\nThermal state before:\n'
        thermal_snapshot before || true
    } > "$report_dir/environment.txt"

    if (( USE_PERF )); then
        command -v perf >/dev/null 2>&1 || fail "perf not found"
        perf stat -o "$report_dir/perf.stat.txt" -- "${command[@]}" 2>&1 | tee "$report_dir/benchmark.log"
    else
        "${command[@]}" 2>&1 | tee "$report_dir/benchmark.log"
    fi

    {
        printf '\nThermal state after:\n'
        thermal_snapshot after || true
        if [[ -n "$CPU" ]]; then
            printf 'CPU current frequency kHz after: %s\n' "$(cpu_property cpufreq/scaling_cur_freq)"
        fi
        if (( ! EXECUTION_ONLY )); then
            printf '\nGit status after:\n'
            git -C "$MODULE_DIR" status --short || true
        fi
    } >> "$report_dir/environment.txt"

    local -a verify_args=("$report_dir/results.json")
    (( QUICK || ALLOW_DIRTY )) && verify_args+=(--allow-dirty)
    (( QUICK || ALLOW_UNPINNED )) && verify_args+=(--allow-unpinned)
    python3 "$SCRIPT_DIR/verify_benchmark_results.py" "${verify_args[@]}"
    find "$report_dir" -maxdepth 1 -type f ! -name SHA256SUMS.txt -print0 | \
        sort -z | xargs -0 sha256sum > "$report_dir/SHA256SUMS.txt"
    printf 'TICKSYNCHRONIZER_BENCHMARK_REPORT_OK precision=%s report=%s\n' "$precision" "$report_dir"
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
        --cpu)
            [[ $# -ge 2 ]] || fail "--cpu requires a value"
            CPU="$2"
            shift 2
            ;;
        --cpu-class)
            [[ $# -ge 2 ]] || fail "--cpu-class requires a value"
            CPU_CLASS="$2"
            shift 2
            ;;
        --l3-cache-id)
            [[ $# -ge 2 ]] || fail "--l3-cache-id requires a value"
            L3_CACHE_ID="$2"
            shift 2
            ;;
        --list-cpus)
            LIST_CPUS=1
            shift
            ;;
        --quick)
            QUICK=1
            shift
            ;;
        --no-build)
            NO_BUILD=1
            shift
            ;;
        --perf)
            USE_PERF=1
            shift
            ;;
        --allow-dirty)
            ALLOW_DIRTY=1
            shift
            ;;
        --allow-unpinned)
            ALLOW_UNPINNED=1
            shift
            ;;
        --execution-only)
            EXECUTION_ONLY=1
            NO_BUILD=1
            shift
            ;;
        --binary-dir)
            [[ $# -ge 2 ]] || fail "--binary-dir requires a value"
            BINARY_DIR="$(realpath -m -- "$2")"
            shift 2
            ;;
        --output-dir)
            [[ $# -ge 2 ]] || fail "--output-dir requires a value"
            OUTPUT_ROOT="$2"
            shift 2
            ;;
        --)
            shift
            EXTRA_ARGS=("$@")
            break
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
[[ -z "$CPU" || "$CPU" =~ ^[0-9]+$ ]] || fail "invalid CPU: $CPU"
[[ -z "$L3_CACHE_ID" || "$L3_CACHE_ID" =~ ^[0-9]+$ ]] || fail "invalid L3 cache ID: $L3_CACHE_ID"
[[ "$CPU_CLASS" =~ ^[A-Za-z0-9._-]+$ ]] || fail "invalid CPU class label: $CPU_CLASS"
command -v python3 >/dev/null 2>&1 || fail "python3 not found"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum not found"
verify_package_integrity
if (( LIST_CPUS )); then
    list_cpu_topology
    exit 0
fi
resolve_cpu_selection
verify_official_preconditions
mkdir -p "$OUTPUT_ROOT"

if [[ "$PRECISION" == "all" ]]; then
    run_one double
    run_one single
else
    run_one "$PRECISION"
fi

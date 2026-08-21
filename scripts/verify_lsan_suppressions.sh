#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
SUPPRESSION_FILE="${SCRIPT_DIR}/sanitizer_suppressions/godot-4.7.1-lsan.supp"
SCONS_BIN="${TICKSYNC_SCONS_BIN:-scons}"
CXX_BIN="${TICKSYNC_LSAN_CXX:-clang++}"
WORK_DIR=""
POLICY_ONLY="no"

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/verify_lsan_suppressions.sh [options]

Options:
  --scons-bin COMMAND  SCons executable. Default: scons.
  --cxx COMMAND        ASAN/LSAN-capable C++17 compiler. Default: clang++.
  --policy-only        Verifies only the exact static rule set.
  -h, --help           Shows this help.

The guard accepts only the reviewed Godot 4.7.1 SDL joypad leak suppression.
It then builds an unrelated leak probe through SCons and requires LeakSanitizer
to report that allocation while the reviewed file is active.

Host temporary directories use ../tick_synchronizer_tmp by default. Set
TICKSYNC_TEMP_DIR to select another safe host directory; /tmp is rejected.
USAGE
}

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

cleanup() {
    if [[ -n "$WORK_DIR" && -d "$WORK_DIR" ]]; then
        rm -rf -- "$WORK_DIR"
    fi
}

trim_line() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s\n' "$value"
}

verify_exact_rule() {
    local -a actual_rules=()
    local line

    while IFS= read -r line || [[ -n "$line" ]]; do
        line="$(trim_line "$line")"
        if [[ -z "$line" || "${line:0:1}" == "#" ]]; then
            continue
        fi
        actual_rules+=("$line")
    done < "$SUPPRESSION_FILE"

    [[ "${#actual_rules[@]}" -eq 1 ]] || \
        fail "the Godot 4.7.1 LSAN suppression set is not the reviewed one-rule policy"
    [[ "${actual_rules[0]}" == "leak:JoypadSDL::initialize" ]] || \
        fail "the Godot 4.7.1 LSAN suppression rule is not the reviewed function pattern"

    printf 'TICKSYNCHRONIZER_LSAN_SUPPRESSION_POLICY_OK\n'
}

initialize_work_dir() {
    local temp_root
    temp_root="${TICKSYNC_TEMP_DIR:-$(realpath -m -- "${MODULE_DIR}/../tick_synchronizer_tmp")}"
    temp_root="$(realpath -m -- "$temp_root")"
    case "$temp_root" in
        /|/tmp|/tmp/*)
            fail "host temporary files must use ../tick_synchronizer_tmp or a safe TICKSYNC_TEMP_DIR override"
            ;;
    esac
    mkdir -p -- "$temp_root"
    WORK_DIR="$(mktemp -d -- "$temp_root/ticksync-lsan-suppression-guard.XXXXXX")"
}

write_probe_sources() {
    cat > "$WORK_DIR/SConstruct" <<'SCONS'
from SCons.Script import ARGUMENTS, Default, Environment, SConsignFile


environment = Environment(CXX=ARGUMENTS.get("cxx", "clang++"))
environment.Append(
    CXXFLAGS=[
        "-std=c++17",
        "-O0",
        "-g",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-omit-frame-pointer",
        "-fsanitize=address",
    ],
    LINKFLAGS=["-fsanitize=address", "-fuse-ld=lld"],
)

leak_probe = environment.Program(
    target="lsan_unrelated_leak_probe",
    source="lsan_unrelated_leak_probe.cpp",
)

SConsignFile(".sconsign.dblite")
Default(leak_probe)
SCONS

    cat > "$WORK_DIR/lsan_unrelated_leak_probe.cpp" <<'CPP'
// Deliberately leaks one allocation unrelated to the reviewed Godot stack.
// Proves that the function-specific LSan rule does not disable leak detection.

#include <cstdlib>
#include <cstring>


[[gnu::noinline]] void allocate_unrelated_leak() {
    void *leaked = std::malloc(4096U);
    if (leaked == nullptr) {
        std::abort();
    }
    std::memset(leaked, 0xA5, 4096U);
}


int main() {
    allocate_unrelated_leak();
    return 0;
}
CPP
}

build_probe() {
    local build_log="$WORK_DIR/build.log"
    if ! "$SCONS_BIN" -C "$WORK_DIR" "cxx=$CXX_BIN" -j1 >"$build_log" 2>&1; then
        cat "$build_log" >&2
        fail "SCons could not build the LSAN suppression probe"
    fi

    [[ -x "$WORK_DIR/lsan_unrelated_leak_probe" ]] || \
        fail "the unrelated leak probe is missing or not executable"
}

require_fatal_unrelated_leak() {
    local output_log="$WORK_DIR/unrelated-leak.log"
    local status

    if ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:symbolize=1:detect_leaks=1" \
        LSAN_OPTIONS="suppressions=${SUPPRESSION_FILE}:print_suppressions=1:exitcode=23" \
        timeout --signal=TERM --kill-after=5s 120s \
        "$WORK_DIR/lsan_unrelated_leak_probe" >"$output_log" 2>&1; then
        status=0
    else
        status=$?
    fi

    if [[ "$status" -ne 23 ]]; then
        cat "$output_log" >&2
        fail "the unrelated leak probe returned $status instead of the required LSAN status 23"
    fi
    if ! grep -Fq "ERROR: LeakSanitizer: detected memory leaks" "$output_log" || \
        ! grep -Fq "Direct leak of 4096 byte(s) in 1 object(s)" "$output_log" || \
        ! grep -Fq "4096 byte(s) leaked in 1 allocation(s)" "$output_log" || \
        ! grep -Fq "allocate_unrelated_leak" "$output_log"; then
        cat "$output_log" >&2
        fail "the unrelated allocation did not produce the expected LeakSanitizer report"
    fi
    if grep -Fq "JoypadSDL::initialize" "$output_log"; then
        cat "$output_log" >&2
        fail "the unrelated allocation unexpectedly matched the reviewed Godot rule"
    fi

    printf 'TICKSYNCHRONIZER_LSAN_UNRELATED_LEAK_NEGATIVE_CONTROL_OK\n'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
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
        --policy-only)
            POLICY_ONLY="yes"
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

[[ -f "$SUPPRESSION_FILE" ]] || fail "LSAN suppression file not found"
verify_exact_rule

if [[ "$POLICY_ONLY" == "yes" ]]; then
    printf 'TICKSYNCHRONIZER_LSAN_SUPPRESSION_POLICY_ONLY_OK\n'
    exit 0
fi

command -v "$SCONS_BIN" >/dev/null 2>&1 || fail "SCons not found: $SCONS_BIN"
command -v "$CXX_BIN" >/dev/null 2>&1 || fail "compiler not found: $CXX_BIN"
command -v ld.lld >/dev/null 2>&1 || fail "ld.lld not found"
command -v timeout >/dev/null 2>&1 || fail "timeout not found"

trap cleanup EXIT
initialize_work_dir
write_probe_sources
build_probe
require_fatal_unrelated_leak

printf 'TICKSYNCHRONIZER_LSAN_SUPPRESSION_GUARD_OK\n'

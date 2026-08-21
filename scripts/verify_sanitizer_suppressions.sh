#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
SUPPRESSION_FILE="${SCRIPT_DIR}/sanitizer_suppressions/godot-4.7.1-ubsan.supp"
SCONS_BIN="${TICKSYNC_SCONS_BIN:-scons}"
CXX_BIN="${TICKSYNC_UBSAN_CXX:-g++}"
WORK_DIR=""

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/verify_sanitizer_suppressions.sh [options]

Options:
  --scons-bin COMMAND  SCons executable. Default: scons.
  --cxx COMMAND        UBSAN-capable C++17 compiler. Default: g++.
  -h, --help           Shows this help.

The guard accepts only the three reviewed Godot 4.7.1 category-and-source
suppressions. It then builds unrelated bounds and alignment probes through
SCons and requires both probes to remain fatal under the accepted file.

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

verify_exact_rules() {
    local -a expected_rules=(
        "nonnull-attribute:core/string/ustring.cpp"
        "bounds:thirdparty/sdl/thread/SDL_thread.c"
        "alignment:modules/gdscript/gdscript_vm.cpp"
    )
    local -a actual_rules=()
    local line

    while IFS= read -r line || [[ -n "$line" ]]; do
        line="$(trim_line "$line")"
        if [[ -z "$line" || "${line:0:1}" == "#" ]]; then
            continue
        fi
        actual_rules+=("$line")
    done < "$SUPPRESSION_FILE"

    [[ "${#actual_rules[@]}" -eq "${#expected_rules[@]}" ]] || \
        fail "the Godot 4.7.1 UBSAN suppression set is not the reviewed three-rule policy"

    local index
    for index in "${!expected_rules[@]}"; do
        [[ "${actual_rules[$index]}" == "${expected_rules[$index]}" ]] || \
            fail "unexpected UBSAN suppression rule at position $((index + 1))"
    done

    printf 'TICKSYNCHRONIZER_UBSAN_SUPPRESSION_POLICY_OK\n'
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
    WORK_DIR="$(mktemp -d -- "$temp_root/ticksync-ubsan-suppression-guard.XXXXXX")"
}

write_probe_sources() {
    cat > "$WORK_DIR/SConstruct" <<'SCONS'
from SCons.Script import ARGUMENTS, Default, Environment, SConsignFile


environment = Environment(CXX=ARGUMENTS.get("cxx", "g++"))
environment.Append(
    CXXFLAGS=[
        "-std=c++17",
        "-O1",
        "-g",
        "-fno-omit-frame-pointer",
        "-fsanitize=undefined",
    ],
    LINKFLAGS=["-fsanitize=undefined", "-fuse-ld=lld"],
)

bounds_probe = environment.Program(
    target="ubsan_bounds_probe",
    source="ubsan_bounds_probe.cpp",
)
alignment_probe = environment.Program(
    target="ubsan_alignment_probe",
    source="ubsan_alignment_probe.cpp",
)

SConsignFile(".sconsign.dblite")
Default([bounds_probe, alignment_probe])
SCONS

    cat > "$WORK_DIR/ubsan_bounds_probe.cpp" <<'CPP'
#include <cstddef>


int main(int argc, char **) {
    int values[1] = { 0 };
    volatile std::size_t index = argc == 1 ? 1U : 0U;
    return values[index];
}
CPP

    cat > "$WORK_DIR/ubsan_alignment_probe.cpp" <<'CPP'
#include <cstdint>


int main() {
    alignas(std::uint64_t) unsigned char storage[sizeof(std::uint64_t) + 1] = {};
    volatile std::uint64_t *value =
            reinterpret_cast<volatile std::uint64_t *>(storage + 1);
    *value = UINT64_C(0x0123456789ABCDEF);
    return 0;
}
CPP
}

build_probes() {
    local build_log="$WORK_DIR/build.log"
    if ! "$SCONS_BIN" -C "$WORK_DIR" "cxx=$CXX_BIN" -j1 >"$build_log" 2>&1; then
        cat "$build_log" >&2
        fail "SCons could not build the UBSAN suppression probes"
    fi

    [[ -x "$WORK_DIR/ubsan_bounds_probe" ]] || \
        fail "the bounds probe is missing or not executable"
    [[ -x "$WORK_DIR/ubsan_alignment_probe" ]] || \
        fail "the alignment probe is missing or not executable"
}

require_fatal_probe() {
    local label="$1"
    local executable="$2"
    local source_name="$3"
    local diagnostic_text="$4"
    local output_log="$WORK_DIR/${label}.log"
    local status

    if UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:suppressions=${SUPPRESSION_FILE}" \
        "$executable" >"$output_log" 2>&1; then
        status=0
    else
        status=$?
    fi

    if [[ "$status" -eq 0 ]]; then
        cat "$output_log" >&2
        fail "the unrelated ${label} probe was incorrectly suppressed"
    fi
    if ! grep -Fq "runtime error:" "$output_log" || \
        ! grep -Fq "$source_name" "$output_log" || \
        ! grep -Fq "$diagnostic_text" "$output_log"; then
        cat "$output_log" >&2
        fail "the unrelated ${label} probe did not produce the expected UBSAN diagnostic"
    fi

    printf 'TICKSYNCHRONIZER_UBSAN_%s_NEGATIVE_CONTROL_OK\n' \
        "${label^^}"
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
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1"
            ;;
    esac
done

[[ -f "$SUPPRESSION_FILE" ]] || fail "UBSAN suppression file not found"
command -v "$SCONS_BIN" >/dev/null 2>&1 || fail "SCons not found: $SCONS_BIN"
command -v "$CXX_BIN" >/dev/null 2>&1 || fail "compiler not found: $CXX_BIN"
command -v ld.lld >/dev/null 2>&1 || fail "ld.lld not found"

trap cleanup EXIT
verify_exact_rules
initialize_work_dir
write_probe_sources
build_probes
require_fatal_probe \
    bounds "$WORK_DIR/ubsan_bounds_probe" \
    ubsan_bounds_probe.cpp "out of bounds"
require_fatal_probe \
    alignment "$WORK_DIR/ubsan_alignment_probe" \
    ubsan_alignment_probe.cpp "misaligned address"

printf 'TICKSYNCHRONIZER_UBSAN_SUPPRESSION_GUARD_OK\n'

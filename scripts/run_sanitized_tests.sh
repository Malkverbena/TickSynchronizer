#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BUILD_SCRIPT="${SCRIPT_DIR}/build_and_validate.sh"

readonly EXPECTED_BUILD_SCRIPT_API="5"

PRECISION="double"
PROFILE="module"
RUN_MODE="split"
FORCE_TOOLCHAIN="auto"
STATIC_CPP="no"
EDITOR_DEV_BUILD="no"
CLEAN_FIRST="no"
STACK_USE_AFTER_RETURN="no"
INVALID_POINTER_PAIRS="no"
GODOT_UBSAN_SUPPRESSIONS="yes"
PASSTHROUGH=()

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/run_sanitized_tests.sh [single|double] [options]

Default profile:
  Runs two independent builds:
    1. ASAN with Clang + LLD.
    2. UBSAN with GCC + LLD.

Separation avoids depending on Clang’s UBSAN C++ runtime, which may not be
installed, and avoids the excessively large combined Godot 4.7.1 link.

Options:
  --module-profile   Disables raycast/Embree. Default.
  --full-engine      Keeps all engine modules.
  --split            ASAN/Clang and UBSAN/GCC in separate passes. Default.
  --asan-only        Runs only ASAN with Clang + LLD.
  --ubsan-only       Runs only UBSAN with GCC + LLD.
  --combined         Runs ASAN + UBSAN together with Clang + LLD.
                     Diagnostic mode; requires a complete compiler-rt UBSAN C++ runtime.
  --llvm             Forces Clang + LLD for all selected passes.
                     Diagnostic mode; UBSAN may require an additional compiler-rt package.
  --gcc              Forces GCC + LLD for all selected passes.
                     Diagnostic mode; ASAN may exceed relocation limits.
  --dev-build        Uses dev_build=yes. Diagnostic mode; greatly increases binary size.
  --static-cpp       Uses a static C++ runtime. Diagnostic mode; not recommended.
  --clean-first      Cleans each sanitized configuration before building.
  --stack-use-after-return
                     Enables ASAN use-after-return detection. Slower.
  --invalid-pointer-pairs
                     Enables optional comparison/subtraction checking between
                     pointers to distinct objects. Engine diagnostic mode; in
                     Godot 4.7.1 it aborts in StringName before the tests.
  --no-godot-ubsan-suppressions
                     Disables the strict suppression for the engine-level false positive
                     nonnull-attribute in core/string/ustring.cpp. Diagnostic mode.
  -h, --help         Shows this help.

Unknown arguments are forwarded to build_and_validate.sh.

Host temporary directories use ../tick_synchronizer_tmp by default. Set
TICKSYNC_TEMP_DIR to select another safe host directory; /tmp is rejected.
USAGE
}

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '[TickSynchronizer] %s\n' "$*" >&2
}

initialize_temp_root() {
    TEMP_ROOT="${TICKSYNC_TEMP_DIR:-$(realpath -m -- "${MODULE_DIR}/../tick_synchronizer_tmp")}"
    TEMP_ROOT="$(realpath -m -- "$TEMP_ROOT")"
    case "$TEMP_ROOT" in
        /|/tmp|/tmp/*)
            fail "host temporary files must use ../tick_synchronizer_tmp or a safe TICKSYNC_TEMP_DIR override"
            ;;
    esac
    mkdir -p -- "$TEMP_ROOT"
}

make_probe_dir() {
    mktemp -d -- "$TEMP_ROOT/ticksync-sanitizer-probe.XXXXXX"
}

verify_build_script_contract() {
    [[ -x "$BUILD_SCRIPT" ]] || \
        fail "build_and_validate.sh not found or not executable: $BUILD_SCRIPT"

    local actual_api
    actual_api="$($BUILD_SCRIPT --print-script-api-version 2>/dev/null)" || \
        fail "build_and_validate.sh does not provide the expected interface. Extract the complete ZIP."

    [[ "$actual_api" == "$EXPECTED_BUILD_SCRIPT_API" ]] || \
        fail "incompatible scripts: run_sanitized_tests requires API $EXPECTED_BUILD_SCRIPT_API, but build_and_validate provides '$actual_api'."
}

make_probe_source() {
    local output="$1"
    cat > "$output" <<'CPP'
struct Base {
    virtual ~Base() = default;
    virtual int value() const { return 1; }
};

struct Derived final : Base {
    int value() const override { return 2; }
};

int main() {
    Base *object = new Derived;
    const int result = object->value();
    delete object;
    return result == 2 ? 0 : 1;
}
CPP
}

check_lld() {
    command -v ld.lld >/dev/null 2>&1 || \
        fail "ld.lld not found. Install an LLD linker compatible with your system."
}

check_clang_asan_toolchain() {
    command -v clang++ >/dev/null 2>&1 || \
        fail "clang++ not found. Install Clang and compiler-rt."
    check_lld

    local probe_dir
    probe_dir="$(make_probe_dir)"
    trap 'rm -rf -- "$probe_dir"' RETURN
    make_probe_source "$probe_dir/probe.cpp"

    if ! clang++ -std=c++17 -O1 -g -fuse-ld=lld \
        -fsanitize=address -fno-omit-frame-pointer \
        "$probe_dir/probe.cpp" -o "$probe_dir/probe" >"$probe_dir/link.log" 2>&1; then
        cat "$probe_dir/link.log" >&2
        fail "Clang/LLD could not link a C++17 program with ASAN. Check the compiler-rt installation."
    fi

    ASAN_OPTIONS=halt_on_error=1 "$probe_dir/probe" >/dev/null 2>&1 || \
        fail "the ASAN test executable did not start correctly"
    rm -rf -- "$probe_dir"
    trap - RETURN
}

check_gcc_ubsan_toolchain() {
    command -v g++ >/dev/null 2>&1 || \
        fail "g++ not found. Install the GCC UBSAN runtime."
    check_lld

    local probe_dir
    probe_dir="$(make_probe_dir)"
    trap 'rm -rf -- "$probe_dir"' RETURN
    make_probe_source "$probe_dir/probe.cpp"

    if ! g++ -std=c++17 -O1 -g -fuse-ld=lld \
        -fsanitize=undefined -fno-omit-frame-pointer \
        "$probe_dir/probe.cpp" -o "$probe_dir/probe" >"$probe_dir/link.log" 2>&1; then
        cat "$probe_dir/link.log" >&2
        fail "GCC/LLD could not link a C++17 program with UBSAN. Check libubsan and LLD."
    fi

    UBSAN_OPTIONS=halt_on_error=1 "$probe_dir/probe" >/dev/null 2>&1 || \
        fail "the UBSAN test executable did not start correctly"
    rm -rf -- "$probe_dir"
    trap - RETURN
}


check_clang_ubsan_toolchain() {
    command -v clang++ >/dev/null 2>&1 || \
        fail "clang++ not found."
    check_lld

    local probe_dir
    probe_dir="$(make_probe_dir)"
    trap 'rm -rf -- "$probe_dir"' RETURN
    make_probe_source "$probe_dir/probe.cpp"

    if ! clang++ -std=c++17 -O1 -g -fuse-ld=lld \
        -fsanitize=undefined -fno-omit-frame-pointer \
        "$probe_dir/probe.cpp" -o "$probe_dir/probe" >"$probe_dir/link.log" 2>&1; then
        cat "$probe_dir/link.log" >&2
        local resource_dir
        resource_dir="$(clang++ -print-resource-dir 2>/dev/null || true)"
        printf 'Clang resource directory: %s\n' "${resource_dir:-unknown}" >&2
        fail "Clang does not have the required UBSAN C++ runtime. Use UBSAN with GCC, which is the default."
    fi

    UBSAN_OPTIONS=halt_on_error=1 "$probe_dir/probe" >/dev/null 2>&1 || \
        fail "the Clang/UBSAN test executable did not start correctly"
    rm -rf -- "$probe_dir"
    trap - RETURN
}

check_clang_combined_toolchain() {
    command -v clang++ >/dev/null 2>&1 || \
        fail "clang++ not found."
    check_lld

    local probe_dir
    probe_dir="$(make_probe_dir)"
    trap 'rm -rf -- "$probe_dir"' RETURN
    make_probe_source "$probe_dir/probe.cpp"

    if ! clang++ -std=c++17 -O1 -g -fuse-ld=lld \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        "$probe_dir/probe.cpp" -o "$probe_dir/probe" >"$probe_dir/link.log" 2>&1; then
        cat "$probe_dir/link.log" >&2
        local resource_dir
        resource_dir="$(clang++ -print-resource-dir 2>/dev/null || true)"
        printf 'Clang resource directory: %s\n' "${resource_dir:-unknown}" >&2
        fail "Clang does not have a complete ASAN+UBSAN C++ runtime. Use the default --split profile."
    fi

    "$probe_dir/probe" >/dev/null 2>&1 || \
        fail "the combined ASAN+UBSAN executable did not start correctly"
    rm -rf -- "$probe_dir"
    trap - RETURN
}

if [[ $# -gt 0 && ( "$1" == "single" || "$1" == "double" ) ]]; then
    PRECISION="$1"
    shift
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --module-profile)
            PROFILE="module"
            shift
            ;;
        --full-engine)
            PROFILE="full-engine"
            shift
            ;;
        --split)
            RUN_MODE="split"
            shift
            ;;
        --asan-only)
            RUN_MODE="asan"
            shift
            ;;
        --ubsan-only)
            RUN_MODE="ubsan"
            shift
            ;;
        --combined)
            RUN_MODE="combined"
            shift
            ;;
        --llvm)
            FORCE_TOOLCHAIN="llvm"
            shift
            ;;
        --gcc)
            FORCE_TOOLCHAIN="gcc"
            shift
            ;;
        --dev-build)
            EDITOR_DEV_BUILD="yes"
            shift
            ;;
        --static-cpp)
            STATIC_CPP="yes"
            shift
            ;;
        --clean-first)
            CLEAN_FIRST="yes"
            shift
            ;;
        --stack-use-after-return)
            STACK_USE_AFTER_RETURN="yes"
            shift
            ;;
        --invalid-pointer-pairs)
            INVALID_POINTER_PAIRS="yes"
            shift
            ;;
        --no-godot-ubsan-suppressions)
            GODOT_UBSAN_SUPPRESSIONS="no"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            PASSTHROUGH+=("$1")
            shift
            ;;
    esac
done

[[ "$PRECISION" == "single" || "$PRECISION" == "double" ]] || \
    fail "invalid precision: $PRECISION"

verify_build_script_contract
initialize_temp_root

COMMON_ARGS=(
    --mode quick
    --precision "$PRECISION"
    --editor-dev-build "$EDITOR_DEV_BUILD"
    --scons-arg accesskit=no
)

if [[ "$CLEAN_FIRST" == "yes" ]]; then
    COMMON_ARGS+=(--clean-first)
fi

if [[ "$PROFILE" == "module" ]]; then
    COMMON_ARGS+=(--scons-arg module_raycast_enabled=no)
else
    printf '%s\n' \
        'WARNING: --full-engine greatly increases build time and size and is not required to accept module changes.' >&2
fi

if [[ "$STATIC_CPP" == "yes" ]]; then
    printf '%s\n' \
        'WARNING: --static-cpp may reproduce relocation overflows and is not recommended with sanitizers.' >&2
    COMMON_ARGS+=(--scons-arg use_static_cpp=yes)
else
    COMMON_ARGS+=(--scons-arg use_static_cpp=no)
fi

if [[ "$EDITOR_DEV_BUILD" == "no" ]]; then
    COMMON_ARGS+=(
        --scons-arg optimize=debug
        --scons-arg debug_symbols=yes
    )
fi

COMMON_ARGS+=("${PASSTHROUGH[@]}")

if command -v llvm-symbolizer >/dev/null 2>&1 && [[ -z "${ASAN_SYMBOLIZER_PATH:-}" ]]; then
    export ASAN_SYMBOLIZER_PATH="$(command -v llvm-symbolizer)"
fi
ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1:symbolize=1}"
if [[ "$INVALID_POINTER_PAIRS" == "yes" ]]; then
    ASAN_OPTIONS+=":detect_invalid_pointer_pairs=2"
else
    # Godot 4.7.1 orders StringName values by their interned-data pointers.
    # The optional pointer-pair checker treats that engine-level ordering as an
    # invalid pair and aborts during register_core_settings(), before module tests.
    # Keep normal ASAN memory checks enabled while disabling only this optional
    # runtime check. Appending the value makes it override inherited settings.
    ASAN_OPTIONS+=":detect_invalid_pointer_pairs=0"
fi
if [[ "$STACK_USE_AFTER_RETURN" == "yes" ]]; then
    ASAN_OPTIONS+=":detect_stack_use_after_return=1"
fi
export ASAN_OPTIONS

UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
UBSAN_SUPPRESSION_FILE="${SCRIPT_DIR}/sanitizer_suppressions/godot-4.7.1-ubsan.supp"
if [[ "$GODOT_UBSAN_SUPPRESSIONS" == "yes" ]]; then
    [[ -f "$UBSAN_SUPPRESSION_FILE" ]] || \
        fail "UBSAN suppression file not found: $UBSAN_SUPPRESSION_FILE"
    # Godot 4.7.1 test_setup() does not load a project before loading core
    # extensions. Its empty project_data_dir_name reaches memcpy(dst, nullptr, 0)
    # in String::append_utf32_unchecked(). GCC UBSAN reports nonnull-attribute.
    # Suppress only that check in that engine source file. Every report from the
    # TickSynchronizer module and every other source remains fatal.
    UBSAN_OPTIONS+=":suppressions=${UBSAN_SUPPRESSION_FILE}"
fi
export UBSAN_OPTIONS

run_pass() {
    local label="$1"
    local sanitizer="$2"
    local toolchain="$3"
    local -a args=("${COMMON_ARGS[@]}")

    case "$sanitizer" in
        asan)
            args+=(--scons-arg use_asan=yes --scons-arg use_ubsan=no)
            ;;
        ubsan)
            args+=(--scons-arg use_asan=no --scons-arg use_ubsan=yes)
            ;;
        combined)
            args+=(--scons-arg use_asan=yes --scons-arg use_ubsan=yes)
            ;;
        *)
            fail "invalid internal sanitizer: $sanitizer"
            ;;
    esac

    case "$toolchain" in
        llvm)
            args+=(--scons-arg use_llvm=yes --scons-arg linker=lld)
            ;;
        gcc)
            args+=(--scons-arg use_llvm=no --scons-arg linker=lld)
            ;;
        *)
            fail "invalid internal toolchain: $toolchain"
            ;;
    esac

    log "Starting ${label} pass: sanitizer=${sanitizer}, toolchain=${toolchain}/lld, precision=${PRECISION}"
    "$BUILD_SCRIPT" "${args[@]}"
    log "${label} pass completed successfully."
}

log "Sanitizer profile: $PROFILE"
log "Execution mode: $RUN_MODE"
log "Precision: $PRECISION"
log "Editor dev_build: $EDITOR_DEV_BUILD"
log "C++ runtime linkage: $([[ "$STATIC_CPP" == "yes" ]] && printf 'static' || printf 'shared')"
log "ASAN invalid pointer pairs: $([[ "$INVALID_POINTER_PAIRS" == "yes" ]] && printf 'enabled' || printf 'disabled')"
log "Godot UBSAN suppression: $([[ "$GODOT_UBSAN_SUPPRESSIONS" == "yes" ]] && printf 'enabled' || printf 'disabled')"

case "$RUN_MODE" in
    split)
        if [[ "$FORCE_TOOLCHAIN" == "auto" ]]; then
            check_clang_asan_toolchain
            check_gcc_ubsan_toolchain
            run_pass "ASAN" asan llvm
            run_pass "UBSAN" ubsan gcc
        elif [[ "$FORCE_TOOLCHAIN" == "llvm" ]]; then
            check_clang_asan_toolchain
            check_clang_ubsan_toolchain
            run_pass "ASAN" asan llvm
            run_pass "UBSAN" ubsan llvm
        else
            check_lld
            run_pass "ASAN" asan gcc
            run_pass "UBSAN" ubsan gcc
        fi
        ;;
    asan)
        if [[ "$FORCE_TOOLCHAIN" == "gcc" ]]; then
            check_lld
            run_pass "ASAN" asan gcc
        else
            check_clang_asan_toolchain
            run_pass "ASAN" asan llvm
        fi
        ;;
    ubsan)
        if [[ "$FORCE_TOOLCHAIN" == "llvm" ]]; then
            check_clang_ubsan_toolchain
            run_pass "UBSAN" ubsan llvm
        else
            check_gcc_ubsan_toolchain
            run_pass "UBSAN" ubsan gcc
        fi
        ;;
    combined)
        if [[ "$FORCE_TOOLCHAIN" == "gcc" ]]; then
            printf '%s\n' \
                'WARNING: Combined ASAN+UBSAN with GCC may exceed relocation limits in Godot 4.7.1.' >&2
            check_lld
            run_pass "ASAN+UBSAN" combined gcc
        else
            check_clang_combined_toolchain
            run_pass "ASAN+UBSAN" combined llvm
        fi
        ;;
    *)
        fail "invalid internal mode: $RUN_MODE"
        ;;
esac

log "Sanitized validation completed."

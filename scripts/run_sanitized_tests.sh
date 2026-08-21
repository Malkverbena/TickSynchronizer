#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BUILD_SCRIPT="${SCRIPT_DIR}/build_and_validate.sh"
LSAN_SUPPRESSION_GUARD="${SCRIPT_DIR}/verify_lsan_suppressions.sh"
UBSAN_SUPPRESSION_GUARD="${SCRIPT_DIR}/verify_sanitizer_suppressions.sh"

readonly EXPECTED_BUILD_SCRIPT_API="5"

PRECISION_REQUEST="double"
PRECISION=""
PROFILE="module"
RUN_MODE="split"
FORCE_TOOLCHAIN="auto"
STATIC_CPP="no"
EDITOR_DEV_BUILD="no"
CLEAN_FIRST="no"
STACK_USE_AFTER_RETURN="no"
INVALID_POINTER_PAIRS="no"
GODOT_LSAN_SUPPRESSIONS="yes"
GODOT_UBSAN_SUPPRESSIONS="yes"
LSAN_NEGATIVE_CONTROL_COMPLETED="no"
SCONS_BIN="${TICKSYNC_SCONS_BIN:-scons}"
PASSTHROUGH=()

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/run_sanitized_tests.sh [single|double|all] [options]

Default profile:
  Runs two independent builds:
    1. ASAN with Clang + LLD.
    2. UBSAN with GCC + LLD.

Separation avoids depending on Clang’s UBSAN C++ runtime, which may not be
installed, and avoids the excessively large combined Godot 4.7.1 link.

The `all` precision batch runs `double` and `single` serially. It executes the
full unrelated LSAN leak control once, while rechecking the exact static rule
before every later ASAN pass in the same uninterrupted batch.

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
  --scons-bin COMMAND
                     SCons executable forwarded to the build and suppression guard.
  --stack-use-after-return
                     Enables ASAN use-after-return detection. Slower.
  --invalid-pointer-pairs
                     Enables optional comparison/subtraction checking between
                     pointers to distinct objects. Engine diagnostic mode; in
                     Godot 4.7.1 it aborts in StringName before the tests.
  --no-godot-lsan-suppressions
                     Disables the reviewed Godot 4.7.1 SDL joypad leak
                     suppression. Diagnostic mode; leak detection stays enabled.
  --no-godot-ubsan-suppressions
                     Disables the reviewed Godot 4.7.1 category-and-source
                     suppressions. Diagnostic mode.
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
    [[ -x "$LSAN_SUPPRESSION_GUARD" ]] || \
        fail "verify_lsan_suppressions.sh not found or not executable"
    [[ -x "$UBSAN_SUPPRESSION_GUARD" ]] || \
        fail "verify_sanitizer_suppressions.sh not found or not executable"

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

if [[ $# -gt 0 && \
    ( "$1" == "single" || "$1" == "double" || "$1" == "all" ) ]]; then
    PRECISION_REQUEST="$1"
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
        --scons-bin)
            [[ $# -ge 2 ]] || fail "--scons-bin requires a value"
            SCONS_BIN="$2"
            PASSTHROUGH+=("--scons-bin" "$2")
            shift 2
            ;;
        --stack-use-after-return)
            STACK_USE_AFTER_RETURN="yes"
            shift
            ;;
        --invalid-pointer-pairs)
            INVALID_POINTER_PAIRS="yes"
            shift
            ;;
        --no-godot-lsan-suppressions)
            GODOT_LSAN_SUPPRESSIONS="no"
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

case "$PRECISION_REQUEST" in
    single|double)
        PRECISIONS=("$PRECISION_REQUEST")
        ;;
    all)
        PRECISIONS=("double" "single")
        ;;
    *)
        fail "invalid precision: $PRECISION_REQUEST"
        ;;
esac

verify_build_script_contract
initialize_temp_root

COMMON_ARGS_BASE=(
    --mode quick
    --editor-dev-build "$EDITOR_DEV_BUILD"
    --scons-arg accesskit=no
)

if [[ "$CLEAN_FIRST" == "yes" ]]; then
    COMMON_ARGS_BASE+=(--clean-first)
fi

if [[ "$PROFILE" == "module" ]]; then
    COMMON_ARGS_BASE+=(--scons-arg module_raycast_enabled=no)
else
    printf '%s\n' \
        'WARNING: --full-engine greatly increases build time and size and is not required to accept module changes.' >&2
fi

if [[ "$STATIC_CPP" == "yes" ]]; then
    printf '%s\n' \
        'WARNING: --static-cpp may reproduce relocation overflows and is not recommended with sanitizers.' >&2
    COMMON_ARGS_BASE+=(--scons-arg use_static_cpp=yes)
else
    COMMON_ARGS_BASE+=(--scons-arg use_static_cpp=no)
fi

if [[ "$EDITOR_DEV_BUILD" == "no" ]]; then
    COMMON_ARGS_BASE+=(
        --scons-arg optimize=debug
        --scons-arg debug_symbols=yes
    )
fi

COMMON_ARGS_BASE+=("${PASSTHROUGH[@]}")

configure_precision_args() {
    PRECISION="$1"
    COMMON_ARGS=(
        "${COMMON_ARGS_BASE[@]}"
        --precision "$PRECISION"
    )
}

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
ASAN_OPTIONS+=":detect_leaks=1"
export ASAN_OPTIONS

LSAN_OPTIONS="${LSAN_OPTIONS:-}"
LSAN_SUPPRESSION_FILE="${SCRIPT_DIR}/sanitizer_suppressions/godot-4.7.1-lsan.supp"
if [[ "$GODOT_LSAN_SUPPRESSIONS" == "yes" ]]; then
    [[ -f "$LSAN_SUPPRESSION_FILE" ]] || \
        fail "LSAN suppression file not found: $LSAN_SUPPRESSION_FILE"
    # The one reviewed rule matches an exact Godot 4.7.1 SDL joypad startup
    # stack reproduced without TickSynchronizer. Leak detection remains enabled,
    # and the guard proves that an unrelated allocation still fails the gate.
    if [[ -n "$LSAN_OPTIONS" ]]; then
        LSAN_OPTIONS+=":"
    fi
    LSAN_OPTIONS+="suppressions=${LSAN_SUPPRESSION_FILE}:print_suppressions=1:exitcode=23"
fi
export LSAN_OPTIONS

UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
UBSAN_SUPPRESSION_FILE="${SCRIPT_DIR}/sanitizer_suppressions/godot-4.7.1-ubsan.supp"
if [[ "$GODOT_UBSAN_SUPPRESSIONS" == "yes" ]]; then
    [[ -f "$UBSAN_SUPPRESSION_FILE" ]] || \
        fail "UBSAN suppression file not found: $UBSAN_SUPPRESSION_FILE"
    # The file contains only reviewed category-and-source pairs for three
    # diagnostics reproduced with the exact Godot 4.7.1 baseline without
    # TickSynchronizer: test setup, bundled SDL TLS startup, and a GDScript VM
    # pointer store. The guard enforces the exact set and proves that unrelated
    # bounds and alignment diagnostics remain fatal before each UBSAN pass.
    UBSAN_OPTIONS+=":suppressions=${UBSAN_SUPPRESSION_FILE}"
fi
export UBSAN_OPTIONS

verify_lsan_policy_for_pass() {
    local toolchain="$1"
    local compiler

    if [[ "$toolchain" == "llvm" ]]; then
        compiler="clang++"
    else
        compiler="g++"
    fi

    if [[ "$LSAN_NEGATIVE_CONTROL_COMPLETED" == "no" ]]; then
        log "Validating the strict Godot 4.7.1 LSAN policy and unrelated leak control"
        TICKSYNC_TEMP_DIR="$TEMP_ROOT" \
            "$LSAN_SUPPRESSION_GUARD" \
            --scons-bin "$SCONS_BIN" \
            --cxx "$compiler"
        LSAN_NEGATIVE_CONTROL_COMPLETED="yes"
    else
        log "Revalidating the exact Godot 4.7.1 LSAN rule for this pass"
        "$LSAN_SUPPRESSION_GUARD" --policy-only
    fi
}

run_ubsan_suppression_guard() {
    local toolchain="$1"
    local compiler

    if [[ "$toolchain" == "llvm" ]]; then
        compiler="clang++"
    else
        compiler="g++"
    fi

    log "Validating the strict Godot 4.7.1 UBSAN suppression policy"
    TICKSYNC_TEMP_DIR="$TEMP_ROOT" \
        "$UBSAN_SUPPRESSION_GUARD" \
        --scons-bin "$SCONS_BIN" \
        --cxx "$compiler"
}

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

    if [[ "$GODOT_LSAN_SUPPRESSIONS" == "yes" && \
        ( "$sanitizer" == "asan" || "$sanitizer" == "combined" ) ]]; then
        verify_lsan_policy_for_pass "$toolchain"
    fi

    if [[ "$GODOT_UBSAN_SUPPRESSIONS" == "yes" && \
        ( "$sanitizer" == "ubsan" || "$sanitizer" == "combined" ) ]]; then
        run_ubsan_suppression_guard "$toolchain"
    fi

    log "Starting ${label} pass: sanitizer=${sanitizer}, toolchain=${toolchain}/lld, precision=${PRECISION}"
    "$BUILD_SCRIPT" "${args[@]}"
    log "${label} pass completed successfully."
}

check_selected_toolchains() {
    case "$RUN_MODE" in
        split)
            if [[ "$FORCE_TOOLCHAIN" == "auto" ]]; then
                check_clang_asan_toolchain
                check_gcc_ubsan_toolchain
            elif [[ "$FORCE_TOOLCHAIN" == "llvm" ]]; then
                check_clang_asan_toolchain
                check_clang_ubsan_toolchain
            else
                check_lld
            fi
            ;;
        asan)
            if [[ "$FORCE_TOOLCHAIN" == "gcc" ]]; then
                check_lld
            else
                check_clang_asan_toolchain
            fi
            ;;
        ubsan)
            if [[ "$FORCE_TOOLCHAIN" == "llvm" ]]; then
                check_clang_ubsan_toolchain
            else
                check_gcc_ubsan_toolchain
            fi
            ;;
        combined)
            if [[ "$FORCE_TOOLCHAIN" == "gcc" ]]; then
                printf '%s\n' \
                    'WARNING: Combined ASAN+UBSAN with GCC may exceed relocation limits in Godot 4.7.1.' >&2
                check_lld
            else
                check_clang_combined_toolchain
            fi
            ;;
        *)
            fail "invalid internal mode: $RUN_MODE"
            ;;
    esac
}

run_selected_passes() {
    case "$RUN_MODE" in
        split)
            if [[ "$FORCE_TOOLCHAIN" == "auto" ]]; then
                run_pass "ASAN" asan llvm
                run_pass "UBSAN" ubsan gcc
            elif [[ "$FORCE_TOOLCHAIN" == "llvm" ]]; then
                run_pass "ASAN" asan llvm
                run_pass "UBSAN" ubsan llvm
            else
                run_pass "ASAN" asan gcc
                run_pass "UBSAN" ubsan gcc
            fi
            ;;
        asan)
            if [[ "$FORCE_TOOLCHAIN" == "gcc" ]]; then
                run_pass "ASAN" asan gcc
            else
                run_pass "ASAN" asan llvm
            fi
            ;;
        ubsan)
            if [[ "$FORCE_TOOLCHAIN" == "llvm" ]]; then
                run_pass "UBSAN" ubsan llvm
            else
                run_pass "UBSAN" ubsan gcc
            fi
            ;;
        combined)
            if [[ "$FORCE_TOOLCHAIN" == "gcc" ]]; then
                run_pass "ASAN+UBSAN" combined gcc
            else
                run_pass "ASAN+UBSAN" combined llvm
            fi
            ;;
        *)
            fail "invalid internal mode: $RUN_MODE"
            ;;
    esac
}

log "Sanitizer profile: $PROFILE"
log "Execution mode: $RUN_MODE"
log "Precision request: $PRECISION_REQUEST"
log "Editor dev_build: $EDITOR_DEV_BUILD"
log "C++ runtime linkage: $([[ "$STATIC_CPP" == "yes" ]] && printf 'static' || printf 'shared')"
log "ASAN invalid pointer pairs: $([[ "$INVALID_POINTER_PAIRS" == "yes" ]] && printf 'enabled' || printf 'disabled')"
log "ASAN leak detection: enabled"
log "Godot LSAN suppression: $([[ "$GODOT_LSAN_SUPPRESSIONS" == "yes" ]] && printf 'enabled' || printf 'disabled')"
log "Godot UBSAN suppression: $([[ "$GODOT_UBSAN_SUPPRESSIONS" == "yes" ]] && printf 'enabled' || printf 'disabled')"

check_selected_toolchains

for selected_precision in "${PRECISIONS[@]}"; do
    configure_precision_args "$selected_precision"
    log "Starting sanitizer precision: $PRECISION"
    run_selected_passes
done

log "Sanitized validation completed."

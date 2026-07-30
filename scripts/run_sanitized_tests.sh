#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
BUILD_SCRIPT="${SCRIPT_DIR}/build_and_validate.sh"

readonly EXPECTED_BUILD_SCRIPT_API="4"

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
Uso:
  ./scripts/run_sanitized_tests.sh [single|double] [opções]

Perfil padrão:
  Executa duas compilações independentes:
    1. ASAN com Clang + LLD.
    2. UBSAN com GCC + LLD.

A separação evita depender do runtime C++ UBSAN do Clang, que pode não estar
instalado, e evita o link combinado excessivamente grande do Godot 4.7.1.

Opções:
  --module-profile   Desabilita raycast/Embree. Padrão.
  --full-engine      Mantém todos os módulos da engine.
  --split            ASAN/Clang e UBSAN/GCC em passagens separadas. Padrão.
  --asan-only        Executa somente ASAN com Clang + LLD.
  --ubsan-only       Executa somente UBSAN com GCC + LLD.
  --combined         Executa ASAN + UBSAN juntos com Clang + LLD.
                     Diagnóstico; exige compiler-rt UBSAN C++ completo.
  --llvm             Força Clang + LLD para todas as passagens selecionadas.
                     Diagnóstico; UBSAN pode exigir pacote compiler-rt adicional.
  --gcc              Força GCC + LLD para todas as passagens selecionadas.
                     Diagnóstico; ASAN pode exceder limites de relocação.
  --dev-build        Usa dev_build=yes. Diagnóstico; aumenta muito o binário.
  --static-cpp       Usa runtime C++ estático. Diagnóstico; não recomendado.
  --clean-first      Limpa cada configuração sanitizada antes de compilar.
  --stack-use-after-return
                     Ativa detecção de use-after-return no ASAN. Mais lenta.
  --invalid-pointer-pairs
                     Ativa a verificação opcional de comparação/subtração entre
                     ponteiros de objetos distintos. Diagnóstico da engine; no
                     Godot 4.7.1 ela aborta em StringName antes dos testes.
  --no-godot-ubsan-suppressions
                     Desativa a supressão estrita do falso positivo engine-level
                     nonnull-attribute em core/string/ustring.cpp. Diagnóstico.
  -h, --help         Mostra esta ajuda.

Argumentos desconhecidos são repassados a build_and_validate.sh.
USAGE
}

fail() {
    printf 'ERRO: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '[TickSynchronizer] %s\n' "$*" >&2
}

verify_build_script_contract() {
    [[ -x "$BUILD_SCRIPT" ]] || \
        fail "build_and_validate.sh não encontrado ou sem permissão de execução: $BUILD_SCRIPT"

    local actual_api
    actual_api="$($BUILD_SCRIPT --print-api-version 2>/dev/null)" || \
        fail "build_and_validate.sh não oferece a interface esperada. Extraia o ZIP completo."

    [[ "$actual_api" == "$EXPECTED_BUILD_SCRIPT_API" ]] || \
        fail "scripts incompatíveis: run_sanitized_tests exige API $EXPECTED_BUILD_SCRIPT_API, mas build_and_validate fornece '$actual_api'."
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
        fail "ld.lld não encontrado. Instale o linker LLD compatível com seu sistema."
}

check_clang_asan_toolchain() {
    command -v clang++ >/dev/null 2>&1 || \
        fail "clang++ não encontrado. Instale Clang e compiler-rt."
    check_lld

    local probe_dir
    probe_dir="$(mktemp -d)"
    trap 'rm -rf -- "$probe_dir"' RETURN
    make_probe_source "$probe_dir/probe.cpp"

    if ! clang++ -std=c++17 -O1 -g -fuse-ld=lld \
        -fsanitize=address -fno-omit-frame-pointer \
        "$probe_dir/probe.cpp" -o "$probe_dir/probe" >"$probe_dir/link.log" 2>&1; then
        cat "$probe_dir/link.log" >&2
        fail "Clang/LLD não conseguiu ligar um programa C++17 com ASAN. Verifique a instalação do compiler-rt."
    fi

    ASAN_OPTIONS=halt_on_error=1 "$probe_dir/probe" >/dev/null 2>&1 || \
        fail "o executável de teste ASAN não iniciou corretamente"
    rm -rf -- "$probe_dir"
    trap - RETURN
}

check_gcc_ubsan_toolchain() {
    command -v g++ >/dev/null 2>&1 || \
        fail "g++ não encontrado. Instale o runtime UBSAN correspondente ao GCC."
    check_lld

    local probe_dir
    probe_dir="$(mktemp -d)"
    trap 'rm -rf -- "$probe_dir"' RETURN
    make_probe_source "$probe_dir/probe.cpp"

    if ! g++ -std=c++17 -O1 -g -fuse-ld=lld \
        -fsanitize=undefined -fno-omit-frame-pointer \
        "$probe_dir/probe.cpp" -o "$probe_dir/probe" >"$probe_dir/link.log" 2>&1; then
        cat "$probe_dir/link.log" >&2
        fail "GCC/LLD não conseguiu ligar um programa C++17 com UBSAN. Verifique libubsan e LLD."
    fi

    UBSAN_OPTIONS=halt_on_error=1 "$probe_dir/probe" >/dev/null 2>&1 || \
        fail "o executável de teste UBSAN não iniciou corretamente"
    rm -rf -- "$probe_dir"
    trap - RETURN
}


check_clang_ubsan_toolchain() {
    command -v clang++ >/dev/null 2>&1 || \
        fail "clang++ não encontrado."
    check_lld

    local probe_dir
    probe_dir="$(mktemp -d)"
    trap 'rm -rf -- "$probe_dir"' RETURN
    make_probe_source "$probe_dir/probe.cpp"

    if ! clang++ -std=c++17 -O1 -g -fuse-ld=lld \
        -fsanitize=undefined -fno-omit-frame-pointer \
        "$probe_dir/probe.cpp" -o "$probe_dir/probe" >"$probe_dir/link.log" 2>&1; then
        cat "$probe_dir/link.log" >&2
        local resource_dir
        resource_dir="$(clang++ -print-resource-dir 2>/dev/null || true)"
        printf 'Clang resource directory: %s\n' "${resource_dir:-unknown}" >&2
        fail "Clang não possui o runtime C++ UBSAN necessário. Use UBSAN com GCC, que é o padrão."
    fi

    UBSAN_OPTIONS=halt_on_error=1 "$probe_dir/probe" >/dev/null 2>&1 || \
        fail "o executável de teste Clang/UBSAN não iniciou corretamente"
    rm -rf -- "$probe_dir"
    trap - RETURN
}

check_clang_combined_toolchain() {
    command -v clang++ >/dev/null 2>&1 || \
        fail "clang++ não encontrado."
    check_lld

    local probe_dir
    probe_dir="$(mktemp -d)"
    trap 'rm -rf -- "$probe_dir"' RETURN
    make_probe_source "$probe_dir/probe.cpp"

    if ! clang++ -std=c++17 -O1 -g -fuse-ld=lld \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        "$probe_dir/probe.cpp" -o "$probe_dir/probe" >"$probe_dir/link.log" 2>&1; then
        cat "$probe_dir/link.log" >&2
        local resource_dir
        resource_dir="$(clang++ -print-resource-dir 2>/dev/null || true)"
        printf 'Clang resource directory: %s\n' "${resource_dir:-unknown}" >&2
        fail "Clang não possui um runtime ASAN+UBSAN C++ completo. Use o perfil padrão --split."
    fi

    "$probe_dir/probe" >/dev/null 2>&1 || \
        fail "o executável combinado ASAN+UBSAN não iniciou corretamente"
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
    fail "precision inválida: $PRECISION"

verify_build_script_contract

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
        'AVISO: --full-engine aumenta muito o tempo e o tamanho do build e não é requisito para aceitar alterações do módulo.' >&2
fi

if [[ "$STATIC_CPP" == "yes" ]]; then
    printf '%s\n' \
        'AVISO: --static-cpp pode reproduzir estouros de relocação e não é recomendado com sanitizers.' >&2
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
        fail "arquivo de supressões UBSAN não encontrado: $UBSAN_SUPPRESSION_FILE"
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
            fail "sanitizer interno inválido: $sanitizer"
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
            fail "toolchain interno inválido: $toolchain"
            ;;
    esac

    log "Iniciando passagem ${label}: sanitizer=${sanitizer}, toolchain=${toolchain}/lld, precision=${PRECISION}"
    "$BUILD_SCRIPT" "${args[@]}"
    log "Passagem ${label} concluída com sucesso."
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
                'AVISO: ASAN+UBSAN combinados com GCC podem exceder limites de relocação no Godot 4.7.1.' >&2
            check_lld
            run_pass "ASAN+UBSAN" combined gcc
        else
            check_clang_combined_toolchain
            run_pass "ASAN+UBSAN" combined llvm
        fi
        ;;
    *)
        fail "modo interno inválido: $RUN_MODE"
        ;;
esac

log "Validação sanitizada concluída."

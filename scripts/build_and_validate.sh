#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_API_VERSION="4"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"

GODOT_DIR="${TICKSYNC_GODOT_DIR:-${MODULE_DIR}/../godot}"
CUSTOM_MODULES="${TICKSYNC_CUSTOM_MODULES:-../tick_synchronizer}"
PLATFORM="${TICKSYNC_PLATFORM:-linuxbsd}"
PRECISION="${TICKSYNC_PRECISION:-double}"
JOBS="${TICKSYNC_JOBS:-}"
SCONS_BIN="${TICKSYNC_SCONS_BIN:-scons}"
MODE="quick"
EDITOR_DEV_BUILD="yes"
TEST_FILTER="*TickSynchronizer*"
MIN_TEST_CASES=98
RUN_SMOKE=1
CLEAN_FIRST=0
ALLOW_GODOT_MISMATCH=0
ALLOW_DIRTY_GODOT=0
GODOT_BASELINE="${TICKSYNC_GODOT_BASELINE_COMMIT:-}"
EXTRA_SCONS_ARGS=()

usage() {
    cat <<'USAGE'
Uso:
  ./scripts/build_and_validate.sh [opções]

Modos:
  quick       Compila editor, executa testes C++ e smoke test. Padrão.
  editor      Igual a quick; nome explícito para tarefas do editor.
  templates   Compila e valida template_debug e template_release.
  all         Executa editor/testes/smoke e os dois templates.

Opções:
  --mode MODE                 quick, editor, templates ou all.
  --precision MODE            double ou single. Padrão: double.
  --godot-dir PATH            Árvore fonte do Godot. Padrão: ../godot.
  --custom-modules PATH       Valor de custom_modules. Padrão: ../tick_synchronizer.
  --platform NAME             Plataforma SCons. Padrão: linuxbsd.
  --jobs N                    Quantidade de jobs paralelos.
  --scons-bin COMMAND         Executável SCons. Padrão: scons.
  --editor-dev-build yes|no   Define dev_build do editor. Padrão: yes.
  --test-filter FILTER        Filtro doctest. Padrão: *TickSynchronizer*.
  --min-test-cases N          Quantidade mínima aceita. Padrão: 98.
  --clean-first               Limpa cada configuração antes de compilar.
  --no-smoke                  Não executa o smoke test GDScript.
  --scons-arg ARG             Argumento SCons adicional; pode ser repetido.
  --godot-baseline REF        Sobrescreve GODOT_COMMIT para diagnóstico.
  --allow-godot-mismatch      Permite HEAD diferente da baseline.
  --allow-dirty-godot         Permite alterações locais na engine.
  --print-api-version         Imprime a versão da interface deste script.
  -h, --help                  Mostra esta ajuda.

Exemplos:
  ./scripts/build_and_validate.sh --mode quick --precision double
  ./scripts/build_and_validate.sh --mode all --precision single
  ./scripts/build_and_validate.sh --mode templates --clean-first
  ./scripts/build_and_validate.sh --scons-arg use_llvm=yes
USAGE
}

fail() {
    printf 'ERRO: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '[TickSynchronizer] %s\n' "$*" >&2
}

command_string() {
    printf '%q ' "$@"
    printf '\n'
}

get_default_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.logicalcpu 2>/dev/null || printf '1\n'
    else
        printf '1\n'
    fi
}

resolve_path() {
    local base="$1"
    local path="$2"
    (
        cd -- "$base"
        realpath -m -- "$path"
    )
}

get_scons_arg_value() {
    local key="$1"
    local default_value="$2"
    local arg
    local value="$default_value"
    for arg in "${EXTRA_SCONS_ARGS[@]}"; do
        if [[ "$arg" == "${key}="* ]]; then
            value="${arg#*=}"
        fi
    done
    printf '%s\n' "$value"
}

is_sanitized_build() {
    local key
    for key in use_asan use_ubsan use_lsan use_tsan use_msan; do
        if [[ "$(get_scons_arg_value "$key" no)" == "yes" ]]; then
            return 0
        fi
    done
    return 1
}

verify_inputs() {
    [[ "$PRECISION" == "double" || "$PRECISION" == "single" ]] || \
        fail "precision inválida: $PRECISION"
    [[ "$MODE" == "quick" || "$MODE" == "editor" || "$MODE" == "templates" || "$MODE" == "all" ]] || \
        fail "mode inválido: $MODE"
    [[ "$EDITOR_DEV_BUILD" == "yes" || "$EDITOR_DEV_BUILD" == "no" ]] || \
        fail "--editor-dev-build aceita apenas yes ou no"
    [[ "$MIN_TEST_CASES" =~ ^[0-9]+$ ]] || fail "--min-test-cases deve ser inteiro não negativo"
    [[ -d "$GODOT_DIR" ]] || fail "diretório do Godot não encontrado: $GODOT_DIR"
    [[ -f "$GODOT_DIR/SConstruct" ]] || fail "SConstruct não encontrado em: $GODOT_DIR"
    [[ -f "$MODULE_DIR/SCsub" ]] || fail "SCsub do módulo não encontrado"
    [[ -f "$MODULE_DIR/config.py" ]] || fail "config.py do módulo não encontrado"
    [[ -x "$SCRIPT_DIR/build_and_validate.sh" ]] || fail "script sem permissão de execução"
    [[ -x "$SCRIPT_DIR/verify_source_consistency.sh" ]] || fail "verify_source_consistency.sh ausente ou sem permissão de execução"
    command -v "$SCONS_BIN" >/dev/null 2>&1 || fail "SCons não encontrado: $SCONS_BIN"
    command -v git >/dev/null 2>&1 || fail "git não encontrado"
    command -v python3 >/dev/null 2>&1 || fail "python3 não encontrado"

    "$SCRIPT_DIR/verify_source_consistency.sh" >/dev/null || \
        fail "os arquivos do módulo pertencem a revisões incompatíveis; extraia novamente o pacote completo"

    local custom_modules_resolved
    custom_modules_resolved="$(resolve_path "$GODOT_DIR" "$CUSTOM_MODULES")"
    [[ "$custom_modules_resolved" == "$MODULE_DIR" ]] || \
        fail "custom_modules resolve para '$custom_modules_resolved', mas o módulo está em '$MODULE_DIR'"

    if [[ -z "$JOBS" ]]; then
        JOBS="$(get_default_jobs)"
    fi
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "jobs inválido: $JOBS"
}

verify_godot_baseline() {
    git -C "$GODOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
        fail "a árvore do Godot não é um repositório Git"

    local configured_version="unknown"
    if [[ -f "$MODULE_DIR/GODOT_VERSION" ]]; then
        configured_version="$(tr -d '[:space:]' < "$MODULE_DIR/GODOT_VERSION")"
    fi

    if [[ -z "$GODOT_BASELINE" ]]; then
        [[ -f "$MODULE_DIR/GODOT_COMMIT" ]] || fail "GODOT_COMMIT não encontrado"
        GODOT_BASELINE="$(tr -d '[:space:]' < "$MODULE_DIR/GODOT_COMMIT")"
    fi
    [[ -n "$GODOT_BASELINE" ]] || fail "baseline vazia"

    local baseline_full
    local head_full
    baseline_full="$(git -C "$GODOT_DIR" rev-parse "${GODOT_BASELINE}^{commit}" 2>/dev/null)" || \
        fail "não foi possível resolver a baseline '$GODOT_BASELINE' na árvore do Godot"
    head_full="$(git -C "$GODOT_DIR" rev-parse HEAD)"

    if [[ "$head_full" != "$baseline_full" ]]; then
        if (( ALLOW_GODOT_MISMATCH )); then
            printf 'AVISO: HEAD do Godot (%s) difere da baseline (%s).\n' "$head_full" "$baseline_full" >&2
        else
            fail "HEAD do Godot difere da baseline $configured_version ($baseline_full)"
        fi
    fi

    local dirty_status
    dirty_status="$(git -C "$GODOT_DIR" status --porcelain --untracked-files=all)"
    if [[ -n "$dirty_status" ]]; then
        if (( ALLOW_DIRTY_GODOT )); then
            printf 'AVISO: árvore do Godot possui alterações locais:\n%s\n' "$dirty_status" >&2
        else
            fail "A árvore do Godot possui alterações locais; o projeto proíbe modificações na engine"
        fi
    fi

    RESOLVED_GODOT_BASELINE="$baseline_full"
    GODOT_HEAD="$head_full"
    GODOT_VERSION_LABEL="$configured_version"
}

artifact_matches_configuration() {
    local artifact="$1"
    local target="$2"
    local filename
    filename="$(basename -- "$artifact")"

    if [[ "$PRECISION" == "double" ]]; then
        [[ "$filename" == *.double.* || "$filename" == *.double ]] || return 1
    else
        [[ "$filename" != *.double.* && "$filename" != *.double ]] || return 1
    fi

    local llvm
    llvm="$(get_scons_arg_value use_llvm no)"
    if [[ "$llvm" == "yes" ]]; then
        [[ "$filename" == *.llvm* ]] || return 1
    else
        [[ "$filename" != *.llvm* ]] || return 1
    fi

    if is_sanitized_build; then
        [[ "$filename" == *.san* ]] || return 1
    else
        [[ "$filename" != *.san* ]] || return 1
    fi

    if [[ "$target" == "editor" ]]; then
        if [[ "$EDITOR_DEV_BUILD" == "yes" ]]; then
            [[ "$filename" == *.editor.dev.* ]] || return 1
        else
            [[ "$filename" != *.editor.dev.* ]] || return 1
        fi
    fi

    return 0
}

find_artifact() {
    local target="$1"
    local candidate
    local newest=""
    local newest_mtime=0

    while IFS= read -r -d '' candidate; do
        artifact_matches_configuration "$candidate" "$target" || continue
        local mtime
        mtime="$(stat -c '%Y' "$candidate" 2>/dev/null || stat -f '%m' "$candidate")"
        if (( mtime >= newest_mtime )); then
            newest="$candidate"
            newest_mtime="$mtime"
        fi
    done < <(
        find "$GODOT_DIR/bin" -maxdepth 1 -type f \
            -name "godot.${PLATFORM}.${target}*" -print0 2>/dev/null
    )

    [[ -n "$newest" ]] || return 1
    printf '%s\n' "$newest"
}

write_environment_report() {
    local report="$1"
    {
        printf 'TickSynchronizer build environment\n'
        printf 'Generated UTC: %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
        printf 'Script API: %s\n' "$SCRIPT_API_VERSION"
        printf 'Module dir: %s\n' "$MODULE_DIR"
        printf 'Godot dir: %s\n' "$GODOT_DIR"
        printf 'Godot version: %s\n' "$GODOT_VERSION_LABEL"
        printf 'Godot baseline: %s\n' "$RESOLVED_GODOT_BASELINE"
        printf 'Godot HEAD: %s\n' "$GODOT_HEAD"
        printf 'Godot branch: %s\n' "$(git -C "$GODOT_DIR" branch --show-current 2>/dev/null || true)"
        printf 'Godot describe: %s\n' "$(git -C "$GODOT_DIR" describe --always --dirty --tags 2>/dev/null || true)"
        printf 'Godot origin: %s\n' "$(git -C "$GODOT_DIR" remote get-url origin 2>/dev/null || true)"
        printf 'custom_modules: %s\n' "$CUSTOM_MODULES"
        printf 'Mode: %s\n' "$MODE"
        printf 'Platform: %s\n' "$PLATFORM"
        printf 'Precision: %s\n' "$PRECISION"
        printf 'Editor dev_build: %s\n' "$EDITOR_DEV_BUILD"
        printf 'Test filter: %s\n' "$TEST_FILTER"
        printf 'Minimum test cases: %s\n' "$MIN_TEST_CASES"
        printf 'Jobs: %s\n' "$JOBS"
        printf 'SCons command: %s\n' "$SCONS_BIN"
        printf 'Extra SCons args:'
        printf ' %q' "${EXTRA_SCONS_ARGS[@]}"
        printf '\n\nSystem:\n'
        uname -a 2>/dev/null || true
        command -v lsb_release >/dev/null 2>&1 && lsb_release -a 2>/dev/null || true
        command -v lscpu >/dev/null 2>&1 && lscpu 2>/dev/null || true
        command -v free >/dev/null 2>&1 && free -h 2>/dev/null || true
        printf '\nTools:\n'
        "$SCONS_BIN" --version 2>/dev/null || true
        python3 --version 2>/dev/null || true
        c++ --version 2>/dev/null || true
        clang++ --version 2>/dev/null || true
        ld.lld --version 2>/dev/null || true
    } > "$report"
}

build_target() {
    local target="$1"
    local -a target_args=(
        "platform=${PLATFORM}"
        "target=${target}"
        "precision=${PRECISION}"
        "custom_modules=${CUSTOM_MODULES}"
        "module_tick_synchronizer_enabled=yes"
    )

    if [[ "$target" == "editor" ]]; then
        target_args+=("tests=yes" "dev_build=${EDITOR_DEV_BUILD}")
    fi

    local -a full_args=("${target_args[@]}" "${EXTRA_SCONS_ARGS[@]}")
    local clean_log="$REPORT_DIR/${target}.clean.log"
    local build_log="$REPORT_DIR/${target}.build.log"
    local command_file="$REPORT_DIR/${target}.command.txt"
    local time_file="$REPORT_DIR/${target}.time.txt"

    if (( CLEAN_FIRST )); then
        log "Limpando ${target}..."
        (
            cd -- "$GODOT_DIR"
            "$SCONS_BIN" --clean "${full_args[@]}"
        ) > "$clean_log" 2>&1 || fail "Limpeza falhou para ${target}. Consulte $clean_log"
    fi

    local -a command=("$SCONS_BIN" "${full_args[@]}" "-j${JOBS}")
    command_string "${command[@]}" > "$command_file"
    log "Compilando ${target} com precision=${PRECISION}..."

    local status
    set +e
    if [[ -x /usr/bin/time ]]; then
        (
            cd -- "$GODOT_DIR"
            /usr/bin/time -v -o "$time_file" "${command[@]}"
        ) 2>&1 | tee "$build_log"
        status=${PIPESTATUS[0]}
    else
        (
            cd -- "$GODOT_DIR"
            "${command[@]}"
        ) 2>&1 | tee "$build_log"
        status=${PIPESTATUS[0]}
    fi
    set -e

    (( status == 0 )) || fail "Build ${target} falhou. Consulte $build_log"

    local artifact
    artifact="$(find_artifact "$target")" || \
        fail "não foi possível localizar o artefato de ${target} compatível com esta configuração"
    validate_artifact "$target" "$artifact"
    LAST_ARTIFACT="$artifact"
}

validate_artifact() {
    local target="$1"
    local artifact="$2"
    local log_file="$REPORT_DIR/${target}.validation.log"

    [[ -s "$artifact" ]] || fail "artefato vazio: $artifact"
    if [[ "$PLATFORM" == "linuxbsd" ]]; then
        [[ -x "$artifact" ]] || fail "artefato não executável: $artifact"
    fi

    local status
    set +e
    (
        printf 'Target: %s\n' "$target"
        printf 'Precision: %s\n' "$PRECISION"
        printf 'Artifact: %s\n' "$artifact"
        printf 'Size bytes: %s\n' "$(stat -c '%s' "$artifact" 2>/dev/null || stat -f '%z' "$artifact")"
        printf 'Modified: %s\n' "$(stat -c '%y' "$artifact" 2>/dev/null || stat -f '%Sm' "$artifact")"
        file "$artifact" 2>/dev/null || true

        if [[ "$PLATFORM" == "linuxbsd" ]]; then
            if command -v readelf >/dev/null 2>&1; then
                printf '\nELF header:\n'
                readelf -h "$artifact"
            fi

            if command -v ldd >/dev/null 2>&1; then
                printf '\nDynamic dependencies:\n'
                ldd_output="$(ldd "$artifact" 2>&1)"
                printf '%s\n' "$ldd_output"
                if grep -Eq '=>[[:space:]]+not found([[:space:]]|$)' <<<"$ldd_output"; then
                    printf '\nERROR: uma ou mais dependências dinâmicas não foram encontradas.\n'
                    exit 97
                fi
            fi

            if is_sanitized_build; then
                printf '\nExecution probe:\n'
                printf '%s\n' \
                    'Skipped for sanitized editor. The mandatory filtered C++ tests' \
                    ' and smoke test are the runtime validation.'
            else
                printf '\n--version:\n'
                timeout 30s "$artifact" --version
            fi
        fi
    ) > "$log_file" 2>&1
    status=$?
    set -e

    if (( status != 0 )); then
        printf '--- %s ---\n' "$log_file" >&2
        tail -n 160 "$log_file" >&2 || true
        fail "validação do artefato falhou: $artifact"
    fi

    log "${target} validado: ${artifact}"
}

run_cpp_tests() {
    local editor="$1"
    local log_file="$REPORT_DIR/editor.tests.log"
    log "Executando testes C++ com filtro ${TEST_FILTER}..."

    local status
    set +e
    timeout 900s "$editor" --test --test-case="$TEST_FILTER" 2>&1 | tee "$log_file"
    status=${PIPESTATUS[0]}
    set -e
    (( status == 0 )) || fail "testes C++ falharam. Consulte $log_file"

    local found
    found="$(sed -nE 's/.*test cases:[[:space:]]*([0-9]+).*/\1/p' "$log_file" | tail -n 1)"
    [[ -n "$found" ]] || fail "não foi possível determinar a quantidade de testes em $log_file"
    (( found >= MIN_TEST_CASES )) || \
        fail "apenas ${found} testes encontrados; mínimo esperado: ${MIN_TEST_CASES}"
    TEST_CASES_FOUND="$found"
}

run_smoke_test() {
    local editor="$1"
    local log_file="$REPORT_DIR/editor.smoke.log"
    log "Executando smoke test GDScript pelo editor..."

    local status
    set +e
    TICKSYNC_EXPECTED_PRECISION="$PRECISION" \
        timeout 120s "$editor" \
        --headless \
        --path "$MODULE_DIR/tests/smoke_project" \
        --quit-after 600 2>&1 | tee "$log_file"
    status=${PIPESTATUS[0]}
    set -e
    (( status == 0 )) || fail "smoke test falhou. Consulte $log_file"

    local marker
    for marker in \
        "TICKSYNCHRONIZER_BUILD_PRECISION=${PRECISION}" \
        "TICKSYNCHRONIZER_BUFFER_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_INTEGER_CODEC_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_FLOAT_CODEC_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_RESOURCE_LIMIT_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK" \
        "TICKSYNCHRONIZER_SMOKE_TEST_OK"; do
        grep -Fq "$marker" "$log_file" || fail "marcador ausente no smoke test: $marker"
    done
}

write_artifact_record() {
    local label="$1"
    local artifact="$2"
    local size
    local hash
    size="$(stat -c '%s' "$artifact" 2>/dev/null || stat -f '%z' "$artifact")"
    hash="$(sha256sum "$artifact" | awk '{print $1}')"
    printf '%s\t%s\t%s\t%s\n' "$label" "$artifact" "$size" "$hash" >> "$REPORT_DIR/artifacts.tsv"
    printf '%s  %s\n' "$hash" "$artifact" >> "$REPORT_DIR/sha256sums.txt"
}

write_summary() {
    {
        printf 'TickSynchronizer validation summary\n'
        printf 'Status: SUCCESS\n'
        printf 'Generated UTC: %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
        printf 'Script API: %s\n' "$SCRIPT_API_VERSION"
        printf 'Mode: %s\n' "$MODE"
        printf 'Platform: %s\n' "$PLATFORM"
        printf 'Precision: %s\n' "$PRECISION"
        printf 'Godot version: %s\n' "$GODOT_VERSION_LABEL"
        printf 'Godot baseline: %s\n' "$RESOLVED_GODOT_BASELINE"
        printf 'Godot HEAD: %s\n' "$GODOT_HEAD"
        printf 'Test filter: %s\n' "$TEST_FILTER"
        printf 'Minimum test cases: %s\n' "$MIN_TEST_CASES"
        printf 'Test cases found: %s\n' "${TEST_CASES_FOUND:-not-run}"
        printf 'Smoke test: %s\n' "$([[ $RUN_SMOKE -eq 1 && ( "$MODE" == quick || "$MODE" == editor || "$MODE" == all ) ]] && printf passed || printf not-run)"
        printf '\nArtifacts:\n'
        cat "$REPORT_DIR/artifacts.tsv" 2>/dev/null || true
    } > "$REPORT_DIR/summary.txt"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --print-api-version)
            printf '%s\n' "$SCRIPT_API_VERSION"
            exit 0
            ;;
        --mode)
            [[ $# -ge 2 ]] || fail "--mode exige um valor"
            MODE="$2"
            shift 2
            ;;
        --precision)
            [[ $# -ge 2 ]] || fail "--precision exige um valor"
            PRECISION="$2"
            shift 2
            ;;
        --godot-dir)
            [[ $# -ge 2 ]] || fail "--godot-dir exige um valor"
            GODOT_DIR="$2"
            shift 2
            ;;
        --custom-modules)
            [[ $# -ge 2 ]] || fail "--custom-modules exige um valor"
            CUSTOM_MODULES="$2"
            shift 2
            ;;
        --platform)
            [[ $# -ge 2 ]] || fail "--platform exige um valor"
            PLATFORM="$2"
            shift 2
            ;;
        --jobs)
            [[ $# -ge 2 ]] || fail "--jobs exige um valor"
            JOBS="$2"
            shift 2
            ;;
        --scons-bin)
            [[ $# -ge 2 ]] || fail "--scons-bin exige um valor"
            SCONS_BIN="$2"
            shift 2
            ;;
        --editor-dev-build)
            [[ $# -ge 2 ]] || fail "--editor-dev-build exige um valor"
            EDITOR_DEV_BUILD="$2"
            shift 2
            ;;
        --test-filter)
            [[ $# -ge 2 ]] || fail "--test-filter exige um valor"
            TEST_FILTER="$2"
            shift 2
            ;;
        --min-test-cases)
            [[ $# -ge 2 ]] || fail "--min-test-cases exige um valor"
            MIN_TEST_CASES="$2"
            shift 2
            ;;
        --clean-first)
            CLEAN_FIRST=1
            shift
            ;;
        --no-smoke)
            RUN_SMOKE=0
            shift
            ;;
        --scons-arg)
            [[ $# -ge 2 ]] || fail "--scons-arg exige um valor"
            EXTRA_SCONS_ARGS+=("$2")
            shift 2
            ;;
        --godot-baseline)
            [[ $# -ge 2 ]] || fail "--godot-baseline exige um valor"
            GODOT_BASELINE="$2"
            shift 2
            ;;
        --allow-godot-mismatch)
            ALLOW_GODOT_MISMATCH=1
            shift
            ;;
        --allow-dirty-godot)
            ALLOW_DIRTY_GODOT=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "opção desconhecida: $1"
            ;;
    esac
done

GODOT_DIR="$(realpath -m -- "$GODOT_DIR")"
verify_inputs
verify_godot_baseline

REPORT_TAG="$(date -u +'%Y%m%dT%H%M%SZ')-${PLATFORM}-${PRECISION}-${MODE}"
REPORT_DIR="$MODULE_DIR/build_reports/$REPORT_TAG"
mkdir -p "$REPORT_DIR"
ln -sfn "$REPORT_TAG" "$MODULE_DIR/build_reports/latest"
: > "$REPORT_DIR/artifacts.tsv"
: > "$REPORT_DIR/sha256sums.txt"
write_environment_report "$REPORT_DIR/environment.txt"

log "Relatórios: $REPORT_DIR"
log "Godot: $GODOT_DIR"
log "Baseline: $GODOT_VERSION_LABEL ($RESOLVED_GODOT_BASELINE)"
log "Módulo: $MODULE_DIR"
log "Modo: $MODE"
log "Precisão: $PRECISION"

EDITOR_ARTIFACT=""
if [[ "$MODE" == "quick" || "$MODE" == "editor" || "$MODE" == "all" ]]; then
    build_target editor
    EDITOR_ARTIFACT="$LAST_ARTIFACT"
    write_artifact_record editor "$EDITOR_ARTIFACT"
fi

if [[ "$MODE" == "templates" || "$MODE" == "all" ]]; then
    build_target template_debug
    TEMPLATE_DEBUG_ARTIFACT="$LAST_ARTIFACT"
    write_artifact_record template_debug "$TEMPLATE_DEBUG_ARTIFACT"
    build_target template_release
    TEMPLATE_RELEASE_ARTIFACT="$LAST_ARTIFACT"
    write_artifact_record template_release "$TEMPLATE_RELEASE_ARTIFACT"
fi

if [[ -n "$EDITOR_ARTIFACT" ]]; then
    run_cpp_tests "$EDITOR_ARTIFACT"
    if (( RUN_SMOKE )); then
        run_smoke_test "$EDITOR_ARTIFACT"
    fi
fi

write_summary
log "Build e validação concluídos."
log "Resumo: $REPORT_DIR/summary.txt"
if [[ -n "$EDITOR_ARTIFACT" ]]; then
    log "Testes C++: $REPORT_DIR/editor.tests.log"
    if (( RUN_SMOKE )); then
        log "Smoke test: $REPORT_DIR/editor.smoke.log"
    fi
fi

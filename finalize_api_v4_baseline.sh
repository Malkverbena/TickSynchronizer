#!/usr/bin/env bash
set -Eeuo pipefail

# Formaliza o commit atual do TickSynchronizer como baseline estável da API v4.
#
# Uso:
#   chmod +x finalize_api_v4_baseline.sh
#   ./finalize_api_v4_baseline.sh
#
# Variáveis opcionais:
#   TAG_NAME=baseline-api-v4
#   GODOT_DIR=/caminho/para/godot
#   DOUBLE_REPORT=/caminho/para/build_reports/...-double-all
#   SINGLE_REPORT=/caminho/para/build_reports/...-single-all

fail() {
    printf 'ERRO: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '[TickSynchronizer baseline] %s\n' "$*"
}

command -v git >/dev/null 2>&1 || fail "git não encontrado"
command -v grep >/dev/null 2>&1 || fail "grep não encontrado"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

if git -C "$SCRIPT_DIR" rev-parse --show-toplevel >/dev/null 2>&1; then
    MODULE_DIR="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
elif git -C "$SCRIPT_DIR/.." rev-parse --show-toplevel >/dev/null 2>&1; then
    MODULE_DIR="$(git -C "$SCRIPT_DIR/.." rev-parse --show-toplevel)"
else
    fail "o script deve estar dentro do repositório TickSynchronizer"
fi

TAG_NAME="${TAG_NAME:-baseline-api-v4}"
GODOT_DIR="${GODOT_DIR:-$(cd -- "$MODULE_DIR/../godot" 2>/dev/null && pwd -P || true)}"

DOUBLE_REPORT="${DOUBLE_REPORT:-$MODULE_DIR/build_reports/20260730T195205Z-linuxbsd-double-all}"
SINGLE_REPORT="${SINGLE_REPORT:-$MODULE_DIR/build_reports/20260730T195950Z-linuxbsd-single-all}"

API_SCRIPT="$MODULE_DIR/scripts/build_and_validate.sh"
CONSISTENCY_SCRIPT="$MODULE_DIR/scripts/verify_source_consistency.sh"

[[ -x "$API_SCRIPT" ]] || fail "script ausente ou não executável: $API_SCRIPT"
[[ -x "$CONSISTENCY_SCRIPT" ]] || fail "script ausente ou não executável: $CONSISTENCY_SCRIPT"
[[ -d "$GODOT_DIR/.git" ]] || fail "repositório Godot não encontrado em: $GODOT_DIR"

MODULE_HEAD="$(git -C "$MODULE_DIR" rev-parse HEAD)"
MODULE_BRANCH="$(git -C "$MODULE_DIR" branch --show-current)"
MODULE_STATUS="$(git -C "$MODULE_DIR" status --porcelain)"

[[ -z "$MODULE_STATUS" ]] || {
    printf '%s\n' "$MODULE_STATUS" >&2
    fail "a árvore do TickSynchronizer não está limpa; a baseline deve apontar para um commit fechado"
}

GODOT_HEAD="$(git -C "$GODOT_DIR" rev-parse HEAD)"
GODOT_STATUS="$(git -C "$GODOT_DIR" status --porcelain)"
[[ -z "$GODOT_STATUS" ]] || {
    printf '%s\n' "$GODOT_STATUS" >&2
    fail "a árvore do Godot não está limpa"
}

EXPECTED_GODOT_VERSION="4.7.1-stable"
EXPECTED_GODOT_COMMIT="a13da4feb8d8aefc283c3763d33a2f170a18d541"

if [[ -f "$MODULE_DIR/GODOT_VERSION" ]]; then
    ACTUAL_GODOT_VERSION="$(tr -d '[:space:]' < "$MODULE_DIR/GODOT_VERSION")"
    [[ "$ACTUAL_GODOT_VERSION" == "$EXPECTED_GODOT_VERSION" ]] ||
        fail "GODOT_VERSION=$ACTUAL_GODOT_VERSION; esperado $EXPECTED_GODOT_VERSION"
fi

if [[ -f "$MODULE_DIR/GODOT_COMMIT" ]]; then
    ACTUAL_GODOT_COMMIT="$(tr -d '[:space:]' < "$MODULE_DIR/GODOT_COMMIT")"
    [[ "$ACTUAL_GODOT_COMMIT" == "$EXPECTED_GODOT_COMMIT" ]] ||
        fail "GODOT_COMMIT=$ACTUAL_GODOT_COMMIT; esperado $EXPECTED_GODOT_COMMIT"
fi

[[ "$GODOT_HEAD" == "$EXPECTED_GODOT_COMMIT" ]] ||
    fail "HEAD do Godot=$GODOT_HEAD; esperado $EXPECTED_GODOT_COMMIT"

log "Verificando API pública..."
API_VERSION="$("$API_SCRIPT" --print-api-version)"
[[ "$API_VERSION" == "4" ]] || fail "API version=$API_VERSION; esperado 4"

log "Verificando consistência de fontes, testes e Mermaid..."
CONSISTENCY_OUTPUT="$("$CONSISTENCY_SCRIPT")"
printf '%s\n' "$CONSISTENCY_OUTPUT"

grep -Fq "TICKSYNCHRONIZER_SOURCE_CONSISTENCY_OK methods=41 tests=127" <<<"$CONSISTENCY_OUTPUT" ||
    fail "resultado de consistência diferente da baseline esperada"

grep -Fq "TICKSYNCHRONIZER_MERMAID_OK diagrams=19 files=11 renderer=static" <<<"$CONSISTENCY_OUTPUT" ||
    fail "resultado Mermaid diferente da baseline esperada"

validate_report() {
    local precision="$1"
    local report_dir="$2"
    local tests_log="$report_dir/editor.tests.log"
    local smoke_log="$report_dir/editor.smoke.log"
    local summary="$report_dir/summary.txt"

    [[ -d "$report_dir" ]] || fail "diretório de relatório ausente: $report_dir"
    [[ -f "$tests_log" ]] || fail "log de testes ausente: $tests_log"
    [[ -f "$smoke_log" ]] || fail "log de smoke test ausente: $smoke_log"
    [[ -f "$summary" ]] || fail "resumo ausente: $summary"

    grep -Fq "[doctest] test cases:   127 |   127 passed | 0 failed" "$tests_log" ||
        fail "contagem de casos C++ inválida em $tests_log"

    grep -Fq "[doctest] assertions: 66838 | 66838 passed | 0 failed" "$tests_log" ||
        fail "contagem de assertions inválida em $tests_log"

    grep -Fq "[doctest] Status: SUCCESS!" "$tests_log" ||
        fail "testes C++ não registram sucesso em $tests_log"

    grep -Fq "TICKSYNCHRONIZER_BUILD_PRECISION=$precision" "$smoke_log" ||
        fail "precisão incorreta ou ausente em $smoke_log"

    local marker
    for marker in \
        TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK \
        TICKSYNCHRONIZER_BUFFER_SMOKE_TEST_OK \
        TICKSYNCHRONIZER_INTEGER_CODEC_SMOKE_TEST_OK \
        TICKSYNCHRONIZER_FLOAT_CODEC_SMOKE_TEST_OK \
        TICKSYNCHRONIZER_RESOURCE_LIMIT_SMOKE_TEST_OK \
        TICKSYNCHRONIZER_SMOKE_TEST_OK
    do
        grep -Fq "$marker" "$smoke_log" ||
            fail "marcador ausente em $smoke_log: $marker"
    done

    log "Relatório $precision validado: $report_dir"
}

validate_report double "$DOUBLE_REPORT"
validate_report single "$SINGLE_REPORT"

TAG_MESSAGE=$(cat <<EOF
TickSynchronizer API v4 stable baseline

Date: 2026-07-30
Module commit: $MODULE_HEAD
Module branch: ${MODULE_BRANCH:-detached}
Godot version: $EXPECTED_GODOT_VERSION
Godot commit: $EXPECTED_GODOT_COMMIT
Platform: linuxbsd x86_64

Source consistency:
- Public API version: 4
- Documented/bound methods: 41
- C++ test cases: 127
- Mermaid diagrams: 19 in 11 files

Validated configurations:
- precision=double, mode=all, jobs=45
- precision=single, mode=all, jobs=45

Results per precision:
- 127/127 C++ test cases passed
- 66,838/66,838 assertions passed
- protocol smoke test passed
- buffer smoke test passed
- integer codec smoke test passed
- float codec smoke test passed
- resource-limit smoke test passed
- complete smoke test passed

Reports:
- double: $DOUBLE_REPORT
- single: $SINGLE_REPORT
EOF
)

if git -C "$MODULE_DIR" rev-parse -q --verify "refs/tags/$TAG_NAME" >/dev/null; then
    TAG_COMMIT="$(git -C "$MODULE_DIR" rev-list -n 1 "$TAG_NAME")"
    [[ "$TAG_COMMIT" == "$MODULE_HEAD" ]] ||
        fail "a tag $TAG_NAME já existe e aponta para $TAG_COMMIT, não para $MODULE_HEAD"

    log "A tag $TAG_NAME já existe no commit validado."
else
    git -C "$MODULE_DIR" tag -a "$TAG_NAME" "$MODULE_HEAD" -m "$TAG_MESSAGE"
    log "Tag anotada criada: $TAG_NAME"
fi

printf '\n'
git -C "$MODULE_DIR" show \
    --no-patch \
    --decorate=full \
    --format='Commit: %H%nRefs: %D%nAuthor: %an <%ae>%nDate: %aI%nSubject: %s' \
    "$TAG_NAME"

printf '\n'
log "Baseline API v4 formalizada com sucesso."
log "Para publicar a tag: git push origin $TAG_NAME"

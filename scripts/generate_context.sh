#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
GODOT_DIR="${TICKSYNC_GODOT_DIR:-${MODULE_DIR}/../godot}"
OUTPUT="${MODULE_DIR}/build/context/TICKSYNCHRONIZER_CONTEXT.md"

usage() {
	cat <<'USAGE'
Uso:
  ./scripts/generate_context.sh [--output ARQUIVO] [--godot-dir PATH]

Gera um resumo compacto para iniciar uma nova sessão de trabalho sem incluir
código-fonte completo ou logs extensos.
USAGE
}

fail() {
	printf 'ERRO: %s\n' "$*" >&2
	exit 1
}

while (( $# > 0 )); do
	case "$1" in
		--output)
			[[ $# -ge 2 ]] || fail "--output exige um valor"
			OUTPUT="$2"
			shift 2
			;;
		--godot-dir)
			[[ $# -ge 2 ]] || fail "--godot-dir exige um valor"
			GODOT_DIR="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			fail "Opção desconhecida: $1"
			;;
	esac
done

OUTPUT="$(realpath -m -- "$OUTPUT")"
GODOT_DIR="$(realpath -m -- "$GODOT_DIR")"
mkdir -p "$(dirname -- "$OUTPUT")"

module_git() {
	if git -C "$MODULE_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		printf -- '- Branch: `%s`\n' "$(git -C "$MODULE_DIR" branch --show-current 2>/dev/null || true)"
		printf -- '- HEAD: `%s`\n' "$(git -C "$MODULE_DIR" rev-parse HEAD 2>/dev/null || true)"
		printf -- '- Describe: `%s`\n' "$(git -C "$MODULE_DIR" describe --always --dirty --tags 2>/dev/null || true)"
		if [[ -x "$MODULE_DIR/scripts/compute_module_build_id.py" ]]; then
			printf -- '- Module build ID: `%s`\n' \
				"$("$MODULE_DIR/scripts/compute_module_build_id.py" --repo "$MODULE_DIR" 2>/dev/null || printf unavailable)"
		fi
		printf '\n### Working tree\n\n```text\n'
		git -C "$MODULE_DIR" status --short 2>/dev/null || true
		printf '```\n'
	else
		printf -- '- Repositório Git ainda não detectado.\n'
	fi
}

godot_git() {
	if git -C "$GODOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		printf -- '- Branch: `%s`\n' "$(git -C "$GODOT_DIR" branch --show-current 2>/dev/null || true)"
		printf -- '- HEAD: `%s`\n' "$(git -C "$GODOT_DIR" rev-parse HEAD 2>/dev/null || true)"
		printf -- '- Describe: `%s`\n' "$(git -C "$GODOT_DIR" describe --always --dirty --tags 2>/dev/null || true)"
		printf -- '- Dirty: `%s`\n' "$([[ -n "$(git -C "$GODOT_DIR" status --porcelain 2>/dev/null)" ]] && printf yes || printf no)"
	else
		printf -- '- Árvore Git do Godot não detectada em `%s`.\n' "$GODOT_DIR"
	fi
}

latest_summary=""
if [[ -L "$MODULE_DIR/build_reports/latest" && -f "$MODULE_DIR/build_reports/latest/summary.txt" ]]; then
	latest_summary="$MODULE_DIR/build_reports/latest/summary.txt"
fi

{
	printf '# TickSynchronizer — Contexto de trabalho\n\n'
	printf 'Gerado em UTC: `%s`\n\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
	printf '## Instruções obrigatórias\n\n'
	cat "$MODULE_DIR/AGENTS.md"
	printf '\n## Baseline declarada\n\n'
	printf -- '- Godot version: `%s`\n' "$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_VERSION")"
	printf -- '- Godot commit: `%s`\n' "$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_COMMIT")"
	printf '\n## Estado do módulo no Git\n\n'
	module_git
	printf '\n## Estado da engine local\n\n'
	godot_git
	printf '\n## Estado atual do projeto\n\n'
	cat "$MODULE_DIR/documentation/PROJECT_STATE.md"
	printf '\n## ADRs aceitos\n\n'
	for adr in "$MODULE_DIR"/documentation/adr/*.md; do
		[[ -f "$adr" ]] || continue
		printf -- '- `%s`: %s\n' "$(basename -- "$adr")" "$(sed -n '1s/^# //p' "$adr")"
	done
	printf '\n## Árvore relevante\n\n```text\n'
	find "$MODULE_DIR" -maxdepth 3 \
		\( -path "$MODULE_DIR/.git" -o -path "$MODULE_DIR/build" -o -path "$MODULE_DIR/build_reports" \) -prune \
		-o -print | sed "s#^$MODULE_DIR#.#" | sort
	printf '```\n'
	if [[ -n "$latest_summary" ]]; then
		printf '\n## Último resumo de validação\n\n```text\n'
		sed -n '1,160p' "$latest_summary"
		printf '```\n'
	fi
	printf '\n## Próxima leitura\n\n'
	printf -- '- `documentation/ARCHITECTURE.md`\n'
	printf -- '- `documentation/TESTING.md`\n'
	printf -- '- ADR relevante à tarefa\n'
	printf -- '- `documentation/ROADMAP.md`\n'
} >"$OUTPUT"

printf 'Contexto gerado: %s\n' "$OUTPUT"

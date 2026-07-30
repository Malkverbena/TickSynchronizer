#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
GODOT_DIR="${TICKSYNC_GODOT_DIR:-${MODULE_DIR}/../godot}"
ALLOW_DIRTY=0
ALLOW_MISMATCH=0

usage() {
	cat <<'USAGE'
Uso:
  ./scripts/verify_godot_baseline.sh [opções]

Opções:
  --godot-dir PATH       Árvore fonte do Godot. Padrão: ../godot
  --allow-dirty          Aceita alterações locais na engine
  --allow-mismatch       Aceita HEAD diferente de GODOT_COMMIT
  -h, --help             Mostra esta ajuda
USAGE
}

fail() {
	printf 'ERRO: %s\n' "$*" >&2
	exit 1
}

while (( $# > 0 )); do
	case "$1" in
		--godot-dir)
			[[ $# -ge 2 ]] || fail "--godot-dir exige um valor"
			GODOT_DIR="$2"
			shift 2
			;;
		--allow-dirty)
			ALLOW_DIRTY=1
			shift
			;;
		--allow-mismatch)
			ALLOW_MISMATCH=1
			shift
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

GODOT_DIR="$(realpath -m -- "$GODOT_DIR")"
[[ -f "$GODOT_DIR/SConstruct" ]] || fail "SConstruct não encontrado em $GODOT_DIR"
[[ -f "$MODULE_DIR/GODOT_COMMIT" ]] || fail "GODOT_COMMIT ausente"
[[ -f "$MODULE_DIR/GODOT_VERSION" ]] || fail "GODOT_VERSION ausente"
git -C "$GODOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail "A árvore do Godot não é um repositório Git"

EXPECTED_COMMIT="$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_COMMIT")"
EXPECTED_VERSION="$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_VERSION")"
HEAD_COMMIT="$(git -C "$GODOT_DIR" rev-parse HEAD)"
VERSION_COMMIT="$(git -C "$GODOT_DIR" rev-parse --verify "${EXPECTED_VERSION}^{commit}" 2>/dev/null || true)"
DIRTY_STATE="$(git -C "$GODOT_DIR" status --porcelain)"
BRANCH="$(git -C "$GODOT_DIR" branch --show-current)"
[[ -n "$BRANCH" ]] || BRANCH="(detached HEAD)"

[[ -n "$VERSION_COMMIT" ]] || fail "A referência $EXPECTED_VERSION não existe no repositório Godot local"
[[ "$VERSION_COMMIT" == "$EXPECTED_COMMIT" ]] || fail "GODOT_VERSION e GODOT_COMMIT apontam para commits diferentes"

if [[ "$HEAD_COMMIT" != "$EXPECTED_COMMIT" && "$ALLOW_MISMATCH" -ne 1 ]]; then
	fail "HEAD do Godot ($HEAD_COMMIT) difere da baseline ($EXPECTED_COMMIT)"
fi

if [[ -n "$DIRTY_STATE" && "$ALLOW_DIRTY" -ne 1 ]]; then
	printf '%s\n' "$DIRTY_STATE" >&2
	fail "A árvore do Godot possui alterações locais; a política do projeto proíbe patches na engine"
fi

printf 'TICKSYNCHRONIZER_GODOT_BASELINE_OK\n'
printf 'Godot version: %s\n' "$EXPECTED_VERSION"
printf 'Godot commit: %s\n' "$HEAD_COMMIT"
printf 'Godot branch: %s\n' "$BRANCH"
printf 'Godot working tree dirty: %s\n' "$([[ -n "$DIRTY_STATE" ]] && printf yes || printf no)"

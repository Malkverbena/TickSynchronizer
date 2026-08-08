#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
default_godot_dir() {
    local in_tree_root
    in_tree_root="$(realpath -m -- "${MODULE_DIR}/../..")"
    if [[ "$(basename -- "$(dirname -- "$MODULE_DIR")")" == "modules" && -f "$in_tree_root/SConstruct" ]]; then
        printf '%s\n' "$in_tree_root"
    else
        realpath -m -- "${MODULE_DIR}/../godot"
    fi
}

GODOT_DIR="${TICKSYNC_GODOT_DIR:-$(default_godot_dir)}"
ALLOW_DIRTY=0
ALLOW_MISMATCH=0

usage() {
	cat <<'USAGE'
Usage:
  ./scripts/verify_godot_baseline.sh [options]

Options:
  --godot-dir PATH       Godot source tree. Auto-detected for both layouts
  --allow-dirty          Accepts local engine changes
  --allow-mismatch       Accept a HEAD different from GODOT_COMMIT
  -h, --help             Show this help
USAGE
}

fail() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

while (( $# > 0 )); do
	case "$1" in
		--godot-dir)
			[[ $# -ge 2 ]] || fail "--godot-dir requires a value"
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
			fail "Unknown option: $1"
			;;
	esac
done

GODOT_DIR="$(realpath -m -- "$GODOT_DIR")"
[[ -f "$GODOT_DIR/SConstruct" ]] || fail "SConstruct not found in $GODOT_DIR"
[[ -f "$MODULE_DIR/GODOT_COMMIT" ]] || fail "GODOT_COMMIT missing"
[[ -f "$MODULE_DIR/GODOT_VERSION" ]] || fail "GODOT_VERSION missing"
git -C "$GODOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail "The Godot tree is not a Git repository"

EXPECTED_COMMIT="$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_COMMIT")"
EXPECTED_VERSION="$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_VERSION")"
HEAD_COMMIT="$(git -C "$GODOT_DIR" rev-parse HEAD)"
VERSION_COMMIT="$(git -C "$GODOT_DIR" rev-parse --verify "${EXPECTED_VERSION}^{commit}" 2>/dev/null || true)"
IN_TREE_MODULE="$(realpath -m -- "${GODOT_DIR}/modules/tick_synchronizer")"
if [[ "$MODULE_DIR" == "$IN_TREE_MODULE" ]]; then
    MODULE_LAYOUT="in-tree"
    DIRTY_STATE="$(git -C "$GODOT_DIR" status --porcelain --untracked-files=all -- . ':(exclude)modules/tick_synchronizer')"
else
    MODULE_LAYOUT="external"
    DIRTY_STATE="$(git -C "$GODOT_DIR" status --porcelain --untracked-files=all)"
fi
BRANCH="$(git -C "$GODOT_DIR" branch --show-current)"
[[ -n "$BRANCH" ]] || BRANCH="(detached HEAD)"

[[ -n "$VERSION_COMMIT" ]] || fail "The reference $EXPECTED_VERSION does not exist in the local Godot repository"
[[ "$VERSION_COMMIT" == "$EXPECTED_COMMIT" ]] || fail "GODOT_VERSION and GODOT_COMMIT refer to different commits"

if [[ "$HEAD_COMMIT" != "$EXPECTED_COMMIT" && "$ALLOW_MISMATCH" -ne 1 ]]; then
	fail "Godot HEAD ($HEAD_COMMIT) differs from baseline ($EXPECTED_COMMIT)"
fi

if [[ -n "$DIRTY_STATE" && "$ALLOW_DIRTY" -ne 1 ]]; then
	printf '%s\n' "$DIRTY_STATE" >&2
	fail "The Godot tree has local changes; project policy forbids engine patches"
fi

printf 'TICKSYNCHRONIZER_GODOT_BASELINE_OK\n'
printf 'Godot version: %s\n' "$EXPECTED_VERSION"
printf 'Godot commit: %s\n' "$HEAD_COMMIT"
printf 'Godot branch: %s\n' "$BRANCH"
printf 'Module layout: %s\n' "$MODULE_LAYOUT"
printf 'Godot working tree dirty: %s\n' "$([[ -n "$DIRTY_STATE" ]] && printf yes || printf no)"

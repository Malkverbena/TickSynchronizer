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

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/verify_godot_baseline.sh [options]

This compatibility verifier accepts clean, unmodified Godot 4.x source trees
at version 4.4.0 or newer. GODOT_VERSION and GODOT_COMMIT identify the qualified
validation baseline; they do not restrict compilation to one commit.

Options:
  --godot-dir PATH       Godot source tree. Auto-detected for both layouts
  --allow-dirty          Accepts local engine changes for diagnostics only
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
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1"
            ;;
    esac
done

GODOT_DIR="$(realpath -m -- "$GODOT_DIR")"
[[ -f "$GODOT_DIR/SConstruct" ]] || fail "SConstruct not found in $GODOT_DIR"
[[ -f "$GODOT_DIR/version.py" ]] || fail "version.py not found in $GODOT_DIR"
[[ -f "$MODULE_DIR/GODOT_MINIMUM_VERSION" ]] || fail "GODOT_MINIMUM_VERSION missing"
[[ -f "$MODULE_DIR/GODOT_COMMIT" ]] || fail "GODOT_COMMIT missing"
[[ -f "$MODULE_DIR/GODOT_VERSION" ]] || fail "GODOT_VERSION missing"
[[ -x "$SCRIPT_DIR/verify_godot_version.py" ]] || fail "verify_godot_version.py missing or not executable"
git -C "$GODOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail "the Godot tree is not a Git repository"

QUALIFIED_COMMIT="$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_COMMIT")"
QUALIFIED_VERSION="$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_VERSION")"
HEAD_COMMIT="$(git -C "$GODOT_DIR" rev-parse HEAD)"
VERSION_OUTPUT="$(
    "$SCRIPT_DIR/verify_godot_version.py" \
        --godot-dir "$GODOT_DIR" \
        --minimum-file "$MODULE_DIR/GODOT_MINIMUM_VERSION"
)" || fail "Godot version is outside the supported range"
LIVE_VERSION="$(sed -n 's/^godot_version=//p' <<<"$VERSION_OUTPUT")"
MINIMUM_VERSION="$(sed -n 's/^minimum_godot_version=//p' <<<"$VERSION_OUTPUT")"
[[ -n "$LIVE_VERSION" && -n "$MINIMUM_VERSION" ]] || fail "incomplete Godot version verification output"

IN_TREE_MODULE="$(realpath -m -- "${GODOT_DIR}/modules/tick_synchronizer")"
if [[ "$MODULE_DIR" == "$IN_TREE_MODULE" ]]; then
    MODULE_LAYOUT="in-tree"
    DIRTY_STATE="$(git -C "$GODOT_DIR" status --porcelain --untracked-files=all -- . ':(exclude)modules/tick_synchronizer')"
else
    MODULE_LAYOUT="external"
    DIRTY_STATE="$(git -C "$GODOT_DIR" status --porcelain --untracked-files=all)"
fi

if [[ -n "$DIRTY_STATE" && "$ALLOW_DIRTY" -ne 1 ]]; then
    printf '%s\n' "$DIRTY_STATE" >&2
    fail "the Godot tree has local changes; project policy forbids engine patches"
fi

BRANCH="$(git -C "$GODOT_DIR" branch --show-current)"
[[ -n "$BRANCH" ]] || BRANCH="(detached HEAD)"
QUALIFIED_MATCH="no"
if [[ "$HEAD_COMMIT" == "$QUALIFIED_COMMIT" && "$LIVE_VERSION" == "$QUALIFIED_VERSION" ]]; then
    QUALIFIED_MATCH="yes"
fi

printf 'TICKSYNCHRONIZER_GODOT_COMPATIBILITY_OK\n'
printf 'Godot version: %s\n' "$LIVE_VERSION"
printf 'Minimum Godot version: %s\n' "$MINIMUM_VERSION"
printf 'Supported Godot series: 4.x\n'
printf 'Godot commit: %s\n' "$HEAD_COMMIT"
printf 'Qualified baseline match: %s\n' "$QUALIFIED_MATCH"
printf 'Qualified baseline version: %s\n' "$QUALIFIED_VERSION"
printf 'Qualified baseline commit: %s\n' "$QUALIFIED_COMMIT"
printf 'Godot branch: %s\n' "$BRANCH"
printf 'Module layout: %s\n' "$MODULE_LAYOUT"
printf 'Godot working tree dirty: %s\n' "$([[ -n "$DIRTY_STATE" ]] && printf yes || printf no)"

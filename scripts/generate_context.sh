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
OUTPUT="${MODULE_DIR}/build/context/TICKSYNCHRONIZER_CONTEXT.md"

usage() {
	cat <<'USAGE'
Usage:
  ./scripts/generate_context.sh [--output FILE] [--godot-dir PATH]

Generates a compact summary for a new work session without including
complete source code or extensive logs.
USAGE
}

fail() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

while (( $# > 0 )); do
	case "$1" in
		--output)
			[[ $# -ge 2 ]] || fail "--output requires a value"
			OUTPUT="$2"
			shift 2
			;;
		--godot-dir)
			[[ $# -ge 2 ]] || fail "--godot-dir requires a value"
			GODOT_DIR="$2"
			shift 2
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
		printf -- '- Module Git repository not detected.\n'
	fi
}

godot_git() {
	if git -C "$GODOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		printf -- '- Branch: `%s`\n' "$(git -C "$GODOT_DIR" branch --show-current 2>/dev/null || true)"
		printf -- '- HEAD: `%s`\n' "$(git -C "$GODOT_DIR" rev-parse HEAD 2>/dev/null || true)"
		printf -- '- Describe: `%s`\n' "$(git -C "$GODOT_DIR" describe --always --dirty --tags 2>/dev/null || true)"
		printf -- '- Dirty: `%s`\n' "$([[ -n "$(git -C "$GODOT_DIR" status --porcelain 2>/dev/null)" ]] && printf yes || printf no)"
	else
		printf -- '- Godot Git tree not detected at `%s`.\n' "$GODOT_DIR"
	fi
}

relevant_source_files() {
	if git -C "$MODULE_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		git -C "$MODULE_DIR" ls-files --cached --others --exclude-standard \
			| sed 's#^#./#' \
			| sort
		return
	fi

	find "$MODULE_DIR" -type d \
		\( -name .git -o -name build -o -name build_reports \
			-o -name benchmark_dist -o -name benchmark_reports \
			-o -name bin -o -name .build -o -name __pycache__ \) -prune \
		-o -type f -print \
		| sed "s#^$MODULE_DIR#.#" \
		| sort
}

latest_summary=""
if [[ -L "$MODULE_DIR/build_reports/latest" && -f "$MODULE_DIR/build_reports/latest/summary.txt" ]]; then
	latest_summary="$MODULE_DIR/build_reports/latest/summary.txt"
fi

{
	printf '# TickSynchronizer work context\n\n'
	printf 'Generated at UTC: `%s`\n\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
	printf '## Mandatory instructions\n\n'
	cat "$MODULE_DIR/AGENTS.md"
	printf '\n## Declared baseline\n\n'
	printf -- '- Godot version: `%s`\n' "$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_VERSION")"
	printf -- '- Godot commit: `%s`\n' "$(tr -d '[:space:]' <"$MODULE_DIR/GODOT_COMMIT")"
	printf '\n## Module Git state\n\n'
	module_git
	printf '\n## Local engine state\n\n'
	godot_git
	printf '\n## Current project state\n\n'
	cat "$MODULE_DIR/documentation/PROJECT_STATE.md"
	printf '\n## Accepted ADRs\n\n'
	for adr in "$MODULE_DIR"/documentation/adr/*.md; do
		[[ -f "$adr" ]] || continue
		printf -- '- `%s`: %s\n' "$(basename -- "$adr")" "$(sed -n '1s/^# //p' "$adr")"
	done
	printf '\n## Relevant source files\n\n```text\n'
	relevant_source_files
	printf '```\n'
	if [[ -n "$latest_summary" ]]; then
		printf '\n## Most recent validation summary\n\n'
		printf 'This is the most recent individual run, not an aggregate of the complete matrix.\n\n```text\n'
		sed -n '1,160p' "$latest_summary"
		printf '```\n'
	fi
	printf '\n## Next reading\n\n'
	printf -- '- `documentation/ARCHITECTURE.md`\n'
	printf -- '- `documentation/TESTING.md`\n'
	printf -- '- ADR relevant to the task\n'
	printf -- '- `documentation/ROADMAP.md`\n'
} >"$OUTPUT"

printf 'Context generated: %s\n' "$OUTPUT"

#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ALLOWED_DECLARATIONS = (
    "flowchart ",
    "graph ",
    "sequenceDiagram",
    "stateDiagram-v2",
    "classDiagram",
)
GENERATED_PREFIXES = (
    ".git/",
    ".godot/",
    "build/",
    "build_reports/",
    "benchmark_reports/",
    "benchmarks/.build/",
    "benchmarks/bin/",
    "benchmark_dist/",
)


def find_markdown_files(root: pathlib.Path) -> list[pathlib.Path]:
    return sorted(
        path
        for path in root.rglob("*.md")
        if not path.relative_to(root).as_posix().startswith(GENERATED_PREFIXES)
    )


def extract_mermaid_blocks(path: pathlib.Path) -> tuple[list[str], list[str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    blocks: list[str] = []
    errors: list[str] = []
    index = 0

    while index < len(lines):
        match = re.match(r"^\s*(`{3,})mermaid\s*$", lines[index])
        if not match:
            index += 1
            continue

        fence = match.group(1)
        start_line = index + 1
        index += 1
        content: list[str] = []
        while index < len(lines) and not re.match(
            rf"^\s*{re.escape(fence)}\s*$", lines[index]
        ):
            content.append(lines[index])
            index += 1

        if index >= len(lines):
            errors.append(f"{path}:{start_line}: unclosed Mermaid block")
            break

        block = "\n".join(content).strip()
        if not block:
            errors.append(f"{path}:{start_line}: empty Mermaid block")
        else:
            first_line = next(
                (line.strip() for line in content if line.strip() and not line.lstrip().startswith("%%")),
                "",
            )
            if not first_line.startswith(ALLOWED_DECLARATIONS):
                errors.append(
                    f"{path}:{start_line}: unapproved or missing Mermaid type: {first_line!r}"
                )
            blocks.append(block)
        index += 1

    return blocks, errors


def resolve_host_temp_root(root: pathlib.Path) -> pathlib.Path:
    configured = os.environ.get("TICKSYNC_TEMP_DIR")
    temp_root = pathlib.Path(configured).expanduser() if configured else root.parent / "tick_synchronizer_tmp"
    temp_root = temp_root.resolve()
    system_tmp = pathlib.Path("/tmp")
    if temp_root == pathlib.Path("/") or temp_root == system_tmp or system_tmp in temp_root.parents:
        raise ValueError(
            "host temporary files must use ../tick_synchronizer_tmp or a safe "
            "TICKSYNC_TEMP_DIR override"
        )
    temp_root.mkdir(parents=True, exist_ok=True)
    return temp_root


def render_with_mmdc(
    blocks: list[tuple[pathlib.Path, int, str]],
    temp_root: pathlib.Path,
) -> list[str]:
    mmdc = shutil.which("mmdc")
    if not mmdc:
        return []

    errors: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ticksync-mermaid-", dir=temp_root) as temp_dir:
        temp = pathlib.Path(temp_dir)
        for path, ordinal, block in blocks:
            source = temp / f"diagram-{ordinal}.mmd"
            target = temp / f"diagram-{ordinal}.svg"
            source.write_text(block + "\n", encoding="utf-8")
            result = subprocess.run(
                [mmdc, "--input", str(source), "--output", str(target), "--quiet"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            if result.returncode != 0:
                detail = result.stderr.strip() or result.stdout.strip() or "unknown error"
                errors.append(f"{path}: diagram {ordinal} failed in mmdc: {detail}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Validates Mermaid blocks in project documentation.")
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--min-diagrams", type=int, default=10)
    parser.add_argument(
        "--render-if-available",
        action="store_true",
        help="Renders with mmdc when Mermaid CLI is installed.",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    all_blocks: list[tuple[pathlib.Path, int, str]] = []
    errors: list[str] = []
    files_with_diagrams = 0

    for path in find_markdown_files(root):
        blocks, file_errors = extract_mermaid_blocks(path)
        errors.extend(file_errors)
        if blocks:
            files_with_diagrams += 1
            for block in blocks:
                all_blocks.append((path.relative_to(root), len(all_blocks) + 1, block))

    if len(all_blocks) < args.min_diagrams:
        errors.append(
            f"only {len(all_blocks)} Mermaid diagrams; minimum expected: {args.min_diagrams}"
        )

    if args.render_if_available:
        try:
            temp_root = resolve_host_temp_root(root)
        except ValueError as error:
            errors.append(str(error))
        else:
            errors.extend(render_with_mmdc(all_blocks, temp_root))

    if errors:
        print("TickSynchronizer Mermaid validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    renderer = "mmdc" if args.render_if_available and shutil.which("mmdc") else "static"
    print(
        "TICKSYNCHRONIZER_MERMAID_OK "
        f"diagrams={len(all_blocks)} files={files_with_diagrams} renderer={renderer}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3

from __future__ import annotations

import argparse
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
EXCLUDED_PARTS = {".git", "build", "build_reports", ".godot"}


def find_markdown_files(root: pathlib.Path) -> list[pathlib.Path]:
    return sorted(
        path
        for path in root.rglob("*.md")
        if not any(part in EXCLUDED_PARTS for part in path.parts)
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
            errors.append(f"{path}:{start_line}: bloco Mermaid sem fechamento")
            break

        block = "\n".join(content).strip()
        if not block:
            errors.append(f"{path}:{start_line}: bloco Mermaid vazio")
        else:
            first_line = next(
                (line.strip() for line in content if line.strip() and not line.lstrip().startswith("%%")),
                "",
            )
            if not first_line.startswith(ALLOWED_DECLARATIONS):
                errors.append(
                    f"{path}:{start_line}: tipo Mermaid não aprovado ou ausente: {first_line!r}"
                )
            blocks.append(block)
        index += 1

    return blocks, errors


def render_with_mmdc(blocks: list[tuple[pathlib.Path, int, str]]) -> list[str]:
    mmdc = shutil.which("mmdc")
    if not mmdc:
        return []

    errors: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ticksync-mermaid-") as temp_dir:
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
                detail = result.stderr.strip() or result.stdout.strip() or "erro desconhecido"
                errors.append(f"{path}: diagrama {ordinal} falhou no mmdc: {detail}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Valida blocos Mermaid da documentação.")
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--min-diagrams", type=int, default=10)
    parser.add_argument(
        "--render-if-available",
        action="store_true",
        help="Renderiza com mmdc quando o Mermaid CLI estiver instalado.",
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
            f"somente {len(all_blocks)} diagramas Mermaid; mínimo esperado: {args.min_diagrams}"
        )

    if args.render_if_available:
        errors.extend(render_with_mmdc(all_blocks))

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

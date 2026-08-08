#!/usr/bin/env python3
"""Compute the deterministic 20-byte TickSynchronizer module build ID.

Clean repositories use the exact Git commit SHA-1. Dirty repositories use the
first 20 bytes of a domain-separated SHA-256 over HEAD, the binary diff and all
non-ignored untracked files in sorted path order.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys


def run_git(repo: pathlib.Path, *args: str) -> bytes:
    completed = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return completed.stdout


def compute(repo: pathlib.Path) -> tuple[str, bool, str]:
    repo = repo.resolve()
    run_git(repo, "rev-parse", "--is-inside-work-tree")
    head = run_git(repo, "rev-parse", "HEAD").decode("ascii").strip().lower()
    if len(head) != 40 or any(char not in "0123456789abcdef" for char in head):
        raise RuntimeError("Git HEAD is not a 40-character hexadecimal commit")

    status = run_git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all")
    dirty = bool(status)
    if not dirty:
        return head, False, head

    digest = hashlib.sha256()
    digest.update(b"TickSynchronizer module build id v1\0")
    digest.update(bytes.fromhex(head))
    digest.update(run_git(repo, "diff", "--binary", "HEAD", "--", "."))

    entries = status.split(b"\0")
    untracked: list[pathlib.Path] = []
    for entry in entries:
        if not entry or not entry.startswith(b"?? "):
            continue
        relative = pathlib.Path(entry[3:].decode("utf-8", errors="surrogateescape"))
        path = repo / relative
        if path.is_file():
            untracked.append(relative)

    for relative in sorted(untracked, key=lambda item: item.as_posix()):
        encoded_path = relative.as_posix().encode("utf-8", errors="surrogateescape")
        content = (repo / relative).read_bytes()
        digest.update(len(encoded_path).to_bytes(8, "little"))
        digest.update(encoded_path)
        digest.update(len(content).to_bytes(8, "little"))
        digest.update(content)

    return digest.digest()[:20].hex(), True, head


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--format", choices=("hex", "json"), default="hex")
    args = parser.parse_args()

    try:
        build_id, dirty, head = compute(args.repo)
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"ERROR: could not calculate module_build_id: {error}", file=sys.stderr)
        return 1

    if args.format == "json":
        print(json.dumps({"module_build_id": build_id, "dirty": dirty, "head": head}, sort_keys=True))
    else:
        print(build_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

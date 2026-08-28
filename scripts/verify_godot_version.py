#!/usr/bin/env python3
"""Validate a Godot source version against the supported engine series."""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True


class VersionPolicyError(ValueError):
    """Raised when the Godot version source or support policy is invalid."""


@dataclass(frozen=True, order=True)
class NumericVersion:
    major: int
    minor: int
    patch: int

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"


@dataclass(frozen=True)
class GodotVersion:
    numeric: NumericVersion
    status: str

    def canonical(self) -> str:
        suffix = f"-{self.status}" if self.status else ""
        return f"{self.numeric}{suffix}"


def parse_version_source(source: str, source_name: str) -> GodotVersion:
    try:
        tree = ast.parse(source, filename=source_name)
    except SyntaxError as error:
        raise VersionPolicyError(f"invalid Python syntax in {source_name}: {error.msg}") from error

    values: dict[str, object] = {}
    required = {"major", "minor", "patch", "status"}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name) or target.id not in required:
            continue
        if target.id in values:
            raise VersionPolicyError(f"duplicate {target.id} assignment in {source_name}")
        try:
            values[target.id] = ast.literal_eval(node.value)
        except (ValueError, TypeError) as error:
            raise VersionPolicyError(
                f"{target.id} must be a literal in {source_name}"
            ) from error

    missing = sorted(required - values.keys())
    if missing:
        raise VersionPolicyError(
            f"missing version fields in {source_name}: {', '.join(missing)}"
        )

    for field in ("major", "minor", "patch"):
        value = values[field]
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise VersionPolicyError(f"{field} must be a nonnegative integer in {source_name}")

    status = values["status"]
    if not isinstance(status, str) or not re.fullmatch(r"[a-z0-9.]*", status):
        raise VersionPolicyError(
            f"status must be a lowercase canonical token in {source_name}"
        )

    return GodotVersion(
        NumericVersion(
            int(values["major"]),
            int(values["minor"]),
            int(values["patch"]),
        ),
        status,
    )


def parse_minimum(text: str, source_name: str) -> NumericVersion:
    value = text.strip()
    match = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)", value)
    if match is None:
        raise VersionPolicyError(
            f"minimum version must use major.minor.patch in {source_name}"
        )
    return NumericVersion(*(int(component) for component in match.groups()))


def enforce_policy(version: GodotVersion, minimum: NumericVersion) -> None:
    if minimum.major != 4:
        raise VersionPolicyError("the supported Godot series must remain 4.x")
    if version.numeric.major != minimum.major:
        raise VersionPolicyError(
            f"unsupported Godot major version {version.numeric.major}; supported series is 4.x"
        )
    if version.numeric < minimum:
        raise VersionPolicyError(
            f"unsupported Godot version {version.canonical()}; minimum is {minimum}"
        )


def run_self_test() -> None:
    minimum = parse_minimum("4.4.0\n", "minimum")
    passing = (
        ('major = 4\nminor = 4\npatch = 0\nstatus = "stable"\n', "4.4.0-stable"),
        ('major = 4\nminor = 8\npatch = 0\nstatus = "dev"\n', "4.8.0-dev"),
        ('major = 4\nminor = 10\npatch = 2\nstatus = ""\n', "4.10.2"),
    )
    for source, expected in passing:
        version = parse_version_source(source, "self-test")
        enforce_policy(version, minimum)
        if version.canonical() != expected:
            raise VersionPolicyError(f"self-test canonical mismatch: {version.canonical()}")

    failing = (
        'major = 4\nminor = 3\npatch = 9\nstatus = "stable"\n',
        'major = 5\nminor = 0\npatch = 0\nstatus = "dev"\n',
        'major = 4\nminor = 4\npatch = build_patch()\nstatus = "stable"\n',
        'major = 4\nminor = 4\nstatus = "stable"\n',
    )
    for source in failing:
        try:
            version = parse_version_source(source, "self-test")
            enforce_policy(version, minimum)
        except VersionPolicyError:
            continue
        raise VersionPolicyError("self-test accepted an invalid or unsupported version")

    print("TICKSYNCHRONIZER_GODOT_VERSION_POLICY_SELF_TEST_OK")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate a Godot version.py against TickSynchronizer support policy."
    )
    parser.add_argument("--godot-dir", type=Path)
    parser.add_argument("--minimum-file", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    try:
        if args.self_test:
            if args.godot_dir is not None or args.minimum_file is not None:
                raise VersionPolicyError("--self-test does not accept path arguments")
            run_self_test()
            return 0

        if args.godot_dir is None or args.minimum_file is None:
            raise VersionPolicyError("--godot-dir and --minimum-file are required")

        version_path = args.godot_dir.resolve() / "version.py"
        minimum_path = args.minimum_file.resolve()
        version = parse_version_source(
            version_path.read_text(encoding="utf-8"), str(version_path)
        )
        minimum = parse_minimum(
            minimum_path.read_text(encoding="utf-8"), str(minimum_path)
        )
        enforce_policy(version, minimum)
    except (OSError, UnicodeError, VersionPolicyError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"godot_version={version.canonical()}")
    print(f"godot_version_numeric={version.numeric}")
    print(f"minimum_godot_version={minimum}")
    print("supported_godot_series=4.x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

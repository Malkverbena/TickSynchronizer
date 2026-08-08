# ADR 0014 — Clang and LLD sanitizer toolchain

## Status

Accepted

## Context

Godot 4.7.1 sanitizer builds are more predictable with a controlled Clang/LLD toolchain than with implicit host defaults.

## Decision

Use Clang and LLD in the sanitizer wrapper where supported, while keeping normal SCons builds compiler-configurable.

## Consequences

- Sanitizer reproduction improves.
- Tool versions are recorded.
- Changing the sanitizer toolchain requires revalidation.

# ADR 0013 — Shared C++ runtime for sanitizer builds

## Status

Accepted

## Context

Sanitizer runtimes must be linked consistently across the large Godot executable and module test code.

## Decision

Use the supported shared C++ runtime arrangement selected by the sanitizer wrapper and validate runtime dependencies before execution.

## Consequences

- Runtime loading is explicit and reportable.
- Toolchain mismatches fail before tests.
- Release builds are unaffected.

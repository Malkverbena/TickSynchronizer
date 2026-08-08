# ADR 0019 — Disable invalid-pointer-pair checking in the accepted ASAN profile

## Status

Accepted

## Context

Godot 4.7.1 engine code triggers invalid-pointer-pair diagnostics unrelated to TickSynchronizer in the acceptance environment.

## Decision

Disable that specific engine-level check by default in the accepted ASAN profile while preserving an explicit diagnostic option to re-enable it.

## Consequences

- The module ASAN gate remains usable.
- The exception is narrow and version-specific.
- Any TickSynchronizer memory finding remains blocking.

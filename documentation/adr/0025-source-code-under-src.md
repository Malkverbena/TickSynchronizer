# ADR 0025 — Project-owned C++ code under src

## Status

Accepted

## Context

Keeping public, protocol, and internal implementation directories at the repository root mixed runtime code with scripts, tests, and documentation.

## Decision

Place project-owned implementation under `src/public`, `src/protocol`, and `src/internal`. Keep conventional Godot module glue at the root.

## Consequences

- The root is reserved for integration and project metadata.
- Cross-directory includes use explicit `src/` paths.
- The layout works externally and under `godot/modules/tick_synchronizer`.

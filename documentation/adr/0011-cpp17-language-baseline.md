# ADR 0011 — C++17 language baseline

## Status

Accepted; restricted further by ADR 0033 for Godot-linked source and standalone
benchmark compiler features.

## Context

The module must compile with the language level supported by the Godot 4.7.1 baseline across target platforms.

## Decision

Require C++17 and reject older compiler modes at compile time. Do not require C++20 features in runtime or benchmark-shared contracts.

## Consequences

- Linux, Windows, Android, and macOS toolchains share one baseline.
- Contributors must use C++17 alternatives when newer features are unnecessary.
- C++17 availability does not permit features excluded by the Godot policy
  boundary recorded in ADR 0033.
- A future language update requires an ADR and full platform validation.

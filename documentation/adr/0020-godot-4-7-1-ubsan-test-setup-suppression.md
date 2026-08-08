# ADR 0020 — Strict Godot 4.7.1 UBSAN test-setup suppression

## Status

Accepted

## Context

A known UBSAN diagnostic in Godot test setup can prevent module tests from running even though it is external to the module.

## Decision

Apply only the documented file-based suppression for the exact Godot 4.7.1 setup path, with an option to disable suppressions for diagnosis.

## Consequences

- The suppression is auditable and narrow.
- Unexpected diagnostics still fail.
- The suppression must be reevaluated on engine updates.

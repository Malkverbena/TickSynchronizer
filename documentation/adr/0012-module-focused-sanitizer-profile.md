# ADR 0012 — Module-focused sanitizer profile

## Status

Accepted

## Context

A full Godot sanitizer build can surface unrelated engine and third-party diagnostics before module code is exercised.

## Decision

Provide a sanitizer wrapper that builds and runs the TickSynchronizer tests with controlled flags, artifacts, timeouts, and reports.

## Consequences

- Module findings remain actionable.
- Engine-level diagnostics are documented separately.
- The profile does not waive normal build or smoke requirements.

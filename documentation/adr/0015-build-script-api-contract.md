# ADR 0015 — Build-script API contract

## Status

Accepted

## Context

Wrapper scripts depend on stable options and output from the main build script; silent drift causes confusing failures.

## Decision

Version the script interface independently. Wrappers query `--print-script-api-version` and reject an unexpected value.

## Consequences

- Script compatibility failures are immediate and actionable.
- The public module API remains separate from the script API.
- Interface changes require coordinated wrapper updates.

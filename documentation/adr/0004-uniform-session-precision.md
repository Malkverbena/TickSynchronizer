# ADR 0004 — Uniform build precision per session

## Status

Accepted

## Context

Godot may use single or double precision, and mixed peers can diverge or interpret values inconsistently.

## Decision

Support both precision builds, default to `double`, and reject mixed-precision sessions with `PRECISION_MISMATCH`. Never serialize `real_t` directly.

## Consequences

- Precision is part of session compatibility.
- Tests cover both precision modes and mismatch rejection.
- Schemas choose explicit wire widths per field.

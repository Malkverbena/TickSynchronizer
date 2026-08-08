# ADR 0007 — Sticky errors and atomic bitstream operations

## Status

Accepted

## Context

Silent or partial failures can shift every following field and make corrupted packets difficult to diagnose.

## Decision

Preserve the first buffer error until explicit reinitialization. Any operation that fails due to mode, width, resource limit, or end of input leaves cursors and outputs unchanged.

## Consequences

- Callers can check one error after a sequence.
- Later operations cannot hide the first failure.
- Tests verify stable cursors and zeroed outputs after invalid reads.

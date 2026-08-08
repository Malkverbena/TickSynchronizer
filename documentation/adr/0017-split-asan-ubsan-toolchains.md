# ADR 0017 — Separate ASAN and UBSAN passes

## Status

Accepted

## Context

Combining sanitizers can obscure diagnostics, increase memory use, and create toolchain-specific interactions.

## Decision

Run ASAN and UBSAN as separate accepted passes. Keep combined mode diagnostic only.

## Consequences

- Failures are easier to classify.
- Peak resource use is lower.
- Both passes remain required at security-sensitive milestones.

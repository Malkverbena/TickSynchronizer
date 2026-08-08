# ADR 0022 — Fixed control packet envelope

## Status

Accepted

## Context

The first decoder must be easy to audit, validate lengths before allocation, and remain independent of transport, ABI, and `real_t`.

## Decision

Use a fixed 40-byte little-endian control header with magic, envelope version, type, sizes, session, sequence, tick, and zero-required reserved fields. Keep the 1.1 envelope distinct from the experimental wire version.

## Consequences

- Parsing is predictable and atomic.
- Length and padding validation precede payload copies.
- The overhead is accepted for rare control traffic only.
- A compact realtime header remains a benchmark-driven future decision.

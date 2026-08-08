# ADR 0028 — Deterministic protocol benchmark suite

## Status

Accepted

## Context

Protocol choices require comparable evidence for size, CPU, allocation, correctness, and portability rather than intuition.

## Decision

Create standalone benchmark suite version 1 with deterministic semantic datasets, the `reference_fixed_width` candidate, correctness gates, independent encode/decode timing, robust statistics, and provenance-rich reports.

## Consequences

- The reference candidate is a baseline, not the selected wire protocol.
- Official reports require clean source and CPU affinity.
- Candidates share identical data and semantics.
- Windows and Android ports must preserve methodology before candidate selection.

## Cross-platform implementation

ADR 0029 defines the Linux, Windows, and Android build/run backends and report schema 3 without changing suite methodology.

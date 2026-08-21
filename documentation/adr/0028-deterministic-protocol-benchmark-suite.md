# ADR 0028 — Deterministic protocol benchmark suite

## Status

Accepted; suite 1 is retained as historical infrastructure evidence and suite 2
is defined by ADR 0037.

## Context

Protocol choices require comparable evidence for size, CPU, allocation, correctness, and portability rather than intuition.

## Decision

Create standalone benchmark suite version 1 with deterministic semantic datasets, the `reference_fixed_width` candidate, correctness gates, independent encode/decode timing, robust statistics, and provenance-rich reports.

## Consequences

- The reference candidate is a baseline, not the selected wire protocol.
- Official reports require clean source and the exact platform CPU execution
  policy from ADRs 0029 and 0032.
- Candidates share identical data and semantics.
- Windows, Android, and macOS ports must preserve methodology before candidate selection.

## Cross-platform implementation

ADR 0029 defines the Linux, Windows, and Android build/run backends and report
schema 3 without changing suite methodology. ADR 0032 extends that graph to
macOS Universal 2 and defines its scheduler-managed execution policy.

ADR 0037 advances the corpus and correctness methodology to suite 2. Reports
from the two suite versions are not numerically comparable.

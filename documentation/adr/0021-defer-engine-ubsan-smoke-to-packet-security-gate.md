# ADR 0021 — Defer full engine UBSAN smoke to the packet-security gate

## Status

Accepted

## Context

Module tests pass under UBSAN, but the editor smoke stops in bundled SDL/HIDAPI initialization outside TickSynchronizer.

## Decision

Accept normal smoke tests plus module-focused ASAN/UBSAN as the current gate. Reevaluate or isolate the full engine UBSAN smoke before external untrusted packet delivery.

## Consequences

- Functional work is not blocked by unrelated third-party startup code.
- The risk is bounded by an explicit security gate.
- Broad suppressions remain forbidden.
- The issue reopens on packet decoder exposure, endpoint integration, engine update, or an in-module finding.

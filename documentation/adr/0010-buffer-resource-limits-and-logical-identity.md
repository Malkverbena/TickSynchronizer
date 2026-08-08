# ADR 0010 — Buffer resource limits and logical identity

## Status

Accepted

## Context

The buffer will process untrusted data; unlimited growth and cursor-dependent identity would create memory and determinism risks.

## Decision

Give each buffer a configurable byte limit, canonicalize read storage, and define equality/hash only from canonical bytes and logical bit size.

## Consequences

- Limit failures are atomic.
- The 1 MiB default is a ceiling, not an approved packet size.
- Higher layers need smaller per-message and aggregate budgets.
- A zero-copy path requires benchmark evidence and equivalent safety.

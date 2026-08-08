# ADR 0006 — Commits represent complete logical changes

## Status

Accepted

## Context

Microcommits for every rename or include create noisy history and reduce review and bisect quality.

## Decision

Create a commit only when a coherent change compiles, passes relevant tests, and includes matching documentation.

## Consequences

- Intermediate work remains in the working tree, a stash, or a local patch.
- Commits may be larger than microcommits but remain focused.
- Unrelated refactors are not mixed into a feature.

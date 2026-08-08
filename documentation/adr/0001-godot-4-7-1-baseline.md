# ADR 0001 — Godot 4.7.1-stable baseline

## Status

Accepted

## Context

Developing against an unstable branch makes it difficult to distinguish engine regressions from module regressions.

## Decision

Use only Godot `4.7.1-stable` at commit `a13da4feb8d8aefc283c3763d33a2f170a18d541`. Keep the engine source unmodified.

## Consequences

- Builds and bugs are reproducible.
- APIs introduced after 4.7.1 require a formal baseline update.
- Validation rejects a different commit or dirty engine tree by default.

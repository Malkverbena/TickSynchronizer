# ADR 0001 — Godot 4.7.1-stable baseline

## Status

Superseded by ADR 0040.

## Context

Developing against an unstable branch makes it difficult to distinguish engine regressions from module regressions.

## Decision

Use only Godot `4.7.1-stable` at commit `a13da4feb8d8aefc283c3763d33a2f170a18d541`. Keep the engine source unmodified.

ADR 0040 later retains this exact source as the qualified validation baseline
while allowing compilation on clean Godot 4.x source at version 4.4.0 or newer.

## Consequences

- Builds and bugs are reproducible.
- APIs introduced after 4.7.1 require a formal baseline update.
- Historical validation remains tied to the exact commit.
- Current build policy rejects an unsupported version or dirty engine tree by
  default, rather than rejecting every different supported commit.

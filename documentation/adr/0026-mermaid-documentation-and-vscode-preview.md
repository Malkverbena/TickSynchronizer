# ADR 0026 — Embedded architecture diagrams as validated text

## Status

Accepted

## Context

Architecture and protocol relationships are easier to review with diagrams, but general diagram-tool tutorials are outside the module scope.

## Decision

Keep project-specific Mermaid diagrams embedded in relevant Markdown documents. Validate a small stable subset statically. Do not maintain a standalone Mermaid usage manual or require Mermaid CLI for module builds.

## Consequences

- Diagrams remain reviewable text.
- Normative prose remains authoritative.
- The repository contains no general Mermaid tutorial.
- Unsupported diagram types require an explicit validator update.

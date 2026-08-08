# ADR 0018 — Runtime validation of sanitized artifacts

## Status

Accepted

## Context

A successful link does not guarantee that the sanitizer executable has valid runtime dependencies or is the intended artifact.

## Decision

Validate file identity, precision, sanitizer suffixes, ELF metadata, dependencies, and test execution before accepting a sanitized build.

## Consequences

- Stale or wrong artifacts are rejected.
- Missing runtimes are diagnosed before tests.
- Artifact metadata is retained in reports.

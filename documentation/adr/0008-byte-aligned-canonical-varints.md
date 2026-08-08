# ADR 0008 — Byte-aligned canonical varints

## Status

Accepted

## Context

Unaligned or redundant varint encodings complicate inspection, fuzzing, hashing, and interoperability.

## Decision

Varuint and ZigZag varint values start on byte boundaries, use explicit zero padding, accept only minimal encodings, and are limited to 10 bytes for 64-bit values.

## Consequences

- Each integer has exactly one valid wire encoding.
- Packet inspection and golden vectors are simpler.
- Explicit alignment trades a few potential bits for robustness.
- Truncation, overflow, excess continuation, and redundant forms fail atomically.

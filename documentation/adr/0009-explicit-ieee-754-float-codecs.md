# ADR 0009 — Explicit IEEE 754 codecs independent of real_t

## Status

Accepted

## Context

`real_t` width changes with the Godot precision build, and floating-point values include special bit patterns that require defined handling.

## Decision

Provide separate `float32` and `float64` little-endian codecs. Preserve low-level IEEE 754 bits, never serialize `real_t`, and require higher-level schemas to reject or canonicalize non-finite semantic values.

## Consequences

- Both build precisions produce the same bytes for the same explicit wire type.
- Schemas choose cost and precision explicitly.
- GDScript float32 conversion reports finite overflow instead of silently producing infinity.
- State hashing must not depend on uncanonicalized NaNs.

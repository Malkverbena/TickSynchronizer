# ADR 0024 — Safe header inspection and pure handshake evaluator

## Status

Accepted

## Context

The atomic decoder must reject incompatible envelopes, while the session still needs observed header values for structured disconnects.

## Decision

Provide a limited `inspect_control_header()` that reads only validated fixed fields without accepting the packet, and a pure evaluator that produces deterministic compatibility actions.

## Consequences

- Diagnostics do not weaken the decoder.
- Transport, state order, and serialization remain separate.
- Compatibility tests run without networking or SceneTree.

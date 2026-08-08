# ADR 0023 — Strict handshake identity and capabilities

## Status

Accepted

Amended by ADR 0030 for Godot engine identity: the canonical complete version
is exact, while a commit mismatch is diagnostic and non-fatal.

## Context

Experimental peers must not exchange gameplay when engine, module, game, schema, precision, API, wire, or required features differ.

## Decision

HELLO and HELLO_ACK carry fixed identities, capabilities, precision, API/wire versions, and a correlation nonce. Compatibility remains exact during wire version 0.

## Consequences

- Failures produce structured actionable reasons.
- Dirty builds receive deterministic identities.
- Optional unknown capabilities may be ignored; required capabilities must be mutual.
- Flexible minor-version compatibility is deferred.

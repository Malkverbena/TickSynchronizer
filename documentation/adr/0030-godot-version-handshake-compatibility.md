# ADR 0030 — Godot version handshake compatibility

## Status

Accepted

## Context

ADR 0023 required exact engine identity during the experimental handshake.
That policy tied otherwise compatible peers to one Godot Git commit, including
commits whose changes do not affect networking or deterministic simulation.
The project still needs a clear engine compatibility boundary and provenance
for diagnosing custom or patched builds.

## Decision

Require exact equality of the canonical complete Godot version in the form
`major.minor.patch-status`, such as `4.7.1-stable`. Carry that version as a
32-byte zero-terminated, zero-padded ASCII field in HELLO and HELLO_ACK.

Retain the Godot commit SHA-1 in both profiles, but treat a mismatch as the
structured `GODOT_COMMIT_MISMATCH` warning. The warning does not reject an
otherwise compatible peer and is stored in handshake actions and established
session metadata for the future session layer to log.

Module build, game build, schema, precision, API, wire, and required capability
mismatches remain fatal. Source qualification continues to require the exact
unmodified Godot baseline commit; only peer connection compatibility changes.

This incompatible experimental layout advances wire protocol 0 from revision 1
to revision 2 and HELLO/HELLO_ACK payload version 3 to 4. This decision amends only the Godot
commit requirement in ADR 0023.

## Consequences

- Rebuilds and upstream commits that retain the same complete Godot release can
  connect when all other strict identities match.
- A same-version custom engine can contain behavior or ABI changes that the
  handshake cannot prove safe. The commit warning and both commit values must
  therefore remain available for incident diagnostics.
- Exact game and module build matching continues to prevent different shipped
  game code from silently connecting.
- The evaluator remains pure: it returns warning flags instead of printing or
  depending on global logging state.
- Historical HELLO golden vectors remain immutable; payload version 4 receives
  a new golden vector.

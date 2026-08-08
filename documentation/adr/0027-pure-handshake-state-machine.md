# ADR 0027 — Pure handshake state machine

## Status

Accepted

## Context

The packet codec and compatibility evaluator do not define legal message order, and that responsibility does not belong in a transport.

## Decision

Use a C++17 `ProtocolHandshakeStateMachine` with no `Node`, singleton, clock, or endpoint dependency. It consumes decoded packets and returns declarative actions across IDLE, waiting, established, rejected, and closed states.

## Consequences

- Ordering is deterministic and transport-independent.
- Tests cover initiator/responder flows, duplicates, invalid order, disconnects, and nonce handling.
- Future session code executes actions without embedding protocol rules in endpoints.

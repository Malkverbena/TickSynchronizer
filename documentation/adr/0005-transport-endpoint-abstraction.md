# ADR 0005 — SyncTransportEndpoint abstraction

## Status

Accepted

## Context

RPC, raw bytes, MultiplayerPeer, and direct ENet must be comparable without coupling synchronization logic to one transport.

## Decision

Runtime synchronization will depend on `SyncTransportEndpoint`. Endpoints own peers, channels, reliability, send/receive events, and transport metrics, but not snapshots or rollback.

## Consequences

- One protocol can run over multiple transports.
- A loopback endpoint precedes real endpoints.
- Benchmarks can compare transports under identical workloads.

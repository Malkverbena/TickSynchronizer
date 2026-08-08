# ADR 0003 — PackedByteArray as canonical wire storage

## Status

Accepted

## Context

The protocol needs Godot-compatible, ABI-independent storage and explicit support for non-byte-aligned fields.

## Decision

Use `PackedByteArray` for completed packets and implement an explicit codec over it. Fields consume bits LSB-first, byte-aligned integers are little-endian, and logical bit size is stored separately.

## Consequences

- C++ structs are never copied directly to the network.
- Endianness, widths, bit order, and padding are explicit and tested.
- Golden vectors protect cross-platform byte identity.
- Godot convenience encode methods do not define this project’s wire format.

# ADR 0002 — Independent repository with two supported module layouts

## Status

Accepted

## Context

Independent history is easiest when the module is outside the engine tree, but users and integration builds may need a conventional in-tree Godot module.

## Decision

Maintain `tick_synchronizer` as an independent repository. Support both `custom_modules=<path-to-tick_synchronizer>` and placement at `godot/modules/tick_synchronizer`.

## Consequences

- Engine and module history remain independent.
- Build files, includes, and validation scripts must not assume only one layout.
- The main build script auto-detects the in-tree layout and omits `custom_modules`.
- The external sibling layout remains the recommended development setup.

# ADR 0016 — Source, tests, and documentation consistency

## Status

Accepted

## Context

Partial overlays previously allowed headers, bindings, XML, tests, scripts, and documentation from different revisions to be combined.

## Decision

Maintain `FILE_MANIFEST.txt` and run a repository consistency preflight before SCons. Validate public methods, test counts, version contracts, protocol files, benchmark files, language policy, documentation invariants, the single benchmark SConstruct, and the absence of alternative compilation graphs. Exclude only explicit repository-owned generated paths; file extensions alone never make an unexpected file invisible.

## Consequences

- Incomplete packages fail early.
- Generated overlays must preserve complete logical revisions.
- The consistency script becomes part of the build contract.
- Unexpected objects, executables, build descriptors, or sources outside declared generated directories fail the manifest check.

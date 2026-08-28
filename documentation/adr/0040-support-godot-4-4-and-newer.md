# ADR 0040: Support Godot 4.4 and newer 4.x releases

## Status

Accepted.

## Context

ADR 0001 restricted every build to one Godot 4.7.1 commit so early module work
and engine diagnostics were reproducible. The module now uses a deliberately
small Godot C++ surface and has a separate handshake compatibility policy.
Keeping a single-commit compilation restriction would unnecessarily prevent
users from integrating the module with other supported Godot 4 releases.

Build compatibility, validation provenance, and peer compatibility are
different contracts and must not be conflated.

## Decision

Support clean, unmodified Godot 4.x source trees at version 4.4.0 or newer.
`GODOT_MINIMUM_VERSION` is the machine-readable build floor. The build scripts
read Godot's `version.py` without executing it, reject versions below 4.4.0,
and reject other major versions. Godot 5.x is not implicitly supported.

`GODOT_VERSION` and `GODOT_COMMIT` remain the qualified validation baseline,
currently Godot 4.7.1-stable at
`a13da4feb8d8aefc283c3763d33a2f170a18d541`. A different supported Godot 4.x
version or commit may compile without an override, but it does not inherit the
complete normal, sanitizer, or benchmark qualification evidence of that
baseline. The validation report records whether the live tree matches it.

The Godot tree must remain clean. A dirty-tree override remains diagnostic
only. The accepted LSAN and UBSAN suppressions are version-specific external
engine policy, so `run_sanitized_tests.sh` continues to require the exact
qualified Godot version and commit until a separate review qualifies another
engine baseline.

Peer compatibility remains governed by ADR 0030: peers must match the exact
canonical complete Godot version. A Godot commit mismatch remains a structured
warning, while module build, game build, schema, precision, API, wire, and
required-capability mismatches remain fatal.

This decision supersedes ADR 0001's single-commit build restriction and amends
ADR 0030's source-qualification statement. It does not alter the historical
provenance of accepted tests or benchmark reports.

## Consequences

- Users may compile the module with clean Godot 4.4.0 or newer 4.x source.
- Godot 4.4 is the API compatibility floor; project code must not depend on a
  later API without either a guarded fallback or a new minimum-version ADR.
- Supporting compilation is not a claim that every later 4.x release has
  passed the full acceptance matrix.
- Godot 5.x and later major versions require an explicit compatibility review.
- The external `custom_modules` and in-tree
  `godot/modules/tick_synchronizer` layouts remain supported.
- Existing Godot 4.7.1 sanitizer suppression decisions remain exact and
  auditable instead of silently applying to unreviewed engine revisions.

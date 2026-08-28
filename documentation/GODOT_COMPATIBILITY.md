# Godot compatibility

## Supported engine range

TickSynchronizer supports clean, unmodified Godot 4.x source at version 4.4.0
or newer. `GODOT_MINIMUM_VERSION` is the machine-readable floor. Other major
versions, including Godot 5.x, are not implicitly supported.

The module must use APIs available in Godot 4.4. A later API requires a guarded
fallback or an ADR that raises the minimum version.

## Qualified validation baseline

`GODOT_VERSION` and `GODOT_COMMIT` record the engine source that passed the
complete accepted validation matrix. They are qualification provenance, not a
single-commit compilation restriction. The current baseline is Godot
`4.7.1-stable` at
`a13da4feb8d8aefc283c3763d33a2f170a18d541`.

A supported 4.x version may compile without an override but does not inherit
the full normal, sanitizer, or benchmark evidence of that baseline. Validation
reports record whether the live engine matches it.

## Supported installation layouts

The module is compatible with:

1. an external repository passed through `custom_modules`;
2. a conventional directory at `godot/modules/tick_synchronizer`.

External development is recommended because it keeps engine and module history
independent. In-tree compilation remains a supported deployment and integration
path.

## Engine cleanliness

The project does not patch Godot. Validation rejects:

- an engine version below 4.4.0 or outside the 4.x series;
- engine changes outside the module directory;
- a missing `SConstruct` or `version.py`;
- a module path that resolves to the wrong repository.

When TickSynchronizer is located at `godot/modules/tick_synchronizer`, that
directory is the expected module source and is excluded from the engine-dirty
check. This exception does not permit changes elsewhere in the Godot tree.
The dirty-tree override is diagnostic only.

## Sanitizer qualification

The accepted LSAN and UBSAN suppressions describe reviewed external Godot 4.7.1
diagnostics. `run_sanitized_tests.sh` therefore requires the exact qualified
Godot version and commit. Qualifying sanitizers on another supported 4.x
version requires a separate engine-diagnostic review; the normal compilation
range is not narrowed by that requirement.

## Peer handshake compatibility

Build support and peer compatibility are deliberately different. During a
connection, peers require the same canonical complete Godot version, such as
`4.7.1-stable`. A 4.4 peer and a 4.7 peer are both build-supported but may not
join the same session.

The handshake retains each peer's Godot commit as provenance. A commit mismatch
sets `GODOT_COMMIT_MISMATCH` and allows the connection to complete; session
integration must log that warning. This permits compatible rebuilds of the same
Godot release, but it cannot prove that a custom or patched engine preserved
network semantics. Exact module build, game build, schema, precision, API,
wire, and required-capability checks remain fatal.

## C++ baseline and precision

The project uses C++17. Runtime code does not use exceptions or RTTI and follows
Godot ownership and error conventions. Both Godot precision modes compile, but
peers in one session must match `single` or `double`; wire fields use explicit
widths rather than `real_t`.

## Class documentation

Every public bound method must have a matching Godot class-reference XML entry.
The consistency script compares headers, bindings, and XML to prevent revision
drift.

## Compatibility update policy

Raising the minimum version or adding another major series requires:

1. a dedicated ADR;
2. an updated `GODOT_MINIMUM_VERSION`;
3. complete builds in both precisions;
4. sanitizer reevaluation on a named qualified baseline;
5. review of APIs, warnings, third-party diagnostics, and golden behavior;
6. no unreviewed local engine changes.

Updating only the qualified baseline additionally updates `GODOT_VERSION` and
`GODOT_COMMIT` while preserving the supported range unless an ADR says
otherwise.

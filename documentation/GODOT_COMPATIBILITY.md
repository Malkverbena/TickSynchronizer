# Godot compatibility

## Baseline

TickSynchronizer targets Godot `4.7.1-stable` at the exact commit stored in `GODOT_COMMIT`. The module must not depend on APIs introduced after that baseline without an explicit engine-baseline ADR.

## Supported installation layouts

The module is compatible with:

1. an external repository passed through `custom_modules`;
2. a conventional directory at `godot/modules/tick_synchronizer`.

External development is recommended because it keeps engine and module history independent. In-tree compilation remains a supported deployment and integration path.

## Engine cleanliness

The project does not patch Godot. Validation rejects:

- a different engine commit;
- engine changes outside the module directory;
- a missing `SConstruct`;
- a module path that resolves to the wrong repository.

When TickSynchronizer is located at `godot/modules/tick_synchronizer`, that directory is the expected module source and is excluded from the engine-dirty check. This exception does not permit changes elsewhere in the Godot tree.

Diagnostic overrides exist only for investigation and must not become release defaults.

## Peer handshake compatibility

The source-validation baseline and the peer connection policy are deliberately
different. Building and qualifying TickSynchronizer still requires the exact
unmodified Godot commit in `GODOT_COMMIT`. During a connection, peers instead
require the same canonical complete Godot version, such as
`4.7.1-stable`.

The handshake retains each peer's Godot commit as provenance. A commit mismatch
sets `GODOT_COMMIT_MISMATCH` and allows the connection to complete; session
integration must log that warning. This permits compatible rebuilds of the same
Godot release, but it cannot prove that a custom or patched engine preserved
network semantics. Exact module build, game build, schema, and precision checks
remain fatal safeguards.

## C++ baseline

The project uses C++17 to match the supported Godot baseline. Runtime code does not use exceptions or RTTI and follows engine ownership and error conventions.

## Precision

Both Godot precision modes compile. Session compatibility remains strict: peers must match `single` or `double`, although wire fields use explicit widths rather than `real_t`.

## Class documentation

Every public bound method must have a matching Godot class-reference XML entry. The consistency script compares headers, bindings, and XML to prevent revision drift.

## Baseline update policy

An engine update requires:

1. a dedicated ADR;
2. updated `GODOT_VERSION` and `GODOT_COMMIT`;
3. successful complete builds in both precisions;
4. sanitizer reevaluation;
5. review of APIs, warnings, third-party diagnostics, and golden behavior;
6. no unreviewed local engine changes.

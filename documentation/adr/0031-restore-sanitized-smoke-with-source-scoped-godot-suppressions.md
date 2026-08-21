# ADR 0031 — Restore sanitized smoke with source-scoped Godot suppressions

## Status

Accepted

Supersedes ADR 0021.

## Context

The normal editor smoke passes in both precisions. The UBSAN editor smoke was
previously deferred because Godot 4.7.1 stopped during bundled SDL/HIDAPI
startup before the TickSynchronizer project ran.

An exact-commit UBSAN editor built without TickSynchronizer reproduced the SDL
TLS bounds diagnostic. After isolating that occurrence, a pure GDScript project
using the same no-module editor reproduced a GDScript VM alignment diagnostic.
Neither stack entered TickSynchronizer. A temporary source-scoped rule set let
the real double-precision smoke emit every required marker, while unrelated
bounds and alignment probes remained fatal.

## Decision

Retain the existing Godot 4.7.1 test-setup rule and accept exactly two
additional category-and-source pairs:

```text
nonnull-attribute:core/string/ustring.cpp
bounds:thirdparty/sdl/thread/SDL_thread.c
alignment:modules/gdscript/gdscript_vm.cpp
```

Run the real sanitized GDScript smoke in both precisions. Before every UBSAN
pass that uses these rules, require a regression guard to:

1. reject any suppression set other than the exact reviewed three-rule policy;
2. build unrelated C++17 bounds and alignment probes through SCons;
3. require both unrelated diagnostics to remain fatal with the accepted file.

Do not patch the Godot source. The rules are locked to Godot 4.7.1 and must be
reevaluated when the engine baseline or sanitizer behavior changes.

## Consequences

- ADR 0021's temporary smoke deferral is closed while its history is retained.
- Sanitized smoke again exercises the real editor integration path.
- Every other UBSAN source location, including TickSynchronizer, remains fatal.
- Broad category-only, wildcard, module, or path-prefix suppressions are rejected.
- A rule must be removed when an unsuppressed exact-baseline run proves it is no
  longer required.

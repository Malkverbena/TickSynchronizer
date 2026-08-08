# TickSynchronizer Agent Instructions

This file is the mandatory entry point for every person or automated agent that works in this repository.

## Sources of truth

Read the following files before changing code:

1. `AGENTS.md`;
2. `documentation/PROJECT_STATE.md`;
3. `documentation/ARCHITECTURE.md`;
4. the relevant ADRs in `documentation/adr/`;
5. `documentation/ROADMAP.md`;
6. task-specific documentation.

Conversations, old messages, and agent memory are not authoritative. Versioned files and executable tests take precedence when information conflicts.

## Language policy

All repository content must be written in English, including:

- source comments and diagnostics;
- documentation and ADRs;
- build and validation scripts;
- tests and smoke-test messages;
- class reference XML;
- commit messages created for this project.

Public identifiers that are already part of the API or wire contract must not be renamed merely to satisfy this policy.

## Engine baseline

- Godot baseline: `4.7.1-stable`.
- Exact commit: stored in `GODOT_COMMIT`.
- Human-readable version: stored in `GODOT_VERSION`.
- The Godot source tree is normally a sibling at `../godot`.
- The module may be built externally with `custom_modules=../tick_synchronizer`.
- The same module may also be copied or checked out at `godot/modules/tick_synchronizer` and built as a conventional in-tree module.
- Do not modify, patch, or keep local cherry-picks in the Godot engine source.
- Validation must reject a mismatched or dirty engine tree by default.

Both supported layouts must remain functional. Do not introduce path assumptions that work only for the external layout.

## Build and precision

- SCons is the only compilation system for the Godot module and every standalone benchmark target.
- `benchmarks/SConstruct` is the single benchmark compilation graph for Linux, Windows, and Android.
- Windows executables are cross-compiled on Linux with MinGW-w64 or LLVM-MinGW.
- Android executables are cross-compiled with the Android NDK Clang driver.
- Default precision: `double`.
- Supported precisions: `single` and `double`.
- Every peer in one session must use the same build precision.
- The wire format must never serialize `real_t` directly.
- Initial target platforms: Linux, Windows, and Android.
- Web is unsupported; macOS and iOS are deferred.

Host temporary directories must default to the sibling `../tick_synchronizer_tmp` directory. `TICKSYNC_TEMP_DIR` may select another safe host location, but `/tmp` and its descendants are forbidden. `/data/local/tmp` is allowed only as the remote ADB deployment directory on an Android device.

Benchmark deployment packages must remain execution-only. Test machines receive prebuilt binaries, runners, hashes, and instructions; they must not require a compiler, SCons, an SDK/NDK, Git checkout, or project sources.

## Build-storage integrity

Builds and qualification runs must use healthy writable storage. An I/O or
filesystem error invalidates the affected run; it is neither a source failure
nor a test result. Discard its generated artifacts and rebuild cleanly. A
source-only archive has no Git provenance and cannot produce an official
benchmark report by itself.

## C++ and Godot conventions

- Follow Godot 4.7.1 conventions.
- Use `#pragma once` in project headers, except existing test headers that intentionally use include guards.
- Include every used type directly; do not rely on transitive includes.
- Use exactly one blank line between consecutive function declarations or definitions in project-owned `.h` files.
- Use exactly two blank lines between consecutive namespace-scope or class-method definitions in project-owned `.cpp` files.
- Keep method comments attached to the declaration or definition they describe; spacing belongs before the comment block.
- Prefer Godot runtime types where appropriate: `PackedByteArray`, `Vector`, `LocalVector`, `HashMap`, `StringName`, `ObjectID`, `Ref<T>`, and `Variant`.
- Do not use exceptions or RTTI in module runtime code.
- Use `Error`, `ERR_FAIL_*`, `WARN_PRINT`, and `ERR_PRINT` according to engine conventions.
- Never serialize C++ structs by copying their memory representation.
- Wire integers use explicit little-endian encoding and documented bit order.
- Do not introduce a mandatory singleton to represent a synchronization session.

## File and API documentation

Every project-owned `.h` and `.cpp` file must begin with a short comment that describes its responsibility and architectural purpose.

Method comments should:

- appear immediately above the declaration or definition when context is useful;
- contain no more than two lines;
- explain the contract, side effect, limit, or role rather than restating the method name.

Comment non-obvious invariants, wire-layout rules, ownership assumptions, error atomicity, and benchmark methodology. Avoid comments that duplicate straightforward code.

## Security and compatibility

- Do not implement custom cryptographic primitives.
- Future cryptography must use an mbedTLS-based backend.
- Do not accept gameplay input, snapshots, or state before the handshake completes.
- Compatibility failures must provide actionable structured errors, especially `PRECISION_MISMATCH`.
- Peers must match the canonical complete Godot version exactly. A Godot commit
  mismatch is diagnostic only and must produce a structured warning without
  rejecting an otherwise compatible peer.
- Module build, game build, schema, and precision mismatches remain fatal.
- Untrusted lengths and counts must be validated before allocation.

## Required validation

A complete functional change must:

1. compile the editor with `tests=yes`;
2. pass all TickSynchronizer C++ tests;
3. pass the automated GDScript smoke test;
4. compile `template_debug`;
5. compile `template_release`;
6. update documentation, ADRs, and `PROJECT_STATE.md` when applicable.

Primary command:

```bash
./scripts/build_and_validate.sh --mode all --precision double
```

Fast development cycle:

```bash
./scripts/build_and_validate.sh --mode quick --precision double
```

The benchmark harness has its own build and execution scripts. Official benchmark reports require a clean Git tree and explicit CPU affinity.

## Temporary sanitized gate

Run the module C++ sanitizer suites in both precisions. While the known Godot 4.7.1 UBSAN diagnostic remains isolated to bundled SDL/HIDAPI initialization, use:

```bash
./scripts/run_sanitized_tests.sh double --no-smoke
./scripts/run_sanitized_tests.sh single --no-smoke
```

Normal smoke tests remain mandatory in both precisions. The external diagnostic does not justify broad suppressions and never permits ignoring reports that enter TickSynchronizer code.

## Git policy

- Do not create a commit for every small edit.
- Group changes into complete logical units.
- A commit must compile, pass its relevant tests, and contain matching documentation.
- Do not mix unrelated refactors with a feature.
- Never modify the Godot engine source as part of a module change.

## Request quality

Explicitly warn when a request would:

- contradict accepted engineering practice;
- introduce unnecessary coupling;
- reduce performance, security, portability, or reproducibility;
- create avoidable technical debt;
- produce noisy Git history;
- violate an accepted ADR.

Do not silently implement a harmful decision without documenting its consequences.

## AI-assisted development

AI-assisted contributions are welcome only when the responsible developer genuinely understands the concepts, architecture, technical decisions, and code involved.

AI tools may support research, analysis, implementation, documentation, testing, refactoring, and review. AI-generated output must never be accepted or committed without complete human technical review.

The human contributor remains responsible for:

- understanding every submitted change;
- validating the technical reasoning;
- checking ownership, lifetime, memory safety, and thread safety;
- preserving protocol compatibility and documented invariants;
- reviewing error handling and resource limits;
- running the applicable tests, validation scripts, and sanitizers;
- keeping code, documentation, ADRs, and diagrams consistent;
- being able to explain, debug, modify, and maintain the contribution without the AI tool.

Code that the responsible developer cannot fully explain or maintain must not be committed. AI output is advisory; versioned source, tests, protocol specifications, architecture documentation, and accepted ADRs remain authoritative.

## Context updates

After completing a logical change:

- update `documentation/PROJECT_STATE.md`;
- update the relevant `documentation/ROADMAP.md` checklist;
- create or revise an ADR when an architectural decision changes;
- run `./scripts/generate_context.sh` to verify the compact project summary;
- do not place extensive logs or full source listings in generated context.

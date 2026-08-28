# TickSynchronizer Agent Instructions

This file is the mandatory entry point for every person or automated agent that works in this repository.

## Sources of truth

Read the following files before changing code:

1. `AGENTS.md`;
2. `documentation/README.md`;
3. `documentation/development/PROJECT_STATE.md`;
4. `documentation/ARCHITECTURE.md`;
5. the relevant ADRs in `documentation/adr/`;
6. `documentation/development/ROADMAP.md`;
7. task-specific documentation.

Conversations, old messages, and agent memory are not authoritative. Versioned files and executable tests take precedence when information conflicts.

## Documentation surfaces

- The versioned repository is authoritative for technical contracts, ADRs,
  source-specific state, validation evidence, and the strategic phase roadmap.
- The GitHub Project is authoritative for actionable backlog items, status,
  priority, assignment, scheduling, milestones, and board or timeline views.
- The GitHub Wiki is a derived user-facing publication. It starts only after
  the current source acceptance gate and official benchmark matrix are complete,
  and it never replaces versioned normative documents or ADRs.
- Public Project and Wiki content must not expose credentials, private local
  paths, host names, operator identity, serial identifiers, or unpublished
  artifact names and hashes.
- ADR 0035 defines the complete ownership and publication policy.
- `documentation/PRIVACY.md` defines the repository publication checklist.

## Language policy

All repository content must be written in English, including:

- source comments and diagnostics;
- documentation and ADRs;
- build and validation scripts;
- tests and smoke-test messages;
- class reference XML;
- commit messages created for this project.

Public identifiers that are already part of the API or wire contract must not be renamed merely to satisfy this policy.

## Engine compatibility

- Supported engine range: Godot 4.x at version `4.4.0` or newer, as stored in
  `GODOT_MINIMUM_VERSION`.
- Godot 5.x and later major versions require an explicit compatibility ADR.
- Qualified validation version: stored in `GODOT_VERSION`.
- Qualified validation commit: stored in `GODOT_COMMIT`.
- A supported engine may compile without matching the qualified commit, but it
  does not inherit that commit's complete validation evidence.
- The Godot source tree is normally a sibling at `../godot`.
- The module may be built externally with `custom_modules=../tick_synchronizer`.
- The same module may also be copied or checked out at `godot/modules/tick_synchronizer` and built as a conventional in-tree module.
- Do not modify, patch, or keep local cherry-picks in the Godot engine source.
- Validation must reject an unsupported or dirty engine tree by default.
- The accepted Godot sanitizer suppressions remain locked to the exact
  qualified version and commit until a separate review qualifies another one.

Both supported layouts must remain functional. Do not introduce path assumptions that work only for the external layout.

## Build and precision

- SCons is the only compilation system for the Godot module and every standalone benchmark target.
- `benchmarks/SConstruct` is the single benchmark compilation graph for Linux, Windows, Android, and macOS.
- Windows executables are cross-compiled on Linux with MinGW-w64 or LLVM-MinGW.
- Android executables are cross-compiled with the Android NDK Clang driver.
- macOS Universal 2 executables are built locally from Apple Clang `x86_64`
  and `arm64` thin targets, then merged with `lipo`.
- Default precision: `double`.
- Supported precisions: `single` and `double`.
- Every peer in one session must use the same build precision.
- The wire format must never serialize `real_t` directly.
- Initial target platforms: Linux, Windows, Android, and macOS.
- Web and iOS are unsupported.

Host temporary directories must default to the sibling `../tick_synchronizer_tmp` directory. `TICKSYNC_TEMP_DIR` may select another safe host location, but `/tmp` and its descendants are forbidden. `/data/local/tmp` is allowed only as the remote ADB deployment directory on an Android device.

Benchmark deployment packages are private qualification artifacts and must
remain execution-only. Test machines receive prebuilt binaries, runners, hashes,
and instructions; they must not require a compiler, SCons, an SDK/NDK, Git
checkout, or project sources. The macOS target additionally must not require
Homebrew or Python. Platform-standard runtime utilities and the documented Linux
or Android controller tools remain allowed. Public GitHub distribution is
source-only: do not commit or release generated benchmark binaries or deployment
packages.

Generated benchmark provenance must use privacy-safe compiler identifiers and
binary basenames instead of build-host user directories, mounted-volume paths,
drive-qualified paths, host names, or device serial identifiers. Package export
and report validation must reject host-specific path leakage.

Never vendor, upload, host, or redistribute Xcode, Apple developer tools, an
Apple SDK, or extracted Apple headers and libraries. macOS builds must use a
locally supplied Apple toolchain and SDK on Apple-branded hardware running
macOS. Linux cross-compilation with a copied Apple SDK is unsupported.

## Build-storage integrity

Builds and qualification runs must use healthy writable storage. An I/O or
filesystem error invalidates the affected run; it is neither a source failure
nor a test result. Discard its generated artifacts and rebuild cleanly. A
source-only archive has no Git provenance and cannot produce an official
benchmark report by itself.

## C++ and Godot conventions

- Use APIs and conventions available in the supported Godot 4.4 floor.
- Use `#pragma once` in project headers, except existing test headers that intentionally use include guards.
- Include every used type directly; do not rely on transitive includes.
- Use exactly one blank line between consecutive function declarations or definitions in project-owned `.h` files.
- Use exactly two blank lines between consecutive namespace-scope or class-method definitions in project-owned `.cpp` files.
- Keep method comments attached to the declaration or definition they describe; spacing belongs before the comment block.
- Module-linked source and tests must not use STL containers, `auto`, avoidable
  lambdas, exceptions, or RTTI. Prefer Godot runtime types where appropriate:
  `PackedByteArray`, `Vector`, `LocalVector`, `HashMap`, `StringName`, `ObjectID`,
  `Ref<T>`, and `Variant`.
- Standalone benchmark sources may use STL containers and measurement lambdas,
  but must remain exception-free and RTTI-free. Every benchmark target compiles
  with `-fno-exceptions` and `-fno-rtti` through SCons.
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

## Protocol selection boundary

- `varint_zigzag_fixed_float` is the accepted default scalar-encoding profile
  for subsequent gameplay protocol design under ADR 0039.
- Unsigned integers use minimal canonical ULEB128; signed integers use portable
  ZigZag followed by minimal canonical ULEB128; floats retain canonical
  fixed-width IEEE 754 encoding.
- This selection does not stabilize the complete realtime wire. Framing,
  state references, masks, delta recovery, quantization, and transport behavior
  require separate evidence and ADRs.
- Wire version 0 revision 2 remains experimental, and the final macOS
  portability gate remains blocking.

## Required validation

A complete functional change must:

1. compile the editor with `tests=yes`;
2. pass all TickSynchronizer C++ tests;
3. pass the automated GDScript smoke test;
4. compile `template_debug`;
5. compile `template_release`;
6. update documentation, ADRs, and
   `documentation/development/PROJECT_STATE.md` when applicable.

Primary command:

```bash
./scripts/build_and_validate.sh --mode all --precision double
```

Fast development cycle:

```bash
./scripts/build_and_validate.sh --mode quick --precision double
```

The benchmark harness has its own build and execution scripts. Official
benchmark reports require a clean Git tree and verified hard CPU affinity on
Linux, Windows, and Android. macOS accepts only the exact scheduler-managed
representative policy in ADR 0032 because public hard logical-CPU pinning is
unavailable there.

## Sanitized gate

Run the module sanitizer suites and the real GDScript smoke in both precisions:

```bash
./scripts/run_sanitized_tests.sh all
```

The Godot 4.7.1 LSAN file contains exactly one reviewed function rule for an
SDL joypad/UDEV leak reproduced without TickSynchronizer. Leak detection must
remain enabled, and a SCons-built negative control must prove that an unrelated
leak remains fatal. The UBSAN file contains exactly three reviewed
category-and-source pairs, protected by unrelated bounds and alignment negative
controls. `--no-smoke` is diagnostic only and cannot satisfy the accepted gate.
No sanitizer suppression may match TickSynchronizer.

The `all` batch runs both precisions serially. It executes the full LSAN
unrelated-leak control once per uninterrupted batch and rechecks the exact
static LSAN rule before every later applicable pass.

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

- update `documentation/development/PROJECT_STATE.md` when capabilities change;
- update `documentation/development/VALIDATION.md` when acceptance evidence changes;
- update `documentation/development/ROADMAP.md` only when the high-level phase
  structure changes;
- create or revise an ADR when an architectural decision changes;
- keep task assignment, scheduling, priorities, and Kanban status in the GitHub
  Project rather than the module manual;
- keep normative and version-bound documentation in the repository; publish
  only derived user-facing material to the Wiki under ADR 0035;
- run `./scripts/generate_context.sh` to verify the compact project summary;
- do not place extensive logs or full source listings in generated context.

# Validation status

## Identification

- Godot: `4.7.1-stable`
- primary module acceptance platform: Linux x86_64
- benchmark qualification platforms: Linux x86_64, Windows x86_64, Android
  ARM64, and macOS Universal 2
- build precisions: `single` and `double`
- public API: 4
- experimental wire: 0 revision 2
- benchmark suite: 2

## Source consistency

Current source-consistency target:

```text
methods=41
tests=140
```

## Current complete normal build matrix

The current wire revision 2 source passed in both precision modes:

- editor build;
- 140/140 C++ tests;
- 66,999/66,999 assertions;
- GDScript smoke markers;
- `template_debug`;
- `template_release`.

Accepted report identities:

```text
20260821T150207Z-linuxbsd-double-all
20260821T150435Z-linuxbsd-single-all
```

## Sanitizers

The current suite 2 source passed the complete integrated split sanitizer gate
in both precisions. The accepted command was:

```bash
./scripts/run_sanitized_tests.sh all
```

Accepted report identities:

| Report | Precision | Sanitizer |
|---|---|---|
| `20260821T212102Z-linuxbsd-double-quick` | `double` | ASAN plus LSAN |
| `20260821T212313Z-linuxbsd-double-quick` | `double` | UBSAN |
| `20260821T212544Z-linuxbsd-single-quick` | `single` | ASAN plus LSAN |
| `20260821T212918Z-linuxbsd-single-quick` | `single` | UBSAN |

Every pass completed 140/140 TickSynchronizer C++ tests, 66,999/66,999
assertions, and all required GDScript smoke markers. The ASAN passes used
LLVM/LLD, the UBSAN passes used GCC/LLD, and all builds used the exact clean
Godot `4.7.1-stable` commit
`a13da4feb8d8aefc283c3763d33a2f170a18d541`.

Leak detection remained enabled. Each ASAN smoke reported exactly the reviewed
172 suppressed allocations and 6,991 bytes through
`JoypadSDL::initialize`. The one full SCons-built unrelated leak control
remained fatal with status 23 and exposed its 4,096-byte allocation; the exact
one-rule policy was rechecked before the later ASAN pass. The no-module Godot
reproduction remains closed ADR 0038 evidence and was not repeated.

Both UBSAN passes used the exact three-rule Godot policy. Before each pass, the
independent bounds and alignment negative controls remained fatal. No ASAN,
LSAN, or UBSAN diagnostic entered TickSynchronizer. The module source hashes
and Git status were identical before and after the sanitizer batch, while the
Godot worktree remained clean at the exact baseline commit.

`--no-smoke` and both suppression-disable options remain available only for
diagnosis and cannot satisfy acceptance.

## Benchmark infrastructure

The standalone suite 2 passes its self-test and validates:

- eight deterministic datasets and 952 semantic messages;
- semantic round-trips;
- deterministic encoding;
- candidate-independent semantic hashes;
- exact malformed error categories and atomic decode failure;
- canonical message-kind fields and atomic encode failure;
- 21/21 fixed-width and 25/25 varint malformed packet rejections;
- canonical ULEB128, ZigZag, NaN, infinity, signed-zero, subnormal, and
  binary32 rounding boundaries;
- non-zero diagnostic checksums;
- JSON and CSV schema 3 consistency;
- native CPU affinity verification on Linux, Windows, and Android;
- the exact scheduler-managed representative policy for macOS;
- native logical CPU, L3-domain, NUMA, and SMT topology discovery;
- cross-platform device, OS, SoC, and backend provenance;
- executable and environment provenance;
- one SCons compilation graph for all four target platforms;
- precision-separated comparator baselines and report identities;
- matched candidate analysis with predeclared workload gates;
- execution-only packages for test machines without development environments,
  including Universal 2 macOS packaging without target-side Homebrew or Python;
- source-policy guards for the Godot C++ subset and exception-free, RTTI-free
  standalone benchmark compilation;
- privacy-safe compiler and executable provenance with rejection of host user,
  mounted-volume, and drive-qualified paths.

Official benchmark baselines must be generated from a clean tree. Candidate
selection requires verified hard affinity for the 16 matched Linux, Windows,
and Android pairs. The deferred final macOS gate uses the exact
scheduler-managed policy from ADR 0032.

The current Linux close-out reran source consistency, shell and Python syntax,
diff-whitespace checks, the public SCons build wrapper, and both candidate
self-tests in each precision. It also revalidated the schema and SHA-256
manifest of every report in the final preliminary campaign. No macOS command
was executed. The complete normal Godot matrix was not repeated during the
sanitizer close-out because the LSAN reconciliation changed only project-owned
policy, scripts, documentation, manifests, and the smoke-project UID; it did
not change module C++ or tests. The accepted 140-test, 66,999-assertion normal
matrix above remains the integration baseline, and the fresh integrated
sanitizer matrix closes the remaining source-relevant acceptance gate.

## Suite 2 Linux evidence

The optimized GCC 13.3 binaries compile without exceptions or RTTI in both
precisions. Four normal and four ASAN plus UBSAN self-tests pass across both
candidates and precisions. Leak detection is disabled only because the
execution environment uses ptrace; address and undefined-behavior diagnostics
remain fatal.

An independent two-million-sample binary64-to-binary32 cross-check found zero
mismatches against the platform conversion after applying the protocol's
explicit NaN and overflow rules.

The final available preliminary campaign pinned all four serial runs to logical
CPU 0, used 5 warmups, 15 measured rounds, and a 50 ms minimum sample. Every
report passed schema, semantic, deterministic, malformed-input, affinity, and
hash validation. The paired analysis is:

| Precision | Workload bytes | Size ratio | Weighted encode | Weighted decode |
|---|---:|---:|---:|---:|
| `double` | 480,120 -> 344,785 | 0.718x | 0.825x | 0.974x |
| `single` | 340,344 -> 205,009 | 0.602x | 0.806x | 0.985x |

Both pairs pass the predeclared ADR 0037 screen. The geometric-mean ratios are
0.658x for workload size, 0.815x for weighted encode, and 0.979x for weighted
decode. Maximum MAD divided by median was 11.05% in the fixed-width
`player_input` decode microcase; the decision margins are sufficiently large
that no redundant rerun was performed.

These reports are diagnostic because the reconstructed source tree is dirty
and each records `official=no`. They select the integer primitive for continued
qualification but do not close the clean-tree cross-platform matrix or select
the complete realtime wire.

## Historical suite 1 platform evidence

Earlier Linux and Windows x86_64 L3-domain runs and Android ARM64 device/core
runs passed suite 1 correctness and affinity checks. They established backend,
topology, packaging, and platform sensitivity behavior. They are not
numerically comparable with suite 2 and are not candidate-selection evidence.

The macOS path also passed Linux-side contract controls for shell and Python
syntax, repository consistency, explicit compiler and SDK selection without an
unnecessary `xcrun` lookup, non-Apple compiler rejection, clean-versus-dirty
report eligibility, execution-only package contents and hashes, and rejection
of host-specific paths. The package exporter also emits basename-only archive
hash sidecars, and deployment-target inspection treats equivalent two- and
three-component macOS versions identically.

Physical Intel Mac diagnostics confirmed a compatible system Python, the local
Apple C++ toolchain and SDK, all required standard utilities, both thin compile
probes, and native execution without installing software or changing host
configuration. The first probe exposed an incompatible `lipo` argument order.
The first real link then exposed that resolving the compiler symlink changed
the requested `clang++` driver into `clang`, which omitted the C++ runtime at
link time. The source now places each `lipo` input before `-verify_arch`,
preserves the requested C++ driver basename, and rejects both regressions in
source consistency.

After those corrections, the historical physical build log confirms the
`single`-precision `x86_64` and `arm64` thin links, Universal 2 structural
validation, a suite 1 self-test with all seven datasets and 27 malformed inputs,
and the native Intel self-test. This is accepted partial infrastructure evidence
from the current dirty source; it is not a benchmark report and does not close
the macOS qualification gate.

All current Mac work is deferred under ADR 0036. In final development, the
suite 2 `double` and `single` candidate pairs, package checksums,
execution-only validation, Universal 2 structure, and Gatekeeper observation
must be completed. Raw host inventory and build logs remain private and are
represented here only by privacy-safe conclusions.

The 16-pair, 32-report clean-tree Linux/Windows/Android selection matrix and the
final two-pair, four-report macOS gate in `BENCHMARKS.md` remain pending.

The earlier C++ policy refactor replaced module `std::array` use and the module
lambda, removed standalone benchmark exception handling, and added compiler and
source guards. Its then-current editor, template, sanitizer, and suite 1
acceptance remains historical evidence. The current suite 2 normal matrix,
standalone sanitizer evidence, and fresh integrated ASAN and UBSAN matrix are
complete. The clean-tree cross-platform qualification matrix remains pending.

## Retiring external sanitizer rules

Each Godot 4.7.1 source-scoped or function-specific rule must be removed when
one of the following is available and an unsuppressed acceptance run confirms
the diagnostic is gone:

- the applicable upstream correction is present in the baseline;
- an official Godot build path removes the external occurrence without losing
  meaningful integration coverage;
- an engine-baseline update removes the diagnostic.

Broad, wildcard, module, and path-prefix suppressions remain unacceptable.

## License and contribution responsibility

The project is MIT licensed under Malkverbena, 2026. AI-assisted work is allowed only when the human developer understands, validates, and can maintain the contribution.

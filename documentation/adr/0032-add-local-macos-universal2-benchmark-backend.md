# ADR 0032: Add a local macOS Universal 2 benchmark backend

## Status

Accepted. Public distribution is constrained by ADR 0034; ADR 0036 defers
execution to the final portability gate, and ADR 0037 advances the shared
methodology to suite 2.

## Context

The benchmark qualification matrix needs macOS evidence without changing the
shared datasets, candidate, timed regions, or report schema. The available Mac
uses Intel hardware and macOS 12.7.6. Build automation on a hosted runner is not
approved for this phase, and the target execution environment must not require
a compiler, Homebrew, an SDK, SCons, Python, Git, or project sources.

macOS does not expose a public hard logical-CPU affinity contract equivalent to
Linux `sched_setaffinity` or Windows processor-group affinity. A Mach thread
affinity tag is a scheduler hint for grouping related threads, not proof that a
benchmark remained on one requested logical processor. Reporting it as verified
hard affinity would make provenance misleading.

Apple documents Universal 2 as separate architecture builds merged into one
executable with `lipo`. Apple also documents `otool -L` as the native mechanism
for inspecting linked runtime libraries.

## Decision

The standalone benchmark gains a fourth backend under the existing
`benchmarks/SConstruct` graph:

- `platform=macos`, `toolchain=apple-clang`;
- thin `x86_64` and `arm64` builds with C++17 and a default macOS 12.0
  deployment target;
- one Universal 2 executable per precision produced by merging the two linked
  thin executables with `/usr/bin/lipo`;
- native self-tests on the build host and structural verification of both
  slices;
- `/usr/bin/otool -L` rejection of any dependency outside `/usr/lib` and
  `/System/Library`.

The build runs locally on macOS and may use Xcode or Apple Command Line Tools,
Python, SCons, and Git. Those are build-host requirements only. A private
qualification ZIP contains Universal 2 executables, the execution-only runner,
system-library inspection records, integrity hashes, metadata, the license, and
instructions. The target Mac uses only standard macOS runtime utilities. ADR
0034 prohibits publishing that ZIP or any macOS executable in GitHub releases.

macOS reports preserve schema 3 and use the current shared benchmark suite. The
existing affinity fields encode one exact platform policy:

```text
runtime_backend=macos-native
logical_cpu=unbound
cpu_class=representative
processor_group=unsupported
affinity_requested=no
affinity_applied=no
affinity_actual_cpu=unknown
affinity_error=unsupported-by-platform-policy
```

Only a macOS-compiled executable and the report verifier may accept this exact
combination for official eligibility. Linux, Windows, and Android continue to
require requested, applied, and verified hard affinity. The macOS runner rejects
Rosetta execution so the available architecture runs natively.

The available Intel Mac provides the required native `x86_64` single- and
double-precision candidate pairs. Its `arm64` slices are structurally validated
but do not count as executed Apple Silicon evidence. ADR 0036 defines the
current matched-pair counts and places this execution in the final portability
gate.

The build adds no explicit signing or notarization step. It records whether the
resulting executable has a detectable signature, while the runner records only
whether quarantine is present and attempts normal execution without removing
it. A deliberate signing or notarization policy will be considered only after
observing actual Gatekeeper behavior on the target Mac.

## Consequences

- macOS joins the supported standalone benchmark platforms without introducing
  another build system.
- The Universal 2 package can be copied to Intel or Apple Silicon machines, but
  only natively executed slices count as performance evidence.
- macOS results represent the system scheduler rather than a selected core and
  must not be compared as if a logical CPU had been pinned.
- Compiler and SDK tooling remain isolated to the local build host.
- Public GitHub distribution remains source-only; generated binaries and
  packages are private qualification artifacts.
- The runner and reports avoid host names, user names, serial identifiers,
  mounted-volume paths, and other irrelevant machine inventory.
- Gatekeeper evidence remains observable instead of being preempted by an
  unproven signing or notarization requirement.

## References

- [Apple: Building a universal macOS binary](https://developer.apple.com/documentation/apple-silicon/building-a-universal-macos-binary)
- [Apple: Compiling Your Code in OS X](https://developer.apple.com/library/archive/documentation/Porting/Conceptual/PortingUnix/compiling/compiling.html)
- [Apple: Building from the Command Line with Xcode FAQ](https://developer.apple.com/library/archive/technotes/tn2339/_index.html)

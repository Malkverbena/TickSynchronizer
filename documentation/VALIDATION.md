# Validation status

## Identification

- Godot: `4.7.1-stable`
- platform: Linux x86_64
- build precisions: `single` and `double`
- public API: 4
- experimental wire: 0 revision 2
- benchmark suite: 1

## Source consistency

Current source-consistency target:

```text
methods=41
tests=140
mermaid_diagrams=22
mermaid_files=11
```

## Current complete normal build matrix

The current wire revision 2 source passed in both precision modes:

- editor build;
- 140/140 C++ tests;
- 66,999/66,999 assertions;
- GDScript smoke markers;
- `template_debug`;
- `template_release`.

## Sanitizers

The current wire revision 2 module-focused C++ suites pass 140 tests and 66,999
assertions in both precisions. The full editor smoke under UBSAN stops in Godot
4.7.1 bundled SDL/HIDAPI initialization. The stack does not enter
TickSynchronizer.

Accepted temporary gate:

```bash
./scripts/run_sanitized_tests.sh double --no-smoke
./scripts/run_sanitized_tests.sh single --no-smoke
```

Normal smoke remains mandatory.

## Benchmark infrastructure

The standalone suite passes its self-test and validates:

- deterministic datasets;
- semantic round-trips;
- deterministic encoding;
- 27/27 malformed packet rejections;
- non-zero diagnostic checksums;
- JSON and CSV schema 3 consistency;
- native CPU affinity verification on Linux, Windows, and Android;
- native logical CPU, L3-domain, NUMA, and SMT topology discovery;
- cross-platform device, OS, SoC, and backend provenance;
- executable and environment provenance;
- one SCons compilation graph for all three target platforms;
- precision-separated comparator baselines and report identities;
- execution-only packages for test machines without development environments.

Official benchmark baselines must be generated from a clean tree with explicit CPU affinity.

The selected quick reports passed in both precisions:

- Linux and Windows x86_64 across distinct L3 domains;
- Android ARM64 across recent and older devices and their exposed core classes
  under controlled power settings.

Every selected report used schema 3, recorded requested and applied affinity,
completed all seven datasets, and rejected 27/27 malformed packets while
accepting zero. Results without confirmed environmental controls are excluded
from qualified performance comparisons.

The selected reports are still preliminary because the source state was dirty
and each report records `official=no`. Local evidence archives retain their
integrity hashes outside the public source tree.

A second Windows x86_64 machine and all clean-tree official reports remain
pending.

## Closing the external sanitizer issue

The temporary exception can be closed when one of the following is available:

- an upstream SDL/HIDAPI correction;
- an official Godot build option that avoids unrelated subsystem initialization;
- an isolated module smoke harness that preserves meaningful integration coverage;
- an engine-baseline update that removes the diagnostic.

Broad suppressions are not acceptable.

## License and contribution responsibility

The project is MIT licensed under Malkverbena, 2026. AI-assisted work is allowed only when the human developer understands, validates, and can maintain the contribution.

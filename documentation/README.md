# TickSynchronizer documentation

This directory separates the module manual from architecture decisions and
version-bound development records.

## Module manual

These documents define behavior, contracts, integration, validation methods,
and other information required to understand and maintain TickSynchronizer:

- [`ARCHITECTURE.md`](ARCHITECTURE.md): component boundaries and ownership;
- [`BINARY_BUFFER.md`](BINARY_BUFFER.md): binary storage and codec invariants;
- [`PROTOCOL.md`](PROTOCOL.md): wire contracts and handshake behavior;
- [`GODOT_COMPATIBILITY.md`](GODOT_COMPATIBILITY.md): engine and precision compatibility;
- [`BUILD.md`](BUILD.md): supported layouts and build procedures;
- [`TESTING.md`](TESTING.md): test strategy and acceptance gates;
- [`SMOKE_TEST.md`](SMOKE_TEST.md): editor integration smoke test;
- [`BENCHMARKS.md`](BENCHMARKS.md): benchmark methodology and execution;
- [`BENCHMARK_DECISIONS.md`](BENCHMARK_DECISIONS.md): evidence-based protocol decisions;
- [`PRIVACY.md`](PRIVACY.md): publication boundaries and privacy review checklist;
- [`FUZZING.md`](FUZZING.md): packet fuzzing and sanitizer requirements.

These versioned pages remain authoritative for every source state. After the
current qualification gate closes, derived navigation, tutorials, platform
walkthroughs, and frequently asked questions may be published in the project
Wiki. Wiki publication does not replace the repository copy.

## Architecture decisions

[`adr/`](adr/) contains versioned decisions that explain why technical
contracts and constraints exist. ADRs remain coupled to the source history.

## Development records

[`development/`](development/) contains current implementation state, phased
planning, and validation evidence. These records describe a particular stage
of development and do not define module behavior.

The GitHub Project owns actionable backlog status, priority, assignment,
scheduling, milestones, and operational views. The strategic phase map and
source-specific evidence remain versioned here. ADR 0035 defines the complete
repository, Project, and Wiki responsibility split.

ADR 0039 records the selected default scalar profile and the explicit Android
evidence waiver. ADR 0040 separates the Godot 4.4+ compilation range from the
exact qualified validation baseline.

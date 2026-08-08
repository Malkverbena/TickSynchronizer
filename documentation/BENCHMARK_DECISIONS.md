# Decisions derived from protocol benchmarks

## Purpose

This document records what benchmark evidence has established, what remains undecided, and the path contributors must follow before changing the wire strategy. It prevents later contributors from treating a preliminary result or a single-machine win as an architectural decision.

## Current decision state

The production realtime protocol has **not** been selected.

Current contract:

```text
public API: 4
wire protocol: 0 (experimental)
wire revision: 2
benchmark suite: 1
reference candidate: reference_fixed_width
```

The fixed-width candidate is a control baseline. It provides predictable CPU cost and a transparent wire-size floor for uncompressed fixed fields; it is not a default winner.

## Evidence already obtained

Initial Linux x86_64 measurements showed:

- deterministic output and successful semantic round-trips;
- rejection of the complete malformed-packet corpus;
- low run-to-run dispersion under CPU affinity;
- encode paths with no per-message allocation after reservation;
- throughput that scales approximately with bytes processed;
- materially smaller snapshots in `single` builds when the reference format carries 32-bit instead of 64-bit floating-point fields.

These observations validate the harness and characterize the reference candidate. They do not establish a final wire format.

The current source completed quick qualification on Linux and Windows x86_64
and on Android ARM64 across recent and older devices. Across all selected
runs, the reference candidate preserved deterministic output, completed all
seven datasets, and rejected all 27 malformed packets while accepting zero.

The preliminary comparisons also establish that Android core class materially
changes measured latency, while the tested desktop L3 domains are much closer
for this workload. Only runs with confirmed environmental controls enter
performance comparisons. These observations characterize the harness and
platform sensitivity; they do not select the production wire protocol.

All current reports were built from a dirty source tree and record
`official=no`. A second Windows machine is unavailable. Therefore the current
reports remain infrastructure and characterization evidence rather than
official candidate-selection evidence.

## Methodological decisions

1. Official reports require a clean Git tree.
2. Official reports require explicit CPU affinity.
3. Quick reports qualify infrastructure only and are not selection evidence.
4. Encode and decode are measured independently.
5. Candidate correctness is evaluated before performance.
6. All candidates receive identical semantic datasets and seeds.
7. Results must include absolute measurements; weighted scores may not hide regressions.
8. The benchmark suite version changes when methodology or dataset semantics change, not when a candidate is added.

## Cross-platform requirement

No protocol may be stabilized using results from only one architecture or operating system.

The initial evidence matrix is:

- Linux x86_64 across distinct L3 domains;
- Windows x86_64 across distinct L3 domains;
- Android ARM64 on a recent device across exposed core classes;
- Android ARM64 on an older device across exposed core classes;
- Windows x86_64 on a second machine when one becomes available.

The Android devices represent different performance generations. Windows adds
scheduler, allocator, timer, compiler, and CPU diversity even though it remains
x86_64.

The Android build/run backend and the Linux-to-Windows cross-build plus execution backend are implemented under one SCons graph and report schema 3. Execution-only packages carry prebuilt binaries and runners to machines without development environments. The first official Windows baseline uses MinGW-w64 GCC explicitly; LLVM-MinGW is reserved for a later compiler-sensitivity comparison so compiler choice does not vary silently between runs. Quick execution has now validated these paths on the available target machines, but only clean-tree reports archived with hashes may enter the protocol decision.

Multi-domain hosts must not be represented by an unidentified logical CPU.
Linux and Windows qualification records the L3 domain and runs one primary
hardware thread from each relevant domain. Reports without complete topology
remain execution diagnostics only. The comparator maintains independent
`single` and `double` baselines and never computes a ratio across precisions.

## Deferred next candidate

Do not implement an additional candidate until the current qualification gate
is explicitly closed. The available Linux, Windows, and Android quick matrix is
complete, but clean-tree official reports and the deferred second Windows
machine are still outstanding.

When that gate is complete, the next candidate should isolate one variable:

```text
varint_zigzag_fixed_float
```

It should use:

- ULEB128 for unsigned integers;
- ZigZag plus ULEB128 for signed integers;
- the same framing as the reference candidate;
- the same floating-point representation as the reference candidate;
- identical validation limits and semantic outputs.

Do not combine varints, delta encoding, bit packing, and quantization in the same first comparison. A mixed candidate would make it impossible to attribute a gain or regression to one technique.

## Candidate progression

After fixed-width versus varint evidence exists on all initial platforms, later candidates may evaluate:

1. delta plus varint;
2. change masks and bit packing;
3. quantized numeric fields;
4. hybrid field-specific encoding;
5. stateful snapshot references.

Each candidate must state which independent variable it introduces.

## Elimination criteria

A candidate is eliminated regardless of speed if it:

- fails a round-trip or deterministic-output test;
- accepts a malformed input that the contract rejects;
- permits attacker-controlled unbounded allocation;
- depends on host endianness, padding, ABI, or `real_t` layout;
- cannot provide canonical encoding;
- cannot evolve without ambiguous parsing;
- produces unexplained architecture-specific semantic differences.

## Selection dimensions

The final decision will consider:

- median and p95 encoded size;
- encode latency;
- decode latency, weighted more heavily for server fan-in;
- allocation count and temporary memory;
- malformed-input rejection cost;
- implementation complexity and auditability;
- extensibility and compatibility strategy;
- consistency across CPU architectures, core classes, and thermal regimes.

A candidate that wins narrowly on one desktop but regresses severely on older ARM cores should not be selected without a compelling workload-specific reason.

## What contributors must not assume

- API version 4 does not mean wire version 4.
- Control envelope version 1.1 is not the production realtime packet format.
- Fixed-width is not selected merely because it is implemented first.
- Smaller packets are not automatically faster.
- Desktop peak throughput does not represent mobile sustained performance.
- `single` and `double` benchmark results must not be merged into one aggregate.
- Preliminary or dirty-tree reports must not be used as official baselines.

## Decision record procedure

When evidence supports a protocol path:

1. archive official reports and their hashes;
2. update this document with the comparison and trade-offs;
3. create or revise an ADR for the selected wire decision;
4. increment `WIRE_PROTOCOL_VERSION` only when a stable incompatible contract is declared;
5. add golden packets and compatibility tests before accepting external gameplay traffic.

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
benchmark suite: 2
reference candidate: reference_fixed_width
screened integer candidate: varint_zigzag_fixed_float
```

The fixed-width candidate remains the control baseline. Canonical ULEB128 and
ZigZag have passed the suite 2 Linux screen and are selected as the integer
primitive to carry into cross-platform qualification. This bounded decision is
not selection of framing, state references, change masks, quantization,
transport behavior, or the complete production packet format.

## Evidence boundary

Source-specific measurements and preliminary observations belong in
[`development/VALIDATION.md`](development/VALIDATION.md). They may reject a
candidate, qualify methodology, or support a bounded design screen, but they do
not stabilize the production wire. Candidate selection requires the clean-tree
Linux, Windows, and Android matrix; stabilization additionally requires the
deferred final macOS gate in ADR 0036.

## Methodological decisions

1. Official reports require a clean Git tree.
2. Official reports require verified hard CPU affinity, except for the exact
   scheduler-managed macOS policy in ADR 0032.
3. Preliminary reports may screen a primitive but cannot close the official
   candidate-selection or stabilization gates.
4. Encode and decode are measured independently.
5. Candidate correctness is evaluated before performance.
6. All candidates receive identical semantic datasets and seeds.
7. Results must include absolute measurements; weighted scores may not hide regressions.
8. The benchmark suite version changes when methodology or dataset semantics change, not when a candidate is added.
9. Candidate reports are evaluated as matched reference/contender pairs with
   the same executable, precision, hardware identity, and execution policy.
10. Coverage-only boundary datasets cannot dominate workload-weighted gates.

## Cross-platform requirement

No protocol may be stabilized using results from only one architecture or
operating system. ADR 0036 defines two gates:

- candidate selection: 16 matched pairs, or 32 reports, across Linux x86_64,
  Windows x86_64, and two Android ARM64 generations;
- final portability: two additional macOS x86_64 pairs, or four reports, for a
  complete total of 18 matched pairs and 36 reports.

The Android devices represent different performance generations. Windows adds
scheduler, allocator, timer, compiler, and CPU diversity even though it remains
x86_64.

Android, Linux-to-Windows cross-build, and local macOS Universal 2 targets share
one SCons graph and report schema 3. Private execution-only packages carry
prebuilt binaries and runners to qualification machines without development
environments. Public GitHub distribution remains source-only. The first
official Windows baseline uses MinGW-w64 GCC explicitly; LLVM-MinGW is reserved
for a later compiler-sensitivity comparison so compiler choice does not vary
silently between runs. Only clean-tree reports archived with hashes may enter
the protocol decision.

Multi-domain hosts must not be represented by an unidentified logical CPU.
Linux and Windows qualification records the L3 domain and runs one primary
hardware thread from each relevant domain. Reports without complete topology
remain execution diagnostics only. The comparator maintains independent
`single` and `double` baselines and never computes a ratio across precisions.

## Current bounded decision

ADR 0037 accepts benchmark suite 2 and candidate 2. The final available Linux
diagnostic produced these workload results:

| Precision | Byte reduction | Weighted encode | Weighted decode | Verdict |
|---|---:|---:|---:|---|
| `double` | 28.2% | 0.825x | 0.974x | pass |
| `single` | 39.8% | 0.806x | 0.985x | pass |

Every workload became smaller. The largest measured workload latency regression
was 1.090x for dense-snapshot decode in `single`; all other final workload
ratios were at or below 1.005x. All remain below the predeclared limits.
Correctness, exact error, bounds, canonicality, and sanitizer gates passed.

This evidence selects canonical ULEB128/ZigZag for further qualification. The
clean-tree 16-pair Linux/Windows/Android matrix remains necessary before the
complete candidate decision can close. macOS is not inferred; it remains the
final blocking portability gate.

## Candidate progression

Later candidates may evaluate:

1. delta plus varint;
2. change masks and bit packing;
3. quantized numeric fields;
4. hybrid field-specific encoding;
5. stateful snapshot references.

Each candidate must state which independent variable it introduces.

The list is intentionally blocked until representative ordered snapshot
captures, loss/reorder scenarios, baseline recovery policy, and numeric error
budgets exist. Additional synthetic distributions would not resolve those
stateful and lossy decisions.

## Elimination criteria

A candidate is eliminated regardless of speed if it:

- fails a round-trip or deterministic-output test;
- accepts a malformed input that the contract rejects;
- reports the wrong error category or mutates output on failure;
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
- malformed-input rejection cost on an identical, separately identified common
  corpus (candidate-specific attack corpora remain correctness-only evidence);
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
- Passing the integer screen does not select the full realtime packet format.
- macOS is deferred, not waived; a final Mac failure reopens the decision.

## Decision record procedure

When evidence supports a protocol path:

1. archive every matched official report pair and its hashes;
2. run the predeclared candidate analyzer without preliminary overrides;
3. update this document with the comparison and trade-offs;
4. create or revise an ADR for the selected wire decision;
5. complete the final macOS portability gate before stabilization or release;
6. increment `WIRE_PROTOCOL_VERSION` only when a stable incompatible contract is declared;
7. add golden packets and compatibility tests before accepting external gameplay traffic.

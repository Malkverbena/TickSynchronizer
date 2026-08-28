# ADR 0037: Adopt suite 2 and screen canonical varint integers

## Status

Accepted; default-profile decision completed by ADR 0039.

## Context

Benchmark suite 1 qualified the standalone infrastructure, but its snapshot
generator described integer deltas while frequently drawing full-width random
64-bit values. That distribution disproportionately favored fixed-width
integers. Its malformed corpus also depended on a small set of fixed offsets
instead of deriving common corruptions from every dataset. Those reports cannot
decide whether canonical variable-length integers suit gameplay workloads.

The first additional candidate must isolate integer representation. Combining
varints, delta state, masks, bit packing, and numeric quantization would prevent
attribution of any size or latency change.

## Decision

Advance the methodology to benchmark suite 2. Suite 2 keeps report schema 3 and
the existing timed operations, provenance, statistics, affinity rules, and
official sampling profile. It changes the semantic corpus and correctness
gates by:

- defining sparse, medium, dense, and sequential gameplay integer profiles;
- adding exact ULEB128 and ZigZag transition values in
  `integer_boundaries`;
- retaining `numeric_extremes` as coverage-only evidence;
- deriving truncation and trailing-data cases from one valid packet in every
  dataset;
- requiring exact decode error categories and atomic failure;
- comparing a candidate-independent canonical semantic hash.
- rejecting message-kind fields that have no canonical wire meaning while
  preserving the caller's output buffer on encode failure.

Candidate 2, `varint_zigzag_fixed_float`, changes only integer coding:

- kind and subtype remain fixed bytes;
- unsigned integers use minimal canonical ULEB128;
- signed integers use portable ZigZag followed by canonical ULEB128;
- framing, resource limits, semantic fields, and scalar widths remain aligned
  with the reference candidate.

Selected-precision scalars have one explicit canonical contract. Targets must
provide IEEE 754 binary32 and binary64 layouts. Bytes are little-endian. Every
NaN encodes as positive quiet NaN `0x7fc00000` or
`0x7ff8000000000000`; every other NaN payload is rejected during decode.
Binary64-to-binary32 conversion uses round-to-nearest, ties-to-even independent
of the host floating-point environment. It preserves signed zero and
subnormals, and maps overflow to the matching signed infinity.

The screening limits were fixed before measurement:

- at least 5% fewer workload bytes;
- no workload dataset may increase in size;
- weighted encode and decode latency must each remain at or below 1.50x;
- every individual workload encode and decode ratio must remain at or below
  2.00x;
- every correctness, determinism, canonicality, bounds, and malformed-input
  gate must pass.

Malformed-input rejection counts and aggregate timings are not selection
ratios: suite 2 includes candidate-specific attacks, so those measurements do
not share identical inputs. They remain correctness evidence unless a future
study supplies a separately identified common corpus.

The final available Linux diagnostic used 15 measured rounds and 50 ms minimum
samples under verified CPU 0 affinity. Workload results were:

| Precision | Bytes | Size ratio | Weighted encode | Weighted decode |
|---|---:|---:|---:|---:|
| `double` | 480,120 -> 344,785 | 0.718x | 0.825x | 0.974x |
| `single` | 340,344 -> 205,009 | 0.602x | 0.806x | 0.985x |

Both pairs pass the screen. The geometric means are 0.658x for workload size,
0.815x for weighted encode latency, and 0.979x for weighted decode latency.
Accordingly, canonical ULEB128/ZigZag is selected as the integer primitive to
carry into cross-platform qualification and subsequent protocol design. This
is not selection or stabilization of the complete production realtime wire
protocol.

ADR 0039 later adopts `varint_zigzag_fixed_float` as the default profile for
subsequent gameplay protocol design after the official cross-platform
qualification campaign. The complete realtime wire remains experimental.

Stateful delta encoding, change masks, bit packing, and quantization remain
undecided. They require real ordered snapshot traces, explicit loss and reorder
models, baseline-recovery policy, and numeric error budgets. Synthetic guesses
must not be used to select those features.

## Consequences

- Suite 1 reports remain useful only as infrastructure evidence and are not
  numerically comparable with suite 2.
- Candidate selection now uses the matched-pair matrix in ADR 0036.
- The selected integer primitive has deterministic edge behavior for all
  64-bit values, NaNs, infinities, signed zero, and binary32 rounding
  boundaries.
- The production wire version 0 revision 2 remains experimental; no packet
  compatibility claim changes in this work unit.
- The next stateful or lossy candidate is blocked on representative capture
  data and a written error/recovery contract, not on additional synthetic
  microbenchmarks.

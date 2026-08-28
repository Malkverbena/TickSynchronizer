# ADR 0039: Adopt varint/ZigZag with fixed-width floats as the default profile

## Status

Accepted.

## Context

ADR 0037 selected canonical ULEB128 and ZigZag for cross-platform
qualification without selecting a default protocol profile. ADR 0036 required
16 matched Linux, Windows, and Android pairs before that bounded selection
could close, followed by a separate blocking macOS portability gate.

The authenticated suite 2 campaign produced 14 complete matched pairs:

- four Linux x86_64 pairs, all passing;
- four Windows x86_64 pairs, all passing; the raw delivery envelope failed
  only after measurement because its PowerShell validator collapsed valid
  processor-group identities;
- four Galaxy ARM64 pairs, all passing after a controlled requalification of
  the contested efficiency/double pair;
- two Redmi ARM64 efficiency-core pairs, both passing.

Across the two measured Redmi pairs, the geometric-mean workload ratios were
0.658x for size, 0.877x for weighted encode latency, and 0.956x for weighted
decode latency. The Redmi performance-core `single` and `double` pairs were not
completed. The maintainer explicitly accepted that gap as an informed waiver,
based on the consistent direction of every completed platform and core-class
pair. The missing pairs are not represented as measurements.

## Decision

Adopt `varint_zigzag_fixed_float` as the default scalar-encoding profile for
subsequent gameplay protocol design:

- kind and subtype use fixed bytes;
- unsigned integers use minimal canonical ULEB128;
- signed integers use portable ZigZag followed by minimal canonical ULEB128;
- floating-point scalars retain the explicit canonical fixed-width IEEE 754
  rules from ADRs 0009 and 0037;
- framing, state references, change masks, delta recovery, quantization, and
  transport behavior remain separate future decisions.

This decision amends the Android completion requirement in ADR 0036 only for
the two missing Redmi performance-core pairs. It does not waive the macOS gate.
The final native macOS suite 2 pairs, Universal 2 validation, packaging checks,
and Gatekeeper observation remain blocking for the first public milestone and
for any claim of complete initial-platform portability.

Selecting this profile does not stabilize the production realtime wire. Wire
version 0 revision 2 remains experimental because gameplay packet types,
stateful synchronization, loss/reorder recovery, and compatibility golden
vectors are not yet complete.

## Consequences

- New protocol design uses `varint_zigzag_fixed_float` unless a later ADR
  supplies stronger representative evidence for a replacement.
- The bounded Linux/Windows/Android candidate-selection stage is closed with
  14 measured passing pairs and two explicitly waived, unmeasured Redmi pairs.
- Documentation and reports must preserve the distinction between measured
  evidence, corrected post-measurement validation, and the informed waiver.
- No claim of complete Android core-class coverage may include the waived
  Redmi performance pairs.
- macOS remains deferred and blocking; failure there reopens the affected
  portability or performance decision.
- Wire version 0, revision 2, API version 4, and benchmark suite 2 do not change.

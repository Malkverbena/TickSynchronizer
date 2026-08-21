# ADR 0036: Separate candidate selection from final macOS qualification

## Status

Accepted.

## Context

Protocol-candidate work must continue while the physical Mac is unavailable.
The earlier qualification plan treated one fixed-width report per precision and
platform configuration as the complete evidence unit and counted 18 reports.
Suite 2 compares two candidates, so a meaningful evidence unit is now a matched
pair of reports produced by the same executable, precision, machine, and CPU
execution policy.

Deferring macOS cannot justify claiming Apple portability from Linux results.
It also does not need to prevent bounded integer-coding decisions when Linux,
Windows, and Android cover two operating-system families, two instruction-set
architectures, multiple compilers, desktop cache domains, and mobile core
classes.

## Decision

Candidate selection and final platform stabilization use two explicit gates.

The candidate-selection gate contains these eight execution identities:

- two Linux x86_64 L3 domains;
- two Windows x86_64 L3 domains;
- efficiency and prime cores on the recent Android ARM64 device;
- efficiency and performance cores on the older Android ARM64 device.

Each identity is measured in `single` and `double` precision. Each measurement
requires one `reference_fixed_width` report and one
`varint_zigzag_fixed_float` report from the same benchmark executable. The
reports must use identical sampling configuration and be generated within one
hour by the same uninterrupted runner invocation. The selection gate therefore
contains 16 matched pairs, or 32 reports.

macOS is deferred to the final development portability gate. The available
Intel Mac contributes one scheduler-managed identity in both precisions: two
additional matched pairs, or four reports. Universal 2 `arm64` slice validation
remains structural evidence until native Apple Silicon execution is available.
The complete initial-platform matrix is consequently 18 matched pairs, or 36 candidate reports.

The macOS gate is blocking for wire stabilization and the first public
milestone. A semantic, compiler, linkage, packaging, Gatekeeper, or material
performance failure on macOS blocks release and reopens the affected decision;
it may not be waived by non-Apple results. No macOS command is required during
the current candidate-selection phase.

This decision supersedes the old report counts and ordering in ADRs 0029, 0032,
and 0035. Their toolchain, execution-policy, privacy, packaging, and
source-distribution requirements remain in force.

## Consequences

- Linux, Windows, and Android can close a bounded candidate-selection decision
  without waiting for temporary Mac availability.
- Report accounting cannot confuse an individual report with a matched
  candidate comparison.
- macOS remains a real portability and release gate rather than being silently
  removed from supported-platform evidence.
- Existing suite 1 Mac results remain historical infrastructure evidence only;
  they are not suite 2 candidate evidence.
- Wire version 0 and revision 2 remain unchanged until a production packet
  contract is implemented and accepted.

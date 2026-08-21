# ADR 0038 — Keep LSAN enabled with a Godot joypad suppression

## Status

Accepted

## Context

The current module ASAN pass completed its build and C++ test stage, then the
real headless GDScript smoke exited during leak checking. The report contained
6,991 leaked bytes in 172 allocations. Its symbolized stacks entered bundled
SDL joypad and Linux UDEV code through `JoypadSDL::initialize`; no stack entered
TickSynchronizer.

An exact Godot 4.7.1 ASAN editor built without the module reproduced the same
6,991-byte, 172-allocation report. Restricting the SDL device policy did not
change the allocation set. This establishes an engine/SDL baseline occurrence
rather than a module-owned leak.

A temporary function-specific rule suppressed exactly those 172 allocations.
With the same rule active and `detect_leaks=1`, an unrelated SCons-built probe
still reported one 4,096-byte leak and returned the required LSAN status 23.
The existing module ASAN binary then completed the real double-precision smoke
with every required marker and the exact reviewed suppression total.

A separate immediate `--editor --quit` diagnostic exposed an upstream
use-after-free during Godot editor cleanup. That artificial path is not the
accepted smoke command. An LSAN leak rule cannot suppress an AddressSanitizer
use-after-free, and that diagnostic does not justify any additional rule.

## Decision

Keep LeakSanitizer enabled in every accepted ASAN pass and append
`detect_leaks=1` after inherited ASAN options. Accept exactly this
Godot-4.7.1-specific rule:

```text
leak:JoypadSDL::initialize
```

Store it separately from UBSAN policy in
`scripts/sanitizer_suppressions/godot-4.7.1-lsan.supp`. During each
uninterrupted sanitizer acceptance batch that uses it, require one full guard
to:

1. reject any active rule set other than the exact one-rule policy;
2. build an unrelated C++17 leak probe through SCons with ASAN enabled;
3. require the 4,096-byte allocation to remain visible and fatal with status 23;
4. reject any unexpected match of the Godot rule in the unrelated report.

Before every later ASAN or combined pass in the same batch, revalidate the
exact static one-rule policy without rebuilding the identical unrelated probe.
A standalone single-precision invocation is its own batch and therefore runs
the full guard. The accepted `all` invocation runs `double` and `single`
serially and shares only that batch-independent negative control.

The no-module Godot reproduction is retained as decision evidence rather than
rebuilt before every pass. Disabling the reviewed rule is diagnostic only;
disabling leak detection cannot satisfy the accepted gate.

Do not patch Godot or SDL. Reevaluate and preferably remove the rule when the
engine baseline, bundled SDL revision, or sanitizer toolchain changes.

## Consequences

- The real editor smoke remains part of the ASAN gate.
- Leak detection remains active for TickSynchronizer and all unrelated code.
- A broad library, source-prefix, wildcard, or module suppression remains
  forbidden.
- The exact no-module reproduction and unrelated negative control distinguish
  an external baseline allocation set from module leaks.
- The full unrelated negative control runs once per uninterrupted acceptance
  batch, while the exact rule is checked before each applicable pass.
- Non-leak ASAN findings remain fatal and cannot match this LSAN rule.

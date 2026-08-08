# ADR 0029: Cross-platform benchmark backends

## Status

Accepted for benchmark suite version 1.

## Context

Protocol selection must not depend on one compiler, operating system, or CPU architecture. The deterministic benchmark core already separates semantic datasets and candidate code from Godot and transport startup, so the same executable can be built for additional native platforms without changing measured regions.

## Decision

The benchmark suite uses one shared C++17 core and one `benchmarks/SConstruct` compilation graph with platform-specific compiler and execution backends:

- Linux x86_64 through the native compiler selected by SCons;
- Windows x86_64 through Linux-hosted SCons cross-compilation with MinGW-w64 or LLVM-MinGW and an execution-only PowerShell runner;
- Android ARM64 through SCons, the Android NDK Clang driver, and ADB.

CPU affinity is applied inside the native executable. Linux and Android use
`sched_setaffinity`; Windows uses processor-group-aware thread affinity. The
native executable also enumerates processor group and number, core, package,
NUMA node, L3 identity and size, and SMT siblings. Linux may select a primary
hardware thread by sysfs L3 ID; Windows presents the native L3 domains and
requires an explicit flat CPU index. This makes directed multi-CCD runs
possible without assuming stable logical CPU numbering. Official reports are
eligible only when the requested affinity succeeds.

The report format advances to schema 3 to record runtime backend, device identity, OS build, SoC, CPU class, processor group, and verified affinity state. `BENCHMARK_SUITE_VERSION` remains 1 because datasets, correctness gates, timed regions, statistics, and candidate semantics are unchanged.

Android binaries use the NDK target-specific Clang driver and static libc++ so one executable can be pushed to `/data/local/tmp`. The ADB runner records device properties, CPU topology, battery state, and thermal zones before and after execution. Windows binaries are cross-compiled on Linux with MinGW-w64 or LLVM-MinGW. Self-contained deployment packages carry prebuilt executables, runners, hashes, and instructions to Linux, Windows, and Android test environments without requiring compilers, SCons, SDKs, Git checkouts, or project sources on the test machines.

## Consequences

- Linux, Windows, and Android reports can be compared under one methodology.
- Platform scripts may collect different optional environment details, but required schema fields remain stable.
- An Android or Windows port may not alter datasets or measured operations to accommodate a platform.
- Quick runs remain diagnostic; clean-tree, affinity-verified runs are required for official comparisons.
- Device and compiler differences are visible in reports instead of being inferred from file names.
- Windows source provenance is fixed at cross-build time; the execution host cannot silently rebuild a different binary.
- Report comparison creates a separate baseline for each precision and carries report-directory, OS-build, CPU-class, L3-domain, and binary-hash identity into the output.
- Generated build metadata, objects, signature databases, binaries, and deployment packages remain outside the source manifest and Git index.

# ADR 0029: Cross-platform benchmark backends

## Status

Accepted; extended by ADR 0032 for macOS, ADR 0037 for suite 2, and ADR 0036
for matched-pair gate ordering.

## Context

Protocol selection must not depend on one compiler, operating system, or CPU architecture. The deterministic benchmark core already separates semantic datasets and candidate code from Godot and transport startup, so the same executable can be built for additional native platforms without changing measured regions.

## Decision

The benchmark suite uses one shared C++17 core and one `benchmarks/SConstruct` compilation graph with platform-specific compiler and execution backends:

- Linux x86_64 through the native compiler selected by SCons;
- Windows x86_64 through Linux-hosted SCons cross-compilation with MinGW-w64 or LLVM-MinGW and an execution-only PowerShell runner;
- Android ARM64 through SCons, the Android NDK Clang driver, and ADB;
- macOS Universal 2 through local SCons thin builds, Apple Clang, and `lipo`,
  with the platform scheduling exception defined by ADR 0032.

CPU affinity is applied inside the native executable. Linux and Android use
`sched_setaffinity`; Windows uses processor-group-aware thread affinity. The
native executable also enumerates processor group and number, core, package,
NUMA node, L3 identity and size, and SMT siblings. Linux may select a primary
hardware thread by sysfs L3 ID; Windows presents the native L3 domains and
requires an explicit flat CPU index. This makes directed multi-CCD runs
possible without assuming stable logical CPU numbering. Official reports are
eligible only when the requested affinity succeeds.

The report format advances to schema 3 to record runtime backend, device identity, OS build, SoC, CPU class, processor group, and verified affinity state. ADR 0032 defines the exact scheduler-managed macOS state that may replace hard affinity only on that platform. ADR 0037 later advances the dataset and correctness methodology to suite 2 without changing schema 3 or these backend rules.

Compiler provenance uses the compiler executable name, a toolchain-relative
locator, privacy-safe flags, and the compiler-provided version. Executable
provenance uses a package-relative basename or the fixed Android remote
deployment path. Absolute build-host user directories, mounted-volume paths,
drive-qualified paths, and device serials are not report provenance. Package
export and report verification reject these host-specific paths.

Android binaries use the NDK target-specific Clang driver and static libc++ so one executable can be pushed to `/data/local/tmp`. The ADB runner records device properties, CPU topology, battery state, and thermal zones before and after execution. Windows binaries are cross-compiled on Linux with MinGW-w64 or LLVM-MinGW. Self-contained deployment packages carry prebuilt executables, runners, hashes, and instructions to Linux, Windows, Android, and macOS test environments without requiring compilers, SCons, SDKs, Git checkouts, or project sources on the test machines.

## Consequences

- Linux, Windows, Android, and macOS reports can be compared under one methodology.
- Platform scripts may collect different optional environment details, but required schema fields remain stable.
- A platform port may not alter datasets or measured operations to accommodate the platform.
- Quick runs remain diagnostic; clean-tree runs require either verified hard
  affinity or the exact macOS policy from ADR 0032 for official comparisons.
- Device and compiler differences are visible in reports instead of being inferred from file names.
- Windows source provenance is fixed at cross-build time; the execution host cannot silently rebuild a different binary.
- Report comparison creates a separate baseline for each precision and carries report-directory, OS-build, CPU-class, L3-domain, and binary-hash identity into the output.
- Build and execution provenance remains reproducible without exposing private
  build-host filesystem layout.
- Generated build metadata, objects, signature databases, binaries, and deployment packages remain outside the source manifest and Git index.
- macOS target packages require no development environment and retain
  Gatekeeper behavior for observation instead of removing quarantine.

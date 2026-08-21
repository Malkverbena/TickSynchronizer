# TickSynchronizer protocol benchmarks

This directory contains the standalone C++17 benchmark suite used to compare experimental wire-format candidates without initializing Godot or a transport backend.

The suite owns eight deterministic datasets, correctness gates, timing,
allocation tracking, statistics, matched-candidate analysis, and JSON/CSV
report generation. Candidate implementations live under
`benchmarks/candidates/` and must obey the shared semantic contract. Suite 2
compares `reference_fixed_width` with `varint_zigzag_fixed_float` in both wire
precisions.

The benchmark remains independent of Godot and may use C++ standard-library
containers. It does not use C++ exceptions or RTTI; the shared SCons graph
enforces `-fno-exceptions`, `-fno-rtti`, and a compiler-feature guard. Runtime
failures propagate through explicit result values.

Use the repository scripts instead of invoking SCons directly:

```bash
./scripts/build_protocol_benchmarks.sh --precision all --jobs 45
./scripts/run_protocol_benchmarks.sh --list-cpus
read -r -p "Linux logical CPU: " LINUX_CPU
./scripts/run_protocol_benchmarks.sh \
    --precision all --quick --cpu "$LINUX_CPU" --no-build
```

Official reports require a clean Git tree and the exact platform CPU execution
policy. See `documentation/BENCHMARKS.md` and
`documentation/BENCHMARK_DECISIONS.md`.

Linux, Windows, and Android form the candidate-selection matrix. macOS source
support remains in the shared graph, but native Mac execution is deferred to
the final blocking portability gate under ADR 0036.

## Cross-platform builds and deployment

`benchmarks/SConstruct` is the only compilation graph. Repository wrappers
select native Linux, Linux-hosted Windows cross-compilation, Android NDK Clang,
or local macOS Apple Clang without changing benchmark sources or methodology:

```text
scripts/build_protocol_benchmarks_windows_cross.sh
scripts/run_protocol_benchmarks_windows.ps1
scripts/build_protocol_benchmarks_android.sh
scripts/run_protocol_benchmarks_android.sh
scripts/build_protocol_benchmarks_macos.sh
scripts/run_protocol_benchmarks_macos.sh
```

All backends execute the same datasets and candidate code. Report schema 3
records the selected backend, device, operating system, SoC, CPU class, and CPU
execution policy. macOS Universal 2 packages use the scheduler-managed policy
from ADR 0032 instead of claiming unavailable hard affinity.

`scripts/export_protocol_benchmarks.sh` packages prebuilt executables and
execution-only runners for private qualification. A test machine never needs a
compiler, SCons, a target SDK, Git checkout, or project sources. The macOS target
additionally needs no Homebrew or Python. The Android controller needs ADB and
Python, but the Android device receives only the native executable and generated
run launcher.

Public GitHub distribution is source-only. Generated executables and deployment
packages remain outside Git and releases. macOS builders provide their own local
Apple toolchain and SDK; neither is distributed with TickSynchronizer.

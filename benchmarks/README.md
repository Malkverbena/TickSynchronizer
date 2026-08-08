# TickSynchronizer protocol benchmarks

This directory contains the standalone C++17 benchmark suite used to compare experimental wire-format candidates without initializing Godot or a transport backend.

The suite owns deterministic datasets, correctness gates, timing, allocation tracking, statistics, and JSON/CSV report generation. Candidate implementations live under `benchmarks/candidates/` and must obey the shared semantic contract.

Use the repository scripts instead of invoking SCons directly:

```bash
./scripts/build_protocol_benchmarks.sh --precision all --jobs 45
./scripts/run_protocol_benchmarks.sh --list-cpus
read -r -p "Linux logical CPU: " LINUX_CPU
./scripts/run_protocol_benchmarks.sh \
    --precision all --quick --cpu "$LINUX_CPU" --no-build
```

Official reports require a clean Git tree and explicit CPU affinity. See `documentation/BENCHMARKS.md` and `documentation/BENCHMARK_DECISIONS.md`.

## Cross-platform builds and deployment

`benchmarks/SConstruct` is the only compilation graph. Repository wrappers select native Linux, Linux-hosted Windows cross-compilation, or Android NDK Clang without changing benchmark sources or methodology:

```text
scripts/build_protocol_benchmarks_windows_cross.sh
scripts/run_protocol_benchmarks_windows.ps1
scripts/build_protocol_benchmarks_android.sh
scripts/run_protocol_benchmarks_android.sh
```

All backends execute the same datasets and candidate code. Report schema 3 records the selected backend, device, operating system, SoC, CPU class, and native affinity result.

`scripts/export_protocol_benchmarks.sh` packages prebuilt executables and execution-only runners. A test machine never needs a compiler, SCons, target SDK, Git checkout, or project sources. The Android controller needs ADB, but the Android device receives only the native executable and generated run launcher.

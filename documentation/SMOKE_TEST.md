# Editor smoke test

## Purpose

The smoke project verifies that the module registers its public classes, exposes the expected build/protocol diagnostics, and executes representative buffer operations inside a real Godot editor binary.

## Automated execution

The main validation script runs the smoke project automatically in `quick`, `editor`, and `all` modes:

```bash
./scripts/build_and_validate.sh --mode quick --precision double
```

## Manual execution

Run the built editor headlessly:

```bash
TICKSYNC_EXPECTED_PRECISION=double \
../godot/bin/godot.linuxbsd.editor.dev.double.x86_64 \
    --headless \
    --path tests/smoke_project \
    --quit-after 600
```

Use the corresponding artifact and `single` expectation for a single-precision build.

The generated `tests/smoke_project/smoke_test.gd.uid` sidecar is versioned with
the smoke project. It preserves the script resource identity and remains part
of `FILE_MANIFEST.txt` and `SHA256SUMS.txt`.

Godot may create a `.godot/` cache directory inside the smoke project. Every
file below that directory is generated, remains unversioned, and is ignored by
the source-consistency inventory without requiring cache deletion.

## Required markers

```text
TICKSYNCHRONIZER_BUILD_PRECISION=<single|double>
TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK
TICKSYNCHRONIZER_BUFFER_SMOKE_TEST_OK
TICKSYNCHRONIZER_INTEGER_CODEC_SMOKE_TEST_OK
TICKSYNCHRONIZER_FLOAT_CODEC_SMOKE_TEST_OK
TICKSYNCHRONIZER_RESOURCE_LIMIT_SMOKE_TEST_OK
TICKSYNCHRONIZER_SMOKE_TEST_OK
```

A missing marker or non-zero process status is a failure.

## Sanitized editor execution

The accepted sanitizer profile must run this same smoke project in both
precisions. Godot 4.7.1 diagnostics that were independently reproduced without
the module use one reviewed function-specific LSAN rule and three reviewed
category-and-source UBSAN rules. Leak detection remains enabled. One full LSAN
negative control runs per uninterrupted acceptance batch, and the exact LSAN
rule is rechecked before every later applicable pass. The UBSAN controls still
require unrelated bounds and alignment diagnostics to remain fatal before the
applicable editor smoke runs.

Version-bound smoke results are recorded in
[`development/VALIDATION.md`](development/VALIDATION.md).

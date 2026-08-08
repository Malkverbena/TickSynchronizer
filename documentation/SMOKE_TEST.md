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

## Sanitized editor limitation

The current wire revision 2 normal smoke matrix passes every required marker in
both precisions. The earlier interrupted sanitizer build did not reach smoke
execution and remains invalid infrastructure history; it does not replace or
invalidate the completed normal matrix.

The full UBSAN editor smoke is currently blocked during Godot's bundled
SDL/HIDAPI initialization before module smoke markers run. Use the focused
sanitizer gate documented in `TESTING.md` and `VALIDATION.md` until the external
issue is isolated or fixed.

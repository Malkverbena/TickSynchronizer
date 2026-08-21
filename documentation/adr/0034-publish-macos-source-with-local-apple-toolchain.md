# ADR 0034: Publish macOS source with a local Apple toolchain

## Status

Accepted.

## Context

TickSynchronizer supports macOS benchmark builds, but the public project does not
need to distribute prebuilt executables. Apple licenses Xcode and the Apple SDKs
for use on Apple-branded products running macOS and restricts separate use,
uploading, hosting, and redistribution of Apple software. Copying a macOS SDK to
a Linux host would create an unnecessary licensing and provenance risk.

The public project can instead publish its original source code and SCons build
instructions. Each builder can provide a locally licensed Apple toolchain and
SDK on an appropriate Mac.

## Decision

The public GitHub repository and its releases are source-only for macOS and do
not publish TickSynchronizer benchmark executables or execution packages.

The repository must never contain or distribute:

- Xcode, Apple Clang, or Apple command-line developer tools;
- an Apple SDK, extracted SDK files, headers, libraries, or copied samples;
- a public prebuilt macOS benchmark executable or deployment archive.

macOS builds run locally on Apple-branded hardware running macOS. The builder
supplies and accepts the terms for the installed Apple toolchain and SDK. Build
scripts may discover those local components through `xcrun` or accept explicit
local paths, but they must not download, copy, vendor, or upload them. Linux
cross-compilation with a copied Apple SDK is outside the supported workflow.

Universal 2 remains the preferred local output. Locally generated binaries and
execution-only packages may be used as private qualification artifacts and must remain outside Git and public releases. They must be deleted or retained according
to the responsible builder's local compliance policy. The source repository
retains packaging scripts so a responsible local build operator can perform
private hardware qualification without changing the compilation graph.

This source-publication decision does not alter the MIT license covering original
TickSynchronizer source. It also does not grant rights to Apple software or
replace the builder's obligation to review the agreement applicable to the
installed toolchain.

## Consequences

- Public distribution does not include or redistribute any Apple SDK component.
- Users who want macOS binaries compile them on their own compliant build system.
- Official macOS qualification requires an eligible local Mac build host with
  the required development tools.
- The requirement that an execution target need no development tools applies only
  to a private prebuilt qualification package, not to a source-only public user.
- Signing, notarization, and Gatekeeper behavior can be evaluated later if public
  binary distribution enters scope.

## References

- [Apple Developer agreements and guidelines](https://developer.apple.com/support/terms)
- [Xcode and Apple SDKs Agreement](https://www.apple.com/legal/sla/docs/xcode.pdf)

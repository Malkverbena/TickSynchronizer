# TickSynchronizer roadmap

## Confirmed decisions

- clean Godot 4.x version 4.4.0 or newer for compilation, with no engine
  modifications; Godot `4.7.1-stable` remains the qualified validation baseline.
- C++17; SCons is the only compilation system for the module and every standalone benchmark target.
- external custom-module and in-tree module layouts are both supported.
- `PackedByteArray` is the canonical Godot byte container.
- little-endian integers and LSB-first bit fields are explicit.
- `single` and `double` builds are supported, but peers must match.
- API, wire, wire revision, and benchmark suite versions are independent.
- exact module build, game build, schema, and canonical complete Godot version
  matching are required during experimental wire version 0; the Godot commit
  is diagnostic and warns on mismatch.
- protocol choice is benchmark-driven and cross-platform.
- the GitHub Project owns operational planning, the repository owns versioned
  contracts and evidence, and the Wiki is a post-qualification derived
  publication under ADR 0035.
- logical changes are committed only when complete.

```mermaid
flowchart LR
    Foundation[Binary foundation] --> Control[Control protocol]
    Control --> Benchmark[Benchmark methodology]
    Benchmark --> Candidate[Integer candidate comparison]
    Candidate --> Selection[Default scalar profile selected]
    Selection --> Transport[Transport lab and real captures]
    Transport --> Sync[Authoritative synchronization]
    Sync --> Prediction[Prediction and rollback]
    Prediction --> FinalMac[Final macOS portability gate]
```

## Phase 0 — Repository and engine baseline — complete

- independent module repository;
- Godot version and exact commit files;
- machine-readable Godot 4.4.0 build floor;
- engine cleanliness checks;
- MIT license and contribution policy;
- project-wide English language policy.

## Phase 1 — SCons module skeleton — complete

- conventional Godot module glue;
- public placeholder classes;
- class reference XML;
- external and in-tree layout compatibility;
- editor and template builds.

## Phase 2 — Binary buffer and scalar codecs — complete

- canonical bitstream;
- fixed integers;
- aligned canonical varints and ZigZag;
- explicit IEEE 754 `float32` and `float64`;
- sticky errors and atomic operations;
- resource limits, equality, hashing, and golden vectors;
- normal and sanitizer test coverage.

## Phase 3 — Experimental control protocol — complete for current scope

- 40-byte control envelope;
- HELLO v4 and HELLO_ACK v4;
- structured disconnects;
- compatibility profile;
- pure evaluator and state machine;
- exact API, wire, module build, precision, canonical Godot version, game,
  schema, and capability checks;
- structured diagnostic warning for a Godot commit mismatch.

## Phase 4 — Benchmark methodology — active

- deterministic standalone suite 2 implemented;
- fixed-width reference and canonical ULEB128/ZigZag candidates implemented;
- eight datasets, explicit gameplay integer profiles, boundary coverage, and
  derived malformed corpus implemented;
- canonical IEEE scalar conversion and NaN rules implemented;
- exact malformed error, atomic failure, semantic hash, and paired-analysis
  gates implemented;
- report schema 3 and cross-platform provenance implemented;
- one SCons compilation graph implemented for Linux, Windows, Android, and macOS;
- private execution-only deployment packages implemented for qualification machines without development environments;
- public GitHub distribution constrained to source-only artifacts;
- standalone benchmark exception handling and RTTI removed and disabled by the
  compiler contract;
- native Windows and Linux topology discovery implemented for directed L3-domain and multi-CCD runs;
- benchmark comparison separated by precision with explicit report identity;
- privacy-safe compiler and executable provenance enforced during report
  validation and deployment-package export;
- quick native Linux qualification completed in both precisions with verified affinity;
- extracted Linux execution-only package qualification completed;
- rebuilt quick Windows x86_64 multi-domain qualification completed in both precisions with native topology;
- quick Android ARM64 qualification completed on recent and older devices across multiple core classes under controlled power settings;
- macOS Apple Clang thin builds, Universal 2 packaging, and an execution-only
  standard-system-tools runner implemented;
- local Apple toolchain and SDK required on Apple-branded macOS hardware; copied
  SDK cross-compilation and public macOS binaries are out of scope;
- historical physical Intel Mac suite 1 `single`-precision thin builds,
  Universal 2 structural validation, and native Intel self-test retained as
  infrastructure evidence only;
- final suite 2 macOS work deferred to the blocking final portability gate;
- historical suite 1 Android device/core and Windows OS/CCD diagnostics retained
  as infrastructure evidence only;
- final Linux suite 2 preliminary matched pairs pass the bounded integer screen
  in both precisions;
- canonical ULEB128/ZigZag with fixed-width canonical floats selected as the
  default scalar profile, without selecting the complete realtime wire;
- complete integrated split ASAN and UBSAN matrix accepted in both precisions,
  including the real editor smoke and the exact LSAN and UBSAN policy guards;
- official Linux, Windows, and Android candidate selection closed by ADR 0039
  with 14 measured passing pairs and an informed waiver for two unmeasured
  Redmi performance pairs;
- `varint_zigzag_fixed_float` adopted as the default scalar profile without
  stabilizing the complete realtime wire;
- final two-pair macOS execution and portability gate pending;
- derived Wiki publication begins only after the accepted source baseline and
  final macOS portability gate; versioned documentation remains in the
  repository.

Current item status, priority, assignment, scheduling, and milestones are kept
in the GitHub Project. This file records only strategic phases, dependencies,
and acceptance gates.

```mermaid
flowchart TB
    Linux[Linux x86_64 baseline]
    Windows[Windows x86_64 machine]
    RecentAndroid[Recent Android ARM64 device]
    OlderAndroid[Older Android ARM64 device]
    Linux --> Matrix[14 measured pairs plus explicit waiver]
    Windows --> Matrix
    RecentAndroid --> Matrix
    OlderAndroid --> Matrix
    Matrix --> IntegerDecision[Default scalar profile selected]
    IntegerDecision --> Capture[Real snapshots and loss scenarios]
    Capture --> Stateful[Stateful candidate design]
    Stateful --> MacOS[Final macOS portability gate]
```

### Candidate progression

1. `reference_fixed_width` — implemented baseline;
2. `varint_zigzag_fixed_float` — implemented, qualified across Linux, Windows,
   and Android evidence, and selected as the default scalar profile under ADR
   0039;
3. delta plus varint — blocked on real ordered snapshots and loss/reorder models;
4. masks and bit packing — blocked on real change distributions;
5. quantized fields — blocked on field-specific numeric error budgets;
6. hybrid candidate — deferred until isolated variables have independent evidence.

## Security Gate P1 — before external untrusted traffic

- isolated packet fuzz target;
- ASAN and UBSAN for packet decoder and candidates;
- invalid-input corpus and regression retention;
- bounded allocation verification;
- retention and revalidation of the strict Godot LSAN and UBSAN suppression
  guards;
- no external endpoint completion before this gate passes.

## Phase 5 — Transport endpoint abstraction

- define `SyncTransportEndpoint` contract;
- peer, channel, reliability, receive event, and metrics model;
- no snapshot or rollback knowledge in endpoints;
- loopback endpoint first.

## Phase 6 — Transport lab

Evaluate under identical protocol workloads:

- loopback;
- SceneMultiplayer RPC;
- SceneMultiplayer raw bytes;
- MultiplayerPeer integration;
- direct ENet where justified.

Selection must use latency, overhead, allocation, reliability behavior, debuggability, and portability evidence.

## Phase 7 — Metrics and debugging

- per-peer bytes and packets;
- queue depth and drop reasons;
- RTT, jitter, loss, retransmission, and channel metrics;
- codec timing and snapshot size;
- basic profiler and debugger views.

## Phase 8 — Registry, objects, and schemas

- stable object identity;
- property schemas and explicit field codecs;
- compatibility IDs;
- deterministic registration and canonical ordering;
- bounded resource budgets.

## Phase 9 — Offline simulation

- fixed tick loop;
- input history;
- deterministic state capture;
- snapshot comparison;
- rollback-safe side-effect model;
- no network dependency.

## Phase 10 — Authoritative client/server synchronization

- connection and session lifecycle;
- input submission and acknowledgement;
- authoritative snapshots;
- baseline management;
- disconnect and recovery policy.

## Phase 11 — Prediction, rollback, and reconciliation

- predicted input execution;
- authoritative correction;
- rewind and resimulation;
- bounded history;
- deterministic validation and metrics.

## Phase 12 — Relevance and update frequency

- per-peer relevance;
- update classes and budgets;
- interest management;
- graceful overload behavior.

## Phase 13 — Advanced topologies

Mesh and hybrid authority remain deferred until the authoritative server model is stable, measured, and secure.

## Phase 14 — Compression

Compression is deferred until real snapshot distributions exist. Any codec must be benchmarked for size, CPU, latency, memory, and malformed-input behavior.

## Phase 15 — Cryptography

Prerequisites:

- stable packet boundaries and replay model;
- nonce and key lifecycle design;
- corrected reusable mbedTLS-based backend;
- authenticated encryption, not custom cryptography;
- benchmark and fuzz coverage.

## Phase 16 — Platform expansion

- Windows x86_64;
- Android ARM64 across flagship and older mid-range devices;
- macOS Universal 2 with native execution on the available architecture;
- later iOS when core behavior is stable.

## First public milestone — v0.1 Transport Lab

Included:

- validated binary and control foundations;
- benchmark-selected protocol candidate;
- loopback and at least one Godot transport endpoint;
- metrics sufficient to compare transports;
- authoritative handshake and bounded packet decoder;
- Linux, Windows, Android, and macOS benchmark evidence.

Not included:

- mesh authority;
- compression or encryption;
- full prediction and rollback;
- iOS;
- production editor tooling.

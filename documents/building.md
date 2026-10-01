# Building

The module is compiled as part of the engine using `custom_modules`:

```sh
cd /path/to/godot
scons platform=linuxbsd target=editor custom_modules=/path/to/tick_synchronizer precision=single
scons platform=linuxbsd target=editor custom_modules=/path/to/tick_synchronizer precision=double
```

Double-precision binaries get a `.double` suffix. Servers and clients must use the same precision.

Tests (doctest, built with `tests=yes`):

```sh
bin/godot.linuxbsd.editor.x86_64 --headless --test --test-case="*TickSynchronizer*"
```

## Requirements

| Requirement | Value |
|---|---|
| Godot | **4.6 or newer** (the module disables itself on older versions) |
| Build system | **SCons only** (the module is compiled together with the engine) |
| Language | C++17, no exceptions (engine flags) |
| Precision | Both `precision=single` and `precision=double` builds are supported and tested |
| Transport | ENet, optionally encrypted with DTLS (star and players' mesh) |
| Platforms | Portable to every platform supported by Godot; validated on **linuxbsd, android and windows** |

Every peer of a network must run the same version of the module and the same precision build: a peer of another
version is refused when it joins. GDExtension support is planned for a later version.

## Layout

```mermaid
flowchart TB
    m["tick_synchronizer/"] --> cfg["config.py — build conditions (Godot 4.6+)"]
    m --> scsub["SCsub — compiles register_types.cpp and everything under source/"]
    m --> reg["register_types.h/.cpp — class registration"]
    m --> src["source/ — all module sources"]
    src --> common["common/ — TickBitArray, TickDataBuffer"]
    src --> tick["tick/ — TickFixedStepper, TickClock"]
    src --> codec["codec/ — TickCodec"]
    src --> sync["sync/ — TickEngine, TickSyncCore (single authority), TickMeshCore (distributed authority), protocol"]
    src --> transport["transport/ — TickTransport, EnetStarTransport, EnetMeshTransport, EnetHostedMeshTransport, TickMultiplayerPeer, TickLocalNetwork (tests)"]
    src --> nodes["nodes/ — TickNetwork, TickObject, TickSpawner, DataBuffer"]
    m --> tests["tests/ — doctest suites"]
    m --> docs["doc_classes/ — class reference"]
    m --> demos["demos/ — example projects"]
    m --> documents["documents/ — usage, network models, demos, building and roadmap"]
```

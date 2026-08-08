# Source tree

Project-owned C++ code lives under `src/` to separate runtime implementation from tests, documentation, benchmarks, and SCons integration.

```text
src/
├── public/      Godot-facing classes and bindings
├── protocol/    Control envelope, packet codec, and handshake
└── internal/    Compile-time build and version contracts
```

`register_types.h/.cpp`, `SCsub`, and `config.py` remain at the repository root because Godot's module infrastructure discovers or references them there.

Rules:

- public code must not depend on a concrete transport endpoint;
- protocol code must not depend on `SceneMultiplayer`, ENet, or sockets;
- internal headers are not part of the public Godot API;
- cross-directory project includes use explicit `src/` prefixes;
- source paths must work both as an external custom module and under `godot/modules/tick_synchronizer`.

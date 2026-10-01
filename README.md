# TickSynchronizer

**TickSynchronizer** is a C++ module for Godot 4 that provides **tick-based, real-time network
synchronization**: fixed simulation ticks, client-side prediction, server reconciliation (rewind and
replay), interpolation, lag compensation and per-object authority, over ENet, between a server and its
clients, between servers, and between players.

It is a rewrite, inspired by [NetworkSynchronizer](https://github.com/GameNetworking/NetworkSynchronizer)
(MIT), whose ideas it reuses and extends.

> **Status:** every phase of the 0.x roadmap is done (see [the roadmap](documents/roadmap.md)): server-authoritative
> star networks, meshes between servers with one authority or with an owner per object, and meshes between players over
> the internet (NAT traversal, relay, DTLS, host migration). Development continues on branch `0.2`.

## Purpose

Godot's high-level multiplayer (`SceneMultiplayer`, `MultiplayerSynchronizer`, `MultiplayerSpawner`) replicates
state and RPCs, but it has **no notion of a network tick**. It offers no prediction, no rollback/reconciliation,
no snapshot interpolation and no lag compensation. Fast-paced games (shooters, action, sports, racing) need all
of those to feel responsive and stay consistent across peers.

TickSynchronizer fills that gap. It builds on the engine's ENet, and `SceneMultiplayer` keeps working next to it:
RPCs, authentication, spawners and synchronizers share the connection of a star or of a players' mesh. Game code is
written once, and the module decides how each object is predicted, validated, corrected and replicated.

## What it provides

| Feature | Description |
|---|---|
| **Fixed network tick** | Shared frame index and a clock synchronized with the authority, corrected gradually instead of in jumps; clients speed up or slow down to keep the server's input buffer healthy. |
| **Client-side prediction** | Local inputs are simulated immediately; the server simulates the same inputs authoritatively. |
| **Reconciliation** | Snapshots are compared per frame; on divergence the client resets to the authoritative state and replays pending inputs. Corrections that do not need a replay are applied directly. |
| **Remote entities** | Other players are interpolated from state or, in a mesh, simulated from the inputs they send directly to every player ("dolls"). |
| **Interpolation** | Objects that are not predicted are interpolated between authoritative states. |
| **Per-variable quantization** | Each synchronized variable declares a codec (bit width, range, precision) and a comparison tolerance derived from it; the authority quantizes its own state so prediction and validation agree. |
| **Bandwidth control** | Bit-level encoding, delta state against acknowledged baselines, deltas split across datagrams, and relevance per client decided by the game (interest filter). |
| **Reliable full snapshots** | Late join, resynchronization and recovery use full snapshots on a dedicated reliable channel. |
| **Spawning** | `TickSpawner` replicates the creation and removal of nodes, also to peers that join later. |
| **Events and scheduled actions** | Reliable, frame-stamped events delivered to the object's authority; actions scheduled on a shared frame with latency compensation. |
| **History and lag compensation** | The state of any object at a past frame (`get_state_at`), and the frame a client was seeing (`get_view_frame`), to validate an action where the client saw it. |
| **Per-object authority** | In a mesh of servers, every object has an owner, and ownership is requested, released and assigned through a versioned registry. |
| **Role failover** | When the node with the registry or the clock leaves, a configured reserve (or any node) takes its roles, on the same timeline and with the same object ids; a quorum keeps a mesh that splits from having two registries, and a node that restarts takes its roles back from what the others know. |
| **Players' meshes** | A host introduces the players, which punch direct links through their NATs or are relayed by the host; admission by the host, optional DTLS, and host migration when the host leaves. |
| **Security** | Every message has a sender rule, and the sender is identified by the transport, never by the payload. What untrusted peers send is limited in rate and size and checked, with validation hooks for events and for joining a mesh. |
| **Diagnostics** | Statistics per network (rewinds, late and missing inputs, dropped snapshots, doll delays, round trip), a signal for each rewind, and simulated latency, jitter and packet loss on the star transport (latency on the servers' mesh). |

The same game code runs with an authoritative server and its clients, in a mesh with one authority (between servers,
or between players with a host), and in a mesh of trusted servers with an owner per object; a process can take part in
several networks at once.

The module does **not** rely on deterministic simulation. There is always a single source of truth per object,
so physics engines that are not deterministic across platforms (such as Jolt or GodotPhysics) are fine.

## Documentation

| Document | Contents |
|---|---|
| [documents/usage.md](documents/usage.md) | How to use the module: a server and its clients, clusters of servers, distributed authority, players in a mesh, interest and lag compensation |
| [documents/network-models.md](documents/network-models.md) | The network models and what each one is for |
| [documents/demos.md](documents/demos.md) | The example projects under `demos/` |
| [documents/building.md](documents/building.md) | Requirements, how to build the module with the engine, how to run the tests, and the layout of the sources |
| [documents/roadmap.md](documents/roadmap.md) | What is done and what comes next |
| [`doc_classes/`](doc_classes) | The class reference (also in the editor's help) |

## Credits and license

Based on ideas and code from [NetworkSynchronizer](https://github.com/GameNetworking/NetworkSynchronizer) by
Andrea Catania and contributors. Released under the MIT license (see `LICENSE`).

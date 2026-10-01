# Roadmap

Every phase of the 0.x roadmap is done:

| Phase | Scope |
|---|---|
| F0 | Module skeleton, builds on linuxbsd (single and double) |
| F1 | Core: data buffer, tick clock, transport abstraction |
| F2 | Prediction and reconciliation, replication, codecs, ENet star transport, public API |
| F3 | Spawning, events, sender rules and validation |
| F4–F5 | Mesh networks between servers, per-object authority transfer |
| F6–F7 | Peer-to-peer meshes between players (direct inputs, NAT traversal, relay) |
| F8 | Relevancy, history and lag compensation, optimizations |
| F9 | Validation on android and windows, compatibility check with Godot 4.6 |

After the roadmap, branch `0.1` also got DTLS and host migration in the players' mesh, admission by the host, limits
and checks on what peers send, joins after a migration, gradual clock corrections, and the automatic move of the
registry and the clock master in a distributed mesh.

Branch `0.2` (in progress):

| Change | Scope |
|---|---|
| Role candidates | The nodes that take the registry and the clock of a distributed mesh, by order of preference (a configured reserve) |
| Confirmed losses and quorum | A successor takes a role only when no node it reaches still sees the node that had it, and with enough nodes around it |
| Same timeline | The nodes go on simulating while the clock moves; a clock master that restarts takes the mesh's frame |
| Restarts | A node with a role that restarts takes it again from what the other nodes know |
| Inherited spawns | The spawns of a node that left belong to the registry |

GDExtension support is the main item left for later versions.

# Demos

Headless projects under [`demos/`](../demos), each with its own README. They run with a Godot editor binary built with the
module (see [building.md](building.md)).

| Demo | Shows |
|---|---|
| [`star_headless`](../demos/star_headless) | A server and a client, with simulated latency, jitter and packet loss: prediction, interpolation, spawns and events |
| [`cluster_headless`](../demos/cluster_headless) | Three servers in a mesh with one authority, relayed to each server's clients |
| [`distributed_headless`](../demos/distributed_headless) | Servers with an owner per object: transfers, orphans, the loss of the registry and clock node (to the lowest node or to a reserve), and its restart |
| [`p2p_headless`](../demos/p2p_headless) | Three players in a mesh, with dolls driven by direct inputs |
| [`hosted_mesh_headless`](../demos/hosted_mesh_headless) | A host and players: direct links or relay, RPCs on the same mesh, admission and host migration |
| [`nat_test`](../demos/nat_test) | The players' mesh over the internet, also on phones |
| [`selftest`](../demos/selftest) | Checks the module's main paths on the current platform (how it is validated on Android) |

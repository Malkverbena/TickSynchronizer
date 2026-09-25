# Headless P2P demo

Three players in a mesh with a single authority (MESH + SINGLE, F6). Player 1 hosts: it's the authority and plays
too. Every player predicts its own body and simulates the others as dolls, with the inputs they send directly to
every player. The doll delay reported by each node is the latency to that player plus a small input buffer.

```bash
godot --headless --path . --script main.gd -- --id=1
godot --headless --path . --script main.gd -- --id=2
godot --headless --path . --script main.gd -- --id=3
```

`--via=S` sets the simulated one way latency of the links to the host (0.04 by default) and `--direct=S` the one
between players 2 and 3 (0.01 by default).

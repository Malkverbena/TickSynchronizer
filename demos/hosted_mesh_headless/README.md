# Headless hosted mesh demo

A host and players in an `EnetHostedMeshTransport` (F7). Players join through the host, which gives them their ids and
introduces them to each other: each pair punches a direct link, or is relayed by the host. The same mesh carries a
`TickNetwork` (the host is the authority; the players' bodies are dolls on the other players) and `SceneMultiplayer`
RPCs.

```bash
godot --headless --path . --script main.gd -- host
godot --headless --path . --script main.gd -- player
godot --headless --path . --script main.gd -- player --relay=1
```

`--relay=1` makes a player refuse direct links, so every pair with it is relayed. Across the internet, the host needs a
reachable port (`--port`, 9500 by default); the players connect with `--address=<host address>`.

Host migration: give the host a shorter `--duration` than the players (for example 5 and 10). When it leaves, player 2
becomes the host and the authority, and player 3 follows it; their reports show the new paths.

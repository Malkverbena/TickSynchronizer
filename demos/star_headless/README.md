# Headless star demo

A server and a client over ENet (localhost), with optional simulated latency, jitter and packet loss. The server
spawns the client's player with a `TickSpawner` and schedules a `round_start` event half a second ahead; the client
predicts its player, interpolates the server's NPC and sends a `honk` event every two seconds. Both print their
statistics, the frames the events ran at, and the final positions.

```bash
godot --headless --path . --script main.gd -- server --duration=17 --latency=0.05 --jitter=0.01 --loss=0.05
godot --headless --path . --script main.gd -- client --duration=15 --latency=0.05 --jitter=0.01 --loss=0.05
```

Options: `--port=N`, `--duration=S`, `--latency=S`, `--jitter=S`, `--loss=R` (0 to 1),
`--compression=range|none`, `--trace=1` (statistics every half second).

The simulated conditions apply when sending, on each side, so the round trip gets twice the latency.

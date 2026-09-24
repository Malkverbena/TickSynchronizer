# Headless star demo

A server and a client over ENet (localhost), with optional simulated latency, jitter and packet loss. The client
predicts its player; the server's NPC is interpolated. Both print their statistics and final positions.

```bash
godot --headless --path . --script main.gd -- server --duration=17 --latency=0.05 --jitter=0.01 --loss=0.05
godot --headless --path . --script main.gd -- client --duration=15 --latency=0.05 --jitter=0.01 --loss=0.05
```

Options: `--port=N`, `--duration=S`, `--latency=S`, `--jitter=S`, `--loss=R` (0 to 1),
`--compression=range|none`, `--trace=1` (statistics every half second).

The simulated conditions apply when sending, on each side, so the round trip gets twice the latency.

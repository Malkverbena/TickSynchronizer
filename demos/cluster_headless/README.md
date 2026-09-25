# Headless cluster demo

Three servers in a mesh (MESH + SINGLE): W is the authority and clock master and moves an NPC; the game servers A
and B receive it and relay it to their own clients through a star (STAR + SINGLE) that follows the cluster's
timeline. Each process prints the NPC position and its frames every three seconds.

```bash
godot --headless --path . --script main.gd -- w --duration=24
godot --headless --path . --script main.gd -- a --duration=22 --edge-port=7101
godot --headless --path . --script main.gd -- b --duration=22 --edge-port=7102
godot --headless --path . --script main.gd -- client --duration=16 --edge-port=7101
godot --headless --path . --script main.gd -- client --duration=16 --edge-port=7102
```

At the same moment, W, A, B and both clients report nearly the same frame; the clients' NPC trails W's by the
interpolation delay.

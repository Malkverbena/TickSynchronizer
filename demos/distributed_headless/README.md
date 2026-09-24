# Headless distributed authority demo

Three servers in a mesh with distributed authority (MESH + DISTRIBUTED). Node 1 is the registry and clock master;
a crate starts owned by node 2 and moves while someone owns it. At 4 s node 3 requests the crate; when node 3's
process ends, the registry orphans the crate and node 1 (the project's policy) assigns it back to node 2.

```bash
godot --headless --path . --script main.gd -- --id=1 --duration=14
godot --headless --path . --script main.gd -- --id=2 --duration=14
godot --headless --path . --script main.gd -- --id=3 --duration=8
```

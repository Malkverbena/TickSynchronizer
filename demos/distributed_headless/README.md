# Headless distributed authority demo

Three servers in a mesh with distributed authority (MESH + DISTRIBUTED). Node 1 is the registry and clock master;
a crate starts owned by node 2 and moves while someone owns it. At 4 s node 3 requests the crate; when node 3's
process ends, the registry orphans the crate and assigns it back to node 2 (the project's policy).

```bash
godot --headless --path . --script main.gd -- --id=1 --duration=14
godot --headless --path . --script main.gd -- --id=2 --duration=14
godot --headless --path . --script main.gd -- --id=3 --duration=8
```

Losing the registry and clock node: give node 1 the shortest duration. When it leaves, node 2 (the lowest node left)
takes both roles and every node prints it; the crate keeps its owner, and the orphan at the end goes through node 2.

```bash
godot --headless --path . --script main.gd -- --id=1 --duration=6
godot --headless --path . --script main.gd -- --id=2 --duration=14
godot --headless --path . --script main.gd -- --id=3 --duration=10
```

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

A reserve and a crash: with `--reserve=3` on every node, only node 1 and node 3 take the roles. `--crash=5` kills node
1's process at 5 s without closing anything, so the others notice it when it stops answering (`--timeout`, in
seconds; `--nodes` is the size of the mesh). Node 3 takes both roles once every node lost node 1; the frames go on.

```bash
godot --headless --path . --script main.gd -- --id=1 --nodes=4 --reserve=3 --timeout=2 --crash=5
godot --headless --path . --script main.gd -- --id=2 --nodes=4 --reserve=3 --timeout=2 --duration=12
godot --headless --path . --script main.gd -- --id=3 --nodes=4 --reserve=3 --timeout=2 --duration=12
godot --headless --path . --script main.gd -- --id=4 --nodes=4 --reserve=3 --timeout=2 --duration=12
```

A restart: with `--reserve=1`, only node 1 has the roles. After it crashes, nodes 2 and 3 go on without a registry
(the crate keeps moving); start node 1 again a few seconds later, and it takes its roles back from what the others
know: it jumps to the mesh's frame, and the crate has the same owner.

```bash
godot --headless --path . --script main.gd -- --id=1 --reserve=1 --timeout=1 --crash=5
godot --headless --path . --script main.gd -- --id=2 --reserve=1 --timeout=1 --duration=18
godot --headless --path . --script main.gd -- --id=3 --reserve=1 --timeout=1 --duration=18
# About 4 s after the crash:
godot --headless --path . --script main.gd -- --id=1 --reserve=1 --timeout=1 --duration=8
```

`--quorum=N` sets how many nodes (itself included) a node must be connected to in order to take or keep the roles.

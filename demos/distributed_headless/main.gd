extends SceneTree
## Headless distributed authority demo: `godot --headless --path . --script main.gd -- --id=N [--duration=S]`.
## Node 1 is the registry and clock master; the crate starts with node 2. Node 3 requests it at 4 s; when node 3
## leaves, the registry gets the orphan and gives it back to node 2. When node 1 leaves, another node takes both roles:
## the lowest one, or the reserve with `--reserve=ID` (with `--reserve=1`, only node 1 has them, and takes them again
## when it restarts). Other options: `--nodes=N` (size of the mesh), `--quorum=N`, `--timeout=S` (how long a node
## that stopped answering is waited for) and `--crash=S` (the process dies at S seconds, without closing anything).

const Crate := preload("res://crate.gd")

var options := {"id": 1, "duration": 14.0, "base_port": 9300, "nodes": 3, "reserve": 0, "quorum": 0, "timeout": 5.0, "crash": 0.0}
var network: TickNetwork
var crate: Node2D
var crate_sync: TickObject
var elapsed := 0.0
var next_report := 1.0
var requested := false


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--") and "=" in arg:
			var pair := arg.substr(2).split("=")
			options[pair[0].replace("-", "_")] = float(pair[1]) if "." in pair[1] else int(pair[1])

	var world := Node.new()
	world.name = "World"
	root.add_child(world)
	network = TickNetwork.new()
	network.name = "Mesh"
	network.authority_mode = TickNetwork.AUTHORITY_DISTRIBUTED
	network.trust = TickNetwork.TRUST_TRUSTED
	network.interpolate_remote = false
	if options.reserve > 0:
		# Only node 1 and its reserve take the roles; every node needs the same list.
		network.role_candidates = PackedInt32Array([1] if options.reserve == 1 else [1, options.reserve])
	network.role_quorum = options.quorum
	world.add_child(network)
	crate = Node2D.new()
	crate.name = "Crate"
	world.add_child(crate)
	crate_sync = TickObject.new()
	crate_sync.set_script(Crate)
	crate_sync.controller_peer = 2
	crate.add_child(crate_sync)

	network.authority_orphaned.connect(_on_orphaned)
	network.authority_request_denied.connect(func(_object): print("[node %d] request denied" % options.id))
	network.roles_changed.connect(func(registry_peer: int, clock_master: int): print("[node %d] t=%.1f roles moved: registry %d, clock master %d (frame %d)" % [options.id, elapsed, registry_peer, clock_master, network.get_frame()]))
	network.role_quorum_changed.connect(func(has_quorum: bool): print("[node %d] t=%.1f %s" % [options.id, elapsed, "has its quorum" if has_quorum else "lost its quorum"]))
	network.peer_left.connect(func(peer: int): print("[node %d] t=%.1f node %d left" % [options.id, elapsed, peer]))

	var mesh := EnetMeshTransport.create(options.id, options.base_port + options.id)
	mesh.node_timeout = options.timeout
	for id in range(1, options.nodes + 1):
		if id != options.id:
			mesh.add_node(id, "127.0.0.1", options.base_port + id)
	network.start(mesh)
	print("[node %d] started" % options.id)


func _on_orphaned(object: TickObject, last_owner: int, last_frame: int) -> void:
	print("[node %d] crate orphaned: last owner %d, last frame %d" % [options.id, last_owner, last_frame])
	if network.registry_peer == options.id:
		# The project's policy: the registry gives orphans to node 2.
		object.assign_authority(2)


func _process(delta: float) -> bool:
	elapsed += delta
	if options.id == 3 and not requested and elapsed >= 4.0:
		requested = true
		print("[node 3] requesting the crate")
		crate_sync.request_authority()
	if elapsed >= next_report:
		next_report += 1.0
		print("[node %d] t=%.0f frame=%d owner=%d x=%.2f" % [options.id, elapsed, network.get_frame(), crate_sync.get_owner_peer(), crate.position.x])
	if options.crash > 0.0 and elapsed >= options.crash:
		# The process dies: the other nodes only notice when it stops answering.
		print("[node %d] t=%.1f crashes" % [options.id, elapsed])
		OS.kill(OS.get_process_id())
	if elapsed < options.duration:
		return false
	var stats: Dictionary = network.get_stats()
	print("[node %d] stats: transfers %d, orphans %d, stale %d, malformed %d" % [options.id, stats.transfers, stats.orphans, stats.stale_states, stats.malformed_packets])
	network.stop()
	return true

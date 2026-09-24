extends SceneTree
## Headless distributed authority demo: `godot --headless --path . --script main.gd -- --id=N [--duration=S]`.
## Node 1 is the registry and clock master; the crate starts with node 2. Node 3 requests it at 4 s; when node 3
## leaves, node 1 gets the orphan and gives it back to node 2.

const Crate := preload("res://crate.gd")

var options := {"id": 1, "duration": 14.0, "base_port": 9300}
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

	var mesh := EnetMeshTransport.create(options.id, options.base_port + options.id)
	for id in [1, 2, 3]:
		if id != options.id:
			mesh.add_node(id, "127.0.0.1", options.base_port + id)
	network.start(mesh)
	print("[node %d] started" % options.id)


func _on_orphaned(object: TickObject, last_owner: int, last_frame: int) -> void:
	print("[node %d] crate orphaned: last owner %d, last frame %d" % [options.id, last_owner, last_frame])
	if options.id == 1:
		# The project's policy: node 2 adopts orphans.
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
	if elapsed < options.duration:
		return false
	var stats: Dictionary = network.get_stats()
	print("[node %d] stats: transfers %d, orphans %d, stale %d, malformed %d" % [options.id, stats.transfers, stats.orphans, stats.stale_states, stats.malformed_packets])
	network.stop()
	return true

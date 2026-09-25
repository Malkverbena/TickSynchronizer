extends SceneTree
## Headless P2P demo: three players in a mesh with a single authority (MESH + SINGLE). Player 1 hosts (it's the
## authority and plays too); every player predicts its own body and simulates the others as dolls with the inputs
## they send directly (F6).
##   godot --headless --path . --script main.gd -- --id=N [--duration=S] [--via=S] [--direct=S]
## `--via` is the simulated one way latency of the links to the host, `--direct` the one between players 2 and 3.

const Player := preload("res://player.gd")

var options := {"id": 1, "duration": 12.0, "base_port": 9400, "via": 0.04, "direct": 0.01}
var network: TickNetwork
var players := {}
var elapsed := 0.0
var next_report := 1.0


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
	network.authority_peer = 1
	world.add_child(network)
	for id in [1, 2, 3]:
		var body := Node2D.new()
		body.name = "Player%d" % id
		world.add_child(body)
		var sync := TickObject.new()
		sync.set_script(Player)
		sync.controller_peer = id
		sync.remote_mode = TickObject.REMOTE_MODE_DOLL
		body.add_child(sync)
		players[id] = body

	var mesh := EnetMeshTransport.create(options.id, options.base_port + options.id)
	for id in [1, 2, 3]:
		if id == options.id:
			continue
		mesh.add_node(id, "127.0.0.1", options.base_port + id)
		var direct: bool = options.id != 1 and id != 1
		mesh.set_node_simulated_latency(id, options.direct if direct else options.via)
	network.start(mesh)
	print("[node %d] started: %.0f ms to the host, %.0f ms between players" % [options.id, options.via * 1000.0, options.direct * 1000.0])


func _process(delta: float) -> bool:
	elapsed += delta
	if elapsed >= next_report:
		next_report += 1.0
		var stats: Dictionary = network.get_stats()
		print("[node %d] t=%.0f frame=%d x=[%.1f, %.1f, %.1f] doll delays %s" % [options.id, elapsed, network.get_frame(), players[1].position.x, players[2].position.x, players[3].position.x, stats.get("doll_delays", {})])
	if elapsed < options.duration:
		return false
	var stats: Dictionary = network.get_stats()
	print("[node %d] stats: rewinds %d, doll rewinds %d, doll corrections %d, doll ghost inputs %d, malformed %d" % [options.id, stats.rewinds, stats.doll_rewinds, stats.doll_corrections, stats.doll_ghost_inputs, stats.malformed_packets])
	network.stop()
	return true

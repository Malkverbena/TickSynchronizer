extends SceneTree
## Headless hosted mesh demo (F7): players join through a host, punch direct links to each other or get relayed, and
## use the mesh for a `TickNetwork` (dolls) and for `SceneMultiplayer` RPCs at the same time.
##   godot --headless --path . --script main.gd -- host [--port=N] [--duration=S] [--password=P] [--end=1]
##   godot --headless --path . --script main.gd -- player [--address=A] [--port=N] [--relay=1] [--duration=S] [--password=P]
## `--relay=1` makes the player refuse direct links, so the host relays it. With `--password`, the host admits only the
## players that send the same one. When its duration ends, the host hands the mesh over to the next player
## (`--end=1`: it ends the mesh instead).

const Player := preload("res://player.gd")
const Chat := preload("res://chat.gd")

var role := "host"
var options := {"port": 9500, "address": "127.0.0.1", "duration": 10.0, "relay": 0, "password": "", "end": 0}
var transport: EnetHostedMeshTransport
var network: TickNetwork
var chat: Node
var players := {}
var elapsed := 0.0
var next_report := 1.0
var greeted := false
var left := false


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg in ["host", "player"]:
			role = arg
		elif arg.begins_with("--") and "=" in arg:
			var pair := arg.substr(2).split("=")
			var value: String = pair[1]
			options[pair[0]] = value if not value.is_valid_float() else (float(value) if "." in value else int(value))

	var password := str(options.password)
	if role == "host":
		transport = EnetHostedMeshTransport.create_host(options.port, 8)
		if not password.is_empty():
			transport.join_validator = func(_peer: int, join_data: PackedByteArray, _address: String) -> bool:
				return join_data.get_string_from_utf8() == password
	else:
		transport = EnetHostedMeshTransport.create_player(options.address, options.port, EnetHostedMeshTransport.COMPRESSION_RANGE_CODER, null, "", password.to_utf8_buffer())
		transport.direct_connections = options.relay == 0
	# SceneMultiplayer on the same mesh.
	get_multiplayer().multiplayer_peer = transport.get_multiplayer_peer()

	var world := Node.new()
	world.name = "World"
	root.add_child(world)
	chat = Node.new()
	chat.name = "Chat"
	chat.set_script(Chat)
	world.add_child(chat)
	network = TickNetwork.new()
	network.name = "Mesh"
	world.add_child(network)
	for id in [2, 3]:
		var body := Node2D.new()
		body.name = "Player%d" % id
		world.add_child(body)
		var sync := TickObject.new()
		sync.set_script(Player)
		sync.controller_peer = id
		sync.remote_mode = TickObject.REMOTE_MODE_DOLL
		body.add_child(sync)
		players[id] = body
	network.start(transport)
	print("[%s] started%s" % [role, " (relay only)" if options.relay != 0 else ""])


func _process(delta: float) -> bool:
	elapsed += delta
	var id := transport.get_local_peer_id()
	if not left and transport.get_status() == EnetHostedMeshTransport.STATUS_DISCONNECTED:
		left = true
		var reasons := ["none", "closed", "lost", "refused", "full", "busy", "version", "ended"]
		print("[node %d] t=%.1f left the mesh: %s" % [id, elapsed, reasons[transport.get_disconnect_reason()]])
	if not greeted and elapsed >= 3.0 and transport.get_status() == EnetHostedMeshTransport.STATUS_CONNECTED:
		greeted = true
		chat.rpc("hello", "hi from %d" % id)
	if elapsed >= next_report:
		next_report += 1.0
		var paths := {}
		for peer in transport.get_peers():
			paths[peer] = ["none", "host", "connecting", "direct", "relayed"][transport.get_peer_path(peer)]
		print("[node %d] t=%.0f paths %s x=[%.1f, %.1f] doll delays %s" % [id, elapsed, paths, players[2].position.x, players[3].position.x, network.get_stats().get("doll_delays", {})])
	if elapsed < options.duration:
		return false
	var stats: Dictionary = network.get_stats()
	print("[node %d] stats: rewinds %d, doll rewinds %d, malformed %d; transport %s" % [id, stats.rewinds, stats.doll_rewinds, stats.malformed_packets, transport.get_stats()])
	network.stop()
	if transport.is_hosting() and options.end == 0 and not transport.get_peers().is_empty():
		# The next player of the succession takes over.
		print("[node %d] hands the mesh over" % id)
		transport.hand_over()
	else:
		transport.close()
	return true

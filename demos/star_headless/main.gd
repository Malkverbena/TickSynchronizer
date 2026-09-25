extends SceneTree
## Headless demo: `godot --headless --path . --script main.gd -- <server|client> [options]`.
## Options: --port=N --duration=S --latency=S --jitter=S --loss=R --compression=none|range --trace=1

const Player := preload("res://player.gd")
const Npc := preload("res://npc.gd")

var role := "server"
var options := {"port": 7777, "duration": 15.0, "latency": 0.0, "jitter": 0.0, "loss": 0.0, "compression": "range", "trace": ""}
var world: Node
var network: TickNetwork
var transport: EnetStarTransport
var spawner: TickSpawner
var elapsed := 0.0
var rewinds_at_warmup := -1
var next_honk := 2.0
var round_frame := -1


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg == "server" or arg == "client":
			role = arg
		elif arg.begins_with("--") and "=" in arg:
			var pair := arg.substr(2).split("=")
			var value = pair[1]
			if typeof(options.get(pair[0])) == TYPE_FLOAT:
				value = float(value)
			elif typeof(options.get(pair[0])) == TYPE_INT:
				value = int(value)
			options[pair[0]] = value

	var compression := EnetStarTransport.COMPRESSION_RANGE_CODER
	if options.compression == "none":
		compression = EnetStarTransport.COMPRESSION_NONE

	world = Node.new()
	world.name = "World"
	network = TickNetwork.new()
	network.name = "Net"
	world.add_child(network)
	# The server spawns the players; the clients get them through the spawner.
	spawner = TickSpawner.new()
	spawner.name = "Players"
	spawner.spawn_function = _make_player
	world.add_child(spawner)
	root.add_child(world)
	_add_body("Npc", Npc, 1)
	network.event_received.connect(_on_network_event)

	if role == "server":
		transport = EnetStarTransport.create_server(options.port, 8, compression)
		network.peer_ready.connect(_on_peer_ready)
	else:
		transport = EnetStarTransport.create_client("127.0.0.1", options.port, compression)
		network.peer_ready.connect(_on_connected)
		network.rejected.connect(func(reason): print("[client] rejected: ", reason))
	transport.simulated_latency = options.latency
	transport.simulated_jitter = options.jitter
	transport.simulated_packet_loss = options.loss
	network.start(transport)
	print("[%s] started: latency %.0f ms, jitter %.0f ms, loss %.0f%%, compression %s" % [role, options.latency * 1000.0, options.jitter * 1000.0, options.loss * 100.0, options.compression])


func _add_body(body_name: String, script: Script, controller: int) -> Node2D:
	var body := Node2D.new()
	body.name = body_name
	world.add_child(body)
	var sync := TickObject.new()
	sync.set_script(script)
	sync.controller_peer = controller
	body.add_child(sync)
	return body


func _make_player(_data) -> Node:
	var body := Node2D.new()
	var sync := TickObject.new()
	sync.set_script(Player)
	body.add_child(sync)
	return body


func _on_peer_ready(peer: int) -> void:
	spawner.spawn_custom(null, "Player_%d" % peer, peer)
	print("[server] player %d joined" % peer)
	# Every peer starts the "round" at the same frame, half a second from now.
	round_frame = network.get_event_frame(0.5)
	network.send_event(&"round_start", null, round_frame)


func _on_connected(_peer: int) -> void:
	print("[client] connected as %d" % network.get_local_peer_id())


func _on_network_event(sender: int, event: StringName, _payload: Variant, frame: int) -> void:
	if event == &"round_start":
		print("[client] round_start scheduled for frame %d, ran at frame %d" % [frame, network.get_frame() - 1])
	elif event == &"honk":
		print("[server] honk from %d, sent for frame %d, ran at frame %d" % [sender, frame, network.get_frame() - 1])


func _process(delta: float) -> bool:
	elapsed += delta
	if options.get("trace", "") == "1" and int(elapsed * 2.0) != int((elapsed - delta) * 2.0):
		var s := network.get_stats()
		print("[%s] t=%.1f frame=%d rewinds=%d ghost=%d late=%d scale=%.3f rtt=%.0f predicting=%s" % [role, elapsed, network.get_frame(), s.rewinds, s.ghost_inputs, s.late_inputs, s.time_scale, network.get_rtt() * 1000.0, network.is_predicting()])
	if role == "client" and network.is_predicting() and elapsed >= next_honk:
		next_honk += 2.0
		network.send_event(&"honk")
	if rewinds_at_warmup < 0 and elapsed >= 3.0:
		rewinds_at_warmup = network.get_stats().rewinds
	if elapsed < options.duration:
		return false

	var stats := network.get_stats()
	var host: ENetConnection = (transport.get_multiplayer().multiplayer_peer as ENetMultiplayerPeer).host
	var sent_bytes := host.pop_statistic(ENetConnection.HOST_TOTAL_SENT_DATA)
	print("[%s] frame %d, rtt %.1f ms, time scale %.3f" % [role, network.get_frame(), network.get_rtt() * 1000.0, stats.time_scale])
	print("[%s] stats %s" % [role, stats])
	print("[%s] rewinds after warm-up: %d" % [role, stats.rewinds - max(rewinds_at_warmup, 0)])
	print("[%s] sent %d bytes (%.1f KB/s)" % [role, sent_bytes, sent_bytes / 1024.0 / elapsed])
	for body in world.get_children():
		if body is Node2D:
			print("[%s] %s at %s" % [role, body.name, body.position])
	network.stop()
	return true

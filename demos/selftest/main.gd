extends Node
## Self-test of the module on the running platform (F9): real ENet on localhost, inside one process. Prints one line per
## check and `SELFTEST PASS` or `SELFTEST FAIL`, then quits with the matching exit code. Runs on desktop with
## `godot --headless --path .` and on Android as an exported APK (the engine's unit tests don't run there).

const Mover := preload("res://mover.gd")

var failures := 0


func _ready() -> void:
	var precision := "double" if OS.has_feature("double") else "single"
	print("SELFTEST start: %s, %s precision, Godot %s" % [OS.get_name(), precision, Engine.get_version_info().string])
	await _star()
	await _hosted_mesh()
	print("SELFTEST %s (%d failed)" % ["PASS" if failures == 0 else "FAIL", failures])
	get_tree().quit(0 if failures == 0 else 1)


func _check(condition: bool, what: String) -> void:
	print("SELFTEST %s: %s" % ["ok" if condition else "FAILED", what])
	if not condition:
		failures += 1


func _world(world_name: String, controllers: Array) -> Node:
	var world := Node.new()
	world.name = world_name
	add_child(world)
	var network := TickNetwork.new()
	network.name = "Net"
	world.add_child(network)
	for controller in controllers:
		var body := Node2D.new()
		body.name = "Body%d" % controller
		world.add_child(body)
		var sync := TickObject.new()
		sync.set_script(Mover)
		sync.controller_peer = controller
		sync.remote_mode = TickObject.REMOTE_MODE_DOLL
		# The bodies are the network's siblings: without a path they'd use the first network of the scene.
		sync.network_path = NodePath("../../Net")
		body.add_child(sync)
	return world


func _wait(seconds: float) -> void:
	await get_tree().create_timer(seconds).timeout


# A star over ENetMultiplayerPeer: the server's NPC is interpolated on the client, and a client event reaches the
# server.
func _star() -> void:
	var port := 20000 + randi() % 20000
	var server := _world("StarServer", [1])
	var client := _world("StarClient", [1])
	var server_net: TickNetwork = server.get_node("Net")
	var client_net: TickNetwork = client.get_node("Net")
	var received := []
	server_net.event_received.connect(func(_sender, event, _payload, _frame): received.append(event))
	_check(server_net.start(EnetStarTransport.create_server(port, 4)) == OK, "star server started")
	_check(client_net.start(EnetStarTransport.create_client("127.0.0.1", port)) == OK, "star client started")
	await _wait(3.0)
	_check(client_net.is_predicting(), "star client predicting")
	var server_x: float = server.get_node("Body1").position.x
	var client_x: float = client.get_node("Body1").position.x
	_check(server_x > 10.0 and absf(server_x - client_x) < 3.0, "star NPC interpolated (%.2f on the server, %.2f on the client)" % [server_x, client_x])
	client_net.send_event("ping", 42)
	await _wait(1.0)
	_check(received == [&"ping"], "star event received on the server")
	_check(int(client_net.get_stats().malformed_packets) == 0, "star without malformed packets")
	server_net.stop()
	client_net.stop()
	server.queue_free()
	client.queue_free()


# A hosted mesh: a host and two players punch a direct link on localhost, the players' bodies are dolls on each other,
# and the multiplayer peer carries raw packets between them.
func _hosted_mesh() -> void:
	var port := 20000 + randi() % 20000
	var host := EnetHostedMeshTransport.create_host(port, 8)
	var first := EnetHostedMeshTransport.create_player("127.0.0.1", port)
	# Joins one at a time, so the ids follow the order.
	for i in 300:
		host.poll()
		first.poll()
		if first.get_status() == EnetHostedMeshTransport.STATUS_CONNECTED:
			break
		await get_tree().process_frame
	var second := EnetHostedMeshTransport.create_player("127.0.0.1", port)
	var transports := [host, first, second]
	var worlds := []
	var peers := []
	for i in 3:
		worlds.append(_world("Mesh%d" % (i + 1), [2, 3]))
		peers.append(transports[i].get_multiplayer_peer())
		(worlds[i].get_node("Net") as TickNetwork).start(transports[i])
	await _wait(4.0)
	_check(first.get_local_peer_id() == 2 and second.get_local_peer_id() == 3, "mesh ids given by the host")
	_check(first.get_peer_path(3) == EnetHostedMeshTransport.PATH_DIRECT, "mesh direct link between the players")
	var delays: Dictionary = (worlds[1].get_node("Net") as TickNetwork).get_stats().doll_delays
	_check(delays.has(3), "player 3 is a doll on player 2 (delays %s)" % delays)
	var host_x: float = worlds[0].get_node("Body3").position.x
	var doll_x: float = worlds[1].get_node("Body3").position.x
	_check(host_x > 10.0 and absf(host_x - doll_x) < 3.0, "doll follows the authority (%.2f on the host, %.2f on player 2)" % [host_x, doll_x])
	peers[2].set_target_peer(2)
	peers[2].put_packet(PackedByteArray([1, 2, 3]))
	var got := false
	for i in 120:
		await get_tree().process_frame
		while not got and peers[1].get_available_packet_count() > 0:
			got = peers[1].get_packet_peer() == 3 and peers[1].get_packet() == PackedByteArray([1, 2, 3])
		if got:
			break
	_check(got, "multiplayer peer packet from player 3 to player 2")
	for i in 3:
		(worlds[i].get_node("Net") as TickNetwork).stop()
		transports[i].close()
		worlds[i].queue_free()

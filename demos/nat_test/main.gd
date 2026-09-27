extends Node
## NAT traversal test: a hosted mesh (`EnetHostedMeshTransport`) over the internet. See README.md.

const Player := preload("res://player.gd")

var options := {"role": "", "address": "", "port": 9500, "dtls": 0, "duration": 60.0, "relay": 0, "password": "", "end": 0, "freeze": 0.0}
var transport: EnetHostedMeshTransport
var network: TickNetwork
var bodies := {}
var elapsed := 0.0
var next_report := 1.0
var running := false
var last_paths := {}
var left := false
var frozen := false
var log_label: Label
var address_edit: LineEdit
var lines: PackedStringArray = []


func _ready() -> void:
	# The defaults exported with the app, then a configuration pushed to the device for automated runs (README.md): with
	# a `role`, it starts right away.
	for path in ["res://nat_test.cfg", "user://nat_test.cfg"]:
		var config := ConfigFile.new()
		if config.load(path) == OK and config.has_section("test"):
			for key in config.get_section_keys("test"):
				options[key] = config.get_value("test", key)
	for arg in OS.get_cmdline_user_args():
		if arg in ["host", "player"]:
			options.role = arg
		elif arg.begins_with("--") and "=" in arg:
			var pair := arg.substr(2).split("=")
			var value: String = pair[1]
			options[pair[0]] = value if not value.is_valid_float() else (float(value) if "." in value else int(value))
	if options.role.is_empty():
		_build_ui()
	else:
		_start(options.role, int(options.dtls) != 0)


func _build_ui() -> void:
	var box := VBoxContainer.new()
	box.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	box.add_theme_constant_override("separation", 12)
	add_child(box)
	address_edit = LineEdit.new()
	address_edit.text = "%s" % options.address
	address_edit.custom_minimum_size.y = 80
	box.add_child(address_edit)
	var buttons := HBoxContainer.new()
	box.add_child(buttons)
	for dtls in [false, true]:
		var button := Button.new()
		button.text = "Com DTLS" if dtls else "Sem DTLS"
		button.custom_minimum_size = Vector2(300, 100)
		button.pressed.connect(func():
			options.address = address_edit.text.strip_edges()
			buttons.visible = false
			_start("player", dtls))
		buttons.add_child(button)
	log_label = Label.new()
	log_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	log_label.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(log_label)


func _log(text: String) -> void:
	print("NATTEST ", text)
	lines.append(text)
	if lines.size() > 30:
		lines.remove_at(0)
	if log_label:
		log_label.text = "\n".join(lines)


func _start(role: String, dtls: bool) -> void:
	var tls: TLSOptions
	if dtls:
		if role == "host":
			var crypto := Crypto.new()
			var key := crypto.generate_rsa(2048)
			tls = TLSOptions.server(key, crypto.generate_self_signed_certificate(key, "CN=nat-test,O=TickSynchronizer,C=BR"))
		else:
			tls = TLSOptions.client_unsafe()
	# `--password`: the host admits only the players that send the same one (their join data).
	var password := str(options.password)
	if role == "host":
		transport = EnetHostedMeshTransport.create_host(options.port, 8, "*", EnetHostedMeshTransport.COMPRESSION_RANGE_CODER, tls)
		if transport and not password.is_empty():
			transport.join_validator = func(_peer: int, join_data: PackedByteArray, _address: String) -> bool:
				return join_data.get_string_from_utf8() == password
	else:
		transport = EnetHostedMeshTransport.create_player(options.address, options.port, EnetHostedMeshTransport.COMPRESSION_RANGE_CODER, tls, "", password.to_utf8_buffer())
	if transport == null:
		_log("FAILED to create the %s transport (address %s, port %d)" % [role, options.address, options.port])
		return
	transport.punch_timeout = 5.0
	# `--relay=1`: this player refuses direct links, so its pairs go through the host.
	transport.direct_connections = int(options.relay) == 0

	var world := Node.new()
	world.name = "World"
	add_child(world)
	network = TickNetwork.new()
	network.name = "Net"
	world.add_child(network)
	# The host's NPC and up to four players.
	for id in [1, 2, 3, 4, 5]:
		var body := Node2D.new()
		body.name = "Body%d" % id
		world.add_child(body)
		var sync := TickObject.new()
		sync.set_script(Player)
		sync.controller_peer = id
		sync.remote_mode = TickObject.REMOTE_MODE_DOLL if id != 1 else TickObject.REMOTE_MODE_INTERPOLATE
		sync.network_path = NodePath("../../Net")
		body.add_child(sync)
		bodies[id] = body
	network.start(transport)
	running = true
	_log("started: %s on %s, %s, port %d, address %s" % [role, OS.get_name(), "DTLS" if dtls else "no DTLS", options.port, options.address])


func _path_name(peer: int) -> String:
	return ["none", "host", "connecting", "direct", "relayed"][transport.get_peer_path(peer)]


func _process(delta: float) -> void:
	if not running:
		return
	elapsed += delta
	if not frozen and float(options.freeze) > 0.0 and elapsed >= float(options.freeze):
		# `--freeze=S`: from then on, this node doesn't touch the network (a hung process, or one without network).
		frozen = true
		network.process_mode = Node.PROCESS_MODE_DISABLED
		_log("t=%.1f id=%d freezes: the network isn't serviced anymore" % [elapsed, transport.get_local_peer_id()])
	if frozen:
		if elapsed >= options.duration:
			running = false
			_log("done (frozen)")
			if not options.role.is_empty():
				get_tree().quit()
		return
	# Path changes as they happen.
	var paths := {}
	for peer in transport.get_peers():
		paths[peer] = _path_name(peer)
	if paths != last_paths:
		_log("t=%.1f id=%d paths %s" % [elapsed, transport.get_local_peer_id(), paths])
		last_paths = paths
	if not left and transport.get_status() == EnetHostedMeshTransport.STATUS_DISCONNECTED:
		left = true
		var reasons := ["none", "closed", "lost", "refused", "full", "busy", "version", "ended"]
		_log("t=%.1f id=%d left the mesh: %s" % [elapsed, transport.get_local_peer_id(), reasons[transport.get_disconnect_reason()]])
	if elapsed >= next_report:
		next_report += 5.0 if elapsed > 10.0 else 1.0
		var stats: Dictionary = network.get_stats()
		_log("t=%.0f id=%d host=%d rtt=%.0fms predicting=%s npc_x=%.1f doll_delays=%s rewinds=%d doll_rewinds=%d transport=%s" % [elapsed, transport.get_local_peer_id(), transport.get_host_peer(), network.get_rtt() * 1000.0, network.is_predicting(), bodies[1].position.x, stats.get("doll_delays", {}), stats.rewinds, stats.get("doll_rewinds", 0), transport.get_stats()])
	if elapsed >= options.duration:
		running = false
		_log("done: paths %s, transport %s" % [last_paths, transport.get_stats()])
		network.stop()
		if transport.is_hosting() and int(options.end) == 0 and not transport.get_peers().is_empty():
			# The next player of the succession takes over (`--end=1`: the mesh ends instead).
			_log("hands the mesh over")
			transport.hand_over()
		else:
			transport.close()
		if not options.role.is_empty():
			get_tree().quit()

extends TickObject
## Moves right: NPCs (controller 1) always, players while their controller's input says so.


func _setup_sync() -> void:
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_SINGLE))


func _collect_input(input: DataBuffer) -> void:
	input.add_int(1, 2)


func _process_tick(delta: float, input: DataBuffer) -> void:
	var direction := 1
	if controller_peer != 1:
		direction = input.read_int(2) if input.get_size() > 0 else 0
	(get_root_node() as Node2D).position.x += direction * 10.0 * delta

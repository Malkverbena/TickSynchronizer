extends TickObject
## A player moved by its controller's input (left, stop, right, repeated, each player with its own phase); the other
## players simulate it as a doll with that input.

const SPEED := 100.0

var ticks := 0


func _setup_sync() -> void:
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_SINGLE))


func _collect_input(input: DataBuffer) -> void:
	var direction: int = [-1, 0, 1][(ticks / 30 + controller_peer) % 3]
	ticks += 1
	input.add_int(direction, 2)


func _process_tick(delta: float, input: DataBuffer) -> void:
	var direction := 0
	if input.get_size() > 0:
		direction = input.read_int(2)
	(get_root_node() as Node2D).position.x += direction * SPEED * delta

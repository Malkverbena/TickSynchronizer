extends TickObject
## A body moved by its controller's input: left, stop, right, repeated; then it stops for good.

const SPEED := 100.0
const SCRIPT_TICKS := 600

var ticks := 0


func _setup_sync() -> void:
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_HALF))


func _collect_input(input: DataBuffer) -> void:
	var direction := 0
	if ticks < SCRIPT_TICKS:
		direction = [-1, 0, 1][(ticks / 30) % 3]
	ticks += 1
	input.add_int(direction, 2)


func _process_tick(delta: float, input: DataBuffer) -> void:
	var direction := 0
	if input.get_size() > 0:
		direction = input.read_int(2)
	var body := get_root_node() as Node2D
	body.position.x += direction * SPEED * delta

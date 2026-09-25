extends TickObject
## A server-controlled body moving in a circle; clients interpolate it.


func _setup_sync() -> void:
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_SINGLE))
	declare_var("rotation", TickCodec.real(TickCodec.PRECISION_SINGLE))


func _process_tick(delta: float, _input: DataBuffer) -> void:
	var body := get_root_node() as Node2D
	body.rotation += delta
	body.position = Vector2(cos(body.rotation), sin(body.rotation)) * 50.0

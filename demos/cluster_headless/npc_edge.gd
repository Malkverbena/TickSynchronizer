extends TickObject
## The same NPC in a game server's star: the server only relays what the cluster applied; clients interpolate it.


func _setup_sync() -> void:
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_SINGLE))
	declare_var("rotation", TickCodec.real(TickCodec.PRECISION_SINGLE))

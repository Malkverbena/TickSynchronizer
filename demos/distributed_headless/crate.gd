extends TickObject
## A crate that moves right while someone owns it; only its owner simulates it.


func _setup_sync() -> void:
	declare_var("position", TickCodec.vector2(TickCodec.PRECISION_SINGLE))


func _process_tick(delta: float, _input: DataBuffer) -> void:
	(get_root_node() as Node2D).position.x += 10.0 * delta


func _on_authority_changed(old_owner: int, new_owner: int) -> void:
	print("[node %d] crate owner %d -> %d at x=%.2f" % [get_network().get_local_peer_id(), old_owner, new_owner, (get_root_node() as Node2D).position.x])

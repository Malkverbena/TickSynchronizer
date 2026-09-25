extends Node
## RPCs through `SceneMultiplayer`, on the same mesh as the `TickNetwork`.


@rpc("any_peer", "reliable")
func hello(text: String) -> void:
	print("[node %d] rpc from %d: %s" % [multiplayer.get_unique_id(), multiplayer.get_remote_sender_id(), text])

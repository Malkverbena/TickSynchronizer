# Usage

The class reference is in [`doc_classes/`](../doc_classes) (and in the editor's help); the network models are described
in [network-models.md](network-models.md), and the runnable examples in [demos.md](demos.md).

## A server and its clients

```gdscript
# Server (peer 1) or client, sharing the same scene.
var transport := EnetStarTransport.create_server(7000, 32)            # or create_client("127.0.0.1", 7000)
$TickNetwork.start(transport)
```

```gdscript
# player_sync.gd — a TickObject child of the synchronized body.
extends TickObject

func _setup_sync():
	declare_var("position", TickCodec.vector3(TickCodec.PRECISION_HALF))

func _collect_input(input: DataBuffer):
	input.add_vector2(Input.get_vector("left", "right", "up", "down"), DataBuffer.COMPRESSION_LEVEL_2)

func _process_tick(delta: float, input: DataBuffer):
	var direction := Vector2()
	if input.get_size() > 0:
		direction = input.read_vector2(DataBuffer.COMPRESSION_LEVEL_2)
	get_root_node().position += Vector3(direction.x, 0.0, direction.y) * 5.0 * delta
```

Set `controller_peer` to the id of the client that controls the object (1 for the server), or let a
`TickSpawner` do it:

```gdscript
# Server: create the player of each client that joins; the clients create the same node.
func _on_peer_ready(peer: int):
	$Players/TickSpawner.spawn("res://player.tscn", "Player_%d" % peer, peer)
	# Run something at the same frame on every peer.
	$TickNetwork.send_event(&"round_start", null, $TickNetwork.get_event_frame(0.5))
```

Events sent by clients (`TickObject.send_event()`) run on the server at the frame the client predicted, after
`_validate_event()`; with the default untrusted setting, only the controller of an object can send it events,
and the rate and size of what clients send are limited. The class reference
is in [`doc_classes/`](../doc_classes); a runnable client/server example is in [`demos/star_headless`](../demos/star_headless).

## Clusters of servers

A game server can take part in a mesh with other servers and in a star with its own clients at the same time:

```gdscript
# Mesh between servers: node 1 is the authority and clock master.
var mesh := EnetMeshTransport.create(my_id, 9000 + my_id, addresses[my_id])   # listens on the internal interface
mesh.set_secret(cluster_key)                                                  # the same bytes on every node
for id in other_ids:
	mesh.add_node(id, addresses[id], 9000 + id)
$Cluster.trust = TickNetwork.TRUST_TRUSTED
$Cluster.interpolate_remote = false          # only the final clients interpolate
$Cluster.start(mesh)

# The star with this server's clients follows the cluster's timeline.
$Edge.clock_network = NodePath("../Cluster")
$Edge.start(EnetStarTransport.create_server(7000))
```

A body relayed from the cluster to the clients has a `TickObject` in each network (`network_path`). See
[`demos/cluster_headless`](../demos/cluster_headless).

The engines trust every node of a servers' mesh, so the transport keeps strangers out of it: a connection must come
from the address its node was added with (`check_addresses`), and with a secret (`set_secret()`) the two sides of
every link prove they know it before the link is reported. The links aren't encrypted: keep the mesh on a network
you trust.

## Distributed authority

In a mesh of trusted servers, every object can have its own owner, which simulates it; ownership changes through a
registry node, with a version per object so late packets from a former owner are discarded:

```gdscript
$Mesh.authority_mode = TickNetwork.AUTHORITY_DISTRIBUTED
$Mesh.registry_peer = 1       # keeps the owners
$Mesh.clock_master = 1        # the timeline the other nodes follow
$Mesh.start(mesh_transport)

$Crate/TickObject.request_authority()           # the owner may refuse (_approve_authority_request)
$Crate/TickObject.release_authority(3)          # give it to node 3 (0: leave it orphaned)
$Mesh.authority_orphaned.connect(func(object, last_owner, last_frame): object.assign_authority(2))
```

When the node with the registry or the clock leaves, another node takes its roles: the objects keep their ids and
owners, those of the node that left become orphans, and its spawns become the registry's. The other nodes go on
simulating on the same timeline. `roles_changed` tells every node, so the one that got the roles can take over what
the game ties to them.

```gdscript
# Who takes the roles: node 1 has them, node 2 is its reserve, and no other node ever does.
$Mesh.role_candidates = PackedInt32Array([1, 2])
# Only a node connected to 3 nodes of the mesh (itself included) takes or keeps them: with 5 nodes, a part that is
# cut off never gets a second registry.
$Mesh.role_quorum = 3
# How long a node that stopped answering is waited for.
mesh_transport.node_timeout = 2.0

$Mesh.roles_changed.connect(func(registry_peer: int, clock_master: int):
	if registry_peer == $Mesh.get_local_peer_id():
		become_the_active_controller()
)
$Mesh.role_quorum_changed.connect(func(has_quorum: bool):
	if not has_quorum:
		pause_what_only_the_active_controller_does()
)

# Moving both roles by hand, while the mesh runs (a planned switch to the reserve).
$Mesh.set_roles(2, 2)
```

A successor takes the roles only once no node it reaches still sees the node that had them, so a broken link between
two nodes doesn't give the mesh two registries. A node that comes back finds its roles moved and joins as a plain
node; if nobody took them and it restarted, it takes them again from what the other nodes know, on the mesh's
timeline. Without candidates, any node takes the roles, the lowest id first. See
[`demos/distributed_headless`](../demos/distributed_headless).

## Players in a mesh

Players join through a host, which gives them their ids and introduces them: each pair connects directly through its
NATs, or is relayed by the host. The host is the authority; with `REMOTE_MODE_DOLL`, the other players' bodies are
simulated from the inputs they send directly.

```gdscript
var mesh := EnetHostedMeshTransport.create_host(9500)             # or create_player("203.0.113.7", 9500)
multiplayer.multiplayer_peer = mesh.get_multiplayer_peer()         # optional: RPCs on the same mesh
$Game.start(mesh)

# The host decides who joins, from the data each player sends when joining.
mesh.join_validator = func(peer: int, join_data: PackedByteArray, address: String) -> bool:
	return join_data.get_string_from_utf8() == password

# When the host leaves or stops answering, the next player takes over, with the same objects and timeline.
$Game.host_migrated.connect(func(old_host: int, new_host: int): print("new host: ", new_host))
```

The host's port must be reachable from the internet. `TLSOptions` passed to `create_host` and `create_player` encrypt
every link with DTLS. A player with a `takeover_port` takes new players if it becomes the host.

What players can cost each other is bounded: a connection holds a place of the mesh only once its join data arrived;
the relay takes `relay_rate_limit` bytes per second from a player and holds `relay_queue_limit` bytes for one; a
player leaves a mesh bigger than its `pair_limit`; a doll whose controller's inputs disagree with the host's state is
simulated again only so many times, then interpolated for a while; and a player that loses the host migrates only
when most of the players it asks lost it too. See
[`demos/hosted_mesh_headless`](../demos/hosted_mesh_headless), [`demos/p2p_headless`](../demos/p2p_headless) and
[`demos/nat_test`](../demos/nat_test).

## Interest and lag compensation

```gdscript
# Server: each client only gets what is near its player.
$TickNetwork.interest_filter = func(peer: int, object: TickObject) -> bool:
	return players[peer].position.distance_to(object.get_root_node().position) < 50.0

# Client: send the frame it was seeing along with the action...
$Gun/TickObject.send_event(&"shoot", {"view": $TickNetwork.get_view_frame()})
# ...and the server checks the target where that client saw it.
var past: Dictionary = $TickNetwork.get_state_at(target_sync, payload.view)
```

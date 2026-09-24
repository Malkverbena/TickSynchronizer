extends SceneTree
## Headless cluster demo. Roles:
##   w                  mesh node 1: authority and clock master of the cluster
##   a / b              mesh nodes 2 / 3: game servers, with a star for their clients on --edge-port
##   client             connects to a game server's star on --edge-port
## Options: --duration=S --base-port=N (mesh ports N+1..N+3) --edge-port=N

const NpcCluster := preload("res://npc_cluster.gd")
const NpcEdge := preload("res://npc_edge.gd")

var role := "w"
var options := {"duration": 15.0, "base_port": 9100, "edge_port": 7100}
var world: Node
var cluster: TickNetwork
var edge: TickNetwork
var npc: Node2D
var elapsed := 0.0
var next_report := 1.0


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg in ["w", "a", "b", "client"]:
			role = arg
		elif arg.begins_with("--") and "=" in arg:
			var pair := arg.substr(2).split("=")
			options[pair[0].replace("-", "_")] = float(pair[1]) if "." in pair[1] else int(pair[1])

	world = Node.new()
	world.name = "World"
	root.add_child(world)
	npc = Node2D.new()
	npc.name = "Npc"

	if role != "client":
		# The cluster first: on a game server it receives and applies the proxies before the star simulates.
		cluster = TickNetwork.new()
		cluster.name = "Cluster"
		cluster.trust = TickNetwork.TRUST_TRUSTED
		# Only the final clients interpolate.
		cluster.interpolate_remote = false
		world.add_child(cluster)
	if role != "w":
		edge = TickNetwork.new()
		edge.name = "Edge"
		if role != "client":
			edge.clock_network = NodePath("../Cluster")
		world.add_child(edge)

	world.add_child(npc)
	if cluster:
		_add_sync(NpcCluster, "../../Cluster")
	if edge:
		_add_sync(NpcEdge, "../../Edge")

	if cluster:
		var ids := {"w": 1, "a": 2, "b": 3}
		var mesh := EnetMeshTransport.create(ids[role], options.base_port + ids[role])
		for other in ids:
			if other != role:
				mesh.add_node(ids[other], "127.0.0.1", options.base_port + ids[other])
		cluster.start(mesh)
	if edge:
		if role == "client":
			edge.start(EnetStarTransport.create_client("127.0.0.1", options.edge_port))
		else:
			edge.start(EnetStarTransport.create_server(options.edge_port, 8))
	print("[%s] started" % role)


func _add_sync(script: Script, network_path: String) -> void:
	var sync := TickObject.new()
	sync.set_script(script)
	sync.network_path = NodePath(network_path)
	npc.add_child(sync)


func _report() -> void:
	var line := "[%s] clock=%.2f npc=(%.1f, %.1f)" % [role, fmod(Time.get_unix_time_from_system(), 1000.0), npc.position.x, npc.position.y]
	if cluster:
		line += " cluster_frame=%d" % cluster.get_frame()
	if edge:
		var stats: Dictionary = edge.get_stats()
		line += " edge_frame=%d timeline=%.1f latest_snapshot=%d" % [edge.get_frame(), stats.timeline_frame, stats.latest_snapshot_frame]
	print(line)


func _process(delta: float) -> bool:
	elapsed += delta
	if elapsed >= next_report:
		next_report += 3.0
		_report()
	if elapsed < options.duration:
		return false
	_report()
	for network: TickNetwork in [cluster, edge]:
		if network:
			var stats: Dictionary = network.get_stats()
			print("[%s] %s: rewinds %d, snapshots received %d, malformed %d" % [role, network.name, stats.rewinds, stats.snapshots_received, stats.malformed_packets])
			network.stop()
	return true

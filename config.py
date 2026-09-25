MIN_GODOT_VERSION = (4, 6)


def can_build(env, platform):
    # Minimum supported engine version: Godot 4.6. The engine's SConstruct registers `version` as a helper module.
    import version

    if (int(version.major), int(version.minor)) < MIN_GODOT_VERSION:
        print(
            "tick_synchronizer: requires Godot %d.%d or newer (found %s.%s); the module is disabled."
            % (MIN_GODOT_VERSION + (version.major, version.minor))
        )
        return False
    # EnetStarTransport uses ENet and SceneMultiplayer.
    env.module_add_dependencies("tick_synchronizer", ["enet", "multiplayer"])
    # The code must stay portable to every platform supported by Godot. It is validated on linuxbsd,
    # android and windows only (see notes/decisions.md ADR-013).
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "DataBuffer",
        "EnetHostedMeshTransport",
        "EnetMeshTransport",
        "EnetStarTransport",
        "TickCodec",
        "TickNetwork",
        "TickMultiplayerPeer",
        "TickObject",
        "TickSpawner",
        "TickTransport",
    ]


def get_doc_path():
    return "doc_classes"

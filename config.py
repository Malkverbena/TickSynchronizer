def can_build(env, platform):
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "TickSynchronizer",
        "TickSynchronizerBuffer",
        "TickSynchronizerObject",
        "TickSynchronizerSchema",
        "TickSynchronizerSettings",
    ]


def get_doc_path():
    return "doc_classes"

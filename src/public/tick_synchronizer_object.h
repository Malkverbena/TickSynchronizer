#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "scene/main/node.h"

class TickSynchronizerObject : public Node {
	GDCLASS(TickSynchronizerObject, Node);

protected:
	static void _bind_methods();
};

#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "core/io/resource.h"

class TickSynchronizerSchema : public Resource {
	GDCLASS(TickSynchronizerSchema, Resource);

protected:
	static void _bind_methods();
};

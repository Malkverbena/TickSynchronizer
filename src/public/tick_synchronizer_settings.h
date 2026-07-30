#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "core/io/resource.h"

class TickSynchronizerSettings : public Resource {
	GDCLASS(TickSynchronizerSettings, Resource);

protected:
	static void _bind_methods();
};

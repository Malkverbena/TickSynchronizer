#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "core/string/ustring.h"
#include "scene/main/node.h"

#include <cstdint>

class TickSynchronizer : public Node {
	GDCLASS(TickSynchronizer, Node);

protected:
	static void _bind_methods();

public:
	// Returns the precision used to compile this Godot binary: "single" or "double".
	String get_build_precision() const;
	bool is_double_precision() const;
	String get_protocol_magic() const;
	int32_t get_protocol_major() const;
	int32_t get_protocol_minor() const;
	int32_t get_protocol_precision_mode() const;
};

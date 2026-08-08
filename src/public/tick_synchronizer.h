// Declares the root TickSynchronizer Godot node.
// Provides public diagnostics while session synchronization remains future work.

#pragma once

#include "src/internal/tick_synchronizer_build_config.h"

#include "core/string/ustring.h"
#include "scene/main/node.h"

#include <cstdint>

class TickSynchronizer : public Node {
	GDCLASS(TickSynchronizer, Node);

protected:
	// Binds the class API and constants to Godot ClassDB.
	static void _bind_methods();

public:
	// Returns the precision used to compile this Godot binary: "single" or "double".
	String get_build_precision() const;

	// Reports whether the active Godot binary uses double precision.
	bool is_double_precision() const;

	// Returns the printable identifier of the fixed control envelope.
	String get_protocol_magic() const;

	// Returns the control-envelope major version, not the gameplay wire version.
	int32_t get_protocol_major() const;

	// Returns the control-envelope minor version, not the gameplay wire revision.
	int32_t get_protocol_minor() const;

	// Returns the structured precision value carried by handshake profiles.
	int32_t get_protocol_precision_mode() const;
};

// Differences between the engine versions the module supports (Godot 4.6 and newer, ADR-016), kept in one place: the
// rest of the module includes this file instead of the header whose name changed. No class or function is declared
// here.

#pragma once

// `callable_mp()`: `callable_method_pointer.h` up to 4.6, `callable_mp.h` after it.
#if __has_include("core/object/callable_mp.h")
#include "core/object/callable_mp.h"
#else
#include "core/object/callable_method_pointer.h"
#endif

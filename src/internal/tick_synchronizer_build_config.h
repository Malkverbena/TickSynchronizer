// Enforces the compiler and precision assumptions of the supported Godot build.
// Provides compile-time build precision information to module components.

#pragma once

// TickSynchronizer follows the C++17 baseline used by Godot 4.7.1-stable.
// The module must not require C++20 or newer language features.
#if defined(_MSC_VER)
#if !defined(_MSVC_LANG) || _MSVC_LANG < 201703L
#error "TickSynchronizer requires C++17 or newer compiler support."
#endif
#else
#if !defined(__cplusplus) || __cplusplus < 201703L
#error "TickSynchronizer requires C++17 or newer compiler support."
#endif
#endif

#define TICK_SYNCHRONIZER_CXX_STANDARD 201703L

// Defines independent API, wire, revision, and benchmark version constants.
// Acts as the single source of truth for compatibility and methodology versions.

#pragma once

#include <cstdint>

namespace tick_synchronizer::version {

inline constexpr std::uint32_t API_VERSION = 4;

// Version zero identifies the experimental period before the first stable
// wire protocol. Compatibility is not promised across revisions.
inline constexpr std::uint32_t WIRE_PROTOCOL_VERSION = 0;

// Increment this value after every incompatible experimental wire change.
inline constexpr std::uint32_t WIRE_PROTOCOL_REVISION = 2;

// Increment only when benchmark methodology changes in a way that makes
// previous results no longer directly comparable.
inline constexpr std::uint32_t BENCHMARK_SUITE_VERSION = 1;

// The current project policy requires peers to use the same module build.
inline constexpr bool EXACT_BUILD_MATCH_REQUIRED = true;

inline constexpr bool WIRE_PROTOCOL_IS_STABLE =
        WIRE_PROTOCOL_VERSION != 0;

static_assert(API_VERSION > 0);
static_assert(
        WIRE_PROTOCOL_VERSION != 0 || WIRE_PROTOCOL_REVISION > 0,
        "Experimental wire protocol requires a nonzero revision.");

} // namespace tick_synchronizer::version

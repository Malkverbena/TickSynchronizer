#pragma once

#include "core/typedefs.h"

// Wire protocol of `TickSyncCore`. Every message starts with its type, in 8 bits.

static constexpr uint16_t TICK_PROTOCOL_VERSION = 1;

// Frame index meaning "none".
static constexpr uint32_t TICK_FRAME_NONE = UINT32_MAX;

enum TickMessageType {
	TICK_MESSAGE_HELLO = 1,
	TICK_MESSAGE_WELCOME,
	TICK_MESSAGE_REJECT,
	TICK_MESSAGE_REGISTER,
	TICK_MESSAGE_UNREGISTER,
	TICK_MESSAGE_INPUTS,
	TICK_MESSAGE_SNAPSHOT_DELTA,
	TICK_MESSAGE_SNAPSHOT_FULL,
	TICK_MESSAGE_PING,
	TICK_MESSAGE_PONG,
};

enum TickChannel {
	// Reliable: session, registration.
	TICK_CHANNEL_CONTROL = 0,
	// Unreliable: inputs.
	TICK_CHANNEL_INPUTS = 1,
	// Unreliable: state deltas.
	TICK_CHANNEL_STATE = 2,
	// Unreliable ordered: ping and statistics.
	TICK_CHANNEL_STATS = 3,
	// Reliable, dedicated: full snapshots (ADR-008).
	TICK_CHANNEL_SNAPSHOT = 4,
	TICK_CHANNEL_COUNT = 5,
};

// `true` when frame `p_a` comes after frame `p_b`, handling the wrap around.
static inline bool tick_frame_after(uint32_t p_a, uint32_t p_b) {
	return int32_t(p_a - p_b) > 0;
}

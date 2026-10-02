#pragma once

#include "core/math/math_funcs.h"
#include "core/typedefs.h"

// Wire protocol of `TickSyncCore`. Every message starts with its type, in 8 bits.

// 2: interest (`RELEVANCE`) and snapshots split in parts (F8).
// 3: the roles of a distributed mesh move (`ROLES`, `REGISTRY_REPORT`, the roles in the mesh hello; ADR-073).
// 4: role candidates, confirmed losses and inherited spawns in a distributed mesh (`ROLE_STATUS`, the process and the
//    candidates in the mesh hello, who spawned in `SPAWN`; ADR-074).
static constexpr uint16_t TICK_PROTOCOL_VERSION = 4;

// Frame index meaning "none".
static constexpr uint32_t TICK_FRAME_NONE = UINT32_MAX;

// Longest event name, in bytes of UTF-8; a longer one makes the message malformed.
static constexpr int TICK_MAX_EVENT_NAME_BYTES = 255;

// Most frames an input message describes (`input_redundancy` is at most this); a message with more is malformed.
static constexpr int TICK_MAX_INPUT_FRAMES = 64;

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
	TICK_MESSAGE_SPAWN,
	TICK_MESSAGE_DESPAWN,
	TICK_MESSAGE_EVENT,
	// Mesh with distributed authority (`TickMeshCore`, F5).
	TICK_MESSAGE_MESH_HELLO,
	TICK_MESSAGE_CLAIM,
	TICK_MESSAGE_ANNOUNCE,
	TICK_MESSAGE_DROP,
	TICK_MESSAGE_STATE,
	TICK_MESSAGE_AUTH_REQUEST,
	TICK_MESSAGE_AUTH_ASSIGN,
	TICK_MESSAGE_AUTH_TRANSFER,
	TICK_MESSAGE_AUTH_RELEASE,
	TICK_MESSAGE_AUTH_DENIED,
	TICK_MESSAGE_MESH_EVENT,
	// Interest (F8).
	TICK_MESSAGE_RELEVANCE,
	// Roles of a distributed mesh (ADR-073).
	TICK_MESSAGE_ROLES,
	TICK_MESSAGE_REGISTRY_REPORT,
	// What a node sees of the nodes with the roles (ADR-074).
	TICK_MESSAGE_ROLE_STATUS,
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


// The index of the frame a point of a timeline is in. Frame indices wrap around; a point that isn't a number, or
// before the timeline's start, gives frame 0.
static inline uint32_t tick_frame_at(double p_timeline_frame) {
	if (!(p_timeline_frame > 0.0) || !Math::is_finite(p_timeline_frame)) {
		return 0;
	}
	return uint32_t(uint64_t(Math::fmod(Math::floor(p_timeline_frame), 4294967296.0)));
}

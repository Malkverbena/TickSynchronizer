// The quarantine of the net ids of synchronized objects, shared by the engines (`TickSyncCore` and `TickMeshCore`). No
// class is declared here.
//
// A net id that was released is kept with the frame it was released at (ADR-034), and is reused only once a number of
// frames has passed, so that no snapshot still in flight can refer to it.

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

// Forgets the ids of `r_quarantined` that were released at least `p_quarantine` frames before `p_current`: they may be
// given to new objects again. Frame indices wrap around, and so does the difference.
static inline void tick_prune_quarantine(HashMap<uint16_t, uint32_t> &r_quarantined, uint32_t p_current, uint32_t p_quarantine) {
	LocalVector<uint16_t> expired;
	for (const KeyValue<uint16_t, uint32_t> &E : r_quarantined) {
		if (p_current - E.value >= p_quarantine) {
			expired.push_back(E.key);
		}
	}
	for (const uint16_t id : expired) {
		r_quarantined.erase(id);
	}
}

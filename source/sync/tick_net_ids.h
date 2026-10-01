#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

// Net ids released recently, with the frame each was released at (ADR-034): an id is reused only once it's been
// released for `p_quarantine` frames, so no snapshot in flight can refer to it anymore.

// Forgets the released ids that are past their quarantine.
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

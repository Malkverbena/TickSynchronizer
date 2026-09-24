#include "tick_sync_object.h"

#include "core/templates/hashfuncs.h"

void TickSchema::add(const StringName &p_name, const Ref<TickCodec> &p_codec) {
	ERR_FAIL_COND_MSG(p_codec.is_null(), vformat("The codec of \"%s\" is null.", p_name));
	ERR_FAIL_COND_MSG(find(p_name) >= 0, vformat("The variable \"%s\" is already declared.", p_name));
	ERR_FAIL_COND_MSG(names.size() >= 255, "An object can't declare more than 255 variables.");
	names.push_back(p_name);
	codecs.push_back(p_codec);
}

int TickSchema::find(const StringName &p_name) const {
	for (uint32_t i = 0; i < names.size(); i++) {
		if (names[i] == p_name) {
			return int(i);
		}
	}
	return -1;
}

void TickSchema::clear() {
	names.clear();
	codecs.clear();
}

uint32_t TickSchema::hash() const {
	uint32_t h = hash_murmur3_one_32(names.size());
	for (uint32_t i = 0; i < names.size(); i++) {
		h = hash_murmur3_one_32(String(names[i]).hash(), h);
		h = codecs[i]->hash(h);
	}
	return hash_fmix32(h);
}

void TickSyncObject::apply_interpolated_state(const LocalVector<Variant> &p_values) {
	for (uint32_t i = 0; i < p_values.size(); i++) {
		set_sync_var(int(i), p_values[i]);
	}
}

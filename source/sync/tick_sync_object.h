#pragma once

#include "../codec/tick_codec.h"
#include "../common/tick_data_buffer.h"

#include "core/string/string_name.h"
#include "core/templates/local_vector.h"

// Synchronized variables of an object, in declaration order, with their codecs.
struct TickSchema {
	LocalVector<StringName> names;
	LocalVector<Ref<TickCodec>> codecs;

	void add(const StringName &p_name, const Ref<TickCodec> &p_codec);
	int find(const StringName &p_name) const;
	int size() const { return int(names.size()); }
	void clear();
	// Identifies the variables and their encoding; peers verify it before synchronizing an object.
	uint32_t hash() const;
};

// An object synchronized by `TickSyncCore`. Implemented by the `TickObject` node, and by plain C++ objects in
// the tests.
class TickSyncObject {
public:
	virtual ~TickSyncObject() {}

	// Identity shared by all the peers.
	virtual String get_sync_path() const = 0;
	// Peer whose input drives this object: the server (1) or a client.
	virtual int get_controller_peer() const = 0;
	virtual const TickSchema &get_sync_schema() const = 0;

	virtual Variant get_sync_var(int p_index) const = 0;
	virtual void set_sync_var(int p_index, const Variant &p_value) = 0;

	// Writes the input of the current tick; only called on the controller.
	virtual void collect_input(TickDataBuffer &r_input) = 0;
	// Advances the object by one tick. `p_input` is ready for reading and empty when no input is available.
	virtual void process_tick(double p_delta, TickDataBuffer &p_input) = 0;

	// Applies an interpolated state to an object that isn't simulated locally. The default sets the variables.
	virtual void apply_interpolated_state(const LocalVector<Variant> &p_values);
	// Whether the peers that don't control this object simulate it with its controller's inputs (a doll, ADR-045)
	// instead of interpolating it. Needs the controller's inputs, sent directly in a mesh.
	virtual bool is_doll_enabled() const { return false; }

	// Validates an event sent by `p_sender` to this object: 1 accepts, 0 refuses, -1 (the default) lets the
	// network's trust policy decide.
	virtual int validate_event(int p_sender, const StringName &p_event, const Variant &p_payload) { return -1; }
	// Executes an event. `p_frame` is the frame it was scheduled for.
	virtual void on_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) {}

	// Distributed authority: the owner of this object changed (0: nobody owns it).
	virtual void on_authority_changed(int p_old_owner, int p_new_owner) {}
	// Distributed authority: `p_requester` asks for this object, owned here. 1 approves, 0 refuses, -1 (the
	// default) approves.
	virtual int approve_authority_request(int p_requester) { return -1; }
	// Client: the server started or stopped sending this object's state (interest, ADR-053).
	virtual void on_relevance_changed(bool p_relevant) {}
	// The `Object` behind this synchronized object, if any; used to report it in signals.
	virtual Object *get_sync_instance() { return nullptr; }
};

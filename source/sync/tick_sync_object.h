// What the engines synchronize: `TickSchema` and `TickSyncObject`.
//
// `TickSchema` lists the synchronized variables of an object, in declaration order, each with the codec that writes it
// to the wire. `TickSyncObject` is the interface of an object synchronized by `TickSyncCore` or `TickMeshCore`: through
// it the engines read and write the object's variables, collect its input, run its ticks and tell it what happened
// (events, changes of owner, interest). It's implemented by the `TickObject` node, and by plain C++ objects in the
// tests.

#pragma once

#include "../codec/tick_codec.h"
#include "../common/tick_data_buffer.h"

#include "core/string/string_name.h"
#include "core/templates/local_vector.h"

// Synchronized variables of an object, in declaration order, with their codecs.
struct TickSchema {
	LocalVector<StringName> names;
	LocalVector<Ref<TickCodec>> codecs;

	// Declares a variable. Fails if the codec is null, the name is already declared or the object has 255 variables.
	void add(const StringName &p_name, const Ref<TickCodec> &p_codec);


	// The index of a variable, or -1 when it isn't declared.
	int find(const StringName &p_name) const;


	// How many variables are declared.
	int size() const { return int(names.size()); }


	// Forgets every variable.
	void clear();


	// Identifies the variables and their encoding; peers verify it before synchronizing an object.
	uint32_t hash() const;
};

// An object synchronized by `TickSyncCore`. Implemented by the `TickObject` node, and by plain C++ objects in
// the tests.
class TickSyncObject {
public:
	// Lets an implementation be destroyed through the interface.
	virtual ~TickSyncObject() {}


	// Identity shared by all the peers.
	virtual String get_sync_path() const = 0;


	// Peer whose input drives this object: the server (1) or a client.
	virtual int get_controller_peer() const = 0;


	// The synchronized variables of this object.
	virtual const TickSchema &get_sync_schema() const = 0;


	// The current value of the variable at `p_index` of the schema.
	virtual Variant get_sync_var(int p_index) const = 0;


	// Sets the variable at `p_index` of the schema: a state that came from the network, or a past state restored for a
	// rewind.
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

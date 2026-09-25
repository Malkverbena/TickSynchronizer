#pragma once

#include "../tick/tick_clock.h"
#include "../transport/tick_transport.h"
#include "tick_sync_object.h"

#include "core/variant/dictionary.h"

// A synchronization engine: `TickSyncCore` (a single authority, F2–F4) or `TickMeshCore` (an owner per object, F5).
// `TickNetwork` drives one of them through this interface (ADR-040).
//
// Engines don't read the clock: `process()` receives the current time, so they run the same with a real network
// and with the simulated one of the tests.
class TickEngine {
public:
	class Listener {
	public:
		virtual ~Listener() {}
		// Server: a client passed the handshake. Client: the server accepted this client. Mesh: a node is ready.
		virtual void on_peer_ready(int p_peer) {}
		virtual void on_peer_left(int p_peer) {}
		// The other side refused the connection.
		virtual void on_rejected(const String &p_reason) {}
		// Client: the clock is synchronized and the local objects are now predicted.
		virtual void on_prediction_started(uint32_t p_frame) {}
		// Client: the predicted objects diverged at `p_frame` and `p_frame_count` frames were simulated again.
		virtual void on_rewound(uint32_t p_frame, int p_frame_count) {}
		// Validates an event without target object; 1 accepts, 0 refuses, -1 accepts (the default).
		virtual int validate_network_event(int p_sender, const StringName &p_event, const Variant &p_payload) { return -1; }
		// Executes an event without target object.
		virtual void on_network_event(int p_sender, const StringName &p_event, const Variant &p_payload, uint32_t p_frame) {}
		// Another peer spawned (or despawned) something with `p_spawner`; the game instantiates it.
		virtual void on_spawn(const String &p_spawner, uint32_t p_spawn_id, int p_scene, const String &p_name, int p_controller, const Variant &p_data) {}
		virtual void on_despawn(const String &p_spawner, uint32_t p_spawn_id) {}
		// Mesh: the owner of a local object changed (0: nobody).
		virtual void on_authority_changed(TickSyncObject *p_object, int p_old_owner, int p_new_owner) {}
		// Mesh: the owner of a local object left or released it without a new owner.
		virtual void on_authority_orphaned(TickSyncObject *p_object, int p_last_owner, uint32_t p_last_frame) {}
		// Mesh: a request for the authority of a local object was refused.
		virtual void on_authority_request_denied(TickSyncObject *p_object) {}
		// Server: whether `p_object` is relevant to `p_peer` (1 relevant, 0 not, -1 no opinion). Asked every
		// `interest_interval` ticks for every client and object (ADR-053).
		virtual int filter_relevance(int p_peer, TickSyncObject *p_object) { return -1; }
		// Client: the server started or stopped sending the state of a local object.
		virtual void on_relevance_changed(TickSyncObject *p_object, bool p_relevant) {}
	};

	struct Settings {
		int ticks_per_second = 60;
		// Single authority: the peer that simulates every object and is the clock master (ADR-036): the hub of a
		// star, or the node of a mesh chosen by the project.
		int authority_peer = 1;
		// Distributed authority: the node that keeps the registry of owners (ADR-041) and the clock master
		// (ADR-042).
		int registry_peer = 1;
		int clock_master = 1;
		// When `false`, the remote objects get the latest state without interpolation (proxies on servers that
		// relay them to their own clients, ADR-039).
		bool interpolate_remote = true;
		// Frames of history kept for snapshots, predictions and interpolation.
		int history_size = 128;
		// Past inputs repeated in each input packet, against packet loss.
		int input_redundancy = 5;
		// A snapshot is sent every `snapshot_interval` ticks.
		int snapshot_interval = 1;
		// Distributed authority: every object's state is sent at least every `keyframe_interval` ticks, even if it
		// didn't change (ADR-043).
		int keyframe_interval = 30;
		// How far behind the authority the interpolated objects are rendered, in seconds.
		double interpolation_delay = 0.1;
		// Inputs the server should have buffered ahead of its frame; the client adapts its speed to keep it
		// between these bounds.
		int min_input_buffer = 2;
		int max_input_buffer = 8;
		// Largest speed change of the client, and how much of the buffer error is corrected per frame.
		double max_time_scale_delta = 0.1;
		double time_scale_gain = 0.02;
		// Seconds between pings once the clock is synchronized.
		double ping_interval = 0.5;
		int clock_min_samples = 4;
		// When `false` (untrusted clients), the server limits the rate and size of what clients send, and only
		// accepts events for an object from its controller unless the object validates them itself.
		bool trusted = false;
		// Untrusted: input packets and events accepted per second from each client (with a burst of the same size).
		int max_input_packets_per_second = 180;
		int max_events_per_second = 30;
		// Untrusted: largest input of one object per frame, in bits, and largest event payload, in bytes.
		int max_input_bits = 1024;
		int max_event_bytes = 4096;
		// Largest delay, in seconds, of an event scheduled for a future frame.
		double max_event_delay = 10.0;
		// Interest (ADR-053): whether objects are relevant to every client until told otherwise, and how often, in
		// ticks, the listener's filter is asked.
		bool default_relevant = true;
		int interest_interval = 10;
	};

	virtual ~TickEngine() {}

	virtual void set_settings(const Settings &p_settings) = 0;
	virtual const Settings &get_settings() const = 0;
	virtual void set_listener(Listener *p_listener) = 0;

	virtual Error start(const Ref<TickTransport> &p_transport, uint64_t p_now_usec) = 0;
	virtual void stop() = 0;
	virtual bool is_running() const = 0;

	// Objects can be registered before the engine starts.
	virtual void register_object(TickSyncObject *p_object) = 0;
	virtual void unregister_object(TickSyncObject *p_object) = 0;
	// Net id of an object, or 0 when unknown.
	virtual uint16_t get_net_id(const TickSyncObject *p_object) const = 0;

	// Receives, simulates the pending ticks, sends, and updates the remote objects.
	virtual void process(double p_delta, uint64_t p_now_usec) = 0;
	// Updates only the interpolated objects, for rendering between ticks.
	virtual void update_interpolation(uint64_t p_now_usec) = 0;

	// Next frame to simulate.
	virtual uint32_t get_frame() const = 0;
	// Frame of this network's timeline at `p_now_usec`, with the fraction of the frame, or a negative value while
	// it isn't known yet.
	virtual double get_timeline_frame(uint64_t p_now_usec) const = 0;
	// Follows another engine's timeline (ADR-038) instead of the local delta; null for the local delta.
	virtual void set_clock_source(const TickEngine *p_source) = 0;
	virtual const TickClock &get_clock() const = 0;

	virtual bool is_server() const { return false; }
	virtual bool is_rewinding() const { return false; }
	virtual bool is_predicting() const { return false; }
	// Whether this peer may spawn: the authority of a single authority network, any node of a mesh.
	virtual bool can_spawn() const = 0;

	virtual uint32_t spawn(const String &p_spawner, int p_scene, const String &p_name, int p_controller, const Variant &p_data) = 0;
	virtual void despawn(uint32_t p_spawn_id) = 0;
	virtual uint32_t get_next_spawn_id() const = 0;

	virtual Error send_event(TickSyncObject *p_target, const StringName &p_name, const Variant &p_payload, uint32_t p_frame, int p_peer) = 0;
	virtual uint32_t get_event_frame(double p_seconds) const = 0;

	// Distributed authority (ADR-041). Single authority networks have a fixed owner and refuse these.
	virtual int get_owner(const TickSyncObject *p_object) const = 0;
	virtual Error request_authority(TickSyncObject *p_object) { return ERR_UNAVAILABLE; }
	virtual Error release_authority(TickSyncObject *p_object, int p_to_peer) { return ERR_UNAVAILABLE; }
	virtual Error assign_authority(TickSyncObject *p_object, int p_peer) { return ERR_UNAVAILABLE; }

	// Interest (ADR-053). Server: `p_peer` 0 means every client.
	virtual Error set_relevant(TickSyncObject *p_object, int p_peer, bool p_relevant) { return ERR_UNAVAILABLE; }
	virtual bool is_relevant(const TickSyncObject *p_object, int p_peer) const { return true; }

	// History (ADR-054): the state of an object at a frame of the authority's timeline (with a fraction, interpolated),
	// while it's in the history. On a client, the frame shown by the interpolated objects.
	virtual bool get_state_at(const TickSyncObject *p_object, double p_frame, LocalVector<Variant> &r_values) const { return false; }
	virtual double get_view_frame(uint64_t p_now_usec) const { return get_timeline_frame(p_now_usec); }

	virtual Dictionary get_stats_dictionary() const = 0;
};

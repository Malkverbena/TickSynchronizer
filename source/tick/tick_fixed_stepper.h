// The source of the fixed ticks: `TickFixedStepper`.
//
// It turns the variable deltas of the engine's frames into fixed ticks, each with its own frame index. The engines
// (`TickSyncCore`, `TickMeshCore`) run one stepper each, and speed it up or slow it down to follow the timeline of the
// network.
//
// Usage, once per engine frame:
//     const int ticks = stepper.advance(delta);
//     for (int i = 0; i < ticks; i++) {
//         process_tick(stepper.pop_tick());
//     }

#pragma once

#include "core/typedefs.h"

class TickFixedStepper {
	int ticks_per_second = 60;
	double tick_duration = 1.0 / 60.0;
	double time_scale = 1.0;
	int max_ticks_per_advance = 8;

	double accumulator = 0.0;
	int pending_ticks = 0;
	uint32_t next_frame_index = 0;
	uint64_t dropped_ticks = 0;

public:
	// Sets how many ticks a second has; must be positive.
	void set_ticks_per_second(int p_ticks_per_second);


	// How many ticks a second has.
	int get_ticks_per_second() const { return ticks_per_second; }


	// Seconds a tick lasts: the inverse of the ticks per second.
	double get_tick_duration() const { return tick_duration; }


	// Speeds up (> 1) or slows down (< 1) the ticks, used to keep a peer ahead of or behind another one.
	void set_time_scale(double p_time_scale);


	// See `set_time_scale()`.
	double get_time_scale() const { return time_scale; }


	// Ticks beyond this limit in a single `advance()` are dropped, so a long hitch doesn't stall the game.
	void set_max_ticks_per_advance(int p_max_ticks);


	// See `set_max_ticks_per_advance()`.
	int get_max_ticks_per_advance() const { return max_ticks_per_advance; }


	// Adds `p_delta` seconds and returns the number of ticks now pending.
	int advance(double p_delta);


	// Ticks counted by `advance()` and not yet consumed with `pop_tick()`.
	int get_pending_ticks() const { return pending_ticks; }


	// Consumes one pending tick and returns its frame index.
	uint32_t pop_tick();


	// Returns the next frame index and moves past it, without pending ticks: for a timeline driven from outside
	// (see `TickSyncCore::set_clock_source()`).
	uint32_t step_frame() { return next_frame_index++; }


	// Frame index the next tick will have.
	uint32_t get_next_frame_index() const { return next_frame_index; }


	// Jumps to a frame index, for example after synchronizing with the clock master. Clears pending ticks.
	void set_next_frame_index(uint32_t p_frame_index);


	// Fraction of the next tick already accumulated, in [0, 1); useful to interpolate the visuals.
	double get_interpolation_fraction() const;


	// Ticks dropped so far by the limit of ticks per `advance()`.
	uint64_t get_dropped_ticks() const { return dropped_ticks; }


	// Goes back to frame 0, with no time accumulated and no tick pending or dropped. The settings are kept.
	void reset();
};

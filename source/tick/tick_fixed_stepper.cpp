#include "tick_fixed_stepper.h"

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"

void TickFixedStepper::set_ticks_per_second(int p_ticks_per_second) {
	ERR_FAIL_COND_MSG(p_ticks_per_second <= 0, "The ticks per second must be positive.");
	ticks_per_second = p_ticks_per_second;
	tick_duration = 1.0 / double(p_ticks_per_second);
}

void TickFixedStepper::set_time_scale(double p_time_scale) {
	ERR_FAIL_COND_MSG(!(p_time_scale > 0.0), "The time scale must be positive.");
	time_scale = p_time_scale;
}

void TickFixedStepper::set_max_ticks_per_advance(int p_max_ticks) {
	ERR_FAIL_COND_MSG(p_max_ticks <= 0, "The maximum ticks per advance must be positive.");
	max_ticks_per_advance = p_max_ticks;
}

int TickFixedStepper::advance(double p_delta) {
	ERR_FAIL_COND_V_MSG(!(p_delta >= 0.0), pending_ticks, "The delta can't be negative.");
	accumulator += p_delta * time_scale;

	const double ticks = Math::floor(accumulator / tick_duration);
	if (ticks > double(max_ticks_per_advance)) {
		dropped_ticks += uint64_t(ticks) - uint64_t(max_ticks_per_advance);
		accumulator -= ticks * tick_duration;
		pending_ticks += max_ticks_per_advance;
	} else if (ticks >= 1.0) {
		accumulator -= ticks * tick_duration;
		pending_ticks += int(ticks);
	}
	// Guard against rounding leaving a tiny negative remainder.
	accumulator = MAX(accumulator, 0.0);
	return pending_ticks;
}

uint32_t TickFixedStepper::pop_tick() {
	ERR_FAIL_COND_V_MSG(pending_ticks <= 0, next_frame_index, "There are no pending ticks; call `advance()` first.");
	pending_ticks--;
	// Unsigned overflow wraps around, as expected for frame indices.
	return next_frame_index++;
}

void TickFixedStepper::set_next_frame_index(uint32_t p_frame_index) {
	next_frame_index = p_frame_index;
	pending_ticks = 0;
}

double TickFixedStepper::get_interpolation_fraction() const {
	return CLAMP(accumulator / tick_duration, 0.0, 1.0);
}

void TickFixedStepper::reset() {
	accumulator = 0.0;
	pending_ticks = 0;
	next_frame_index = 0;
	dropped_ticks = 0;
}

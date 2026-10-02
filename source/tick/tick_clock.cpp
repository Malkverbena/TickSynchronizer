// Implementation of `TickClock`: the samples of the ping round trips, the estimate taken from them, the applied offset
// that moves toward it, and the frames of the timeline.

#include "tick_clock.h"

#include "core/error/error_macros.h"

// Makes this clock the master's or a follower's, and drops the samples.
void TickClock::set_master(bool p_master) {
	master = p_master;
	clear_samples();
}


// Number of recent samples used for the estimate, and how many are needed before the clock is
// synchronized.
void TickClock::set_sample_window(int p_max_samples, int p_min_samples) {
	ERR_FAIL_COND_MSG(p_max_samples <= 0, "The sample window must be positive.");
	ERR_FAIL_COND_MSG(p_min_samples <= 0 || p_min_samples > p_max_samples, "The minimum samples must be between 1 and the sample window.");
	max_samples = p_max_samples;
	min_samples = p_min_samples;
	clear_samples();
}


// Whether `add_sample()` takes these times: the local ones are plausible, and so is how far the master's clock is
// from the local one.
bool TickClock::is_plausible_sample(uint64_t p_local_send_usec, uint64_t p_master_usec, uint64_t p_local_receive_usec) {
	if (p_local_send_usec >= uint64_t(MAX_TIME_USEC) || p_local_receive_usec >= uint64_t(MAX_TIME_USEC)) {
		return false;
	}
	// The master's clock may be behind the local one: what counts is how far apart they are.
	return is_plausible_time(int64_t(p_master_usec - p_local_send_usec));
}


// Adds a ping round trip: the local time when the ping was sent, the master time written in the pong, and the
// local time when the pong arrived. Samples with the receive time before the send time are ignored. Returns
// `false`, taking nothing, when a time isn't plausible (see `MAX_TIME_USEC`).
bool TickClock::add_sample(uint64_t p_local_send_usec, uint64_t p_master_usec, uint64_t p_local_receive_usec) {
	ERR_FAIL_COND_V_MSG(master, false, "The clock master doesn't take samples: its clock is the reference.");
	if (!is_plausible_sample(p_local_send_usec, p_master_usec, p_local_receive_usec)) {
		return false;
	}
	if (p_local_receive_usec < p_local_send_usec) {
		return true;
	}

	// Until the clock is synchronized nothing uses it: the estimate applies right away.
	const bool was_synchronized = is_synchronized();
	const int64_t applied = offset_at(p_local_receive_usec);

	Sample sample;
	sample.rtt_usec = p_local_receive_usec - p_local_send_usec;
	// The master wrote its time, on average, half a round trip before the pong arrived.
	const uint64_t local_at_master = p_local_send_usec + sample.rtt_usec / 2;
	sample.offset_usec = int64_t(p_master_usec - local_at_master);

	if (int(samples.size()) < max_samples) {
		samples.push_back(sample);
	} else {
		samples[next_sample] = sample;
	}
	next_sample = (next_sample + 1) % max_samples;
	update_estimate();

	if (holding) {
		if (int(samples.size()) < min_samples) {
			return true;
		}
		// The new master's samples are enough: the frames go on from where the held timeline is, on its clock.
		holding = false;
		master_epoch_usec = pending_epoch_usec;
		const int64_t continued = held_offset_usec - held_epoch_usec + master_epoch_usec;
		const int64_t apart = offset_usec - continued;
		slew_from_usec = (apart > STEP_USEC || apart < -STEP_USEC) ? offset_usec : continued;
		slew_start_usec = p_local_receive_usec;
		return true;
	}

	const int64_t difference = offset_usec - applied;
	slew_from_usec = (!was_synchronized || difference > STEP_USEC || difference < -STEP_USEC) ? offset_usec : applied;
	slew_start_usec = p_local_receive_usec;
	return true;
}


// Drops the samples, the estimate and a held timeline: a follower isn't synchronized again until enough samples
// arrive.
void TickClock::clear_samples() {
	samples.clear();
	next_sample = 0;
	offset_usec = 0;
	rtt_usec = 0;
	slew_from_usec = 0;
	slew_start_usec = 0;
	holding = false;
}


// The frames are known: this is the master, the samples are enough, or the previous timeline is held.
bool TickClock::is_synchronized() const {
	return master || holding || int(samples.size()) >= min_samples;
}


// The master is another node now (its clock has another origin): drops the samples and keeps the frames on the
// timeline given by `p_offset_usec` and `p_epoch_usec` until the new master's samples are enough. Then the applied
// offset starts where the held timeline was and moves toward the new estimate.
void TickClock::hold(int64_t p_offset_usec, int64_t p_epoch_usec) {
	ERR_FAIL_COND_MSG(master, "The clock master doesn't follow a timeline.");
	clear_samples();
	holding = true;
	held_offset_usec = p_offset_usec;
	held_epoch_usec = p_epoch_usec;
	pending_epoch_usec = p_epoch_usec;
}


// Sets the master time at which frame 0 starts. While the previous timeline is held, the epoch is the new master's:
// it applies with its samples. Returns `false`, keeping the epoch it had, when the new one isn't plausible (see
// `MAX_TIME_USEC`).
bool TickClock::set_master_epoch_usec(int64_t p_master_epoch_usec) {
	if (!is_plausible_time(p_master_epoch_usec)) {
		return false;
	}
	if (holding) {
		pending_epoch_usec = p_master_epoch_usec;
		return true;
	}
	master_epoch_usec = p_master_epoch_usec;
	return true;
}


// Takes the estimate from the sample with the lowest round trip.
void TickClock::update_estimate() {
	if (samples.is_empty()) {
		return;
	}
	int best = 0;
	for (uint32_t i = 1; i < samples.size(); i++) {
		if (samples[i].rtt_usec < samples[best].rtt_usec) {
			best = int(i);
		}
	}
	offset_usec = samples[best].offset_usec;
	rtt_usec = samples[best].rtt_usec;
}


// The offset applied at the given local time.
int64_t TickClock::offset_at(uint64_t p_local_usec) const {
	const int64_t remaining = offset_usec - slew_from_usec;
	if (remaining == 0) {
		return offset_usec;
	}
	if (p_local_usec <= slew_start_usec) {
		return slew_from_usec;
	}
	const int64_t moved = int64_t((p_local_usec - slew_start_usec) * uint64_t(SLEW_PER_MILLE) / 1000);
	if (remaining > 0) {
		return slew_from_usec + MIN(moved, remaining);
	}
	return slew_from_usec - MIN(moved, -remaining);
}


// Difference between the highest and the lowest round trip time among the current samples; an estimate of
// the jitter.
uint64_t TickClock::get_rtt_spread_usec() const {
	if (samples.is_empty()) {
		return 0;
	}
	uint64_t highest = samples[0].rtt_usec;
	uint64_t lowest = samples[0].rtt_usec;
	for (const Sample &sample : samples) {
		highest = MAX(highest, sample.rtt_usec);
		lowest = MIN(lowest, sample.rtt_usec);
	}
	return highest - lowest;
}


// The master's time at the given local time, by the applied offset.
uint64_t TickClock::local_to_master_usec(uint64_t p_local_usec) const {
	// Unsigned arithmetic wraps, which is the intended result for negative offsets.
	return master ? p_local_usec : p_local_usec + uint64_t(offset_at(p_local_usec));
}


// The local time at which the master's clock shows `p_master_usec`: the inverse of `local_to_master_usec()`.
uint64_t TickClock::master_to_local_usec(uint64_t p_master_usec) const {
	if (master) {
		return p_master_usec;
	}
	// The applied offset depends on the local time: a few steps from the estimate converge, since it moves at 5% of the
	// time (each step divides the error by 20).
	uint64_t local = p_master_usec - uint64_t(offset_usec);
	for (int i = 0; i < 3; i++) {
		local = p_master_usec - uint64_t(offset_at(local));
	}
	return local;
}


// Sets how many frames a second of the timeline has; must be positive.
void TickClock::set_ticks_per_second(int p_ticks_per_second) {
	ERR_FAIL_COND_MSG(p_ticks_per_second <= 0, "The ticks per second must be positive.");
	ticks_per_second = p_ticks_per_second;
}


// Microseconds of the master's clock since frame 0 at the given local time; negative before the epoch. The offsets and
// the epochs are plausible times (see `MAX_TIME_USEC`), so the result fits; the arithmetic is unsigned, so a local
// time that isn't plausible wraps around instead of overflowing.
int64_t TickClock::get_timeline_usec(uint64_t p_local_usec) const {
	return int64_t(p_local_usec + uint64_t(get_timeline_offset_usec(p_local_usec)) - uint64_t(get_timeline_epoch_usec()));
}


// Frame the master is processing at the given local time.
uint32_t TickClock::get_master_frame(uint64_t p_local_usec) const {
	// Signed, so a local time before the master's clock started doesn't wrap around.
	const int64_t elapsed = get_timeline_usec(p_local_usec);
	if (elapsed < 0) {
		return 0;
	}
	// Frame indices wrap around, like the ones of `TickFixedStepper`.
	return uint32_t(uint64_t(elapsed) * uint64_t(ticks_per_second) / 1000000);
}


// Same as `get_master_frame()`, with the fraction of the frame elapsed; negative before the epoch.
double TickClock::get_master_frame_time(uint64_t p_local_usec) const {
	return double(get_timeline_usec(p_local_usec)) * double(ticks_per_second) / 1000000.0;
}

#include "tick_clock.h"

#include "core/error/error_macros.h"

void TickClock::set_master(bool p_master) {
	master = p_master;
	clear_samples();
}

void TickClock::set_sample_window(int p_max_samples, int p_min_samples) {
	ERR_FAIL_COND_MSG(p_max_samples <= 0, "The sample window must be positive.");
	ERR_FAIL_COND_MSG(p_min_samples <= 0 || p_min_samples > p_max_samples, "The minimum samples must be between 1 and the sample window.");
	max_samples = p_max_samples;
	min_samples = p_min_samples;
	clear_samples();
}

void TickClock::add_sample(uint64_t p_local_send_usec, uint64_t p_master_usec, uint64_t p_local_receive_usec) {
	ERR_FAIL_COND_MSG(master, "The clock master doesn't take samples: its clock is the reference.");
	if (p_local_receive_usec < p_local_send_usec) {
		return;
	}

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
}

void TickClock::clear_samples() {
	samples.clear();
	next_sample = 0;
	offset_usec = 0;
	rtt_usec = 0;
}

bool TickClock::is_synchronized() const {
	return master || int(samples.size()) >= min_samples;
}

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

uint64_t TickClock::local_to_master_usec(uint64_t p_local_usec) const {
	// Unsigned arithmetic wraps, which is the intended result for negative offsets.
	return master ? p_local_usec : p_local_usec + uint64_t(offset_usec);
}

uint64_t TickClock::master_to_local_usec(uint64_t p_master_usec) const {
	return master ? p_master_usec : p_master_usec - uint64_t(offset_usec);
}

void TickClock::set_ticks_per_second(int p_ticks_per_second) {
	ERR_FAIL_COND_MSG(p_ticks_per_second <= 0, "The ticks per second must be positive.");
	ticks_per_second = p_ticks_per_second;
}

uint32_t TickClock::get_master_frame(uint64_t p_local_usec) const {
	// Signed, so a local time before the master's clock started doesn't wrap around.
	const int64_t master_usec = int64_t(p_local_usec) + (master ? 0 : offset_usec);
	if (master_usec < int64_t(master_epoch_usec)) {
		return 0;
	}
	// Frame indices wrap around, like the ones of `TickFixedStepper`.
	return uint32_t(uint64_t(master_usec - int64_t(master_epoch_usec)) * uint64_t(ticks_per_second) / 1000000);
}

double TickClock::get_master_frame_time(uint64_t p_local_usec) const {
	const int64_t master_usec = int64_t(p_local_usec) + (master ? 0 : offset_usec);
	return double(master_usec - int64_t(master_epoch_usec)) * double(ticks_per_second) / 1000000.0;
}

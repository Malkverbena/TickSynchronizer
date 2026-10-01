#pragma once

#include "core/templates/local_vector.h"
#include "core/typedefs.h"

// Estimates the clock of the network's clock master, so all the peers agree on the current frame.
//
// The synchronization is explicit: the clock doesn't send anything. The network layer exchanges ping/pong
// messages and feeds each round trip with `add_sample()`. All the times are in microseconds, taken from a
// monotonic local clock (for example `OS::get_ticks_usec()`), so the result doesn't depend on `real_t`.
//
// The master answers pings with its own time; its clock is the reference and it needs no samples.
//
// The estimate changes whenever a sample with a lower round trip arrives or the best one leaves the window. The frames
// don't follow it at once: once synchronized, the applied offset moves toward the estimate at 5% of the elapsed time,
// so the master frame never jumps nor goes back (ADR-071). A difference above 100 ms is applied at once.
class TickClock {
	struct Sample {
		uint64_t rtt_usec = 0;
		int64_t offset_usec = 0;
	};

	bool master = false;
	int max_samples = 16;
	int min_samples = 4;
	LocalVector<Sample> samples;
	int next_sample = 0;

	// The estimate, from the sample with the lowest round trip.
	int64_t offset_usec = 0;
	uint64_t rtt_usec = 0;
	// The applied offset: `slew_from_usec` at the local time `slew_start_usec`, then moving toward the estimate.
	int64_t slew_from_usec = 0;
	uint64_t slew_start_usec = 0;

	int ticks_per_second = 60;
	// Master time of frame 0; negative when the timeline started before the master's clock (another process).
	int64_t master_epoch_usec = 0;

	void update_estimate();
	// The offset applied at the given local time.
	int64_t offset_at(uint64_t p_local_usec) const;

public:
	// The applied offset moves toward the estimate by this much per thousand of the elapsed time; a difference above
	// `STEP_USEC` is applied at once.
	static constexpr int64_t SLEW_PER_MILLE = 50;
	static constexpr int64_t STEP_USEC = 100000;

	void set_master(bool p_master);
	bool is_master() const { return master; }

	// Number of recent samples used for the estimate, and how many are needed before the clock is
	// synchronized.
	void set_sample_window(int p_max_samples, int p_min_samples);

	// Adds a ping round trip: the local time when the ping was sent, the master time written in the pong, and the
	// local time when the pong arrived. Samples with the receive time before the send time are ignored.
	void add_sample(uint64_t p_local_send_usec, uint64_t p_master_usec, uint64_t p_local_receive_usec);
	void clear_samples();
	int get_sample_count() const { return int(samples.size()); }

	bool is_synchronized() const;

	// Estimated `master time - local time`, from the sample with the lowest round trip, which has the least
	// queuing delay. The frames use the applied offset, which moves toward it.
	int64_t get_offset_usec() const { return offset_usec; }
	int64_t get_applied_offset_usec(uint64_t p_local_usec) const { return master ? 0 : offset_at(p_local_usec); }
	// Lowest round trip time among the current samples.
	uint64_t get_rtt_usec() const { return rtt_usec; }
	// Difference between the highest and the lowest round trip time among the current samples; an estimate of
	// the jitter.
	uint64_t get_rtt_spread_usec() const;

	uint64_t local_to_master_usec(uint64_t p_local_usec) const;
	uint64_t master_to_local_usec(uint64_t p_master_usec) const;

	// Frame timing: frame 0 starts at `p_master_epoch_usec` (master time).
	void set_ticks_per_second(int p_ticks_per_second);
	int get_ticks_per_second() const { return ticks_per_second; }
	void set_master_epoch_usec(int64_t p_master_epoch_usec) { master_epoch_usec = p_master_epoch_usec; }
	int64_t get_master_epoch_usec() const { return master_epoch_usec; }

	// Frame the master is processing at the given local time.
	uint32_t get_master_frame(uint64_t p_local_usec) const;
	// Same as `get_master_frame()`, with the fraction of the frame elapsed; negative before the epoch.
	double get_master_frame_time(uint64_t p_local_usec) const;
};

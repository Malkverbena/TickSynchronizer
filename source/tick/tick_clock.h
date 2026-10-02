// The estimate of the network's clock: `TickClock`.
//
// It estimates the clock of the network's clock master, so all the peers agree on the current frame.
//
// The synchronization is explicit: the clock doesn't send anything. The network layer exchanges ping/pong messages and
// feeds each round trip with `add_sample()`. All the times are in microseconds, taken from a monotonic local clock (for
// example `OS::get_ticks_usec()`), so the result doesn't depend on `real_t`.
//
// The master answers pings with its own time; its clock is the reference and it needs no samples.
//
// The estimate changes whenever a sample with a lower round trip arrives or the best one leaves the window. The frames
// don't follow it at once: once synchronized, the applied offset moves toward the estimate at 5% of the elapsed time,
// so the master frame never jumps nor goes back (ADR-071). A difference above 100 ms is applied at once.
//
// When another node becomes the master, `hold()` keeps the frames going on the timeline followed until then while the
// samples of the new master arrive; the frames then move to its estimate from where they were (ADR-074).

#pragma once

#include "core/templates/local_vector.h"
#include "core/typedefs.h"

class TickClock {
	// A ping round trip: how long it took, and the `master time - local time` it gives.
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

	// The master changed: until the new one's samples are enough, the frames come from the previous timeline (its
	// offset and epoch); the new master's epoch waits in `pending_epoch_usec`.
	bool holding = false;
	int64_t held_offset_usec = 0;
	int64_t held_epoch_usec = 0;
	int64_t pending_epoch_usec = 0;

	// Takes the estimate from the sample with the lowest round trip.
	void update_estimate();


	// The offset applied at the given local time.
	int64_t offset_at(uint64_t p_local_usec) const;


	// Microseconds of the master's clock since frame 0 at the given local time; negative before the epoch.
	int64_t get_timeline_usec(uint64_t p_local_usec) const;

public:
	// The applied offset moves toward the estimate by this much per thousand of the elapsed time; a difference above
	// `STEP_USEC` is applied at once.
	static constexpr int64_t SLEW_PER_MILLE = 50;
	static constexpr int64_t STEP_USEC = 100000;
	// A time or an epoch beyond this (about 35 years, in microseconds) can't come from a running network. A sample or an
	// epoch with one is refused, so nothing a faulty master sends overflows the arithmetic of the frames.
	static constexpr int64_t MAX_TIME_USEC = int64_t(1) << 50;
	// Whether a time or an epoch is closer to zero than `MAX_TIME_USEC`.
	static bool is_plausible_time(int64_t p_usec) { return p_usec > -MAX_TIME_USEC && p_usec < MAX_TIME_USEC; }


	// Whether `add_sample()` takes these times: the local ones are plausible, and so is how far the master's clock is
	// from the local one.
	static bool is_plausible_sample(uint64_t p_local_send_usec, uint64_t p_master_usec, uint64_t p_local_receive_usec);


	// Makes this clock the master's or a follower's, and drops the samples.
	void set_master(bool p_master);


	// Whether this is the clock master, whose own clock is the reference.
	bool is_master() const { return master; }


	// Number of recent samples used for the estimate, and how many are needed before the clock is
	// synchronized.
	void set_sample_window(int p_max_samples, int p_min_samples);


	// Adds a ping round trip: the local time when the ping was sent, the master time written in the pong, and the
	// local time when the pong arrived. Samples with the receive time before the send time are ignored. Returns
	// `false`, taking nothing, when a time isn't plausible (see `MAX_TIME_USEC`).
	bool add_sample(uint64_t p_local_send_usec, uint64_t p_master_usec, uint64_t p_local_receive_usec);


	// Drops the samples, the estimate and a held timeline: a follower isn't synchronized again until enough samples
	// arrive.
	void clear_samples();


	// How many samples the estimate is taken from.
	int get_sample_count() const { return int(samples.size()); }


	// The frames are known: this is the master, the samples are enough, or the previous timeline is held.
	bool is_synchronized() const;


	// More samples are needed soon: not synchronized yet, or holding the previous timeline.
	bool needs_samples() const { return !master && int(samples.size()) < min_samples; }


	// The master is another node now (its clock has another origin): drops the samples and keeps the frames on the
	// timeline given by `p_offset_usec` and `p_epoch_usec` until the new master's samples are enough. Then the applied
	// offset starts where the held timeline was and moves toward the new estimate.
	void hold(int64_t p_offset_usec, int64_t p_epoch_usec);


	// Whether the frames still come from the timeline given to `hold()`.
	bool is_holding() const { return holding; }


	// The timeline the frames come from at the given local time: the held one, or the applied offset and the epoch.
	int64_t get_timeline_offset_usec(uint64_t p_local_usec) const { return holding ? held_offset_usec : (master ? 0 : offset_at(p_local_usec)); }


	// The master time of frame 0 on the timeline the frames come from: the held one's, or the current epoch.
	int64_t get_timeline_epoch_usec() const { return holding ? held_epoch_usec : master_epoch_usec; }


	// Estimated `master time - local time`, from the sample with the lowest round trip, which has the least
	// queuing delay. The frames use the applied offset, which moves toward it.
	int64_t get_offset_usec() const { return offset_usec; }


	// The offset the frames use at the given local time, which moves toward the estimate; 0 on the master.
	int64_t get_applied_offset_usec(uint64_t p_local_usec) const { return master ? 0 : offset_at(p_local_usec); }


	// Lowest round trip time among the current samples.
	uint64_t get_rtt_usec() const { return rtt_usec; }


	// Difference between the highest and the lowest round trip time among the current samples; an estimate of
	// the jitter.
	uint64_t get_rtt_spread_usec() const;


	// The master's time at the given local time, by the applied offset.
	uint64_t local_to_master_usec(uint64_t p_local_usec) const;


	// The local time at which the master's clock shows `p_master_usec`: the inverse of `local_to_master_usec()`.
	uint64_t master_to_local_usec(uint64_t p_master_usec) const;


	// Sets how many frames a second of the timeline has; must be positive.
	void set_ticks_per_second(int p_ticks_per_second);


	// How many frames a second of the timeline has.
	int get_ticks_per_second() const { return ticks_per_second; }


	// Sets the master time at which frame 0 starts. While the previous timeline is held, the epoch is the new master's:
	// it applies with its samples. Returns `false`, keeping the epoch it had, when the new one isn't plausible (see
	// `MAX_TIME_USEC`).
	bool set_master_epoch_usec(int64_t p_master_epoch_usec);


	// The master time at which frame 0 starts.
	int64_t get_master_epoch_usec() const { return master_epoch_usec; }


	// Frame the master is processing at the given local time.
	uint32_t get_master_frame(uint64_t p_local_usec) const;


	// Same as `get_master_frame()`, with the fraction of the frame elapsed; negative before the epoch.
	double get_master_frame_time(uint64_t p_local_usec) const;
};

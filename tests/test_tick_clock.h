#pragma once

#include "../source/tick/tick_clock.h"
#include "../source/tick/tick_fixed_stepper.h"

#include "tests/test_macros.h"

namespace TestTickClock {

TEST_CASE("[Modules][TickSynchronizer][TickFixedStepper] Fixed ticks from variable deltas") {
	TickFixedStepper stepper;
	stepper.set_ticks_per_second(60);

	CHECK(stepper.advance(1.0 / 120.0) == 0);
	CHECK(stepper.advance(1.0 / 120.0 + 1e-9) == 1);
	CHECK(stepper.pop_tick() == 0);
	CHECK(stepper.get_pending_ticks() == 0);

	CHECK(stepper.advance(3.5 / 60.0) == 3);
	CHECK(stepper.pop_tick() == 1);
	CHECK(stepper.pop_tick() == 2);
	CHECK(stepper.pop_tick() == 3);
	CHECK(stepper.get_interpolation_fraction() == doctest::Approx(0.5).epsilon(1e-6));

	// Many frames at an irregular frame rate: the ticks follow the elapsed time.
	stepper.reset();
	int ticks = 0;
	double elapsed = 0.0;
	for (int i = 0; i < 7000; i++) {
		const double delta = (i % 3 == 0) ? 0.004 : 0.010;
		elapsed += delta;
		ticks += stepper.advance(delta);
		while (stepper.get_pending_ticks() > 0) {
			stepper.pop_tick();
		}
	}
	CHECK(Math::abs(double(ticks) - elapsed * 60.0) < 1.0);
	CHECK(stepper.get_next_frame_index() == uint32_t(ticks));
}

TEST_CASE("[Modules][TickSynchronizer][TickFixedStepper] Hitches are capped and time scale applies") {
	TickFixedStepper stepper;
	stepper.set_ticks_per_second(30);
	stepper.set_max_ticks_per_advance(4);
	CHECK(stepper.advance(1.01) == 4);
	CHECK(stepper.get_dropped_ticks() == 26);

	stepper.reset();
	stepper.set_time_scale(2.0);
	CHECK(stepper.advance(1.01 / 30.0) == 2);

	stepper.set_next_frame_index(UINT32_MAX);
	stepper.set_time_scale(1.0);
	CHECK(stepper.advance(2.0 / 30.0) == 2);
	CHECK(stepper.pop_tick() == UINT32_MAX);
	// Frame indices wrap around.
	CHECK(stepper.pop_tick() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][TickClock] Estimates the master clock from ping samples") {
	TickClock clock;
	clock.set_sample_window(8, 3);
	CHECK_FALSE(clock.is_synchronized());

	// The master clock is 5 s ahead of the local one; the path is asymmetric only through queuing delay.
	const int64_t true_offset = 5000000;
	const uint64_t base_rtt = 40000;
	const uint64_t queuing[] = { 30000, 0, 12000, 55000, 7000 };
	uint64_t local = 1000000;
	for (int i = 0; i < 5; i++) {
		const uint64_t send = local;
		// The queuing delay happens on the way back, so the samples that suffered it see a skewed offset.
		const uint64_t master_time = send + base_rtt / 2 + uint64_t(true_offset);
		const uint64_t receive = send + base_rtt + queuing[i];
		clock.add_sample(send, master_time, receive);
		local += 100000;
	}

	CHECK(clock.is_synchronized());
	CHECK(clock.get_sample_count() == 5);
	// The sample without queuing delay wins.
	CHECK(clock.get_rtt_usec() == base_rtt);
	CHECK(clock.get_offset_usec() == true_offset);
	CHECK(clock.local_to_master_usec(2000000) == 7000000);
	CHECK(clock.master_to_local_usec(7000000) == 2000000);

	// The window keeps only the latest samples.
	for (int i = 0; i < 8; i++) {
		clock.add_sample(local, local + 50000 + uint64_t(true_offset), local + 100000);
		local += 100000;
	}
	CHECK(clock.get_sample_count() == 8);
	CHECK(clock.get_rtt_usec() == 100000);
	CHECK(clock.get_offset_usec() == true_offset);
}

TEST_CASE("[Modules][TickSynchronizer][TickClock] Negative offsets and frames") {
	TickClock clock;
	clock.set_sample_window(4, 1);
	// The master started 2 s after the local clock.
	clock.add_sample(3000000, 1000000 + 10000, 3000000 + 20000);
	CHECK(clock.get_offset_usec() == -2000000);

	clock.set_ticks_per_second(60);
	clock.set_master_epoch_usec(500000);
	// Local 2.5 s = master 0.5 s = frame 0.
	CHECK(clock.get_master_frame(2500000) == 0);
	// Local 3.5 s = master 1.5 s = 1 s after the epoch = frame 60.
	CHECK(clock.get_master_frame(3500000) == 60);
	// Before the epoch.
	CHECK(clock.get_master_frame(1000000) == 0);

	// Invalid samples are ignored.
	clock.add_sample(100, 0, 50);
	CHECK(clock.get_sample_count() == 1);

	// The master is always synchronized with itself.
	TickClock master;
	master.set_master(true);
	CHECK(master.is_synchronized());
	CHECK(master.local_to_master_usec(123) == 123);
}

TEST_CASE("[Modules][TickSynchronizer][TickClock] Offset changes are slewed, big ones applied at once") {
	TickClock clock;
	clock.set_sample_window(4, 2);
	const int64_t offset = 3000000;
	// Until the clock is synchronized, the estimate applies right away.
	clock.add_sample(1000000, 1000000 + 20000 + uint64_t(offset), 1040000);
	clock.add_sample(1100000, 1100000 + 20000 + uint64_t(offset), 1140000);
	REQUIRE(clock.is_synchronized());
	CHECK(clock.get_applied_offset_usec(1140000) == offset);

	// A sample with a lower round trip moves the estimate 10 ms: the frames follow at 5% of the elapsed time.
	const uint64_t t = 1200000;
	clock.add_sample(t, t + 10000 + uint64_t(offset + 10000), t + 20000);
	CHECK(clock.get_offset_usec() == offset + 10000);
	CHECK(clock.get_applied_offset_usec(t + 20000) == offset);
	CHECK(clock.get_applied_offset_usec(t + 120000) == offset + 5000);
	CHECK(clock.get_applied_offset_usec(t + 220000) == offset + 10000);
	CHECK(clock.get_applied_offset_usec(t + 1020000) == offset + 10000);
	CHECK(clock.local_to_master_usec(t + 120000) == t + 120000 + uint64_t(offset + 5000));
	// The inverse, within the microsecond the forward rounding loses.
	const int64_t back = int64_t(clock.master_to_local_usec(t + 120000 + uint64_t(offset + 5000))) - int64_t(t + 120000);
	CHECK((back >= -1 && back <= 1));

	// While the offset goes down, the master frame still only goes forward.
	const uint64_t t2 = 2000000;
	clock.add_sample(t2, t2 + 5000 + uint64_t(offset - 10000), t2 + 10000);
	bool forward = true;
	double last = clock.get_master_frame_time(t2 + 10000);
	for (uint64_t now = t2 + 11000; now < t2 + 610000; now += 1000) {
		const double frame = clock.get_master_frame_time(now);
		forward = forward && frame > last;
		last = frame;
	}
	CHECK(forward);
	CHECK(clock.get_applied_offset_usec(t2 + 510000) == offset - 10000);

	// A difference above 100 ms (another master, a long outage) applies at once.
	const uint64_t t3 = 3000000;
	clock.add_sample(t3, t3 + 2500 + uint64_t(offset + 500000), t3 + 5000);
	CHECK(clock.get_applied_offset_usec(t3 + 5000) == offset + 500000);

	// Starting over: nothing to slew from.
	clock.clear_samples();
	clock.add_sample(4000000, 4000000 + 10000 + uint64_t(offset), 4020000);
	CHECK(clock.get_applied_offset_usec(4020000) == offset);
}

} // namespace TestTickClock

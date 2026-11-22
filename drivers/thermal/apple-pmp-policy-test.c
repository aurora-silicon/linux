// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/soc/apple/pmp-cooling.h>
#include <linux/soc/apple/pmp-power-policy.h>
#include "apple-pmp-confirm.h"
#include "apple-pmp-hold.h"

static void apple_pmp_test_fault_recovery(struct kunit *test)
{
	struct apple_pmp_cooling state;
	unsigned int i;

	apple_pmp_cooling_init(&state);
	for (i = 0; i < 100; i++)
		apple_pmp_cooling_sample(&state, 20000000, 50000, 0);
	KUNIT_EXPECT_TRUE(test, state.hard);
	KUNIT_EXPECT_EQ(test, state.good_reads, 1U);
	for (i = 2; i <= 10; i++)
		apple_pmp_cooling_sample(&state, i * 20000000ULL, 50000, 0);
	KUNIT_EXPECT_FALSE(test, state.hard);
	KUNIT_EXPECT_EQ(test, state.level, 8U);
	apple_pmp_cooling_sample(&state, 400000000, 50000, 0);
	KUNIT_EXPECT_EQ(test, state.level, 7U);
	apple_pmp_cooling_sample(&state, 405000000, 50000, -EIO);
	KUNIT_EXPECT_TRUE(test, state.hard);
	KUNIT_EXPECT_EQ(test, state.level, 8U);
}

static void apple_pmp_test_poll_cadence(struct kunit *test)
{
	struct apple_pmp_cooling state;
	unsigned int i;

	apple_pmp_cooling_init(&state);
	KUNIT_EXPECT_TRUE(test, apple_pmp_cooling_fast(&state, 50000, 0));
	for (i = 1; i <= 10; i++)
		apple_pmp_cooling_sample(&state, i * 20000000ULL, 50000, 0);
	KUNIT_EXPECT_FALSE(test, state.hard);
	/* Recovery still holds a cooling level until it has been released. */
	KUNIT_EXPECT_TRUE(test, apple_pmp_cooling_fast(&state, 50000, 0));
	for (i = 0; i < 40 && state.level; i++)
		apple_pmp_cooling_sample(&state, 400000000ULL + i * 100000000ULL,
					 50000, 0);
	KUNIT_EXPECT_EQ(test, state.level, 0U);
	KUNIT_EXPECT_FALSE(test, apple_pmp_cooling_fast(&state, 50000, 0));
	KUNIT_EXPECT_FALSE(test, apple_pmp_cooling_fast(&state, APPLE_PMP_FAST_POLL_MC - 1, 0));
	KUNIT_EXPECT_TRUE(test, apple_pmp_cooling_fast(&state, APPLE_PMP_FAST_POLL_MC, 0));
	KUNIT_EXPECT_TRUE(test, apple_pmp_cooling_fast(&state, 50000, -EIO));
}

static void apple_pmp_test_critical_confirmation(struct kunit *test)
{
	struct apple_pmp_confirmation state = {};
	unsigned int i;

	for (i = 0; i < 100; i++)
		apple_pmp_confirmation_sample(&state, APPLE_PMP_CRITICAL_MC, 0, 10000000);
	KUNIT_EXPECT_EQ(test, state.count, 1U);
	KUNIT_EXPECT_FALSE(test, state.confirmed);
	apple_pmp_confirmation_sample(&state, APPLE_PMP_CRITICAL_MC, -EIO, 15000000);
	KUNIT_EXPECT_EQ(test, state.count, 0U);
	for (i = 0; i < 3; i++)
		apple_pmp_confirmation_sample(&state, APPLE_PMP_CRITICAL_MC, 0,
					      20000000 + i * 5000000ULL);
	KUNIT_EXPECT_TRUE(test, state.confirmed);
	apple_pmp_confirmation_sample(&state, 0, -EIO, 40000000);
	KUNIT_EXPECT_TRUE(test, state.confirmed);
}

static void apple_pmp_test_power_validation(struct kunit *test)
{
	struct apple_pmp_power_policy state;
	u32 mw;
	u32 rails[3] = { 4000, 1000, 0 };

	KUNIT_EXPECT_TRUE(test, apple_pmp_power_decode(0x3f800000, &mw));
	KUNIT_EXPECT_EQ(test, mw, 1000U);
	KUNIT_EXPECT_FALSE(test, apple_pmp_power_decode(0x7f800000, &mw));
	KUNIT_EXPECT_FALSE(test, apple_pmp_power_decode(0xbf800000, &mw));
	KUNIT_EXPECT_FALSE(test, apple_pmp_power_decode(0x42500000, &mw));
	apple_pmp_power_init(&state);
	apple_pmp_power_sample(&state, 500000000, rails, 60000, -EIO);
	KUNIT_EXPECT_TRUE(test, state.fault);
	KUNIT_EXPECT_EQ(test, state.level[0], 8U);
	KUNIT_EXPECT_EQ(test, state.level[1], 8U);
}

static void apple_pmp_test_gpu_budget_freshness(struct kunit *test)
{
	u64 now = 10000000000ULL;
	u64 publication = apple_pmp_gpu_budget_pack(3, now);

	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(0, now), 8U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication, now), 3U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication,
				 now + APPLE_PMP_POWER_STALE_NS), 3U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication,
				 now + APPLE_PMP_POWER_STALE_NS + 1000000), 8U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication, now - 1000000), 8U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication | 15, now), 8U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(
			 apple_pmp_gpu_budget_pack(0, now), now), 0U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(
			 apple_pmp_gpu_budget_pack(100, now), now), 8U);
}

static void apple_pmp_test_shared_power_arbitration(struct kunit *test)
{
	struct apple_pmp_power_policy state;
	u32 rails[3] = {2000, 500, 12000};
	u32 idle[3] = {2000, 500, 500};

	apple_pmp_power_init(&state);
	/* A confirmed, warm, exhausted-burst policy with all domains released. */
	for (unsigned int i = 1; i <= 3; i++)
		apple_pmp_power_sample(&state, i * 500000000ULL, idle, 85000, 0);
	state.level[0] = state.level[1] = state.level[2] = 0;
	apple_pmp_power_sample(&state, 2500000000ULL, rails, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.total_mw, 14500);
	/* GPU is the largest measured consumer even with its existing 4x weight. */
	KUNIT_EXPECT_EQ(test, state.level[0], 0U);
	KUNIT_EXPECT_EQ(test, state.level[1], 0U);
	KUNIT_EXPECT_EQ(test, state.level[2], 1U);
	/* Repeated observations cannot buy another control step. */
	apple_pmp_power_sample(&state, 2500000000ULL, idle, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.level[2], 1U);
	apple_pmp_power_sample(&state, 3500000000ULL, idle, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.level[2], 0U);
	apple_pmp_power_sample(&state, 4000000000ULL, idle, 85000, -EIO);
	KUNIT_EXPECT_TRUE(test, state.fault);
	KUNIT_EXPECT_EQ(test, state.level[2], 8U);
}

static void apple_pmp_test_gpu_attachment(struct kunit *test)
{
	struct apple_pmp_power_policy state;
	u32 rails[3] = {2000, 500, 12000};

	apple_pmp_power_init(&state);
	apple_pmp_power_set_active(&state, 3);
	KUNIT_EXPECT_EQ(test, state.level[2], 0U);
	for (unsigned int i = 1; i <= 3; i++)
		apple_pmp_power_sample(&state, i * 500000000ULL, rails, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.total_mw, 2500);
	KUNIT_EXPECT_EQ(test, state.level[2], 0U);
	apple_pmp_power_set_active(&state, 7);
	KUNIT_EXPECT_EQ(test, state.level[2], 8U);
	apple_pmp_power_sample(&state, 2000000000ULL, rails, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.total_mw, 14500);
	apple_pmp_power_set_active(&state, 3);
	apple_pmp_power_sample(&state, 2500000000ULL, rails, 85000, -EIO);
	KUNIT_EXPECT_EQ(test, state.level[0], 8U);
	KUNIT_EXPECT_EQ(test, state.level[1], 8U);
	KUNIT_EXPECT_EQ(test, state.level[2], 0U);
}

static void apple_pmp_test_gated_hold(struct kunit *test)
{
	struct apple_pmp_gated_hold hold;
	long mc = 0;

	apple_pmp_gated_hold_reset(&hold);
	/* Startup fails closed on -ENODATA even when CPUs are idle. */
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 1000000000ULL, &mc,
						    -ENODATA, true),
			-ENODATA);

	/* Seed with a cool valid reading. */
	mc = 45000;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 1000000000ULL, &mc,
						    0, true),
			0);
	KUNIT_EXPECT_EQ(test, mc, 45000L);
	KUNIT_EXPECT_EQ(test, hold.real_valid_ns, 1000000000ULL);

	/*
	 * Idle CPUs ride out dark sensors up to APPLE_PMP_GATED_IDLE_CAP_NS,
	 * advancing valid_ns while keeping real_valid_ns anchored to the last
	 * real sample.
	 */
	mc = 0;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 10000000000ULL, &mc,
						    -ENODATA, true),
			0);
	KUNIT_EXPECT_EQ(test, mc, 45000L);
	KUNIT_EXPECT_EQ(test, hold.valid_ns, 10000000000ULL);
	KUNIT_EXPECT_EQ(test, hold.real_valid_ns, 1000000000ULL);

	/* Once work begins, the hold lasts < APPLE_PMP_GATED_HOLD_NS, then fails closed. */
	mc = 0;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold,
						    10000000000ULL +
						    APPLE_PMP_GATED_HOLD_NS - 1,
						    &mc, -ENODATA, false),
			0);
	KUNIT_EXPECT_EQ(test, mc, 45000L);
	mc = 0;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold,
						    10000000000ULL +
						    APPLE_PMP_GATED_HOLD_NS,
						    &mc, -ENODATA, false),
			-ENODATA);
	KUNIT_EXPECT_EQ(test, hold.valid_ns, 0ULL);
	KUNIT_EXPECT_EQ(test, hold.real_valid_ns, 0ULL);

	/* A fault cannot clear on subsequent idle -ENODATA without a live sample. */
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 13000000000ULL, &mc,
						    -ENODATA, true),
			-ENODATA);

	/* Absolute idle cap fails closed even when CPUs stay idle continuously. */
	mc = 48000;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 20000000000ULL, &mc,
						    0, true),
			0);
	mc = 0;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold,
						    20000000000ULL +
						    APPLE_PMP_GATED_IDLE_CAP_NS - 1,
						    &mc, -ENODATA, true),
			0);
	KUNIT_EXPECT_EQ(test, mc, 48000L);
	mc = 0;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold,
						    20000000000ULL +
						    APPLE_PMP_GATED_IDLE_CAP_NS,
						    &mc, -ENODATA, true),
			-ENODATA);
	KUNIT_EXPECT_EQ(test, hold.valid_ns, 0ULL);
	KUNIT_EXPECT_EQ(test, hold.real_valid_ns, 0ULL);

	/* Busy transition near the idle cap still cannot exceed the absolute cap. */
	mc = 49000;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 40000000000ULL, &mc,
						    0, true),
			0);
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold,
						    40000000000ULL +
						    APPLE_PMP_GATED_IDLE_CAP_NS -
						    500000000ULL,
						    &mc, -ENODATA, true),
			0);
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold,
						    40000000000ULL +
						    APPLE_PMP_GATED_IDLE_CAP_NS,
						    &mc, -ENODATA, false),
			-ENODATA);

	/* Hot readings fail closed immediately on -ENODATA, even while idle. */
	mc = APPLE_PMP_GATED_HOLD_MC;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 60000000000ULL, &mc,
						    0, true),
			0);
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 60100000000ULL, &mc,
						    -ENODATA, true),
			-ENODATA);
	KUNIT_EXPECT_EQ(test, hold.valid_ns, 0ULL);
	KUNIT_EXPECT_EQ(test, hold.real_valid_ns, 0ULL);

	/* Non-ENODATA errors always fail closed and invalidate the hold. */
	mc = 50000;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 65000000000ULL, &mc,
						    0, true),
			0);
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 65100000000ULL, &mc,
						    -EIO, true),
			-EIO);
	KUNIT_EXPECT_EQ(test, hold.valid_ns, 0ULL);
	KUNIT_EXPECT_EQ(test, hold.real_valid_ns, 0ULL);

	/* Sleep resume refreshes both timestamps only for a cool valid hold. */
	mc = 52000;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 70000000000ULL, &mc,
						    0, false),
			0);
	apple_pmp_gated_hold_resume(&hold, 150000000000ULL);
	KUNIT_EXPECT_EQ(test, hold.valid_ns, 150000000000ULL);
	KUNIT_EXPECT_EQ(test, hold.real_valid_ns, 150000000000ULL);
	mc = 0;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 151000000000ULL, &mc,
						    -ENODATA, false),
			0);
	KUNIT_EXPECT_EQ(test, mc, 52000L);

	/* Resume must not grant a hold after a hot reading or fault. */
	mc = APPLE_PMP_GATED_HOLD_MC;
	KUNIT_EXPECT_EQ(test,
			apple_pmp_gated_hold_sample(&hold, 152000000000ULL, &mc,
						    0, false),
			0);
	apple_pmp_gated_hold_resume(&hold, 160000000000ULL);
	KUNIT_EXPECT_EQ(test, hold.valid_ns, 152000000000ULL);
	KUNIT_EXPECT_EQ(test, hold.real_valid_ns, 152000000000ULL);
	apple_pmp_gated_hold_reset(&hold);
	apple_pmp_gated_hold_resume(&hold, 165000000000ULL);
	KUNIT_EXPECT_EQ(test, hold.valid_ns, 0ULL);
	KUNIT_EXPECT_EQ(test, hold.real_valid_ns, 0ULL);
}

static void apple_pmp_test_cpu_idle_tracker(struct kunit *test)
{
	struct apple_pmp_idle_tracker tracker;
	struct apple_pmp_cpu_sample cpus[10] = {};
	struct apple_pmp_idle_acc acc;
	unsigned int i;

	/* Summed permille avoids per-CPU truncation (10 CPUs x 2.9% = 290). */
	KUNIT_EXPECT_EQ(test, apple_pmp_cpus_busy_permille(0, 0, 10), 1000U);
	KUNIT_EXPECT_EQ(test, apple_pmp_cpus_busy_permille(100, 1000, 0), 1000U);
	KUNIT_EXPECT_EQ(test, apple_pmp_cpus_busy_permille(29000, 1000000, 10), 290U);
	KUNIT_EXPECT_EQ(test, apple_pmp_cpus_busy_permille(24000, 1000000, 10), 240U);
	KUNIT_EXPECT_EQ(test, apple_pmp_cpus_busy_permille(25000, 1000000, 10), 250U);

	apple_pmp_idle_tracker_reset(&tracker);
	KUNIT_EXPECT_TRUE(test, apple_pmp_idle_window_due(&tracker, 100000000ULL));

	/* First call only primes per-CPU baselines and fails closed (not idle). */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	for (i = 0; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], true, 100000, 100000);
	KUNIT_EXPECT_FALSE(test, apple_pmp_idle_finish(&tracker, &acc, 100000000ULL));

	/* Inside APPLE_PMP_IDLE_WINDOW_NS, window is not due yet. */
	KUNIT_EXPECT_FALSE(test,
			   apple_pmp_idle_window_due(&tracker,
						     100000000ULL +
						     APPLE_PMP_IDLE_WINDOW_NS - 1));
	KUNIT_EXPECT_TRUE(test,
			  apple_pmp_idle_window_due(&tracker,
						    100000000ULL +
						    APPLE_PMP_IDLE_WINDOW_NS));

	/* Ten CPUs each at 2.9% busy sum to 290 permille (>= 250) -> busy. */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	for (i = 0; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], true, 197100, 200000);
	KUNIT_EXPECT_FALSE(test, apple_pmp_idle_finish(&tracker, &acc, 200000000ULL));

	/* Ten CPUs each at 2.4% busy sum to 240 permille (< 250) -> idle. */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	for (i = 0; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], true, 294700, 300000);
	KUNIT_EXPECT_TRUE(test, apple_pmp_idle_finish(&tracker, &acc, 300000000ULL));

	/* One CPU at 25% + nine fully idle (with slight tick skew di > dw) -> busy. */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	apple_pmp_idle_add_cpu(&acc, &cpus[0], true, 369700, 400000);
	for (i = 1; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], true, 394800, 400000);
	KUNIT_EXPECT_FALSE(test, apple_pmp_idle_finish(&tracker, &acc, 400000000ULL));

	/* Offline CPU clears its baseline without breaking idle detection. */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	apple_pmp_idle_add_cpu(&acc, &cpus[0], false, 0, 0);
	for (i = 1; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], true, 494800, 500000);
	KUNIT_EXPECT_TRUE(test, apple_pmp_idle_finish(&tracker, &acc, 500000000ULL));
	KUNIT_EXPECT_EQ(test, cpus[0].wall, 0ULL);

	/* Re-onlining CPU 0 fails closed for one window to re-prime its baseline. */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	apple_pmp_idle_add_cpu(&acc, &cpus[0], true, 600000, 600000);
	for (i = 1; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], true, 594800, 600000);
	KUNIT_EXPECT_FALSE(test, apple_pmp_idle_finish(&tracker, &acc, 600000000ULL));

	/* Next window with all CPUs primed and quiet is idle again. */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	apple_pmp_idle_add_cpu(&acc, &cpus[0], true, 700000, 700000);
	for (i = 1; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], true, 694800, 700000);
	KUNIT_EXPECT_TRUE(test, apple_pmp_idle_finish(&tracker, &acc, 700000000ULL));

	/* Zero idle advance on one CPU counts as 100% busy for that core -> busy. */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	apple_pmp_idle_add_cpu(&acc, &cpus[0], true, 700000, 800000);
	for (i = 1; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], true, 794800, 800000);
	KUNIT_EXPECT_FALSE(test, apple_pmp_idle_finish(&tracker, &acc, 800000000ULL));

	/* Non-advancing wall time on any online CPU fails closed. */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	apple_pmp_idle_add_cpu(&acc, &cpus[0], true, 700000, 800000);
	for (i = 1; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], true, 894800, 900000);
	KUNIT_EXPECT_FALSE(test, apple_pmp_idle_finish(&tracker, &acc, 900000000ULL));

	/* All CPUs offline fails closed. */
	apple_pmp_idle_begin(&acc, tracker.window_ns != 0);
	for (i = 0; i < ARRAY_SIZE(cpus); i++)
		apple_pmp_idle_add_cpu(&acc, &cpus[i], false, 0, 0);
	KUNIT_EXPECT_FALSE(test, apple_pmp_idle_finish(&tracker, &acc, 1000000000ULL));
}

static void apple_pmp_test_zone_notify(struct kunit *test)
{
	struct apple_pmp_zone_notify notify;
	const u64 interval = 100000000ULL;

	apple_pmp_zone_notify_reset(&notify);

	/* Healthy updates are throttled to the governor interval (never re-enable). */
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 100000000ULL, 0,
							false, interval),
			APPLE_PMP_ZONE_UPDATE);
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 150000000ULL, 0,
							false, interval),
			APPLE_PMP_ZONE_NONE);
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 200000000ULL, 0,
							false, interval),
			APPLE_PMP_ZONE_UPDATE);

	/* Fault entry notifies once; subsequent fault polls let thermal_core back off. */
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 205000000ULL,
							-ENODATA, false, interval),
			APPLE_PMP_ZONE_UPDATE);
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 210000000ULL,
							-ENODATA, false, interval),
			APPLE_PMP_ZONE_NONE);
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 500000000ULL,
							-ENODATA, false, interval),
			APPLE_PMP_ZONE_NONE);

	/* Recovery notifies immediately without waiting for the governor interval. */
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 505000000ULL, 0,
							false, interval),
			APPLE_PMP_ZONE_UPDATE);
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 510000000ULL, 0,
							false, interval),
			APPLE_PMP_ZONE_NONE);

	/* Recovery after thermal_zone_broken_disable() re-enables, then updates on next poll. */
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 600000000ULL,
							-ENODATA, false, interval),
			APPLE_PMP_ZONE_UPDATE);
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 700000000ULL,
							-ENODATA, true, interval),
			APPLE_PMP_ZONE_NONE);
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 705000000ULL, 0,
							true, interval),
			APPLE_PMP_ZONE_REENABLE);
	/* Failed re-enable retries are throttled to interval_ns. */
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 710000000ULL, 0,
							true, interval),
			APPLE_PMP_ZONE_NONE);
	/* A recurring fault while re-enable is pending stays quiet until recovery. */
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 715000000ULL,
							-ENODATA, true, interval),
			APPLE_PMP_ZONE_NONE);
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 720000000ULL, 0,
							true, interval),
			APPLE_PMP_ZONE_REENABLE);
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 820000000ULL, 0,
							true, interval),
			APPLE_PMP_ZONE_REENABLE);
	/* Once re-enable clears fault_disabled, the next good sample updates immediately. */
	KUNIT_EXPECT_EQ(test,
			(int)apple_pmp_zone_notify_step(&notify, 825000000ULL, 0,
							false, interval),
			APPLE_PMP_ZONE_UPDATE);
}

static struct kunit_case apple_pmp_policy_cases[] = {
	KUNIT_CASE(apple_pmp_test_fault_recovery),
	KUNIT_CASE(apple_pmp_test_poll_cadence),
	KUNIT_CASE(apple_pmp_test_critical_confirmation),
	KUNIT_CASE(apple_pmp_test_power_validation),
	KUNIT_CASE(apple_pmp_test_gpu_budget_freshness),
	KUNIT_CASE(apple_pmp_test_shared_power_arbitration),
	KUNIT_CASE(apple_pmp_test_gpu_attachment),
	KUNIT_CASE(apple_pmp_test_gated_hold),
	KUNIT_CASE(apple_pmp_test_cpu_idle_tracker),
	KUNIT_CASE(apple_pmp_test_zone_notify),
	{},
};

static struct kunit_suite apple_pmp_policy_suite = {
	.name = "apple-pmp-policy",
	.test_cases = apple_pmp_policy_cases,
};
kunit_test_suite(apple_pmp_policy_suite);
MODULE_DESCRIPTION("KUnit tests for the Apple PMP thermal policy");
MODULE_LICENSE("Dual MIT/GPL");

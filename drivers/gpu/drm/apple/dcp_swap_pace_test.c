// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>

#include "dcp-swap-pace.h"

#define TB_HZ 24000000ULL
#define FRAME (TB_HZ / 120)
#define START 4950000ULL

static void swap_ts_current_follows_the_clock(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_CURRENT, 5000000, 4900000, START, TB_HZ);

	KUNIT_EXPECT_EQ(test, ts.ts0, 5000000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts1, 4900000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts3, 0ULL);
	KUNIT_EXPECT_EQ(test, ts.ts4, 0ULL);
	KUNIT_EXPECT_EQ(test, ts.ts6, 5000000ULL);
}

static void swap_ts_current_keeps_a_lone_swap_at_now(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_CURRENT, 5000000, 5000000, START, TB_HZ);

	KUNIT_EXPECT_EQ(test, ts.ts1, 5000000ULL);
}

static void swap_ts_desktop_is_the_constant(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_DESKTOP, 5000000, 5000000, START, TB_HZ);

	KUNIT_EXPECT_EQ(test, ts.ts0, 120ULL);
	KUNIT_EXPECT_EQ(test, ts.ts1, 120ULL);
	KUNIT_EXPECT_EQ(test, ts.ts3, 0ULL);
	KUNIT_EXPECT_EQ(test, ts.ts4, 0ULL);
	KUNIT_EXPECT_EQ(test, ts.ts6, 120ULL);
}

static void swap_ts_lone_frame_backs_up_one_frame(struct kunit *test)
{
	struct dcp_swap_ts lone = dcp_swap_ts_pick(DCP_SWAP_TS_LONE_FRAME, 5000000, 5000000, START, TB_HZ);
	struct dcp_swap_ts burst = dcp_swap_ts_pick(DCP_SWAP_TS_LONE_FRAME, 5000000, 4900000, START, TB_HZ);

	KUNIT_EXPECT_EQ(test, lone.ts0, 5000000ULL);
	KUNIT_EXPECT_EQ(test, lone.ts1, 5000000ULL - FRAME);
	KUNIT_EXPECT_EQ(test, lone.ts3, 0ULL);
	KUNIT_EXPECT_EQ(test, lone.ts4, 0ULL);
	KUNIT_EXPECT_EQ(test, lone.ts6, 5000000ULL);
	KUNIT_EXPECT_EQ(test, burst.ts1, 4900000ULL);
}

static void swap_ts_lone_frame_does_not_wrap_at_boot(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_LONE_FRAME, 100, 100, 100, TB_HZ);

	KUNIT_EXPECT_EQ(test, ts.ts1, 0ULL);
}

static void swap_ts_off_is_all_zero(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_OFF, 5000000, 4900000, START, TB_HZ);

	KUNIT_EXPECT_EQ(test, ts.ts0, 0ULL);
	KUNIT_EXPECT_EQ(test, ts.ts1, 0ULL);
	KUNIT_EXPECT_EQ(test, ts.ts6, 0ULL);
}

/* QuartzCore finish_update on macOS 26.6.2: types 1, 2, 4 and 5; type 7 unset. */
static void swap_ts_macos_targets_an_eighth_frame_before_submit(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_MACOS, 5000000, 4900000, 4000000, TB_HZ);

	KUNIT_EXPECT_EQ(test, ts.ts0, 5000000ULL - FRAME / 8);
	KUNIT_EXPECT_EQ(test, ts.ts1, 4000000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts3, 4000000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts4, 5000000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts6, 0ULL);
}

static void swap_ts_macos_target_is_100us_after_update_start(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_MACOS, 5000000, 4900000, 4999000, TB_HZ);

	KUNIT_EXPECT_EQ(test, ts.ts0, 4999000ULL + TB_HZ / 10000);
}

/* A commit stamped after the submit read (counter skew) must not put the update start in the future. */
static void swap_ts_macos_clamps_a_start_after_submit(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_MACOS, 5000000, 4900000, 5000500, TB_HZ);

	KUNIT_EXPECT_EQ(test, ts.ts1, 5000000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts3, 5000000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts0, 5000000ULL + TB_HZ / 10000);
}

static void swap_ts_unknown_mode_is_current(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(99, 5000000, 4900000, START, TB_HZ);

	KUNIT_EXPECT_EQ(test, ts.ts0, 5000000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts1, 4900000ULL);
}

static void keepalive_delay_is_off_or_bounded(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, dcp_keepalive_delay_ms(0), 0U);
	KUNIT_EXPECT_EQ(test, dcp_keepalive_delay_ms(20), 20U);
	KUNIT_EXPECT_EQ(test, dcp_keepalive_delay_ms(4000000000U), (unsigned int)DCP_KEEPALIVE_MAX_MS);
}

#define MS 1000000ULL
#define T0 (1000 * MS)

/* An idle panel: frames 100 ms apart, the last one at T0 + 300 ms. */
static const u64 idle[DCP_BAND_FRAMES] = { T0, T0 + 100 * MS, T0 + 200 * MS, T0 + 300 * MS };

static void band_hold_waits_past_the_grid_frame(struct kunit *test)
{
	/* 95 ms after the last frame: the next grid frame is 5 ms away. */
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(idle, T0 + 395 * MS), 6 * MS);
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(idle, T0 + 397 * MS), 4 * MS);
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(idle, T0 + 392 * MS), 9 * MS);
}

static void band_hold_ignores_swaps_outside_the_band(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(idle, T0 + 398 * MS), 0ULL);
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(idle, T0 + 391 * MS + MS / 2), 0ULL);
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(idle, T0 + 350 * MS), 0ULL);
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(idle, T0 + 410 * MS), 0ULL);
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(idle, T0 + 299 * MS), 0ULL);
}

static void band_hold_uses_the_median_idle_period(struct kunit *test)
{
	const u64 uneven[DCP_BAND_FRAMES] = { T0, T0 + 108 * MS, T0 + 200 * MS, T0 + 300 * MS };

	/* Intervals 108, 92, 100: period 100, so the grid frame is 5 ms away. */
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(uneven, T0 + 395 * MS), 6 * MS);
}

static void band_hold_needs_an_idle_panel(struct kunit *test)
{
	const u64 busy[DCP_BAND_FRAMES] = { T0, T0 + 100 * MS, T0 + 200 * MS, T0 + 240 * MS };
	const u64 unknown[DCP_BAND_FRAMES] = { 0, T0 + 100 * MS, T0 + 200 * MS, T0 + 300 * MS };

	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(busy, T0 + 335 * MS), 0ULL);
	KUNIT_EXPECT_EQ(test, dcp_band_hold_ns(unknown, T0 + 395 * MS), 0ULL);
}

static struct kunit_case dcp_swap_pace_test_cases[] = {
	KUNIT_CASE(swap_ts_current_follows_the_clock),
	KUNIT_CASE(swap_ts_current_keeps_a_lone_swap_at_now),
	KUNIT_CASE(swap_ts_desktop_is_the_constant),
	KUNIT_CASE(swap_ts_lone_frame_backs_up_one_frame),
	KUNIT_CASE(swap_ts_lone_frame_does_not_wrap_at_boot),
	KUNIT_CASE(swap_ts_off_is_all_zero),
	KUNIT_CASE(swap_ts_macos_targets_an_eighth_frame_before_submit),
	KUNIT_CASE(swap_ts_macos_target_is_100us_after_update_start),
	KUNIT_CASE(swap_ts_macos_clamps_a_start_after_submit),
	KUNIT_CASE(swap_ts_unknown_mode_is_current),
	KUNIT_CASE(keepalive_delay_is_off_or_bounded),
	KUNIT_CASE(band_hold_waits_past_the_grid_frame),
	KUNIT_CASE(band_hold_ignores_swaps_outside_the_band),
	KUNIT_CASE(band_hold_uses_the_median_idle_period),
	KUNIT_CASE(band_hold_needs_an_idle_panel),
	{}
};

static struct kunit_suite dcp_swap_pace_test_suite = {
	.name = "dcp_swap_pace",
	.test_cases = dcp_swap_pace_test_cases,
};
kunit_test_suite(dcp_swap_pace_test_suite);

MODULE_DESCRIPTION("KUnit tests for the DCP swap timestamp choice and keepalive delay");
MODULE_LICENSE("Dual MIT/GPL");

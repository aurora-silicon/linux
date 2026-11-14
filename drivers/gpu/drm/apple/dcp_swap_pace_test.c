// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>

#include "dcp-swap-pace.h"

#define FRAME 200000ULL

static void swap_ts_current_follows_the_clock(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_CURRENT, 5000000, 4900000, FRAME);

	KUNIT_EXPECT_EQ(test, ts.ts0, 5000000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts1, 4900000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts6, 5000000ULL);
}

static void swap_ts_current_keeps_a_lone_swap_at_now(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_CURRENT, 5000000, 5000000, FRAME);

	KUNIT_EXPECT_EQ(test, ts.ts1, 5000000ULL);
}

static void swap_ts_desktop_is_the_constant(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_DESKTOP, 5000000, 5000000, FRAME);

	KUNIT_EXPECT_EQ(test, ts.ts0, 120ULL);
	KUNIT_EXPECT_EQ(test, ts.ts1, 120ULL);
	KUNIT_EXPECT_EQ(test, ts.ts6, 120ULL);
}

static void swap_ts_lone_frame_backs_up_one_frame(struct kunit *test)
{
	struct dcp_swap_ts lone = dcp_swap_ts_pick(DCP_SWAP_TS_LONE_FRAME, 5000000, 5000000, FRAME);
	struct dcp_swap_ts burst = dcp_swap_ts_pick(DCP_SWAP_TS_LONE_FRAME, 5000000, 4900000, FRAME);

	KUNIT_EXPECT_EQ(test, lone.ts0, 5000000ULL);
	KUNIT_EXPECT_EQ(test, lone.ts1, 5000000ULL - FRAME);
	KUNIT_EXPECT_EQ(test, lone.ts6, 5000000ULL);
	KUNIT_EXPECT_EQ(test, burst.ts1, 4900000ULL);
}

static void swap_ts_lone_frame_does_not_wrap_at_boot(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_LONE_FRAME, 100, 100, FRAME);

	KUNIT_EXPECT_EQ(test, ts.ts1, 0ULL);
}

static void swap_ts_off_is_all_zero(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(DCP_SWAP_TS_OFF, 5000000, 4900000, FRAME);

	KUNIT_EXPECT_EQ(test, ts.ts0, 0ULL);
	KUNIT_EXPECT_EQ(test, ts.ts1, 0ULL);
	KUNIT_EXPECT_EQ(test, ts.ts6, 0ULL);
}

static void swap_ts_unknown_mode_is_current(struct kunit *test)
{
	struct dcp_swap_ts ts = dcp_swap_ts_pick(99, 5000000, 4900000, FRAME);

	KUNIT_EXPECT_EQ(test, ts.ts0, 5000000ULL);
	KUNIT_EXPECT_EQ(test, ts.ts1, 4900000ULL);
}

static void keepalive_delay_is_off_or_bounded(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, dcp_keepalive_delay_ms(0), 0U);
	KUNIT_EXPECT_EQ(test, dcp_keepalive_delay_ms(20), 20U);
	KUNIT_EXPECT_EQ(test, dcp_keepalive_delay_ms(4000000000U), (unsigned int)DCP_KEEPALIVE_MAX_MS);
}

static struct kunit_case dcp_swap_pace_test_cases[] = {
	KUNIT_CASE(swap_ts_current_follows_the_clock),
	KUNIT_CASE(swap_ts_current_keeps_a_lone_swap_at_now),
	KUNIT_CASE(swap_ts_desktop_is_the_constant),
	KUNIT_CASE(swap_ts_lone_frame_backs_up_one_frame),
	KUNIT_CASE(swap_ts_lone_frame_does_not_wrap_at_boot),
	KUNIT_CASE(swap_ts_off_is_all_zero),
	KUNIT_CASE(swap_ts_unknown_mode_is_current),
	KUNIT_CASE(keepalive_delay_is_off_or_bounded),
	{}
};

static struct kunit_suite dcp_swap_pace_test_suite = {
	.name = "dcp_swap_pace",
	.test_cases = dcp_swap_pace_test_cases,
};
kunit_test_suite(dcp_swap_pace_test_suite);

MODULE_DESCRIPTION("KUnit tests for the DCP swap timestamp choice and keepalive delay");
MODULE_LICENSE("Dual MIT/GPL");

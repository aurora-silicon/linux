// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the ranking of the timing modes that DCP reports.
 */

#include <kunit/test.h>
#include <linux/array_size.h>
#include <linux/bits.h>

#include "parser.h"
#include "dcp-refresh.h"

struct dcp_mode_rank_case {
	const char *desc;
	bool preferred;
	s64 score;
	bool best_preferred;
	s64 best_score;
	bool above;
};

static const struct dcp_mode_rank_case dcp_mode_rank_cases[] = {
	{ "first mode", false, 0, false, -1, true },
	{ "preferred over a score above bit 32", true, 0, false, BIT_ULL(32) + 1, true },
	{ "score above bit 32 under a preferred mode", false, BIT_ULL(32) + 1, true, 0 },
	{ "preferred with bit 32 already set", true, BIT_ULL(32), false, BIT_ULL(32) + 1, true },
	{ "higher of two preferred scores", true, 5, true, 4, true },
	{ "lower of two preferred scores", true, 4, true, 5 },
	{ "higher of two other scores", false, 7, false, 3, true },
	{ "equal ranks keep the first mode", true, 3, true, 3 },
};

KUNIT_ARRAY_PARAM_DESC(dcp_mode_rank, dcp_mode_rank_cases, desc);

static void dcp_mode_rank_test(struct kunit *test)
{
	const struct dcp_mode_rank_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test, dcp_mode_ranks_above(c->preferred, c->score,
						   c->best_preferred,
						   c->best_score),
			c->above);
}

static void dcp_refresh_range_test(struct kunit *test)
{
	u32 low = 0, high = 0;

	KUNIT_EXPECT_TRUE(test, dcp_refresh_range(120U << 16, 0, 0,
						  true, &low, &high));
	KUNIT_EXPECT_EQ(test, low, 24U << 16);
	KUNIT_EXPECT_EQ(test, high, 120U << 16);
	KUNIT_EXPECT_FALSE(test, dcp_refresh_range(60U << 16, 0, 0,
						   true, &low, &high));
	KUNIT_EXPECT_FALSE(test, dcp_refresh_range(120U << 16, 0, 0,
						   false, &low, &high));
	KUNIT_EXPECT_FALSE(test, dcp_refresh_range(120U << 16, 0, 120U << 16,
						   true, &low, &high));
	KUNIT_EXPECT_FALSE(test, dcp_refresh_range(120U << 16, 24U << 16,
						   144U << 16, true, &low, &high));
	KUNIT_EXPECT_FALSE(test, dcp_refresh_range(120U << 16, 120U << 16,
						   24U << 16, true, &low, &high));
	KUNIT_EXPECT_TRUE(test, dcp_refresh_range(120U << 16, 48U << 16,
						  120U << 16, true, &low, &high));
	KUNIT_EXPECT_EQ(test, low, 48U << 16);
	KUNIT_EXPECT_EQ(test, dcp_desktop_present_deadline(true, true), 120ULL);
	KUNIT_EXPECT_EQ(test, dcp_desktop_present_deadline(false, true), 0ULL);
	KUNIT_EXPECT_EQ(test, dcp_desktop_present_deadline(true, false), 0ULL);
}

static void dcp_refresh_needs_deadline_test(struct kunit *test)
{
	/* 16.16 rates of the fixed timings: 48, 47.952, 50 and 60 Hz. */
	static const u32 fixed[] = { 48U << 16, 3142585, 50U << 16, 60U << 16 };
	unsigned int i;

	KUNIT_EXPECT_TRUE(test, dcp_refresh_needs_deadline(120U << 16, false, true, false));
	KUNIT_EXPECT_TRUE(test, dcp_refresh_needs_deadline(48U << 16, true, false, false));
	KUNIT_EXPECT_FALSE(test, dcp_refresh_needs_deadline(120U << 16, false, false, true));
	for (i = 0; i < ARRAY_SIZE(fixed); i++) {
		/* Legacy internal panels and external outputs keep zero stamps. */
		KUNIT_EXPECT_FALSE(test, dcp_refresh_needs_deadline(fixed[i], false, true, false));
		KUNIT_EXPECT_FALSE(test, dcp_refresh_needs_deadline(fixed[i], false, false, true));
		KUNIT_EXPECT_TRUE(test, dcp_refresh_needs_deadline(fixed[i], false, true, true));
	}
}

static struct kunit_case dcp_mode_rank_test_cases[] = {
	KUNIT_CASE(dcp_refresh_range_test),
	KUNIT_CASE(dcp_refresh_needs_deadline_test),
	KUNIT_CASE_PARAM(dcp_mode_rank_test, dcp_mode_rank_gen_params),
	{}
};

static struct kunit_suite dcp_mode_rank_test_suite = {
	.name = "apple-dcp-mode-rank",
	.test_cases = dcp_mode_rank_test_cases,
};

kunit_test_suite(dcp_mode_rank_test_suite);

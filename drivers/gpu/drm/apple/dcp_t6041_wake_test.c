// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <kunit/test.h>

/* The plane-only commit after a T6041 wake counts only the shown completion of its own swap. */
static void dcp_t6041_wake_shown_swap(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct dcp_wake_swap_mark mark;
	u32 swap;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	WRITE_ONCE(dcp->started_swap_id, 7);
	dcp_note_swap_complete(dcp, 7, true);
	dcp_wake_swap_mark(dcp, &mark);

	/* No swap started by the commit. */
	KUNIT_EXPECT_FALSE(test, dcp_wake_swap_shown(dcp, &mark, &swap));

	/* The commit's swap completes with shown clear: not counted. */
	WRITE_ONCE(dcp->started_swap_id, 8);
	dcp_note_swap_complete(dcp, 8, false);
	KUNIT_EXPECT_FALSE(test, dcp_wake_swap_shown(dcp, &mark, &swap));
	KUNIT_EXPECT_EQ(test, swap, 8U);
	KUNIT_EXPECT_EQ(test, atomic_read(&dcp->shown_swaps), 1);

	/* A late shown completion of the older swap. */
	dcp_note_swap_complete(dcp, 7, true);
	KUNIT_EXPECT_FALSE(test, dcp_wake_swap_shown(dcp, &mark, &swap));

	/* The commit's own swap shown. */
	dcp_note_swap_complete(dcp, 8, true);
	KUNIT_EXPECT_TRUE(test, dcp_wake_swap_shown(dcp, &mark, &swap));
}

static struct kunit_case dcp_t6041_wake_cases[] = {
	KUNIT_CASE(dcp_t6041_wake_shown_swap),
	{}
};

static struct kunit_suite dcp_t6041_wake_suite = {
	.name = "apple-dcp-t6041-wake",
	.test_cases = dcp_t6041_wake_cases,
};
kunit_test_suite(dcp_t6041_wake_suite);

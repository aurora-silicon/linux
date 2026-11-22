// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/* Included by t6040-usb2.c to test its recovery state machine. */

#include <kunit/test.h>

static void t6040_recovery_missed_ack_test(struct kunit *test)
{
	struct t6040_recovery_state state = {};

	t6040_recovery_power_down(&state, false, true);
	KUNIT_EXPECT_TRUE(test, state.cycle_pending);
	KUNIT_EXPECT_TRUE(test, t6040_recovery_start(&state));
	KUNIT_EXPECT_EQ(test, state.retries, (u8)0);
}

static void t6040_recovery_failed_tune_test(struct kunit *test)
{
	struct t6040_recovery_state state = {};

	KUNIT_EXPECT_FALSE(test, t6040_recovery_start(&state));
	KUNIT_EXPECT_TRUE(test, t6040_recovery_retry(&state));
	KUNIT_EXPECT_TRUE(test, state.cycle_pending);
	KUNIT_EXPECT_EQ(test, state.retries, (u8)1);
	KUNIT_EXPECT_EQ(test, state.retried, 1U);
	t6040_recovery_complete(&state);
	KUNIT_EXPECT_FALSE(test, state.cycle_pending);
	/* The counts outlive the bring-up they were taken in. */
	KUNIT_EXPECT_EQ(test, state.retried, 1U);
	KUNIT_EXPECT_EQ(test, state.exhausted, 0U);
	KUNIT_EXPECT_EQ(test, state.failed, 0U);
}

static void t6040_recovery_retry_exhaustion_test(struct kunit *test)
{
	struct t6040_recovery_state state = {};

	KUNIT_ASSERT_TRUE(test, t6040_recovery_retry(&state));
	KUNIT_EXPECT_FALSE(test, t6040_recovery_retry(&state));
	KUNIT_EXPECT_TRUE(test, state.cycle_pending);
	KUNIT_EXPECT_EQ(test, state.retries, (u8)T6040_RECOVERY_RETRIES);
	KUNIT_EXPECT_EQ(test, state.retried, (u32)T6040_RECOVERY_RETRIES);
	KUNIT_EXPECT_EQ(test, state.exhausted, 1U);
	t6040_recovery_failed(&state);
	KUNIT_EXPECT_EQ(test, state.failed, 1U);
	/* A new bring-up starts its retries over, but not the counts. */
	KUNIT_EXPECT_TRUE(test, t6040_recovery_start(&state));
	KUNIT_EXPECT_TRUE(test, t6040_recovery_retry(&state));
	KUNIT_EXPECT_EQ(test, state.retried, (u32)T6040_RECOVERY_RETRIES + 1);
}

/*
 * A power cycle or a wake that does not work ends the bring-up before
 * t6040_recovery_retry() is reached: it is a failure with a retry left.
 */
static void t6040_recovery_failed_cycle_test(struct kunit *test)
{
	struct t6040_recovery_state state = {};

	t6040_recovery_power_down(&state, false, true);
	KUNIT_ASSERT_TRUE(test, t6040_recovery_start(&state));
	t6040_recovery_failed(&state);
	KUNIT_EXPECT_EQ(test, state.failed, 1U);
	KUNIT_EXPECT_EQ(test, state.retried, 0U);
	KUNIT_EXPECT_EQ(test, state.exhausted, 0U);
	/* The next bring-up power-cycles again. */
	KUNIT_EXPECT_TRUE(test, state.cycle_pending);
	KUNIT_EXPECT_TRUE(test, t6040_recovery_start(&state));
	/* A bring-up that works leaves the count of the failed ones alone. */
	t6040_recovery_complete(&state);
	KUNIT_EXPECT_FALSE(test, state.cycle_pending);
	KUNIT_EXPECT_EQ(test, state.failed, 1U);
}

static struct kunit_case t6040_recovery_test_cases[] = {
	KUNIT_CASE(t6040_recovery_missed_ack_test),
	KUNIT_CASE(t6040_recovery_failed_tune_test),
	KUNIT_CASE(t6040_recovery_retry_exhaustion_test),
	KUNIT_CASE(t6040_recovery_failed_cycle_test),
	{}
};

static struct kunit_suite t6040_recovery_test_suite = {
	.name = "phy-apple-t6040-recovery",
	.test_cases = t6040_recovery_test_cases,
};

kunit_test_suite(t6040_recovery_test_suite);

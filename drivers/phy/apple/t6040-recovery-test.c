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
	t6040_recovery_complete(&state);
	KUNIT_EXPECT_FALSE(test, state.cycle_pending);
}

static void t6040_recovery_retry_exhaustion_test(struct kunit *test)
{
	struct t6040_recovery_state state = {};

	KUNIT_ASSERT_TRUE(test, t6040_recovery_retry(&state));
	KUNIT_EXPECT_FALSE(test, t6040_recovery_retry(&state));
	KUNIT_EXPECT_TRUE(test, state.cycle_pending);
	KUNIT_EXPECT_EQ(test, state.retries, (u8)T6040_RECOVERY_RETRIES);
}

static struct kunit_case t6040_recovery_test_cases[] = {
	KUNIT_CASE(t6040_recovery_missed_ack_test),
	KUNIT_CASE(t6040_recovery_failed_tune_test),
	KUNIT_CASE(t6040_recovery_retry_exhaustion_test),
	{}
};

static struct kunit_suite t6040_recovery_test_suite = {
	.name = "phy-apple-t6040-recovery",
	.test_cases = t6040_recovery_test_cases,
};

kunit_test_suite(t6040_recovery_test_suite);

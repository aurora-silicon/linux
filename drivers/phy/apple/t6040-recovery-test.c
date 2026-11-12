// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Included by t6040-usb2.c to test its recovery state machine and which ports
 * it takes through a sleep with the host kept.
 */

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

/* A sleep that powers the SoC down is the power cycle an unclean stop asks for. */
static void t6040_recovery_power_lost_test(struct kunit *test)
{
	struct t6040_recovery_state state = {};

	t6040_recovery_power_down(&state, true, false);
	KUNIT_EXPECT_TRUE(test, state.cycle_pending);
	t6040_recovery_power_lost(&state);
	KUNIT_EXPECT_FALSE(test, state.cycle_pending);
	KUNIT_EXPECT_FALSE(test, t6040_recovery_start(&state));
}

static void t6040_recovery_held_reference_test(struct kunit *test)
{
	unsigned int attempt;
	unsigned int retries = t6040_recovery_pm_retries(false);

	KUNIT_EXPECT_EQ(test, retries, (unsigned int)T6040_RECOVERY_PM_RETRIES);
	for (attempt = 0; attempt < retries; attempt++)
		KUNIT_EXPECT_EQ(test,
				t6040_recovery_pm_action(1, 0, attempt, retries),
				T6040_RECOVERY_PM_RETRY);
	KUNIT_EXPECT_EQ(test,
			t6040_recovery_pm_action(1, 0, attempt, retries),
			T6040_RECOVERY_PM_FALLBACK);
	KUNIT_EXPECT_EQ(test,
			t6040_recovery_pm_action(0, 1, attempt, retries),
			T6040_RECOVERY_PM_FALLBACK);
	KUNIT_EXPECT_EQ(test,
			t6040_recovery_pm_action(0, 0, attempt, retries),
			T6040_RECOVERY_PM_SUSPEND);
}

/*
 * A PHY that is still in use holds the mailbox itself: the recovery doesn't
 * wait for that reference, and suspends the mailbox at once if nothing holds
 * it after all.
 */
static void t6040_recovery_own_reference_test(struct kunit *test)
{
	unsigned int retries = t6040_recovery_pm_retries(true);

	KUNIT_EXPECT_EQ(test, retries, 0U);
	KUNIT_EXPECT_EQ(test, t6040_recovery_pm_action(1, 0, 0, retries),
			T6040_RECOVERY_PM_FALLBACK);
	KUNIT_EXPECT_EQ(test, t6040_recovery_pm_action(0, 1, 0, retries),
			T6040_RECOVERY_PM_FALLBACK);
	KUNIT_EXPECT_EQ(test, t6040_recovery_pm_action(0, 0, 0, retries),
			T6040_RECOVERY_PM_SUSPEND);
}

static struct kunit_case t6040_recovery_test_cases[] = {
	KUNIT_CASE(t6040_recovery_missed_ack_test),
	KUNIT_CASE(t6040_recovery_failed_tune_test),
	KUNIT_CASE(t6040_recovery_retry_exhaustion_test),
	KUNIT_CASE(t6040_recovery_power_lost_test),
	KUNIT_CASE(t6040_recovery_held_reference_test),
	KUNIT_CASE(t6040_recovery_own_reference_test),
	{}
};

static struct kunit_suite t6040_recovery_test_suite = {
	.name = "phy-apple-t6040-recovery",
	.test_cases = t6040_recovery_test_cases,
};

/* A Type-C port's PHY with a USB2 host running on it, as dwc3-apple's start leaves it. */
static struct t6040_usb2_phy *t6040_sleep_test_phy(struct kunit *test)
{
	struct t6040_usb2_phy *tphy = kunit_kzalloc(test, sizeof(*tphy), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, tphy);
	/* Never dereferenced: only that the PHY has a pipehandler counts. */
	tphy->pipehandler = (void __iomem *)tphy;
	tphy->mode = PHY_MODE_USB_HOST;
	tphy->powered = true;
	tphy->dwc3_running = true;
	return tphy;
}

/* USB2 alone, and USB3 with the PIPE on the PHY, are the two states that sleep in place. */
static void t6040_sleep_usb_test(struct kunit *test)
{
	struct t6040_usb2_phy *tphy = t6040_sleep_test_phy(test);

	KUNIT_EXPECT_TRUE(test, t6040_can_sleep_in_place(tphy));
	tphy->usb3_on = true;
	tphy->usb3_pipe = true;
	KUNIT_EXPECT_TRUE(test, t6040_can_sleep_in_place(tphy));
	tphy->usb3_up = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->pipe_switched = true;
	KUNIT_EXPECT_TRUE(test, t6040_can_sleep_in_place(tphy));
}

/* No host, no controller out of reset, or the fixed DisplayPort PHY: nothing to keep. */
static void t6040_sleep_no_host_test(struct kunit *test)
{
	struct t6040_usb2_phy *tphy = t6040_sleep_test_phy(test);

	tphy->powered = false;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->powered = true;
	tphy->dwc3_running = false;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->dwc3_running = true;
	tphy->pipehandler = NULL;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
}

/*
 * DisplayPort's four lanes (pin assignments C and E), a DisplayPort link and
 * USB4/Thunderbolt stop the host. Pin assignment D without a link does not:
 * USB2 alone or with USB3, it is a USB port until the DP phy powers on.
 */
static void t6040_sleep_other_modes_test(struct kunit *test)
{
	struct t6040_usb2_phy *tphy = t6040_sleep_test_phy(test);

	tphy->dp_alt = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->dp_two_lanes = true;
	KUNIT_EXPECT_TRUE(test, t6040_can_sleep_in_place(tphy));
	tphy->usb3_up = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->pipe_switched = true;
	KUNIT_EXPECT_TRUE(test, t6040_can_sleep_in_place(tphy));
	tphy->dp_up = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->dp_alt = false;
	tphy->dp_two_lanes = false;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->dp_up = false;
	KUNIT_EXPECT_TRUE(test, t6040_can_sleep_in_place(tphy));

	tphy->usb3_up = false;
	tphy->pipe_switched = false;
	tphy->usb3_lent = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->cio_up = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->usb3_lent = false;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->cio_up = false;
	tphy->pipe_tunneled = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->pipe_tunneled = false;
	tphy->router_running = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
}

/* A PHY that failed, is down for a sleep already, or didn't come back from one. */
static void t6040_sleep_failed_test(struct kunit *test)
{
	struct t6040_usb2_phy *tphy = t6040_sleep_test_phy(test);

	tphy->shutdown_failed = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->shutdown_failed = false;
	tphy->sleep_no_keep = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->sleep_no_keep = false;
	tphy->sleep_down = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->sleep_down = false;
	tphy->sleep_reset = true;
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	tphy->sleep_reset = false;
	KUNIT_EXPECT_TRUE(test, t6040_can_sleep_in_place(tphy));
}

/* The port can't sleep in place, and the log names the conditions that fail as @names. */
static void t6040_expect_miss(struct kunit *test, const struct t6040_usb2_phy *tphy,
			      const char *names)
{
	char why[T6040_SLEEP_MISSES_LEN];

	t6040_sleep_in_place_misses(tphy, why, sizeof(why));
	KUNIT_EXPECT_FALSE(test, t6040_can_sleep_in_place(tphy));
	KUNIT_EXPECT_STREQ(test, why, names);
}

/*
 * Each of the conditions of t6040_can_sleep_in_place() has its name in the
 * log, all of them fit into the buffer, and a port that sleeps in place has
 * none.
 */
static void t6040_sleep_misses_test(struct kunit *test)
{
	struct t6040_usb2_phy *tphy = t6040_sleep_test_phy(test);
	char why[T6040_SLEEP_MISSES_LEN];

	t6040_sleep_in_place_misses(tphy, why, sizeof(why));
	KUNIT_EXPECT_TRUE(test, t6040_can_sleep_in_place(tphy));
	KUNIT_EXPECT_STREQ(test, why, "");

	tphy->pipehandler = NULL;
	t6040_expect_miss(test, tphy, " no_pipehandler");
	tphy->pipehandler = (void __iomem *)tphy;

	tphy->powered = false;
	t6040_expect_miss(test, tphy, " usb2_off");
	tphy->powered = true;

	tphy->dwc3_running = false;
	t6040_expect_miss(test, tphy, " dwc3_in_reset");
	tphy->dwc3_running = true;

	tphy->mode = PHY_MODE_INVALID;
	t6040_expect_miss(test, tphy, " mode");
	tphy->mode = PHY_MODE_USB_HOST;

	tphy->shutdown_failed = true;
	t6040_expect_miss(test, tphy, " shutdown_failed");
	tphy->shutdown_failed = false;

	tphy->sleep_no_keep = true;
	t6040_expect_miss(test, tphy, " sleep_no_keep");
	tphy->sleep_no_keep = false;

	tphy->sleep_down = true;
	t6040_expect_miss(test, tphy, " sleep_down");
	tphy->sleep_down = false;

	tphy->sleep_reset = true;
	t6040_expect_miss(test, tphy, " sleep_reset");
	tphy->sleep_reset = false;

	tphy->cio_up = true;
	t6040_expect_miss(test, tphy, " cio_up");
	tphy->cio_up = false;

	tphy->pipe_tunneled = true;
	t6040_expect_miss(test, tphy, " pipe_tunneled");
	tphy->pipe_tunneled = false;

	tphy->router_running = true;
	t6040_expect_miss(test, tphy, " router_running");
	tphy->router_running = false;

	tphy->dp_up = true;
	t6040_expect_miss(test, tphy, " dp_up");
	tphy->dp_up = false;

	tphy->dp_alt = true;
	t6040_expect_miss(test, tphy, " dp_four_lanes");
	tphy->dp_alt = false;

	tphy->usb3_lent = true;
	t6040_expect_miss(test, tphy, " usb3_lent");
	tphy->usb3_lent = false;

	tphy->usb3_up = true;
	t6040_expect_miss(test, tphy, " pipe_state");
	tphy->usb3_up = false;

	tphy->pipe_switched = true;
	t6040_expect_miss(test, tphy, " pipe_state");
	tphy->pipe_switched = false;

	/* Pin assignment D without a link fails nothing. */
	tphy->dp_alt = true;
	tphy->dp_two_lanes = true;
	t6040_sleep_in_place_misses(tphy, why, sizeof(why));
	KUNIT_EXPECT_TRUE(test, t6040_can_sleep_in_place(tphy));
	KUNIT_EXPECT_STREQ(test, why, "");
	tphy->dp_two_lanes = false;

	/* A display on all four lanes, with USB3's lanes lent to it. */
	tphy->dp_up = true;
	tphy->usb3_lent = true;
	t6040_expect_miss(test, tphy, " dp_up dp_four_lanes usb3_lent");

	/* Everything at once. */
	tphy->pipehandler = NULL;
	tphy->powered = false;
	tphy->dwc3_running = false;
	tphy->mode = PHY_MODE_INVALID;
	tphy->shutdown_failed = true;
	tphy->sleep_no_keep = true;
	tphy->sleep_down = true;
	tphy->sleep_reset = true;
	tphy->cio_up = true;
	tphy->pipe_tunneled = true;
	tphy->router_running = true;
	tphy->usb3_up = true;
	t6040_expect_miss(test, tphy,
			  " no_pipehandler usb2_off dwc3_in_reset mode shutdown_failed sleep_no_keep sleep_down sleep_reset cio_up pipe_tunneled router_running dp_up dp_four_lanes usb3_lent pipe_state");
}

/*
 * Only the controller's reset and USB2 can fail the keeping of a host, and
 * only when the host was lost: not a sleep refused for a USB change, not what
 * a wake that released the reset reports, and no host that was kept.
 */
static void t6040_sleep_keep_failed_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, t6040_sleep_keep_failed(false, -ENODEV));
	KUNIT_EXPECT_TRUE(test, t6040_sleep_keep_failed(false, -EIO));
	KUNIT_EXPECT_FALSE(test, t6040_sleep_keep_failed(false, -EBUSY));
	KUNIT_EXPECT_FALSE(test, t6040_sleep_keep_failed(false, 0));
	KUNIT_EXPECT_FALSE(test, t6040_sleep_keep_failed(false, APPLE_ATCPHY_SLEEP_RESET));
	KUNIT_EXPECT_FALSE(test, t6040_sleep_keep_failed(true, -ENODEV));
	KUNIT_EXPECT_FALSE(test, t6040_sleep_keep_failed(true, APPLE_ATCPHY_SLEEP_RESET));
}

/*
 * The wake starts USB3 if it ran before the sleep. Not while the coprocessor
 * waits for a power cycle after a stop it didn't acknowledge, until a sleep
 * that powered the SoC down has been that power cycle.
 */
static void t6040_sleep_wake_usb3_test(struct kunit *test)
{
	struct t6040_usb2_phy *tphy = t6040_sleep_test_phy(test);

	KUNIT_EXPECT_FALSE(test, t6040_sleep_wake_usb3(tphy));
	tphy->sleep_usb3 = true;
	KUNIT_EXPECT_TRUE(test, t6040_sleep_wake_usb3(tphy));
	t6040_recovery_power_down(&tphy->recovery, true, false);
	KUNIT_EXPECT_FALSE(test, t6040_sleep_wake_usb3(tphy));
	t6040_recovery_power_lost(&tphy->recovery);
	KUNIT_EXPECT_TRUE(test, t6040_sleep_wake_usb3(tphy));
}

static struct kunit_case t6040_sleep_test_cases[] = {
	KUNIT_CASE(t6040_sleep_usb_test),
	KUNIT_CASE(t6040_sleep_no_host_test),
	KUNIT_CASE(t6040_sleep_other_modes_test),
	KUNIT_CASE(t6040_sleep_failed_test),
	KUNIT_CASE(t6040_sleep_misses_test),
	KUNIT_CASE(t6040_sleep_keep_failed_test),
	KUNIT_CASE(t6040_sleep_wake_usb3_test),
	{}
};

static struct kunit_suite t6040_sleep_test_suite = {
	.name = "phy-apple-t6040-sleep",
	.test_cases = t6040_sleep_test_cases,
};

kunit_test_suites(&t6040_recovery_test_suite, &t6040_sleep_test_suite);

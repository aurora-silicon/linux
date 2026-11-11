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

static struct kunit_case t6040_sleep_test_cases[] = {
	KUNIT_CASE(t6040_sleep_usb_test),
	KUNIT_CASE(t6040_sleep_no_host_test),
	KUNIT_CASE(t6040_sleep_other_modes_test),
	KUNIT_CASE(t6040_sleep_failed_test),
	{}
};

static struct kunit_suite t6040_sleep_test_suite = {
	.name = "phy-apple-t6040-sleep",
	.test_cases = t6040_sleep_test_cases,
};

kunit_test_suites(&t6040_recovery_test_suite, &t6040_sleep_test_suite);

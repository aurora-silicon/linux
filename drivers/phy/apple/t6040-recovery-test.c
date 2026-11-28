// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/* Included by t6040-usb2.c to test its recovery state machine and USB2 modes. */

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

static void t6040_recovery_power_lost_test(struct kunit *test)
{
	struct t6040_usb2_phy *tphy;

	tphy = kunit_kzalloc(test, sizeof(*tphy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tphy);

	t6040_recovery_power_down(&tphy->recovery, false, false);
	KUNIT_ASSERT_TRUE(test, t6040_recovery_retry(&tphy->recovery));
	tphy->shutdown_failed = true;

	t6040_power_lost(tphy);
	KUNIT_EXPECT_FALSE(test, tphy->recovery.cycle_pending);
	KUNIT_EXPECT_EQ(test, tphy->recovery.retries, (u8)0);
	KUNIT_EXPECT_FALSE(test, tphy->shutdown_failed);
}

static struct kunit_case t6040_recovery_test_cases[] = {
	KUNIT_CASE(t6040_recovery_missed_ack_test),
	KUNIT_CASE(t6040_recovery_failed_tune_test),
	KUNIT_CASE(t6040_recovery_retry_exhaustion_test),
	KUNIT_CASE(t6040_recovery_power_lost_test),
	{}
};

static struct kunit_suite t6040_recovery_test_suite = {
	.name = "phy-apple-t6040-recovery",
	.test_cases = t6040_recovery_test_cases,
};

/* USB2 device mode needs the boot loader's device tunables; host mode never does. */
static void t6040_usb2_device_needs_tunables_test(struct kunit *test)
{
	struct t6040_usb2_phy *tphy = kunit_kzalloc(test, sizeof(*tphy), GFP_KERNEL);
	struct phy *phy = kunit_kzalloc(test, sizeof(*phy), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, tphy);
	KUNIT_ASSERT_NOT_NULL(test, phy);
	phy_set_drvdata(phy, tphy);
	mutex_init(&tphy->lock);
	tphy->mode = PHY_MODE_USB_HOST;

	KUNIT_EXPECT_EQ(test, t6040_usb2_validate(phy, PHY_MODE_USB_HOST, 0, NULL), 0);
	KUNIT_EXPECT_EQ(test, t6040_usb2_validate(phy, PHY_MODE_USB_DEVICE, 0, NULL), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, t6040_usb2_validate(phy, PHY_MODE_USB_OTG, 0, NULL), -EOPNOTSUPP);
	/* A refused set_mode leaves the port a host. */
	KUNIT_EXPECT_EQ(test, t6040_usb2_set_mode(phy, PHY_MODE_USB_DEVICE, 0), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, tphy->mode, PHY_MODE_USB_HOST);

	tphy->tunable_usb2_device = kunit_kzalloc(test, sizeof(*tphy->tunable_usb2_device),
						  GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tphy->tunable_usb2_device);
	KUNIT_EXPECT_EQ(test, t6040_usb2_validate(phy, PHY_MODE_USB_DEVICE, 0, NULL), 0);
	KUNIT_EXPECT_EQ(test, t6040_usb2_set_mode(phy, PHY_MODE_USB_DEVICE, 0), 0);
	KUNIT_EXPECT_EQ(test, tphy->mode, PHY_MODE_USB_DEVICE);
	KUNIT_EXPECT_EQ(test, t6040_usb2_validate(phy, PHY_MODE_USB_HOST, 0, NULL), 0);
}

static struct kunit_case t6040_usb2_mode_test_cases[] = {
	KUNIT_CASE(t6040_usb2_device_needs_tunables_test),
	{}
};

static struct kunit_suite t6040_usb2_mode_test_suite = {
	.name = "phy-apple-t6040-usb2-mode",
	.test_cases = t6040_usb2_mode_test_cases,
};

kunit_test_suites(&t6040_recovery_test_suite, &t6040_usb2_mode_test_suite);

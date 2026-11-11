// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/module.h>

#include "dwc3-apple-state.h"

/* Host mode is never selected before the reset; the xHCI root hub selects it. */
static void dwc3_apple_host_left_to_xhci_test(struct kunit *test)
{
	enum phy_mode mode = PHY_MODE_USB_HOST;

	KUNIT_EXPECT_EQ(test, dwc3_apple_usb2_mode_before_reset(DWC3_APPLE_HOST, &mode), 0);
	KUNIT_EXPECT_EQ(test, mode, PHY_MODE_INVALID);
}

/* Device mode has no later selector and is selected before the reset. */
static void dwc3_apple_device_selected_before_reset_test(struct kunit *test)
{
	enum phy_mode mode = PHY_MODE_INVALID;

	KUNIT_EXPECT_EQ(test, dwc3_apple_usb2_mode_before_reset(DWC3_APPLE_DEVICE, &mode), 0);
	KUNIT_EXPECT_EQ(test, mode, PHY_MODE_USB_DEVICE);
}

/* States that are not roles are rejected and select nothing. */
static void dwc3_apple_non_role_test(struct kunit *test)
{
	static const enum dwc3_apple_state states[] = {
		DWC3_APPLE_PROBE_PENDING, DWC3_APPLE_NO_CABLE,
	};
	enum phy_mode mode;
	int i;

	for (i = 0; i < ARRAY_SIZE(states); i++) {
		mode = PHY_MODE_USB_HOST;
		KUNIT_EXPECT_EQ(test, dwc3_apple_usb2_mode_before_reset(states[i], &mode), -EINVAL);
		KUNIT_EXPECT_EQ(test, mode, PHY_MODE_INVALID);
	}
}

/* A role that arrives during a sleep decides what comes back after it. */
static void dwc3_apple_role_during_sleep_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, dwc3_apple_state_for_role(USB_ROLE_HOST), DWC3_APPLE_HOST);
	KUNIT_EXPECT_EQ(test, dwc3_apple_state_for_role(USB_ROLE_DEVICE), DWC3_APPLE_DEVICE);
	KUNIT_EXPECT_EQ(test, dwc3_apple_state_for_role(USB_ROLE_NONE), DWC3_APPLE_NO_CABLE);
}

/* A host kept through a sleep is neither stopped nor started for a role: no device mode. */
static void dwc3_apple_role_while_kept_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, dwc3_apple_state_for_role_kept(USB_ROLE_HOST, true), DWC3_APPLE_HOST);
	KUNIT_EXPECT_EQ(test, dwc3_apple_state_for_role_kept(USB_ROLE_NONE, true),
			DWC3_APPLE_NO_CABLE);
	KUNIT_EXPECT_EQ(test, dwc3_apple_state_for_role_kept(USB_ROLE_DEVICE, true),
			DWC3_APPLE_NO_CABLE);
	KUNIT_EXPECT_EQ(test, dwc3_apple_state_for_role_kept(USB_ROLE_DEVICE, false),
			DWC3_APPLE_DEVICE);
}

/* A fixed hub's host may always be kept; a Type-C port's only by a PHY that restarts it. */
static void dwc3_apple_sleep_may_keep_test(struct kunit *test)
{
	/* fixed hub (T8140): as before, whatever the Type-C parameter says */
	KUNIT_EXPECT_TRUE(test, dwc3_apple_sleep_may_keep(true, false, true, DWC3_APPLE_HOST,
							  false, false));
	KUNIT_EXPECT_TRUE(test, dwc3_apple_sleep_may_keep(true, false, false, DWC3_APPLE_HOST,
							  false, false));
	/* Type-C port with the T6040 PHY, in host mode: kept unless switched off */
	KUNIT_EXPECT_TRUE(test, dwc3_apple_sleep_may_keep(false, true, true, DWC3_APPLE_HOST,
							  false, false));
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_may_keep(false, true, false, DWC3_APPLE_HOST,
							   false, false));
	/* any other Type-C port stops */
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_may_keep(false, false, true, DWC3_APPLE_HOST,
							   false, false));
	/* no host, no core, a glue that goes away, a core that didn't come back */
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_may_keep(false, true, true, DWC3_APPLE_NO_CABLE,
							   false, false));
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_may_keep(false, true, true,
							   DWC3_APPLE_PROBE_PENDING, false, false));
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_may_keep(false, true, true, DWC3_APPLE_DEVICE,
							   false, false));
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_may_keep(false, true, true, DWC3_APPLE_HOST,
							   true, false));
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_may_keep(true, false, true, DWC3_APPLE_HOST,
							   false, true));
}

/* The core's mark is read only where the core may be touched and wasn't reset. */
static void dwc3_apple_sleep_reads_mark_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, dwc3_apple_sleep_reads_mark(0));
	KUNIT_EXPECT_TRUE(test, dwc3_apple_sleep_reads_mark(-EBUSY));
	KUNIT_EXPECT_TRUE(test, dwc3_apple_sleep_reads_mark(APPLE_ATCPHY_SLEEP_POWER_LOST));
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_reads_mark(APPLE_ATCPHY_SLEEP_RESET));
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_reads_mark(-ENODEV));
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_reads_mark(-EIO));
	KUNIT_EXPECT_FALSE(test, dwc3_apple_sleep_reads_mark(-EOPNOTSUPP));
}

/* A PHY that hibernated the core (T8140): the mark and the always-on side decide. */
static void dwc3_apple_wake_hibernated_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(0, true), DWC3_APPLE_WAKE_AS_IS);
	KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(0, false), DWC3_APPLE_WAKE_RESTORE);
	KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(-EBUSY, true), DWC3_APPLE_WAKE_AS_IS);
	KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(-EBUSY, false), DWC3_APPLE_WAKE_RESTORE);
	KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(APPLE_ATCPHY_SLEEP_POWER_LOST, true),
			DWC3_APPLE_WAKE_AS_IS);
	KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(APPLE_ATCPHY_SLEEP_POWER_LOST, false),
			DWC3_APPLE_WAKE_START_OVER);
}

/* A PHY that held the core in reset (T6040): the host starts over whatever the mark reads. */
static void dwc3_apple_wake_reset_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(APPLE_ATCPHY_SLEEP_RESET, false),
			DWC3_APPLE_WAKE_PHY_START_OVER);
	KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(APPLE_ATCPHY_SLEEP_RESET, true),
			DWC3_APPLE_WAKE_PHY_START_OVER);
}

/* A core that may be unpowered, or whose PHY didn't come back, is not resumed. */
static void dwc3_apple_wake_failed_test(struct kunit *test)
{
	static const int errors[] = { -ENODEV, -EIO, -ETIMEDOUT, -EOPNOTSUPP, -ENODATA, 3 };
	int i;

	for (i = 0; i < ARRAY_SIZE(errors); i++) {
		KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(errors[i], false),
				DWC3_APPLE_WAKE_FAILED);
		KUNIT_EXPECT_EQ(test, dwc3_apple_sleep_wake_plan(errors[i], true),
				DWC3_APPLE_WAKE_FAILED);
	}
}

static struct kunit_case dwc3_apple_state_test_cases[] = {
	KUNIT_CASE(dwc3_apple_host_left_to_xhci_test),
	KUNIT_CASE(dwc3_apple_device_selected_before_reset_test),
	KUNIT_CASE(dwc3_apple_non_role_test),
	KUNIT_CASE(dwc3_apple_role_during_sleep_test),
	KUNIT_CASE(dwc3_apple_role_while_kept_test),
	KUNIT_CASE(dwc3_apple_sleep_may_keep_test),
	KUNIT_CASE(dwc3_apple_sleep_reads_mark_test),
	KUNIT_CASE(dwc3_apple_wake_hibernated_test),
	KUNIT_CASE(dwc3_apple_wake_reset_test),
	KUNIT_CASE(dwc3_apple_wake_failed_test),
	{}
};

static struct kunit_suite dwc3_apple_state_test_suite = {
	.name = "dwc3_apple_state",
	.test_cases = dwc3_apple_state_test_cases,
};

kunit_test_suite(dwc3_apple_state_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple Silicon DWC3 glue's USB2 mode and sleep decisions");
MODULE_LICENSE("GPL");

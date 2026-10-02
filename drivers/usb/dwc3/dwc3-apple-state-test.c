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
		DWC3_APPLE_PROBE_PENDING, DWC3_APPLE_NO_CABLE, DWC3_APPLE_SUSPENDED,
	};
	enum phy_mode mode;
	int i;

	for (i = 0; i < ARRAY_SIZE(states); i++) {
		mode = PHY_MODE_USB_HOST;
		KUNIT_EXPECT_EQ(test, dwc3_apple_usb2_mode_before_reset(states[i], &mode), -EINVAL);
		KUNIT_EXPECT_EQ(test, mode, PHY_MODE_INVALID);
	}
}

static struct kunit_case dwc3_apple_state_test_cases[] = {
	KUNIT_CASE(dwc3_apple_host_left_to_xhci_test),
	KUNIT_CASE(dwc3_apple_device_selected_before_reset_test),
	KUNIT_CASE(dwc3_apple_non_role_test),
	{}
};

static struct kunit_suite dwc3_apple_state_test_suite = {
	.name = "dwc3_apple_state",
	.test_cases = dwc3_apple_state_test_cases,
};

kunit_test_suite(dwc3_apple_state_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple Silicon DWC3 glue USB2 mode choice");
MODULE_LICENSE("GPL");

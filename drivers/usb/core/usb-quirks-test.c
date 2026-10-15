// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for USB device and hub quirk matching
 */

#include <kunit/test.h>
#include <kunit/visibility.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/quirks.h>

/* hub.h includes usb.h, which has no include guard */
#include "hub.h"

struct usb_quirks_test {
	struct usb_device udev;
	struct usb_interface intf;
	struct usb_host_interface alt;
};

static struct usb_quirks_test *usb_quirks_test_hub(struct kunit *test, u16 vid, u16 pid)
{
	struct usb_quirks_test *t;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);

	t->udev.descriptor.idVendor = cpu_to_le16(vid);
	t->udev.descriptor.idProduct = cpu_to_le16(pid);
	t->udev.descriptor.bDeviceClass = USB_CLASS_HUB;
	t->alt.desc.bInterfaceClass = USB_CLASS_HUB;
	t->intf.altsetting = &t->alt;
	t->intf.cur_altsetting = &t->alt;
	t->intf.num_altsetting = 1;
	t->intf.dev.parent = &t->udev.dev;

	return t;
}

/*
 * The VIA Labs VL122 hub on the Apple J700 does not pass on remote wakeup while it
 * is suspended. It stays out of autosuspend and keeps its wakeup setting, and is
 * kept awake in suspend-to-idle instead.
 */
static void usb_quirks_vl122_test(struct kunit *test)
{
	struct usb_quirks_test *t = usb_quirks_test_hub(test, 0x2109, 0x2122);
	u32 quirks = usb_detect_device_list_quirks(&t->udev);

	KUNIT_EXPECT_FALSE(test, quirks & USB_QUIRK_IGNORE_REMOTE_WAKEUP);
	KUNIT_EXPECT_TRUE(test, quirks & USB_QUIRK_HUB_NO_SUSPENDED_WAKEUP);
	KUNIT_EXPECT_TRUE(test, usb_hub_id_quirks(&t->intf) & HUB_QUIRK_DISABLE_AUTOSUSPEND);
}

/* Another VIA Labs hub keeps its own device quirk and no hub quirk. */
static void usb_quirks_other_via_hub_test(struct kunit *test)
{
	struct usb_quirks_test *t = usb_quirks_test_hub(test, 0x2109, 0x2817);

	KUNIT_EXPECT_EQ(test, usb_detect_device_list_quirks(&t->udev), USB_QUIRK_NO_LPM);
	KUNIT_EXPECT_EQ(test, usb_hub_id_quirks(&t->intf), 0UL);
}

/* A hub that no table names gets no quirks. */
static void usb_quirks_generic_hub_test(struct kunit *test)
{
	struct usb_quirks_test *t = usb_quirks_test_hub(test, 0x1d6b, 0x0002);

	KUNIT_EXPECT_EQ(test, usb_detect_device_list_quirks(&t->udev), 0U);
	KUNIT_EXPECT_EQ(test, usb_hub_id_quirks(&t->intf), 0UL);
}

static struct kunit_case usb_quirks_test_cases[] = {
	KUNIT_CASE(usb_quirks_vl122_test),
	KUNIT_CASE(usb_quirks_other_via_hub_test),
	KUNIT_CASE(usb_quirks_generic_hub_test),
	{}
};

static struct kunit_suite usb_quirks_test_suite = {
	.name = "usb_quirks",
	.test_cases = usb_quirks_test_cases,
};
kunit_test_suite(usb_quirks_test_suite);

MODULE_DESCRIPTION("KUnit tests for USB device and hub quirk matching");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

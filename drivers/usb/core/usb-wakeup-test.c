// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the USB core's wakeup decisions in system sleep
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/usb/quirks.h>

/* hub.h includes usb.h, which has no include guard */
#include "hub.h"

#define USB_WAKEUP_TEST_PORTS	4U

/* The device quirks of the VIA Labs VL122 hub */
#define USB_WAKEUP_TEST_VL122	(USB_QUIRK_HUB_NO_SUSPENDED_WAKEUP | \
				 USB_QUIRK_HUB_NO_SUSPENDED_PORT_WAKEUP)

/* A suspended USB device, with the hub driver state of a hub */
struct usb_wakeup_test_dev {
	struct usb_device udev;
	struct usb_host_config config;
	struct usb_interface intf;
	struct usb_hub hub;
	struct usb_port ports[USB_WAKEUP_TEST_PORTS];
	struct usb_port *port_list[USB_WAKEUP_TEST_PORTS];
};

/* A hub or another device, attached to the first free port of @parent if any */
static struct usb_wakeup_test_dev *usb_wakeup_test_add(struct kunit *test,
						       struct usb_wakeup_test_dev *parent,
						       bool hub)
{
	struct usb_wakeup_test_dev *d = kunit_kzalloc(test, sizeof(*d), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, d);
	if (hub) {
		for (i = 0; i < USB_WAKEUP_TEST_PORTS; i++)
			d->port_list[i] = &d->ports[i];
		d->hub.hdev = &d->udev;
		d->hub.ports = d->port_list;
		d->config.interface[0] = &d->intf;
		usb_set_intfdata(&d->intf, &d->hub);
		d->udev.actconfig = &d->config;
		d->udev.maxchild = USB_WAKEUP_TEST_PORTS;
	}
	if (parent) {
		for (i = 0; i < USB_WAKEUP_TEST_PORTS && parent->ports[i].child; i++)
			;
		KUNIT_ASSERT_LT(test, i, USB_WAKEUP_TEST_PORTS);
		parent->ports[i].child = &d->udev;
		d->udev.parent = &parent->udev;
		d->udev.bus = parent->udev.bus;
	}

	return d;
}

/*
 * The defaults behind the front port of an Apple J700: a keyboard with remote
 * wakeup enabled, below a hub, below the VIA Labs VL122 hub on the root port,
 * every hub with wakeup disabled. The VL122 does not pass a remote wakeup on
 * while it is suspended, so the keyboard's does not reach the root hub, unless
 * the VL122 is kept awake.
 */
static void usb_wakeup_reaches_vl122_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *vl122 = usb_wakeup_test_add(test, root, true);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, vl122, true);
	struct usb_wakeup_test_dev *keyboard = usb_wakeup_test_add(test, hub, false);

	vl122->udev.quirks = USB_WAKEUP_TEST_VL122;
	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&hub->udev));
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&vl122->udev));
	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&root->udev));

	vl122->udev.awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));
}

/* Any other hub passes it on, whatever its own wakeup setting. */
static void usb_wakeup_reaches_hub_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, root, true);
	struct usb_wakeup_test_dev *keyboard = usb_wakeup_test_add(test, hub, false);

	usb_wakeup_test_add(test, hub, false);
	usb_wakeup_test_add(test, root, false);
	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&root->udev));

	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));
}

/*
 * A hub armed for wakeup signals connect, disconnect and over-current changes
 * on its ports, also if it does not pass wakeups on from below while suspended.
 */
static void usb_wakeup_reaches_armed_hub_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, root, true);

	hub->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));

	hub->udev.quirks = USB_QUIRK_HUB_NO_SUSPENDED_WAKEUP;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));
}

/*
 * A hub that does not pass wakeups on from below while it is suspended, armed
 * for wakeup, with wakeup disabled for the root hub and the device below it:
 * its own changes reach the root hub. A remote wakeup from below it does not.
 */
static void usb_wakeup_reaches_armed_quirky_hub_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, root, true);
	struct usb_wakeup_test_dev *keyboard = usb_wakeup_test_add(test, hub, false);

	hub->udev.quirks = USB_QUIRK_HUB_NO_SUSPENDED_WAKEUP;
	hub->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));

	hub->udev.do_remote_wakeup = 0;
	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&root->udev));
}

/* A root hub with nothing below, and a device that is no hub, reach nothing. */
static void usb_wakeup_reaches_nothing_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *keyboard;

	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&root->udev));
	keyboard = usb_wakeup_test_add(test, root, false);
	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&keyboard->udev));
}

/*
 * The VL122 does not signal the changes of its own ports while it is suspended
 * either: armed for wakeup, it reaches the root hub only while it is kept awake.
 */
static void usb_wakeup_reaches_armed_vl122_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *vl122 = usb_wakeup_test_add(test, root, true);

	vl122->udev.quirks = USB_WAKEUP_TEST_VL122;
	vl122->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&root->udev));

	vl122->udev.awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));
}

static void usb_wakeup_test_restore_target(void *state)
{
	pm_suspend_target_state = *(suspend_state_t *)state;
}

/*
 * A root hub on a host controller that keeps running in suspend-to-idle, with
 * wakeup enabled, as the system goes to suspend-to-idle.
 */
static struct usb_wakeup_test_dev *usb_wakeup_test_s2idle_root(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_hcd *hcd = kunit_kzalloc(test, sizeof(*hcd), GFP_KERNEL);
	struct device *dev = kunit_device_register(test, "usb-wakeup-test");
	suspend_state_t *target = kunit_kzalloc(test, sizeof(*target), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, hcd);
	KUNIT_ASSERT_NOT_NULL(test, target);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	KUNIT_ASSERT_EQ(test, devm_device_init_wakeup(dev), 0);
	hcd->self.controller = dev;
	hcd->runs_in_s2idle = 1;
	root->udev.bus = &hcd->self;

	*target = pm_suspend_target_state;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, usb_wakeup_test_restore_target,
							target), 0);
	pm_suspend_target_state = PM_SUSPEND_TO_IDLE;

	return root;
}

/*
 * The VL122 armed for wakeup, with nothing below it that can wake: it is kept
 * awake in suspend-to-idle for the changes of its own ports, and so is the root
 * hub above it, but not for other system sleep transitions, or on a controller
 * that halts or may not wake the system. A hub that signals those changes while
 * it is suspended is not kept awake for them.
 */
static void usb_wakeup_stay_awake_armed_vl122_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_s2idle_root(test);
	struct usb_wakeup_test_dev *vl122 = usb_wakeup_test_add(test, root, true);
	struct usb_hcd *hcd = bus_to_hcd(root->udev.bus);

	usb_wakeup_test_add(test, vl122, false);
	vl122->udev.quirks = USB_WAKEUP_TEST_VL122;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));

	vl122->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_FREEZE));

	pm_suspend_target_state = PM_SUSPEND_MEM;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));
	pm_suspend_target_state = PM_SUSPEND_TO_IDLE;

	hcd->runs_in_s2idle = 0;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));
	hcd->runs_in_s2idle = 1;

	device_set_wakeup_enable(hcd->self.controller, false);
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));
	device_set_wakeup_enable(hcd->self.controller, true);

	vl122->udev.awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, usb_stay_awake_for_wakeup(&root->udev, PMSG_SUSPEND));
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));

	vl122->udev.awake_in_sleep = 0;
	vl122->udev.quirks = USB_QUIRK_HUB_NO_SUSPENDED_WAKEUP;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));
}

/*
 * The defaults behind the front port of a J700: the VL122 hub, which does not
 * pass a remote wakeup on while it is suspended, has wakeup disabled, as hubs
 * have by default, and a keyboard below it has it enabled. The VL122 is kept
 * awake for the keyboard, and so is the root hub above it.
 */
static void usb_wakeup_stay_awake_vl122_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_s2idle_root(test);
	struct usb_wakeup_test_dev *vl122 = usb_wakeup_test_add(test, root, true);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, vl122, true);
	struct usb_wakeup_test_dev *keyboard = usb_wakeup_test_add(test, hub, false);

	vl122->udev.quirks = USB_WAKEUP_TEST_VL122;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));

	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&keyboard->udev, PMSG_SUSPEND));
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&hub->udev, PMSG_SUSPEND));
	KUNIT_EXPECT_TRUE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));

	vl122->udev.awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, usb_stay_awake_for_wakeup(&root->udev, PMSG_SUSPEND));
}

/*
 * Nothing is kept awake for other system sleep transitions, for a controller
 * that halts or may not wake the system, or with no device below that can wake.
 */
static void usb_wakeup_stay_awake_not_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_s2idle_root(test);
	struct usb_wakeup_test_dev *vl122 = usb_wakeup_test_add(test, root, true);
	struct usb_wakeup_test_dev *keyboard = usb_wakeup_test_add(test, vl122, false);
	struct usb_hcd *hcd = bus_to_hcd(root->udev.bus);

	vl122->udev.quirks = USB_WAKEUP_TEST_VL122;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));
	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_ASSERT_TRUE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));

	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_FREEZE));

	pm_suspend_target_state = PM_SUSPEND_MEM;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));
	pm_suspend_target_state = PM_SUSPEND_TO_IDLE;

	hcd->runs_in_s2idle = 0;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));
	hcd->runs_in_s2idle = 1;

	device_set_wakeup_enable(hcd->self.controller, false);
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&vl122->udev, PMSG_SUSPEND));
}

/*
 * A keyboard that wakes itself whenever it is suspended, behind two hubs that pass a
 * remote wakeup on: with its wakeup enabled it is kept awake in suspend-to-idle, so its
 * own input wakes the system, and so are the hubs and the root hub above it. Not with
 * its wakeup disabled, for other system sleep transitions, or on a controller that halts.
 */
static void usb_wakeup_stay_awake_self_waking_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_s2idle_root(test);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, root, true);
	struct usb_wakeup_test_dev *dock = usb_wakeup_test_add(test, hub, true);
	struct usb_wakeup_test_dev *keyboard = usb_wakeup_test_add(test, dock, false);
	struct usb_hcd *hcd = bus_to_hcd(root->udev.bus);

	keyboard->udev.quirks = USB_QUIRK_WAKES_ITSELF_WHEN_SUSPENDED;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&keyboard->udev, PMSG_SUSPEND));

	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_stay_awake_for_wakeup(&keyboard->udev, PMSG_SUSPEND));
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&keyboard->udev, PMSG_FREEZE));

	pm_suspend_target_state = PM_SUSPEND_MEM;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&keyboard->udev, PMSG_SUSPEND));
	pm_suspend_target_state = PM_SUSPEND_TO_IDLE;

	hcd->runs_in_s2idle = 0;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&keyboard->udev, PMSG_SUSPEND));
	hcd->runs_in_s2idle = 1;

	keyboard->udev.awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, usb_stay_awake_for_wakeup(&dock->udev, PMSG_SUSPEND));
	dock->udev.awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, usb_stay_awake_for_wakeup(&hub->udev, PMSG_SUSPEND));
	hub->udev.awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, usb_stay_awake_for_wakeup(&root->udev, PMSG_SUSPEND));

	/* A keyboard without the quirk is suspended and wakes through the hubs. */
	keyboard->udev.quirks = 0;
	KUNIT_EXPECT_FALSE(test, usb_stay_awake_for_wakeup(&keyboard->udev, PMSG_SUSPEND));
}

static struct kunit_case usb_wakeup_test_cases[] = {
	KUNIT_CASE(usb_wakeup_reaches_vl122_test),
	KUNIT_CASE(usb_wakeup_reaches_hub_test),
	KUNIT_CASE(usb_wakeup_reaches_armed_hub_test),
	KUNIT_CASE(usb_wakeup_reaches_armed_quirky_hub_test),
	KUNIT_CASE(usb_wakeup_reaches_nothing_test),
	KUNIT_CASE(usb_wakeup_reaches_armed_vl122_test),
	KUNIT_CASE(usb_wakeup_stay_awake_armed_vl122_test),
	KUNIT_CASE(usb_wakeup_stay_awake_vl122_test),
	KUNIT_CASE(usb_wakeup_stay_awake_not_test),
	KUNIT_CASE(usb_wakeup_stay_awake_self_waking_test),
	{}
};

static struct kunit_suite usb_wakeup_test_suite = {
	.name = "usb_wakeup",
	.test_cases = usb_wakeup_test_cases,
};

kunit_test_suite(usb_wakeup_test_suite);

MODULE_DESCRIPTION("KUnit tests for the USB core's wakeup decisions in system sleep");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

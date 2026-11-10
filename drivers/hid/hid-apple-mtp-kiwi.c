// SPDX-License-Identifier: GPL-2.0
/*
 * Apple MTP touch-screen (kiwi) interface.
 *
 * The kiwi interface of the dockchannel MTP carries its touches as a raw
 * report pipe. No other HID driver may claim it: the Apple wildcard entries of
 * hid-apple and hid-magicmouse make hid-generic stand off from every
 * BUS_HOST Apple device, and both reject the interface in probe. This driver
 * claims it by name and exposes the pipe on hidraw. The interface is started
 * (firmware, reset) when hidraw is first opened.
 */

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/string.h>

#include "hid-ids.h"

static bool kiwi_match(struct hid_device *hdev, bool ignore_special_drivers)
{
	return hdev->bus == BUS_HOST && !strcmp(hdev->name, "Apple MTP kiwi");
}

static int kiwi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	return hid_hw_start(hdev, HID_CONNECT_HIDRAW);
}

static const struct hid_device_id kiwi_devices[] = {
	{ HID_DEVICE(BUS_HOST, HID_GROUP_ANY, HOST_VENDOR_ID_APPLE, HID_ANY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, kiwi_devices);

static struct hid_driver kiwi_driver = {
	.name = "hid-apple-mtp-kiwi",
	.id_table = kiwi_devices,
	.match = kiwi_match,
	.probe = kiwi_probe,
};
module_hid_driver(kiwi_driver);

MODULE_DESCRIPTION("Apple MTP touch-screen (kiwi) interface");
MODULE_LICENSE("GPL");

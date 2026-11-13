// SPDX-License-Identifier: GPL-2.0-only
/*
 * Transport for the trusted-accessory channel of the Magic Keyboards with
 * Touch ID.
 *
 * Besides its keyboard collections, the keyboard has a vendor collection
 * (usage page 0xff00, usage 0x4b) that carries pairing, session and sensor
 * traffic between the keyboard's Touch ID sensor and the Secure Enclave. On
 * USB it is a separate interface whose input reports arrive on a bulk IN
 * endpoint (see usbhid). Over Bluetooth the collection shares one HID device
 * with the keyboard; that path is not handled here yet.
 *
 * The descriptor declares three input reports, told apart by their declared
 * lengths, but the keyboard also answers commands and streams sensor frames
 * in input reports it does not declare: IDs 0x40-0x4f and 0xb0-0xbf, from 8
 * bytes to almost 11 KiB. HID core drops reports with an undeclared ID before
 * any driver sees them, and usbhid sizes its input buffer from the
 * descriptor, so report_fixup() declares those IDs with room for 16 KiB. The
 * client gets every report with its ID and actual length.
 *
 * The channel is the enclave's, not user space's: this driver claims the
 * collection exclusively, starts it without hidraw, hiddev or input devices,
 * and passes each input report to one in-kernel client.
 *
 * Copyright 2026 Ryan Murray
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/hid-apple-ta.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "hid-ids.h"

#define APPLE_TA_PAGE		0xff00
#define APPLE_TA_USAGE		0x4b

/*
 * The descriptor of the USB Touch ID interface (49 bytes): the vendor
 * collection with input reports 0x20 (107 bytes), 0x21 (649) and 0x22 (62).
 */
static const u8 apple_ta_usb_rdesc[] = {
	0x06, 0x00, 0xff,	/* Usage Page (Vendor 0xff00) */
	0x09, 0x4b,		/* Usage (0x4b) */
	0xa1, 0x01,		/* Collection (Application) */
	0x06, 0x00, 0xff,	/*  Usage Page (Vendor 0xff00) */
	0x09, 0x4b,		/*  Usage (0x4b) */
	0x15, 0x00,		/*  Logical Minimum (0) */
	0x26, 0xff, 0x00,	/*  Logical Maximum (255) */
	0x85, 0x20,		/*  Report ID (0x20) */
	0x96, 0x6b, 0x00,	/*  Report Count (107) */
	0x75, 0x08,		/*  Report Size (8) */
	0x81, 0x02,		/*  Input (Data,Var,Abs) */
	0x85, 0x21,		/*  Report ID (0x21) */
	0x96, 0x89, 0x02,	/*  Report Count (649) */
	0x75, 0x08,		/*  Report Size (8) */
	0x81, 0x02,		/*  Input (Data,Var,Abs) */
	0x85, 0x22,		/*  Report ID (0x22) */
	0x96, 0x3e, 0x00,	/*  Report Count (62) */
	0x75, 0x08,		/*  Report Size (8) */
	0x81, 0x02,		/*  Input (Data,Var,Abs) */
	0xc0,			/* End Collection */
};

/*
 * One undeclared input report: 16383 data bytes (4 x 4095 + 3), so with its
 * ID it fills HID_MAX_BUFFER_SIZE. No usage, so HID core treats the fields as
 * padding: the report exists and is sized, nothing is parsed out of it.
 */
#define APPLE_TA_IN(id)						\
	0x85, (id),		/* Report ID */			\
	0x96, 0xff, 0x0f, 0x81, 0x02,	/* 4095 x Input */	\
	0x96, 0xff, 0x0f, 0x81, 0x02,				\
	0x96, 0xff, 0x0f, 0x81, 0x02,				\
	0x96, 0xff, 0x0f, 0x81, 0x02,				\
	0x95, 0x03, 0x81, 0x02		/* 3 x Input */
#define APPLE_TA_IN4(id)					\
	APPLE_TA_IN(id), APPLE_TA_IN((id) + 1),			\
	APPLE_TA_IN((id) + 2), APPLE_TA_IN((id) + 3)
#define APPLE_TA_IN16(id)					\
	APPLE_TA_IN4(id), APPLE_TA_IN4((id) + 4),		\
	APPLE_TA_IN4((id) + 8), APPLE_TA_IN4((id) + 12)

/* The USB descriptor with the undeclared reports added to its collection. */
static const u8 apple_ta_usb_rdesc_fixed[] = {
	0x06, 0x00, 0xff, 0x09, 0x4b, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x4b,
	0x15, 0x00, 0x26, 0xff, 0x00,
	0x85, 0x20, 0x96, 0x6b, 0x00, 0x75, 0x08, 0x81, 0x02,
	0x85, 0x21, 0x96, 0x89, 0x02, 0x75, 0x08, 0x81, 0x02,
	0x85, 0x22, 0x96, 0x3e, 0x00, 0x75, 0x08, 0x81, 0x02,
	/* Report Size (8) is still in effect. */
	APPLE_TA_IN16(0x40),
	APPLE_TA_IN16(0xb0),
	0xc0,
};

/* The IDs report_fixup() adds. */
static bool apple_ta_is_undeclared(u8 id)
{
	return (id & 0xf0) == 0x40 || (id & 0xf0) == 0xb0;
}

struct apple_ta_device {
	struct hid_device *hdev;
	struct list_head node;
	/* Report ID and data length of each role. */
	u8 id[APPLE_TA_NR_ROLES];
	size_t len[APPLE_TA_NR_ROLES];
	/* Set while the client has been told about this device. */
	bool attached;
};

static DEFINE_MUTEX(apple_ta_lock);	/* devices, client registration */
static LIST_HEAD(apple_ta_devices);
static DEFINE_SPINLOCK(apple_ta_client_lock);	/* client, for raw_event */
static const struct apple_ta_client_ops *apple_ta_ops;
static void *apple_ta_ctx;

static bool apple_ta_is_collection(struct hid_device *hdev)
{
	return hdev->maxcollection > 0 &&
	       hdev->collection[0].type == HID_COLLECTION_APPLICATION &&
	       hdev->collection[0].usage == (APPLE_TA_PAGE << 16 | APPLE_TA_USAGE);
}

/*
 * Assigns the roles from the descriptor: exactly three numbered input
 * reports, the largest carries data, the middle one control replies and the
 * smallest events. Anything else is not a collection this driver knows.
 */
static int apple_ta_assign_roles(struct apple_ta_device *ta)
{
	struct hid_report_enum *in = &ta->hdev->report_enum[HID_INPUT_REPORT];
	struct hid_report *r, *rep[3];
	unsigned int n = 0, i, j;

	if (!in->numbered)
		return -ENODEV;

	list_for_each_entry(r, &in->report_list, list) {
		if (apple_ta_is_undeclared(r->id))
			continue;
		if (n == ARRAY_SIZE(rep))
			return -ENODEV;
		rep[n++] = r;
	}
	if (n != ARRAY_SIZE(rep))
		return -ENODEV;

	/* Sort by length, largest first. */
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (hid_report_len(rep[j]) > hid_report_len(rep[i]))
				swap(rep[i], rep[j]);
	if (hid_report_len(rep[0]) == hid_report_len(rep[1]) ||
	    hid_report_len(rep[1]) == hid_report_len(rep[2]))
		return -ENODEV;

	ta->id[APPLE_TA_DATA] = rep[0]->id;
	ta->id[APPLE_TA_CONTROL] = rep[1]->id;
	ta->id[APPLE_TA_EVENT] = rep[2]->id;
	for (i = 0; i < n; i++) {
		/* hid_report_len() counts the report ID byte. */
		size_t len = hid_report_len(rep[i]) - 1;

		if (!len)
			return -ENODEV;
		ta->len[i == 0 ? APPLE_TA_DATA :
			i == 1 ? APPLE_TA_CONTROL : APPLE_TA_EVENT] = len;
	}
	return 0;
}

static void apple_ta_attach(struct apple_ta_device *ta)
{
	lockdep_assert_held(&apple_ta_lock);
	if (apple_ta_ops && !ta->attached) {
		ta->attached = true;
		apple_ta_ops->attach(apple_ta_ctx, ta);
	}
}

static void apple_ta_detach(struct apple_ta_device *ta)
{
	const struct apple_ta_client_ops *ops = apple_ta_ops;
	unsigned long flags;

	lockdep_assert_held(&apple_ta_lock);
	if (!ta->attached)
		return;
	/* Stop reports for this device before the client forgets it. */
	spin_lock_irqsave(&apple_ta_client_lock, flags);
	ta->attached = false;
	spin_unlock_irqrestore(&apple_ta_client_lock, flags);
	ops->detach(apple_ta_ctx, ta);
}

int apple_ta_register_client(const struct apple_ta_client_ops *ops, void *ctx)
{
	struct apple_ta_device *ta;
	unsigned long flags;

	if (!ops || !ops->attach || !ops->detach || !ops->report)
		return -EINVAL;

	guard(mutex)(&apple_ta_lock);
	if (apple_ta_ops)
		return -EBUSY;
	spin_lock_irqsave(&apple_ta_client_lock, flags);
	apple_ta_ops = ops;
	apple_ta_ctx = ctx;
	spin_unlock_irqrestore(&apple_ta_client_lock, flags);
	list_for_each_entry(ta, &apple_ta_devices, node)
		apple_ta_attach(ta);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_ta_register_client);

void apple_ta_unregister_client(const struct apple_ta_client_ops *ops)
{
	struct apple_ta_device *ta;
	unsigned long flags;

	guard(mutex)(&apple_ta_lock);
	if (apple_ta_ops != ops)
		return;
	list_for_each_entry(ta, &apple_ta_devices, node)
		apple_ta_detach(ta);
	spin_lock_irqsave(&apple_ta_client_lock, flags);
	apple_ta_ops = NULL;
	apple_ta_ctx = NULL;
	spin_unlock_irqrestore(&apple_ta_client_lock, flags);
}
EXPORT_SYMBOL_GPL(apple_ta_unregister_client);

enum apple_ta_bus apple_ta_bus(const struct apple_ta_device *ta)
{
	return APPLE_TA_USB;
}
EXPORT_SYMBOL_GPL(apple_ta_bus);

size_t apple_ta_report_len(const struct apple_ta_device *ta,
			   enum apple_ta_role role)
{
	return role < APPLE_TA_NR_ROLES ? ta->len[role] : 0;
}
EXPORT_SYMBOL_GPL(apple_ta_report_len);

int apple_ta_send(struct apple_ta_device *ta, u8 report_type,
		  const u8 *buf, size_t len)
{
	u8 *dma;
	int ret;

	if (!len || (report_type != HID_FEATURE_REPORT &&
		     report_type != HID_OUTPUT_REPORT))
		return -EINVAL;
	dma = kmemdup(buf, len, GFP_KERNEL);
	if (!dma)
		return -ENOMEM;
	ret = hid_hw_raw_request(ta->hdev, dma[0], dma, len, report_type,
				 HID_REQ_SET_REPORT);
	kfree(dma);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_ta_send);

int apple_ta_get(struct apple_ta_device *ta, u8 *buf, size_t len)
{
	u8 *dma;
	int ret;

	if (!len)
		return -EINVAL;
	dma = kzalloc(len, GFP_KERNEL);
	if (!dma)
		return -ENOMEM;
	ret = hid_hw_raw_request(ta->hdev, buf[0], dma, len, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret > 0)
		memcpy(buf, dma, min_t(size_t, ret, len));
	kfree(dma);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_ta_get);

static int apple_ta_raw_event(struct hid_device *hdev, struct hid_report *report,
			      u8 *data, int size)
{
	struct apple_ta_device *ta = hid_get_drvdata(hdev);
	unsigned long flags;

	if (size < 1)
		return -EINVAL;

	spin_lock_irqsave(&apple_ta_client_lock, flags);
	if (apple_ta_ops && ta->attached)
		apple_ta_ops->report(apple_ta_ctx, ta, data[0], data + 1, size - 1);
	spin_unlock_irqrestore(&apple_ta_client_lock, flags);

	/* Nothing else is connected, so the report goes nowhere else. */
	return 0;
}

static const __u8 *apple_ta_report_fixup(struct hid_device *hdev, __u8 *rdesc,
					 unsigned int *rsize)
{
	if (hdev->bus == BUS_USB && *rsize == sizeof(apple_ta_usb_rdesc) &&
	    !memcmp(rdesc, apple_ta_usb_rdesc, *rsize)) {
		hid_dbg(hdev, "declaring the undeclared Touch ID input reports\n");
		*rsize = sizeof(apple_ta_usb_rdesc_fixed);
		return apple_ta_usb_rdesc_fixed;
	}
	return rdesc;
}

static int apple_ta_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct apple_ta_device *ta;
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;
	/* The keyboard collections belong to hid-apple. */
	if (!apple_ta_is_collection(hdev))
		return -ENODEV;

	ta = devm_kzalloc(&hdev->dev, sizeof(*ta), GFP_KERNEL);
	if (!ta)
		return -ENOMEM;
	ta->hdev = hdev;
	ret = apple_ta_assign_roles(ta);
	if (ret) {
		hid_info(hdev, "unrecognised Touch ID collection layout\n");
		return ret;
	}
	hid_set_drvdata(hdev, ta);

	/* No hidraw, hiddev or input device: the channel is the enclave's. */
	ret = hid_hw_start(hdev, HID_CONNECT_DRIVER);
	if (ret)
		return ret;
	ret = hid_hw_open(hdev);
	if (ret)
		goto stop;

	hid_info(hdev, "Touch ID channel, reports %#x/%#x/%#x (%zu/%zu/%zu bytes)\n",
		 ta->id[APPLE_TA_CONTROL], ta->id[APPLE_TA_DATA],
		 ta->id[APPLE_TA_EVENT], ta->len[APPLE_TA_CONTROL],
		 ta->len[APPLE_TA_DATA], ta->len[APPLE_TA_EVENT]);

	mutex_lock(&apple_ta_lock);
	list_add_tail(&ta->node, &apple_ta_devices);
	apple_ta_attach(ta);
	mutex_unlock(&apple_ta_lock);
	return 0;

stop:
	hid_hw_stop(hdev);
	return ret;
}

static void apple_ta_remove(struct hid_device *hdev)
{
	struct apple_ta_device *ta = hid_get_drvdata(hdev);

	mutex_lock(&apple_ta_lock);
	apple_ta_detach(ta);
	list_del(&ta->node);
	mutex_unlock(&apple_ta_lock);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id apple_ta_devices_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_MAGIC_KEYBOARD_FINGERPRINT_2021) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_MAGIC_KEYBOARD_NUMPAD_2021) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_MAGIC_KEYBOARD_FINGERPRINT_2024) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_MAGIC_KEYBOARD_NUMPAD_2024) },
	{ }
};
MODULE_DEVICE_TABLE(hid, apple_ta_devices_table);

static struct hid_driver apple_ta_driver = {
	.name = "hid-apple-ta",
	.id_table = apple_ta_devices_table,
	.probe = apple_ta_probe,
	.remove = apple_ta_remove,
	.report_fixup = apple_ta_report_fixup,
	.raw_event = apple_ta_raw_event,
};
module_hid_driver(apple_ta_driver);

MODULE_DESCRIPTION("Trusted-accessory channel of the Magic Keyboards with Touch ID");
MODULE_LICENSE("GPL");

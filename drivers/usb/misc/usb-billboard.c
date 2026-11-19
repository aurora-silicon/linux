// SPDX-License-Identifier: GPL-2.0
/*
 * USB Billboard Device Class driver
 *
 * A USB-C device that offers Alternate Modes (DisplayPort, Thunderbolt, ...)
 * exposes a USB 2.0 Billboard device. Its BOS descriptor lists the modes the
 * device container supports and, for each one, whether the host managed to
 * enter it. This driver binds to the Billboard interface, exposes that
 * information in sysfs (readable without root, unlike debugfs) and logs a
 * warning for every mode the device reports as not entered, so a dock or
 * adapter whose display path failed is no longer silent.
 *
 * Reference: USB Billboard Device Class Specification, Revision 1.22.
 *
 * Based on the debugfs driver posted by Niklas Neronin in 2024, reworked
 * around sysfs and the review comments on that posting.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_tbt.h>

/* Revision 1.22 allows at most 0x34 Alternate or USB4 modes. */
#define BB_MAX_MODES		52

/* Billboard Capability descriptor, VCONNPower. */
#define BB_VCONN_NOT_REQUIRED	BIT(15)
#define BB_VCONN_POWER_MASK	GENMASK(2, 0)

/* Billboard Capability descriptor, bmConfigured: two bits per mode. */
enum bb_mode_state {
	BB_STATE_ERROR		= 0,	/* unspecified error */
	BB_STATE_NOT_ATTEMPTED	= 1,	/* not attempted, or exited */
	BB_STATE_FAILED		= 2,	/* attempted, not entered */
	BB_STATE_CONFIGURED	= 3,	/* entered */
};

static const char * const bb_state_names[] = {
	[BB_STATE_ERROR]		= "error",
	[BB_STATE_NOT_ATTEMPTED]	= "not-attempted",
	[BB_STATE_FAILED]		= "failed",
	[BB_STATE_CONFIGURED]		= "configured",
};

static const char * const bb_state_text[] = {
	[BB_STATE_ERROR]		= "unspecified error",
	[BB_STATE_NOT_ATTEMPTED]	= "not attempted or exited",
	[BB_STATE_FAILED]		= "attempted but not entered",
	[BB_STATE_CONFIGURED]		= "configured",
};

struct bb_mode;

struct bb_mode_attr {
	struct device_attribute attr;
	struct bb_mode *mode;
};

enum {
	BB_ATTR_SVID,
	BB_ATTR_MODE,
	BB_ATTR_VDO,
	BB_ATTR_STATE,
	BB_ATTR_DESC,
	BB_NR_MODE_ATTRS
};

struct bb_mode {
	u16 svid;
	u8 mode;
	bool has_vdo;
	u32 vdo;
	enum bb_mode_state state;
	char *desc;
	char name[16];
	struct bb_mode_attr attrs[BB_NR_MODE_ATTRS];
	struct attribute *attr_ptrs[BB_NR_MODE_ATTRS + 1];
	struct attribute_group group;
};

struct usb_billboard {
	struct usb_interface *intf;
	const struct usb_billboard_cap_descriptor *cap;
	char *url;
	unsigned int nr_modes;
	struct bb_mode modes[] __counted_by(nr_modes);
};

static const char *bb_svid_name(u16 svid)
{
	switch (svid) {
	case USB_TYPEC_DP_SID:
		return "DisplayPort";
	case USB_TYPEC_TBT_SID:
		return "Thunderbolt";
	default:
		return NULL;
	}
}

static enum bb_mode_state bb_state(const struct usb_billboard_cap_descriptor *cap,
				   unsigned int i)
{
	return (cap->bmConfigured[i / 4] >> ((i % 4) * 2)) & 0x3;
}

/* Device-wide attributes */

static ssize_t additional_info_url_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct usb_billboard *bb = usb_get_intfdata(to_usb_interface(dev));

	return sysfs_emit(buf, "%s\n", bb->url ?: "");
}
static DEVICE_ATTR_RO(additional_info_url);

static ssize_t preferred_mode_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct usb_billboard *bb = usb_get_intfdata(to_usb_interface(dev));

	return sysfs_emit(buf, "%u\n", bb->cap->bPreferredAlternateMode);
}
static DEVICE_ATTR_RO(preferred_mode);

static ssize_t vconn_power_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	static const char * const watts[] = {
		"1W", "1.5W", "2W", "3W", "4W", "5W", "6W",
	};
	struct usb_billboard *bb = usb_get_intfdata(to_usb_interface(dev));
	u16 vconn = le16_to_cpu(bb->cap->VCONNPower);
	unsigned int idx = vconn & BB_VCONN_POWER_MASK;

	if (vconn & BB_VCONN_NOT_REQUIRED)
		return sysfs_emit(buf, "not-required\n");
	if (idx >= ARRAY_SIZE(watts))
		return sysfs_emit(buf, "reserved\n");
	return sysfs_emit(buf, "%s\n", watts[idx]);
}
static DEVICE_ATTR_RO(vconn_power);

static ssize_t version_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct usb_billboard *bb = usb_get_intfdata(to_usb_interface(dev));
	u16 v = le16_to_cpu(bb->cap->bcdVersion);

	return sysfs_emit(buf, "%x.%02x\n", v >> 8, v & 0xff);
}
static DEVICE_ATTR_RO(version);

static ssize_t additional_failure_info_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct usb_billboard *bb = usb_get_intfdata(to_usb_interface(dev));

	return sysfs_emit(buf, "0x%02x\n", bb->cap->bAdditionalFailureInfo);
}
static DEVICE_ATTR_RO(additional_failure_info);

static ssize_t num_modes_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct usb_billboard *bb = usb_get_intfdata(to_usb_interface(dev));

	return sysfs_emit(buf, "%u\n", bb->nr_modes);
}
static DEVICE_ATTR_RO(num_modes);

static struct attribute *bb_attrs[] = {
	&dev_attr_additional_info_url.attr,
	&dev_attr_preferred_mode.attr,
	&dev_attr_vconn_power.attr,
	&dev_attr_version.attr,
	&dev_attr_additional_failure_info.attr,
	&dev_attr_num_modes.attr,
	NULL
};

static const struct attribute_group bb_group = {
	.name = "billboard",
	.attrs = bb_attrs,
};

/* Per-mode attributes, in billboard/altmodeN */

static ssize_t bb_mode_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct bb_mode_attr *ma = container_of(attr, struct bb_mode_attr, attr);
	struct bb_mode *m = ma->mode;

	switch (ma - m->attrs) {
	case BB_ATTR_SVID:
		return sysfs_emit(buf, "%04x\n", m->svid);
	case BB_ATTR_MODE:
		return sysfs_emit(buf, "%u\n", m->mode);
	case BB_ATTR_VDO:
		return sysfs_emit(buf, "0x%08x\n", m->vdo);
	case BB_ATTR_STATE:
		return sysfs_emit(buf, "%s\n", bb_state_names[m->state]);
	case BB_ATTR_DESC:
		return sysfs_emit(buf, "%s\n", m->desc ?: "");
	}
	return -EINVAL;
}

static const char * const bb_mode_attr_names[BB_NR_MODE_ATTRS] = {
	[BB_ATTR_SVID]	= "svid",
	[BB_ATTR_MODE]	= "mode",
	[BB_ATTR_VDO]	= "vdo",
	[BB_ATTR_STATE]	= "state",
	[BB_ATTR_DESC]	= "description",
};

static int bb_add_mode_group(struct usb_billboard *bb, unsigned int i)
{
	struct bb_mode *m = &bb->modes[i];
	unsigned int a, n = 0;

	snprintf(m->name, sizeof(m->name), "altmode%u", i);
	for (a = 0; a < BB_NR_MODE_ATTRS; a++) {
		struct bb_mode_attr *ma = &m->attrs[a];

		if (a == BB_ATTR_VDO && !m->has_vdo)
			continue;
		sysfs_attr_init(&ma->attr.attr);
		ma->attr.attr.name = bb_mode_attr_names[a];
		ma->attr.attr.mode = 0444;
		ma->attr.show = bb_mode_show;
		ma->mode = m;
		m->attr_ptrs[n++] = &ma->attr.attr;
	}
	m->attr_ptrs[n] = NULL;
	m->group.name = m->name;
	m->group.attrs = m->attr_ptrs;

	return devm_device_add_group(&bb->intf->dev, &m->group);
}

/* Descriptor parsing */

/*
 * Returns the Billboard capability (type USB_BILLBOARD_CAP_TYPE) or the
 * Billboard AUM capability for mode @index (type USB_BILLBOARD_AUM_CAP_TYPE).
 */
static const void *bb_find_cap(struct usb_device *udev, u8 type, u8 index)
{
	const struct usb_bos_descriptor *bos;
	const u8 *p, *end;
	unsigned int n;

	if (!udev->bos)
		return NULL;

	bos = udev->bos->desc;
	p = (const u8 *)bos + bos->bLength;
	end = (const u8 *)bos + le16_to_cpu(bos->wTotalLength);

	for (n = 0; n < bos->bNumDeviceCaps && p + 3 <= end; n++) {
		const struct usb_dev_cap_header *hdr = (const void *)p;

		if (hdr->bLength < 3 || p + hdr->bLength > end)
			break;
		if (hdr->bDescriptorType == USB_DT_DEVICE_CAPABILITY &&
		    hdr->bDevCapabilityType == type) {
			if (type == USB_BILLBOARD_CAP_TYPE)
				return p;
			if (hdr->bLength >= USB_DT_BILLBOARD_AUM_CAP_SIZE &&
			    ((const struct usb_billboard_aum_cap_descriptor *)p)->bIndex == index)
				return p;
		}
		p += hdr->bLength;
	}
	return NULL;
}

static void bb_kfree(void *p)
{
	kfree(p);
}

/*
 * Strings are released by devres after the sysfs groups that print them,
 * because devres runs in reverse order and the strings are registered first.
 */
static char *bb_string(struct usb_interface *intf, u8 index)
{
	char *s;

	if (!index)
		return NULL;
	s = usb_cache_string(interface_to_usbdev(intf), index);
	if (s && devm_add_action_or_reset(&intf->dev, bb_kfree, s))
		return NULL;
	return s;
}

static void bb_log_mode(struct usb_interface *intf, unsigned int i,
			const struct bb_mode *m)
{
	const char *name = bb_svid_name(m->svid);
	bool bad = m->state == BB_STATE_FAILED || m->state == BB_STATE_ERROR;

	dev_printk(bad ? KERN_WARNING : KERN_INFO, &intf->dev,
		   "alternate mode %u: SVID %04x%s%s mode %u%s%s: %s\n",
		   i, m->svid, name ? " " : "", name ?: "", m->mode,
		   m->desc ? ", " : "", m->desc ?: "",
		   bb_state_text[m->state]);
}

static int billboard_probe(struct usb_interface *intf,
			   const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	const struct usb_billboard_cap_descriptor *cap;
	struct usb_billboard *bb;
	unsigned int i, nr;
	bool failed = false;
	int ret;

	cap = bb_find_cap(udev, USB_BILLBOARD_CAP_TYPE, 0);
	if (!cap) {
		dev_warn(&intf->dev, "no Billboard capability descriptor\n");
		return -ENODEV;
	}

	nr = cap->bLength >= USB_DT_BILLBOARD_CAP_SIZE(0) ?
	     cap->bNumberOfAlternateModes : 0;
	if (!nr || nr > BB_MAX_MODES || cap->bLength < USB_DT_BILLBOARD_CAP_SIZE(nr)) {
		dev_warn(&intf->dev,
			 "malformed Billboard capability (length %u, %u modes)\n",
			 cap->bLength, nr);
		return -EINVAL;
	}

	bb = devm_kzalloc(&intf->dev, struct_size(bb, modes, nr), GFP_KERNEL);
	if (!bb)
		return -ENOMEM;
	bb->nr_modes = nr;
	bb->intf = intf;
	/* Keep a copy: a device reset replaces udev->bos. */
	cap = devm_kmemdup(&intf->dev, cap, cap->bLength, GFP_KERNEL);
	if (!cap)
		return -ENOMEM;
	bb->cap = cap;
	bb->url = bb_string(intf, cap->iAddtionalInfoURL);

	for (i = 0; i < nr; i++) {
		const struct usb_billboard_aum_cap_descriptor *aum;
		struct bb_mode *m = &bb->modes[i];

		m->svid = le16_to_cpu(cap->aum[i].wSVID);
		m->mode = cap->aum[i].bAlternateMode;
		m->state = bb_state(cap, i);
		m->desc = bb_string(intf, cap->aum[i].iAlternateModeString);
		aum = bb_find_cap(udev, USB_BILLBOARD_AUM_CAP_TYPE, i);
		if (aum) {
			m->has_vdo = true;
			m->vdo = le32_to_cpu(aum->dwAlternateModeVdo);
		}
		if (m->state == BB_STATE_FAILED || m->state == BB_STATE_ERROR)
			failed = true;
	}

	usb_set_intfdata(intf, bb);

	ret = devm_device_add_group(&intf->dev, &bb_group);
	if (ret)
		return ret;
	for (i = 0; i < nr; i++) {
		ret = bb_add_mode_group(bb, i);
		if (ret)
			return ret;
	}

	for (i = 0; i < nr; i++)
		bb_log_mode(intf, i, &bb->modes[i]);
	if (failed && cap->bAdditionalFailureInfo)
		dev_warn(&intf->dev, "additional failure info 0x%02x\n",
			 cap->bAdditionalFailureInfo);

	return 0;
}

static void billboard_disconnect(struct usb_interface *intf)
{
	/* Everything is device-managed. */
}

static const struct usb_device_id billboard_ids[] = {
	{ USB_INTERFACE_INFO(USB_CLASS_BILLBOARD, 0, 0) },
	{ }
};
MODULE_DEVICE_TABLE(usb, billboard_ids);

static struct usb_driver billboard_driver = {
	.name		= "usb-billboard",
	.probe		= billboard_probe,
	.disconnect	= billboard_disconnect,
	.id_table	= billboard_ids,
};
module_usb_driver(billboard_driver);

MODULE_DESCRIPTION("USB Billboard Device Class driver");
MODULE_LICENSE("GPL");

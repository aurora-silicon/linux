// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/cleanup.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/soc/apple/actuator.h>

#include "hid-ids.h"

int apple_taptic_send(struct hid_device *haptic_hdev, u16 effect_type, u8 strength, u8 softness)
{
	u8 event_click[] = {0x53, 0x01, strength, softness, 0x03, 0x02, 0x22, 0x48, 0x49,
			    0x01, 0x04, 0x60, 0x17, 0x2D, 0x03, 0x02, 0x18, 0x32, 0x1E};
	u8 event_deep_click[] = {0x53, 0x01, strength, softness, 0x01, 0x07, 0x18, 0x18, 0x4F,
				 0x02, 0x02, 0x60, 0x0C, 0x25};
	u8 event_release[] = {0x53, 0x01, strength, softness};
	u8 *msg;
	int msg_length;
	int ret;

	switch (effect_type) {
	case HID_HP_WAVEFORMPRESS & HID_USAGE:
		msg = event_click;
		msg_length = sizeof(event_click);
		break;
	case APPLE_HP_WAVEFORMDEEPCLICK & HID_USAGE:
		msg = event_deep_click;
		msg_length = sizeof(event_deep_click);
		break;
	case HID_HP_WAVEFORMRELEASE & HID_USAGE:
		msg = event_release;
		msg_length = sizeof(event_release);
		break;
	default:
		return -EINVAL;
	}

	ret = hid_hw_raw_request(haptic_hdev, msg[0], msg, msg_length,
				 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);

	if (ret < 0)
		return ret;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_taptic_send);

static bool apple_taptic_has_report(struct hid_device *hdev, unsigned int type,
				    unsigned int id)
{
	return hdev->report_enum[type].report_id_hash[id] != NULL;
}

int apple_taptic_switch_modes(struct hid_device *haptic_hdev, bool enable_host_controlled)
{
	u8 msg[] = { 0x21, enable_host_controlled };
	int ret;

	/*
	 * Report 0x21 is the MTP host actuator mode switch. The J313
	 * SPI actuator descriptor has input 0x3f and output 0x53 only.
	 * Sending 0x21 makes the controller return status 9c 17 e5 f8
	 * instead of the write-ok status. The part powers up
	 * device-controlled, which is what a false argument asks for.
	 * Host control cannot be taken without the report, so refuse it
	 * and let the caller keep the firmware's click. The MTP actuators
	 * take 0x21 whatever their descriptor says, so only an SPI one
	 * is checked.
	 */
	if (haptic_hdev->bus == BUS_SPI &&
	    !apple_taptic_has_report(haptic_hdev, HID_FEATURE_REPORT, 0x21) &&
	    !apple_taptic_has_report(haptic_hdev, HID_OUTPUT_REPORT, 0x21))
		return enable_host_controlled ? -EOPNOTSUPP : 0;

	ret = hid_hw_raw_request(haptic_hdev, msg[0], msg, sizeof(msg),
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	if (ret < 0)
		return ret;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_taptic_switch_modes);

/*
 * While the actuator is device-controlled, its firmware plays a click
 * waveform of its own on every press and release: feature report 0x22 on
 * press and 0x23 on release. Both begin with the report ID, a version byte
 * and the strength the waveform is played at. The MTP firmware boots with a
 * weak strength there and nothing else sets one, so the click is much
 * fainter than the same pad gives under macOS. A machine whose device tree
 * gives the actuator a click strength has it written over the firmware's,
 * leaving the shape of each waveform as the firmware has it.
 *
 * The strength is firmware state, so it is checked again on every resume
 * the transport passes on, and written if the firmware no longer has it.
 */
#define APPLE_TAPTIC_PRESS_WAVEFORM	0x22
#define APPLE_TAPTIC_RELEASE_WAVEFORM	0x23
#define APPLE_TAPTIC_WAVEFORM_STRENGTH	2
#define APPLE_TAPTIC_WAVEFORM_MAX	64

/**
 * struct apple_taptic - state of one actuator
 * @lock: serialises the strength writes of probe, resume and sysfs, and
 *	guards the two arrays below
 * @boot_strength: the firmware's own strength of the press and release
 *	waveforms, read before the first write so that it can be put back
 * @user_strength: press and release strength set through the click_strength
 *	attribute, 0,0 when none is set
 */
struct apple_taptic {
	struct mutex lock; /* guards the strengths and their writes */
	u8 boot_strength[2];
	u8 user_strength[2];
};

static int apple_taptic_set_strength(struct hid_device *haptic_hdev, u8 report,
				     u8 strength, u8 *boot)
{
	u8 *buf __free(kfree) = kzalloc(APPLE_TAPTIC_WAVEFORM_MAX, GFP_KERNEL);
	int len;

	if (!buf)
		return -ENOMEM;

	len = hid_hw_raw_request(haptic_hdev, report, buf,
				 APPLE_TAPTIC_WAVEFORM_MAX, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (len < 0)
		return len;
	if (len <= APPLE_TAPTIC_WAVEFORM_STRENGTH || buf[0] != report)
		return -EPROTO;
	if (!*boot)
		*boot = buf[APPLE_TAPTIC_WAVEFORM_STRENGTH];
	if (!strength)
		strength = *boot;
	if (buf[APPLE_TAPTIC_WAVEFORM_STRENGTH] == strength)
		return 0;

	hid_dbg(haptic_hdev, "click waveform 0x%02x strength 0x%02x -> 0x%02x\n",
		report, buf[APPLE_TAPTIC_WAVEFORM_STRENGTH], strength);
	buf[APPLE_TAPTIC_WAVEFORM_STRENGTH] = strength;
	len = hid_hw_raw_request(haptic_hdev, report, buf, len,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	return len < 0 ? len : 0;
}

static int apple_taptic_set_click_strength(struct hid_device *haptic_hdev,
					   struct apple_taptic *taptic)
{
	static const u8 waveforms[] = {
		APPLE_TAPTIC_PRESS_WAVEFORM,
		APPLE_TAPTIC_RELEASE_WAVEFORM,
	};
	struct device_node *np __free(device_node) = NULL;
	u32 strength[ARRAY_SIZE(waveforms)] = { };
	unsigned int i;
	int ret = -EINVAL;
	int err = 0;

	lockdep_assert_held(&taptic->lock);

	if (haptic_hdev->dev.parent)
		np = of_get_child_by_name(haptic_hdev->dev.parent->of_node,
					  "actuator");
	if (np)
		ret = of_property_read_u32_array(np, "apple,click-strength",
						 strength, ARRAY_SIZE(strength));
	if (ret && ret != -EINVAL)
		hid_warn(haptic_hdev, "invalid apple,click-strength (%d)\n", ret);
	for (i = 0; !ret && i < ARRAY_SIZE(strength); i++)
		if (!strength[i] || strength[i] > U8_MAX) {
			hid_warn(haptic_hdev, "invalid apple,click-strength (%d)\n",
				 -ERANGE);
			ret = -ERANGE;
		}
	if (ret)
		memset(strength, 0, sizeof(strength));

	for (i = 0; i < ARRAY_SIZE(waveforms); i++)
		if (taptic->user_strength[i])
			strength[i] = taptic->user_strength[i];

	/* Nothing asked for and nothing to take back: leave the firmware alone. */
	if (!strength[0] && !strength[1] && !taptic->boot_strength[0] &&
	    !taptic->boot_strength[1])
		return 0;

	for (i = 0; i < ARRAY_SIZE(waveforms); i++) {
		ret = apple_taptic_set_strength(haptic_hdev, waveforms[i],
						strength[i],
						&taptic->boot_strength[i]);
		if (ret) {
			hid_warn(haptic_hdev,
				 "cannot set the strength of click waveform 0x%02x (%d)\n",
				 waveforms[i], ret);
			err = err ?: ret;
		}
	}

	return err;
}

/*
 * click_strength is <press>,<release>, 1-255 each, used over the device
 * tree's values, so that a user can move away from the default. 0,0 (the
 * default) leaves the device tree's values, or the firmware's own when there
 * are none; a strength set earlier is then taken back to the firmware's boot
 * value.
 */
static ssize_t click_strength_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct apple_taptic *taptic = hid_get_drvdata(to_hid_device(dev));

	guard(mutex)(&taptic->lock);
	return sysfs_emit(buf, "%u,%u\n", taptic->user_strength[0],
			  taptic->user_strength[1]);
}

static ssize_t click_strength_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	char *str __free(kfree) = kstrndup(buf, count, GFP_KERNEL);
	struct hid_device *haptic_hdev = to_hid_device(dev);
	struct apple_taptic *taptic = hid_get_drvdata(haptic_hdev);
	u8 strength[2];
	char *release;
	int ret;

	if (!str)
		return -ENOMEM;
	release = strchr(str, ',');
	if (!release)
		return -EINVAL;
	*release++ = '\0';

	ret = kstrtou8(str, 10, &strength[0]);
	if (ret)
		return ret;
	ret = kstrtou8(release, 10, &strength[1]);
	if (ret)
		return ret;
	/* Both waveforms or neither: one alone would keep the firmware's. */
	if (!strength[0] != !strength[1])
		return -EINVAL;

	guard(mutex)(&taptic->lock);
	memcpy(taptic->user_strength, strength, sizeof(strength));
	ret = apple_taptic_set_click_strength(haptic_hdev, taptic);

	return ret ?: count;
}
static DEVICE_ATTR_RW(click_strength);

static struct attribute *apple_taptic_attrs[] = {
	&dev_attr_click_strength.attr,
	NULL
};
ATTRIBUTE_GROUPS(apple_taptic);

static int apple_actuator_probe(struct hid_device *haptic_hdev, const struct hid_device_id *id)
{
	struct apple_taptic *taptic;
	int ret;

	taptic = devm_kzalloc(&haptic_hdev->dev, sizeof(*taptic), GFP_KERNEL);
	if (!taptic)
		return -ENOMEM;
	ret = devm_mutex_init(&haptic_hdev->dev, &taptic->lock);
	if (ret)
		return ret;
	hid_set_drvdata(haptic_hdev, taptic);

	ret = hid_parse(haptic_hdev);
	if (ret) {
		hid_err(haptic_hdev, "hid parse failed\n");
		return ret;
	}

	/*
	 * The driver uses the actuator itself, through raw requests, so it
	 * claims it. hidraw is optional and must not decide the probe.
	 */
	ret = hid_hw_start(haptic_hdev, HID_CONNECT_HIDRAW | HID_CONNECT_DRIVER);
	if (ret) {
		hid_err(haptic_hdev, "hw start failed\n");
		return ret;
	}

	/*
	 * The actuator keeps its mode across a warm reboot, and every user of
	 * it starts out believing it is device-controlled. One left in
	 * host-controlled mode by a previous boot is silent: its firmware plays
	 * nothing, and the host that was driving it is gone. Assert the state
	 * the drivers assume rather than inheriting the last one. A failure
	 * here is not fatal; it only means the actuator keeps whatever mode it
	 * had, which is what would have happened anyway.
	 */
	ret = apple_taptic_switch_modes(haptic_hdev, false);
	if (ret)
		hid_warn(haptic_hdev,
			 "cannot return the actuator to device-controlled mode (%d)\n",
			 ret);

	/* A strength that cannot be set is warned about and is not fatal. */
	guard(mutex)(&taptic->lock);
	apple_taptic_set_click_strength(haptic_hdev, taptic);

	return 0;
}

static int apple_actuator_resume(struct hid_device *haptic_hdev)
{
	struct apple_taptic *taptic = hid_get_drvdata(haptic_hdev);

	guard(mutex)(&taptic->lock);
	apple_taptic_set_click_strength(haptic_hdev, taptic);
	return 0;
}

static bool actuator_match(struct hid_device *hdev, bool ignore_special_drivers)
{
	return apple_taptic_is_actuator(hdev);
}

static void actuator_remove(struct hid_device *haptic_hdev)
{
	apple_taptic_switch_modes(haptic_hdev, false);
	hid_hw_stop(haptic_hdev);
}

static const struct hid_device_id apple_haptic_devices[] = {
	{ HID_DEVICE(BUS_HOST, HID_GROUP_ANY, HOST_VENDOR_ID_APPLE,
		     HID_ANY_ID), .driver_data = 0 },
	{ HID_DEVICE(BUS_SPI, HID_GROUP_ANY, SPI_VENDOR_ID_APPLE,
		     HID_ANY_ID), .driver_data = 0 },
	{ }
};
MODULE_DEVICE_TABLE(hid, apple_haptic_devices);

static struct hid_driver apple_actuator_driver = {
	.name = "hid-apple-mtp-haptic",
	.id_table = apple_haptic_devices,
	.probe = apple_actuator_probe,
	.match = actuator_match,
	.remove = actuator_remove,
	.resume = pm_ptr(apple_actuator_resume),
	.reset_resume = pm_ptr(apple_actuator_resume),
	.driver.dev_groups = apple_taptic_groups,
};
module_hid_driver(apple_actuator_driver);

MODULE_DESCRIPTION("Apple MTP Haptic Actuator driver");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple always-on power telemetry (AON PTD): read-only battery readout.
 *
 * The PTD block holds fixed-size records that firmware keeps up to date. A
 * record is a run of 16-byte slots. Besides its normal read window the block
 * has a peek window (the fourth reg entry) with the same slot layout, whose
 * reads do not consume record updates. This driver maps only the peek window
 * and reads only the six documented words of the battery record that the
 * device tree names in apple,battery-record. It never maps the read, update
 * or interrupt-control windows, takes no interrupts, writes nothing and sets
 * no trips.
 *
 * The record words are 10 ms and 100 ms filtered battery samples, already
 * averaged by the producer, so the driver adds no filtering of its own.
 */

#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/units.h>

#define PTD_PEEK_WINDOW		3
#define PTD_SLOT_SIZE		16

/* The battery record: "IFtb", layout version 1, four slots. */
#define PTD_BATT_ID		0x49467462
#define PTD_BATT_VERSION	1
#define PTD_BATT_MIN_SLOTS	4

/* Words read, from byte 0x04 of the record; see enum ptd_batt_word. */
#define PTD_BATT_FIRST		0x04
#define PTD_BATT_WORDS		6

/* Two identical back-to-back reads make a sample; give up after this many. */
#define PTD_READ_TRIES		8
/* Serve repeated sysfs reads from one sample instead of polling MMIO. */
#define PTD_CACHE_NS		(10 * NSEC_PER_MSEC)

enum ptd_batt_word {
	PTD_VOLT_10MS,		/* 0x04: signed Q16.16 volts */
	PTD_CURR_100MS,		/* 0x08: unsigned milliamps */
	PTD_CURR_10MS,		/* 0x0c: signed Q16.16 amperes */
	PTD_POWER_100MS,	/* 0x10: signed Q16.16 watts */
	PTD_POWER_10MS,		/* 0x14: signed Q16.16 watts */
	PTD_VOLT_100MS,		/* 0x18: signed Q16.16 volts */
};

struct apple_ptd_sample {
	u32 word[PTD_BATT_WORDS];
};

typedef void (*apple_ptd_read_fn)(void *ctx, struct apple_ptd_sample *out);

struct apple_ptd {
	void __iomem *peek;
	u32 offset;		/* byte offset of the battery record in peek */
	struct mutex lock;	/* protects sample, valid and stamp */
	struct apple_ptd_sample sample;
	bool valid;
	ktime_t stamp;
};

/*
 * Byte offset of the battery record in a peek window of @window bytes, from
 * the device tree's record row. The id and version select the layout this
 * driver decodes; anything else is not ours to read.
 */
static int apple_ptd_record_offset(u32 id, u32 version, u32 start, u32 slots,
				   resource_size_t window, u32 *offset)
{
	u64 end;

	if (id != PTD_BATT_ID || version != PTD_BATT_VERSION)
		return -ENODEV;
	if (slots < PTD_BATT_MIN_SLOTS)
		return -EINVAL;
	end = ((u64)start + slots) * PTD_SLOT_SIZE;
	if (end > window)
		return -EINVAL;
	*offset = start * PTD_SLOT_SIZE;
	return 0;
}

/*
 * The producer updates the record while we read it, and nothing orders a
 * multi-word read against that. Take a sample only when two back-to-back
 * reads agree.
 */
static int apple_ptd_stable_read(apple_ptd_read_fn read, void *ctx,
				 struct apple_ptd_sample *out)
{
	struct apple_ptd_sample a, b;
	unsigned int i;

	for (i = 0; i < PTD_READ_TRIES; i++) {
		read(ctx, &a);
		read(ctx, &b);
		if (!memcmp(&a, &b, sizeof(a))) {
			*out = a;
			return 0;
		}
	}
	return -EAGAIN;
}

/* Signed Q16.16 to @scale units per unit, truncated toward zero. */
static int apple_ptd_q16(u32 raw, s64 scale, long *val)
{
	s64 v = div_s64((s64)(s32)raw * scale, 65536);

	if (v > LONG_MAX || v < LONG_MIN)
		return -EOVERFLOW;
	*val = v;
	return 0;
}

static int apple_ptd_convert(const struct apple_ptd_sample *s,
			     enum hwmon_sensor_types type, int channel,
			     long *val)
{
	switch (type) {
	case hwmon_in:
		return apple_ptd_q16(s->word[channel ? PTD_VOLT_100MS : PTD_VOLT_10MS],
				     MILLI, val);
	case hwmon_curr:
		if (!channel)
			return apple_ptd_q16(s->word[PTD_CURR_10MS], MILLI, val);
		if (s->word[PTD_CURR_100MS] > LONG_MAX)
			return -EOVERFLOW;
		*val = s->word[PTD_CURR_100MS];
		return 0;
	case hwmon_power:
		return apple_ptd_q16(s->word[channel ? PTD_POWER_100MS : PTD_POWER_10MS],
				     MICRO, val);
	default:
		return -EOPNOTSUPP;
	}
}

static void apple_ptd_peek(void *ctx, struct apple_ptd_sample *out)
{
	struct apple_ptd *ptd = ctx;
	void __iomem *base = ptd->peek + ptd->offset + PTD_BATT_FIRST;
	unsigned int i;

	for (i = 0; i < PTD_BATT_WORDS; i++)
		out->word[i] = readl(base + 4 * i);
}

static int apple_ptd_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	struct apple_ptd *ptd = dev_get_drvdata(dev);
	ktime_t now = ktime_get_boottime();
	int ret = 0;

	mutex_lock(&ptd->lock);
	if (!ptd->valid || ktime_to_ns(ktime_sub(now, ptd->stamp)) >= PTD_CACHE_NS) {
		ret = apple_ptd_stable_read(apple_ptd_peek, ptd, &ptd->sample);
		ptd->valid = !ret;
		ptd->stamp = now;
	}
	if (!ret)
		ret = apple_ptd_convert(&ptd->sample, type, channel, val);
	mutex_unlock(&ptd->lock);
	return ret;
}

static int apple_ptd_read_string(struct device *dev,
				 enum hwmon_sensor_types type, u32 attr,
				 int channel, const char **str)
{
	*str = channel ? "Battery 100 ms" : "Battery 10 ms";
	return 0;
}

static umode_t apple_ptd_is_visible(const void *data,
				    enum hwmon_sensor_types type, u32 attr,
				    int channel)
{
	return 0444;
}

static const struct hwmon_channel_info * const apple_ptd_info[] = {
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	NULL
};

static const struct hwmon_ops apple_ptd_ops = {
	.is_visible = apple_ptd_is_visible,
	.read = apple_ptd_read,
	.read_string = apple_ptd_read_string,
};

static const struct hwmon_chip_info apple_ptd_chip = {
	.ops = &apple_ptd_ops,
	.info = apple_ptd_info,
};

static int apple_ptd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *hwmon;
	struct apple_ptd *ptd;
	struct resource *res;
	u32 row[4];
	int ret;

	/* Without a described battery record there is nothing to read. */
	if (of_property_read_u32_array(dev->of_node, "apple,battery-record",
				       row, ARRAY_SIZE(row)))
		return -ENODEV;

	ptd = devm_kzalloc(dev, sizeof(*ptd), GFP_KERNEL);
	if (!ptd)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, PTD_PEEK_WINDOW);
	if (!res)
		return dev_err_probe(dev, -EINVAL, "no peek window\n");

	ret = apple_ptd_record_offset(row[0], row[1], row[2], row[3],
				      resource_size(res), &ptd->offset);
	if (ret)
		return dev_err_probe(dev, ret, "unsupported battery record\n");

	/* Only the peek window is mapped; probe reads nothing from it. */
	ptd->peek = devm_ioremap_resource(dev, res);
	if (IS_ERR(ptd->peek))
		return PTR_ERR(ptd->peek);

	ret = devm_mutex_init(dev, &ptd->lock);
	if (ret)
		return ret;

	hwmon = devm_hwmon_device_register_with_info(dev, "apple_aon_ptd", ptd,
						     &apple_ptd_chip, NULL);
	return PTR_ERR_OR_ZERO(hwmon);
}

static const struct of_device_id apple_ptd_of_match[] = {
	{ .compatible = "apple,t6041-aon-ptd" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_ptd_of_match);

static struct platform_driver apple_ptd_driver = {
	.probe = apple_ptd_probe,
	.driver = {
		.name = "apple-aon-ptd-hwmon",
		.of_match_table = apple_ptd_of_match,
	},
};
module_platform_driver(apple_ptd_driver);

#if IS_ENABLED(CONFIG_SENSORS_APPLE_AON_PTD_KUNIT_TEST)
#include "apple-aon-ptd-hwmon-test.c"
#endif

MODULE_DESCRIPTION("Apple AON PTD read-only battery telemetry");
MODULE_LICENSE("Dual MIT/GPL");

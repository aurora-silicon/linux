// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Read-only J613/25G83 PMP telemetry. No cooling or firmware writes. */
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#define SRAM_BASE 0x2d0500000ULL
#define SRAM_SIZE 0x80000
#define RECORD_SIZE 0xa4
#define NAME_OFFSET 0x94
#define CHANNELS 6

static const char * const names[CHANNELS] = {
	"ta000m", "te000m", "te001m", "tp000m", "tp001m", "tp002m",
};
static void __iomem *sram;
static u32 offsets[CHANNELS];
static struct device *hwmon;
static struct platform_device *parent;

/* Explicit aligned 32-bit accesses: never vector-copy unaligned SRAM MMIO. */
static void copy_record(u8 *out, u32 offset)
{
	unsigned int i;

	for (i = 0; i < RECORD_SIZE; i += 4)
		put_unaligned_le32(readl(sram + offset + i), out + i);
}

static int sample(unsigned int index, long *value)
{
	u8 a[RECORD_SIZE], b[RECORD_SIZE];
	s16 raw;
	unsigned int attempt;

	for (attempt = 0; attempt < 20; attempt++) {
		copy_record(a, offsets[index]);
		copy_record(b, offsets[index]);
		if (memcmp(a, b, sizeof(a)))
			continue;
		if (get_unaligned_le32(a) != index + 1 ||
		    get_unaligned_le32(a + 4) != 6 ||
		    memcmp(a + NAME_OFFSET, names[index], 7))
			return -EIO;
		/* +0x30 is the current-valid byte. +0x48 is retained-valid. */
		if (a[0x30] != 1)
			return a[0x30] ? -EIO : -ENODATA;
		raw = (s16)get_unaligned_le16(a + 0x24);
		if (raw < -2560 || raw > 9600)
			return -ERANGE;
		*value = DIV_ROUND_CLOSEST((s64)raw * 1000, 64);
		return 0;
	}
	return -EAGAIN;
}

static umode_t visible(const void *data, enum hwmon_sensor_types type,
			      u32 attr, int channel)
{
	return channel >= 0 && channel <= CHANNELS && type == hwmon_temp &&
	       (attr == hwmon_temp_input || attr == hwmon_temp_label) ? 0444 : 0;
}

static int read_temp_locked(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long *value)
{
	long hottest = LONG_MIN, reading;
	unsigned int i;
	int ret;

	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	if (channel < CHANNELS)
		return sample(channel, value);
	for (i = 0; i < CHANNELS; i++) {
		ret = sample(i, &reading);
		if (ret == -ENODATA)
			continue;
		if (ret)
			return ret;
		hottest = max(hottest, reading);
	}
	if (hottest == LONG_MIN)
		return -ENODATA;
	*value = hottest;
	return 0;
}

/* The supplier lock excludes failed probe and removal during SRAM reads. */
static int read_temp(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long *value)
{
	int ret;

	if (channel < 0 || channel > CHANNELS)
		return -EINVAL;
	device_lock(&parent->dev);
	ret = device_is_bound(&parent->dev) ?
		read_temp_locked(dev, type, attr, channel, value) : -ENODEV;
	device_unlock(&parent->dev);
	return ret;
}

static int read_label(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, const char **value)
{
	if (channel < 0 || channel > CHANNELS)
		return -EINVAL;
	if (type != hwmon_temp || attr != hwmon_temp_label)
		return -EOPNOTSUPP;
	*value = channel < CHANNELS ? names[channel] : "PMP valid-sensor maximum";
	return 0;
}

static const struct hwmon_ops ops = {
	.is_visible = visible,
	.read = read_temp,
	.read_string = read_label,
};
static const struct hwmon_channel_info * const channels[] = {
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL),
	NULL,
};
static const struct hwmon_chip_info chip = { .ops = &ops, .info = channels };

/*
 * The PMP firmware publishes its sensor records some time after the PMP
 * endpoint comes up, and nothing signals when. The modalias below loads this
 * module as soon as the PMP device appears, which can be before the PMP
 * driver has bound and before the records exist, so poll for the records
 * (bounded) instead of scanning once.
 */
#define SCAN_INTERVAL_MS 500
#define SCAN_ATTEMPTS 240

static struct delayed_work scan_work;
static unsigned int attempts;

/* 0 when all six records are found once each, -EAGAIN when not yet published. */
static int scan_locked(void)
{
	u8 *snapshot;
	unsigned int i, j, hits;
	int ret = 0;

	if (!parent->dev.driver)
		return -EAGAIN;
	snapshot = vmalloc(SRAM_SIZE);
	if (!snapshot)
		return -ENOMEM;
	for (i = 0; i < SRAM_SIZE; i += 4)
		put_unaligned_le32(readl(sram + i), snapshot + i);
	for (j = 0; j < CHANNELS; j++) {
		hits = 0;
		for (i = NAME_OFFSET; i + 16 <= SRAM_SIZE; i += 4) {
			if (!memcmp(snapshot + i, names[j], 7) &&
			    get_unaligned_le32(snapshot + i - NAME_OFFSET) == j + 1 &&
			    get_unaligned_le32(snapshot + i - NAME_OFFSET + 4) == 6) {
				offsets[j] = i - NAME_OFFSET;
				hits++;
			}
		}
		if (!hits) {
			ret = -EAGAIN;
			break;
		}
		if (hits != 1 || offsets[j] + RECORD_SIZE > SRAM_SIZE ||
		    (j && offsets[j] != offsets[0] + j * RECORD_SIZE)) {
			ret = -EIO;
			break;
		}
	}
	vfree(snapshot);
	return ret;
}

static int scan(void)
{
	int ret;

	device_lock(&parent->dev);
	ret = device_is_bound(&parent->dev) ? scan_locked() : -EAGAIN;
	device_unlock(&parent->dev);
	return ret;
}

static void scan_fn(struct work_struct *work)
{
	struct device *dev;
	int ret;

	attempts++;
	ret = scan();
	if (ret == -EAGAIN && attempts < SCAN_ATTEMPTS) {
		schedule_delayed_work(&scan_work, msecs_to_jiffies(SCAN_INTERVAL_MS));
		return;
	}
	if (ret) {
		pr_warn("j613-pmp: sensor records %s after %u scans (%d); no telemetry\n",
			ret == -EAGAIN ? "never published" : "inconsistent", attempts, ret);
		return;
	}
	dev = hwmon_device_register_with_info(&parent->dev, "j613_pmp", NULL, &chip, NULL);
	if (IS_ERR(dev)) {
		pr_warn("j613-pmp: hwmon registration failed (%ld)\n", PTR_ERR(dev));
		return;
	}
	hwmon = dev;
	pr_info("j613-pmp: read-only six-channel telemetry after %u scans; current samples only\n",
		attempts);
}

static int __init pmp_j613_init(void)
{
	struct device_node *node;
	struct resource res;
	const char *uuid;
	int index;
	int ret;

	if (!of_machine_is_compatible("apple,j613") ||
	    !of_machine_is_compatible("apple,t8122"))
		return -ENODEV;
	node = of_find_node_by_path("/soc/pmp@2d0500000");
	if (!node)
		return -ENODEV;
	ret = of_property_read_string(node, "apple,tunable-uuid", &uuid);
	if (ret || strcmp(uuid, "EFD60284-7C58-33A9-8522-CB3366AF6040") ||
	    !of_device_is_compatible(node, "apple,t8122-pmp-v2")) {
		of_node_put(node);
		return -ENODEV;
	}
	index = of_property_match_string(node, "reg-names", "pmp");
	if (index < 0 || of_address_to_resource(node, index, &res) ||
	    res.start != SRAM_BASE || resource_size(&res) != SRAM_SIZE) {
		of_node_put(node);
		return -ENODEV;
	}
	parent = of_find_device_by_node(node);
	of_node_put(node);
	if (!parent)
		return -ENODEV;
	sram = ioremap(SRAM_BASE, SRAM_SIZE);
	if (!sram) {
		put_device(&parent->dev);
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&scan_work, scan_fn);
	schedule_delayed_work(&scan_work, 0);
	return 0;
}

static void __exit pmp_j613_exit(void)
{
	cancel_delayed_work_sync(&scan_work);
	if (hwmon)
		hwmon_device_unregister(hwmon);
	iounmap(sram);
	put_device(&parent->dev);
}

/*
 * There is no driver to bind: the PMP driver owns the device. The table only
 * gives udev a modalias, so the module loads once the PMP device appears
 * (on 25G83, appledrm enables it); init then applies the J613 checks above
 * and the delayed scan waits for the PMP driver and its records.
 */
static const struct of_device_id pmp_j613_of_match[] = {
	{ .compatible = "apple,t8122-pmp-v2" },
	{}
};
MODULE_DEVICE_TABLE(of, pmp_j613_of_match);

module_init(pmp_j613_init);
module_exit(pmp_j613_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Read-only J613 25G83 PMP current temperature telemetry");

// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC PMP power state reporting driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/soc/apple/pmp-report.h>
#include <linux/soc/apple/pmp-temps.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>

#define PMP_REPORT_READY 0x1

struct apple_pmp_report_offsets {
	u32 tgt_read;
	u32 tgt_write;
	u32 actual;
	u32 status;
	/* The PMP is started by this driver, once its requests are seeded. */
	bool starts_pmp;
};

struct apple_pmp_report {
	struct device *dev;
	const struct apple_pmp_report_offsets *offsets;
	void __iomem *base;
	void __iomem *temp_sram;
	spinlock_t lock;

	/* Only with offsets->starts_pmp */
	struct device_node *pmp;
	u64 seed;
	u64 ack;
	/* Requests that must be acknowledged before any consumer can proceed. */
	u64 startup_ack;
	struct of_changeset pmp_cs;
	struct work_struct start_work;
	/* The requests are in place; the first consumer starts the PMP. */
	bool seeded;
	atomic_t start_requested;
	atomic_t start_queued;
	struct completion started;
	int start_result;
};


static umode_t apple_pmp_temp_is_visible(const void *data,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel)
{
	if (channel < 0 || channel >= PMP_TEMP_CHANNELS || type != hwmon_temp)
		return 0;
	if (attr == hwmon_temp_input || attr == hwmon_temp_label)
		return 0444;
	return 0;
}

static int apple_pmp_temp_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *value)
{
	struct apple_pmp_report *rep = dev_get_drvdata(dev);

	if (channel < 0 || channel >= PMP_TEMP_CHANNELS)
		return -EINVAL;
	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	if (!channel)
		return apple_pmp_temp_hotspot(rep->temp_sram, value);
	return apple_pmp_temp_sample(rep->temp_sram, channel - 1, value);
}

static int apple_pmp_temp_read_string(struct device *dev, enum hwmon_sensor_types type,
				      u32 attr, int channel, const char **str)
{
	if (channel < 0 || channel >= PMP_TEMP_CHANNELS)
		return -EINVAL;
	if (type != hwmon_temp || attr != hwmon_temp_label)
		return -EOPNOTSUPP;

	*str = channel ? apple_pmp_temp_records[channel - 1].label :
			 "PMP aggregate die hotspot";
	return 0;
}

static const struct hwmon_ops apple_pmp_temp_ops = {
	.is_visible = apple_pmp_temp_is_visible,
	.read = apple_pmp_temp_read,
	.read_string = apple_pmp_temp_read_string,
};

static const struct hwmon_channel_info * const apple_pmp_temp_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL,
};

static const struct hwmon_chip_info apple_pmp_temp_chip = {
	.ops = &apple_pmp_temp_ops,
	.info = apple_pmp_temp_info,
};

static int apple_pmp_temp_register(struct apple_pmp_report *rep)
{
	struct device *hwmon;

	/* Read-only telemetry from running firmware; never start or stop PMP. */
	rep->temp_sram = devm_ioremap(rep->dev, PMP_TEMP_SRAM_BASE, PMP_TEMP_SRAM_SIZE);
	if (!rep->temp_sram)
		return dev_err_probe(rep->dev, -ENOMEM, "cannot map PMP temperature SRAM\n");

	hwmon = devm_hwmon_device_register_with_info(rep->dev, "apple_pmp", rep,
						     &apple_pmp_temp_chip, NULL);
	if (IS_ERR(hwmon))
		return dev_err_probe(rep->dev, PTR_ERR(hwmon), "cannot register PMP hwmon\n");
	return 0;
}

/*
 * T6030: the PMP is described in the device tree, disabled, and this driver
 * starts it. iBoot leaves the PMP loaded but halted. Once the PMP runs it
 * owns the SoC power states, and it keeps a block powered only if that
 * block's request bit is set in the PTD when it starts. So, in order:
 *
 *  1. the display power domains must be on, the DCP CPU domain must already
 *     have "active" as its minimum state (the PMP must not be able to power
 *     the running DCP down), and the PTD SRAM and the PMP must be powered;
 *  2. the PMP CPU must be halted, and its SRAM must hold the firmware image
 *     the device tree describes, with the boot arguments the PMP driver
 *     fills in;
 *  3. the PTD must report the PMP as not ready, with no request set except
 *     the ones this driver seeds;
 *  4. the always-on report entries (display and storage) set their request
 *     bits while the PMP is still halted, and the result is read back;
 *  5. the PMP is started only when a consumer first waits for it through
 *     apple_pmp_report_wait_ready(), after the rest of the SoC has been
 *     brought up: the halted PMP and the PTD are checked again, then the
 *     PMP and its DART and mailbox are enabled, which starts the PMP
 *     through its own driver;
 *  6. once that driver has bound, and after one second in which the PTD is
 *     not touched, the PMP must report ready and acknowledge the requests
 *     required at startup. Entries marked apple,defer-startup-ack still
 *     require their own acknowledgment before their consumer can proceed.
 *
 * A refusal before step 5 leaves the PMP halted, and so does a boot on which
 * no consumer asks for it. Once the PMP has been started it can only be
 * stopped by a reboot.
 */
#define PMP_ASC_CPU_CONTROL		0x44
#define PMP_ASC_CPU_RUN			BIT(4)
#define PMP_SRAM_UUID			0x214
#define PMP_SRAM_BOOTARGS_OFFSET	0x22c
#define PMP_SRAM_BOOTARGS_SIZE		0x230

#define APPLE_PMGR_PS_MIN		GENMASK(19, 16)
#define APPLE_PMGR_PS_ACTUAL		GENMASK(7, 4)
#define APPLE_PMGR_PS_ACTIVE		0xf

#define PMP_START_BIND_TIMEOUT_MS	20000
#define PMP_START_QUIET_MS		1000
#define PMP_START_READY_TIMEOUT_MS	3000
#define PMP_START_ACK_TIMEOUT_MS	1000

#define FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

static const struct {
	const char *label;
	/* The minimum state must already be "active". */
	bool floor;
} apple_pmp_t6030_domains[] = {
	{ "pms_sram", false },
	{ "pmp", false },
	/*
	 * DISP_SYS and DISP_FE only need to be on: their minimum-state field
	 * does not hold a value (it reads back 0 after "active" is written),
	 * and the PMP leaves them on once the DCP CPU domain has its floor.
	 */
	{ "disp_sys", false },
	{ "disp_fe", false },
	/* Without this floor the PMP stops the running DCP CPU when it starts. */
	{ "disp_cpu", true },
};

/* Finds the single T6030 PMGR power state with @label. */
static struct device_node *apple_pmp_t6030_domain(const char *label)
{
	struct device_node *np, *found = NULL;
	const char *name;

	for_each_compatible_node(np, NULL, "apple,t6030-pmgr-pwrstate") {
		if (of_property_read_string(np, "label", &name) || strcmp(name, label))
			continue;
		if (found) {
			of_node_put(np);
			of_node_put(found);
			return NULL;
		}
		found = of_node_get(np);
	}

	return found;
}

static int apple_pmp_t6030_check_power(struct apple_pmp_report *rep)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(apple_pmp_t6030_domains); i++) {
		const char *label = apple_pmp_t6030_domains[i].label;
		struct device_node *np = apple_pmp_t6030_domain(label);
		struct regmap *regmap;
		u32 offset, val;
		int ret;

		if (!np)
			return dev_err_probe(rep->dev, -ENODEV,
					     "T6030 PMP not started: no single %s power state\n",
					     label);
		ret = of_property_read_u32(np, "reg", &offset);
		regmap = syscon_node_to_regmap(np->parent);
		of_node_put(np);
		if (ret)
			return ret;
		if (IS_ERR(regmap))
			return PTR_ERR(regmap);
		ret = regmap_read(regmap, offset, &val);
		if (ret)
			return ret;

		if (FIELD_GET(APPLE_PMGR_PS_ACTUAL, val) != APPLE_PMGR_PS_ACTIVE)
			return dev_err_probe(rep->dev, -EIO,
					     "T6030 PMP not started: %s is not powered (%#x)\n",
					     label, val);
		/* The PMGR power state driver applies the floor when it probes. */
		if (apple_pmp_t6030_domains[i].floor &&
		    FIELD_GET(APPLE_PMGR_PS_MIN, val) != APPLE_PMGR_PS_ACTIVE)
			return dev_err_probe(rep->dev, -EPROBE_DEFER,
					     "waiting for the %s minimum state to be active (%#x)\n",
					     label, val);
	}

	return 0;
}

static void __iomem *apple_pmp_t6030_map(struct apple_pmp_report *rep, const char *name,
					 resource_size_t *size)
{
	struct resource res;
	void __iomem *base;
	int idx;

	idx = of_property_match_string(rep->pmp, "reg-names", name);
	if (idx < 0 || of_address_to_resource(rep->pmp, idx, &res))
		return NULL;
	/* The PMP driver claims these registers; only read them here. */
	base = ioremap_np(res.start, resource_size(&res));
	if (base && size)
		*size = resource_size(&res);
	return base;
}

/* The SRAM must hold the image named by the node's tunables, and its boot arguments. */
static int apple_pmp_t6030_check_sram(struct apple_pmp_report *rep, uuid_t *image)
{
	void __iomem *sram;
	resource_size_t sram_size;
	const char *expected;
	u32 offset, size, pos, found = 0;
	u8 *args;
	uuid_t want;
	int ret = 0;

	if (of_property_read_string(rep->pmp, "apple,tunable-uuid", &expected) ||
	    uuid_parse(expected, &want))
		return -EINVAL;
	sram = apple_pmp_t6030_map(rep, "pmp", &sram_size);
	if (!sram)
		return -ENOMEM;
	if (sram_size < PMP_SRAM_BOOTARGS_SIZE + 4) {
		ret = -EINVAL;
		goto out;
	}
	/* Aligned 32-bit reads only: the SRAM is device memory. */
	for (pos = 0; pos < sizeof(image->b); pos += 4)
		put_unaligned_le32(readl(sram + PMP_SRAM_UUID + pos), image->b + pos);
	offset = readl(sram + PMP_SRAM_BOOTARGS_OFFSET);
	size = readl(sram + PMP_SRAM_BOOTARGS_SIZE);
	if (!uuid_equal(image, &want) || size < 8 || size > SZ_4K ||
	    offset > sram_size - size || offset & 3) {
		ret = -EINVAL;
		goto out;
	}

	/* The boot-argument entries are packed: parse a copy. */
	args = kmalloc(size, GFP_KERNEL);
	if (!args) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy_fromio(args, sram + offset, size);
	for (pos = 0; pos + 8 <= size;) {
		u32 key = get_unaligned_le32(args + pos);
		u32 len = get_unaligned_le32(args + pos + 4);

		if (key == FOURCC('B', 'D', 'I', 'D'))
			found |= BIT(0);
		if (key == FOURCC('D', 'V', 'I', 'D'))
			found |= BIT(1);
		if (len > size - pos - 8)
			break;
		pos += 8 + len;
	}
	kfree(args);
	if (found != (BIT(0) | BIT(1)))
		ret = -EINVAL;
out:
	iounmap(sram);
	return ret;
}

/*
 * The PMP tunables describe the PTD: the request and acknowledge ranges
 * must be where the offsets say, and each seeded device must be the one its
 * index names, acknowledged exactly when the node expects an acknowledgment.
 */
#define PMP_SOC_DEVICE_SIZE	0x7c
#define PMP_SOC_DEVICE_ACK	BIT(1)
#define PMP_PTD_RANGE_SIZE	32
#define PMP_PTD_RANGE_REQUEST	10
#define PMP_PTD_RANGE_ACK	11

static int apple_pmp_t6030_check_layout(struct apple_pmp_report *rep)
{
	const u8 *table;
	unsigned int found = 0;
	int len, i;

	table = of_get_property(rep->pmp, "apple,tunable-ptd-range", &len);
	if (!table || len % PMP_PTD_RANGE_SIZE)
		return -EINVAL;
	for (i = 0; i < len; i += PMP_PTD_RANGE_SIZE) {
		u32 id = get_unaligned_le32(table + i);
		u32 base = get_unaligned_le32(table + i + 4);

		if (id == PMP_PTD_RANGE_REQUEST && base * 16 == rep->offsets->tgt_read &&
		    0x10000 + base * 8 == rep->offsets->tgt_write)
			found |= BIT(0);
		if (id == PMP_PTD_RANGE_ACK && base * 16 == rep->offsets->actual)
			found |= BIT(1);
	}
	if (found != (BIT(0) | BIT(1)))
		return -EINVAL;

	table = of_get_property(rep->pmp, "apple,tunable-soc-device", &len);
	if (!table || len % PMP_SOC_DEVICE_SIZE)
		return -EINVAL;
	for (i = 0; i < 64; i++) {
		const u8 *dev = table + i * PMP_SOC_DEVICE_SIZE;

		if (!(rep->seed & BIT_ULL(i)))
			continue;
		if ((i + 1) * PMP_SOC_DEVICE_SIZE > len || get_unaligned_le32(dev) != i + 1 ||
		    !(get_unaligned_le32(dev + 8) & PMP_SOC_DEVICE_ACK) != !(rep->ack & BIT_ULL(i)))
			return -EINVAL;
	}
	return 0;
}

static int apple_pmp_t6030_check(struct apple_pmp_report *rep)
{
	struct device_node *np = rep->dev->of_node;
	struct device_node *child;
	void __iomem *asc;
	uuid_t image;
	u64 status, request, ack;
	u32 control, id;
	int ret;

	/* 1: power */
	ret = apple_pmp_t6030_check_power(rep);
	if (ret)
		return ret;

	/* 2: a halted PMP holding the described firmware image */
	rep->pmp = of_parse_phandle(np, "apple,pmp", 0);
	if (!rep->pmp || !of_device_is_compatible(rep->pmp, "apple,t6000-pmp-v2") ||
	    of_device_is_available(rep->pmp))
		return dev_err_probe(rep->dev, -EINVAL,
				     "T6030 PMP not started: apple,pmp is not a disabled PMP node\n");
	asc = apple_pmp_t6030_map(rep, "asc", NULL);
	if (!asc)
		return -ENOMEM;
	control = readl(asc + PMP_ASC_CPU_CONTROL);
	iounmap(asc);
	if (control & PMP_ASC_CPU_RUN)
		return dev_err_probe(rep->dev, -EBUSY,
				     "T6030 PMP not started: its CPU is already running (%#x)\n",
				     control);
	ret = apple_pmp_t6030_check_sram(rep, &image);
	if (ret)
		return dev_err_probe(rep->dev, ret,
				     "T6030 PMP not started: its SRAM does not hold the described firmware\n");

	/* 3: the requests this driver seeds, and a PTD with nothing else in it */
	for_each_available_child_of_node(np, child) {
		if (!of_property_read_bool(child, "apple,always-on") ||
		    of_property_read_u32(child, "reg", &id) || id > 63)
			continue;
		rep->seed |= BIT_ULL(id);
		if (!of_property_read_bool(child, "apple,no-ack")) {
			rep->ack |= BIT_ULL(id);
			if (!of_property_read_bool(child, "apple,defer-startup-ack"))
				rep->startup_ack |= BIT_ULL(id);
		}
	}
	if (!rep->seed || !rep->startup_ack)
		return dev_err_probe(rep->dev, -EINVAL,
				     "T6030 PMP not started: no acknowledged always-on report\n");
	if (apple_pmp_t6030_check_layout(rep))
		return dev_err_probe(rep->dev, -EINVAL,
				     "T6030 PMP not started: its tunables do not describe this PTD layout\n");

	status = readq(rep->base + rep->offsets->status);
	request = readq(rep->base + rep->offsets->tgt_read);
	ack = readq(rep->base + rep->offsets->actual);
	if (status || (request & ~rep->seed))
		return dev_err_probe(rep->dev, -EBUSY,
				     "T6030 PMP not started: PTD status %#llx request %#llx before start\n",
				     status, request);

	dev_info(rep->dev,
		 "PMP halted, firmware %pUb; PTD status %#llx request %#llx ack %#llx; requesting %#llx\n",
		 &image, status, request, ack, rep->seed);
	return 0;
}

static void apple_pmp_t6030_finish(struct apple_pmp_report *rep, int result)
{
	rep->start_result = result;
	complete_all(&rep->started);
}

static bool apple_pmp_t6030_bound(struct apple_pmp_report *rep)
{
	struct platform_device *pdev = of_find_device_by_node(rep->pmp);
	bool bound = false;

	if (!pdev)
		return false;
	/* The device lock is held for a whole probe: never wait on it. */
	if (device_trylock(&pdev->dev)) {
		bound = device_is_bound(&pdev->dev);
		device_unlock(&pdev->dev);
	}
	put_device(&pdev->dev);
	return bound;
}

/* Just before the start: the PMP still halted, and the PTD as seeded. */
static int apple_pmp_t6030_recheck(struct apple_pmp_report *rep)
{
	void __iomem *asc = apple_pmp_t6030_map(rep, "asc", NULL);
	u64 status, request;
	u32 control;

	if (!asc)
		return -ENOMEM;
	control = readl(asc + PMP_ASC_CPU_CONTROL);
	iounmap(asc);
	status = readq(rep->base + rep->offsets->status);
	request = readq(rep->base + rep->offsets->tgt_read);
	if ((control & PMP_ASC_CPU_RUN) || status || request != rep->seed) {
		dev_err(rep->dev,
			"T6030 PMP not started: CPU control %#x, PTD status %#llx request %#llx changed since seeding\n",
			control, status, request);
		return -EBUSY;
	}
	return 0;
}

/* The PMP is enabled with its DART and mailbox, which must still be disabled. */
static int apple_pmp_t6030_enable(struct apple_pmp_report *rep)
{
	struct device_node *nodes[3];
	unsigned int i;
	int ret = 0;

	nodes[0] = of_parse_phandle(rep->pmp, "iommus", 0);
	nodes[1] = of_parse_phandle(rep->pmp, "mboxes", 0);
	nodes[2] = of_node_get(rep->pmp);

	of_changeset_init(&rep->pmp_cs);
	for (i = 0; i < ARRAY_SIZE(nodes) && !ret; i++) {
		if (!nodes[i] || of_device_is_available(nodes[i]))
			ret = -EINVAL;
		else
			ret = of_changeset_update_prop_string(&rep->pmp_cs, nodes[i],
							      "status", "okay");
	}
	/* Platform devices are created, and probed, in this order. */
	if (!ret)
		ret = of_changeset_apply(&rep->pmp_cs);
	if (ret)
		of_changeset_destroy(&rep->pmp_cs);

	for (i = 0; i < ARRAY_SIZE(nodes); i++)
		of_node_put(nodes[i]);
	return ret;
}

/*
 * T6030 PMP temperatures, read from the SRAM of the PMP image the J516S
 * overlay names (the report driver starts no other image): 0xa4-byte records
 * with the sensor name at +0x94, the last valid sample (1/64 degree Celsius)
 * at +0x3c and its valid flag at +0x48. The current sample is not used: it is
 * invalid while a CPU cluster is idle, so the last valid sample of an idle
 * cluster is its temperature when it was last active. Te* and Tp* are the CPU
 * cluster sensors; Ta000m is often without a valid sample. None of them is a
 * GPU sensor. Registered once the PMP runs: before that the records hold
 * whatever the halted image left.
 */
#define T6030_PMP_TEMP_RECORD_SIZE	0xa4
#define T6030_PMP_TEMP_NAME		0x94
#define T6030_PMP_TEMP_SAMPLE		0x3c
#define T6030_PMP_TEMP_VALID		0x48
#define T6030_PMP_TEMP_READ_ATTEMPTS	20
#define T6030_PMP_TEMP_ZONE		"apple_pmp_hotspot"

static const struct {
	const char *name;
	const char *label;
	u32 offset;
	/* May have no valid sample; the hotspot then leaves it out. */
	bool optional;
} apple_pmp_t6030_temps[] = {
	{ "ta000m", "Ta000m", 0x69190, true },
	{ "te000m", "Te000m", 0x69234 },
	{ "te001m", "Te001m", 0x692d8 },
	{ "tp000m", "Tp000m", 0x6937c },
	{ "tp001m", "Tp001m", 0x69420 },
	{ "tp002m", "Tp002m", 0x694c4 },
};

#define T6030_PMP_TEMP_CHANNELS		(1 + ARRAY_SIZE(apple_pmp_t6030_temps))

static bool temps = true;
module_param(temps, bool, 0444);
MODULE_PARM_DESC(temps, "Register the T6030 PMP temperatures once the PMP runs (default: on)");

static int apple_pmp_t6030_temp_sample(struct apple_pmp_report *rep, unsigned int index,
				       long *value)
{
	const char *name = apple_pmp_t6030_temps[index].name;
	void __iomem *record = rep->temp_sram + apple_pmp_t6030_temps[index].offset;
	u8 first[T6030_PMP_TEMP_RECORD_SIZE], second[T6030_PMP_TEMP_RECORD_SIZE];
	unsigned int attempt;
	s32 sample;

	/* The PMP updates records while they are read: use a stable copy. */
	for (attempt = 0; attempt < T6030_PMP_TEMP_READ_ATTEMPTS; attempt++) {
		memcpy_fromio(first, record, sizeof(first));
		memcpy_fromio(second, record, sizeof(second));
		if (memcmp(first, second, sizeof(first)))
			continue;
		if (memcmp(first + T6030_PMP_TEMP_NAME, name, strlen(name) + 1))
			return -EIO;
		if (get_unaligned_le32(first + T6030_PMP_TEMP_VALID) != 1)
			return -ENODATA;
		sample = (s32)get_unaligned_le32(first + T6030_PMP_TEMP_SAMPLE);
		if (sample < -2560 || sample > 9600)
			return -ERANGE;
		*value = DIV_ROUND_CLOSEST((s64)sample * 1000, 64);
		return 0;
	}
	return -EAGAIN;
}

/* The hottest record; fails if a record that is not optional cannot be read. */
static int apple_pmp_t6030_temp_hotspot(struct apple_pmp_report *rep, long *value)
{
	bool found = false;
	long hottest = 0, sample;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(apple_pmp_t6030_temps); i++) {
		ret = apple_pmp_t6030_temp_sample(rep, i, &sample);
		if (ret == -ENODATA && apple_pmp_t6030_temps[i].optional)
			continue;
		if (ret)
			return ret;
		if (!found || sample > hottest)
			hottest = sample;
		found = true;
	}
	if (!found)
		return -ENODATA;
	*value = hottest;
	return 0;
}

static umode_t apple_pmp_t6030_temp_is_visible(const void *data,
					       enum hwmon_sensor_types type, u32 attr,
					       int channel)
{
	if (type != hwmon_temp || channel < 0 || channel >= T6030_PMP_TEMP_CHANNELS)
		return 0;
	if (attr == hwmon_temp_input || attr == hwmon_temp_label)
		return 0444;
	return 0;
}

static int apple_pmp_t6030_temp_read(struct device *dev, enum hwmon_sensor_types type,
				     u32 attr, int channel, long *value)
{
	struct apple_pmp_report *rep = dev_get_drvdata(dev);

	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	if (channel < 0 || channel >= T6030_PMP_TEMP_CHANNELS)
		return -EINVAL;
	if (!channel)
		return apple_pmp_t6030_temp_hotspot(rep, value);
	return apple_pmp_t6030_temp_sample(rep, channel - 1, value);
}

static int apple_pmp_t6030_temp_read_string(struct device *dev, enum hwmon_sensor_types type,
					    u32 attr, int channel, const char **str)
{
	if (type != hwmon_temp || attr != hwmon_temp_label)
		return -EOPNOTSUPP;
	if (channel < 0 || channel >= T6030_PMP_TEMP_CHANNELS)
		return -EINVAL;
	*str = channel ? apple_pmp_t6030_temps[channel - 1].label : "PMP CPU hotspot";
	return 0;
}

static const struct hwmon_ops apple_pmp_t6030_temp_ops = {
	.is_visible = apple_pmp_t6030_temp_is_visible,
	.read = apple_pmp_t6030_temp_read,
	.read_string = apple_pmp_t6030_temp_read_string,
};

static const struct hwmon_channel_info * const apple_pmp_t6030_temp_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL,
};

static const struct hwmon_chip_info apple_pmp_t6030_temp_chip = {
	.ops = &apple_pmp_t6030_temp_ops,
	.info = apple_pmp_t6030_temp_info,
};

#if IS_ENABLED(CONFIG_THERMAL)
static int apple_pmp_t6030_zone_get_temp(struct thermal_zone_device *tz, int *temp)
{
	long value;
	int ret;

	ret = apple_pmp_t6030_temp_hotspot(thermal_zone_device_priv(tz), &value);
	if (!ret)
		*temp = value;
	return ret;
}

static const struct thermal_zone_device_ops apple_pmp_t6030_zone_ops = {
	.get_temp = apple_pmp_t6030_zone_get_temp,
};

static void apple_pmp_t6030_zone_unregister(void *data)
{
	thermal_zone_device_unregister(data);
}

/*
 * A thermal zone with the hotspot, for in-kernel users that limit SoC power
 * by temperature (they look it up by name). No trip points, not polled.
 */
static void apple_pmp_t6030_zone_register(struct apple_pmp_report *rep)
{
	struct thermal_zone_device *tz;

	tz = thermal_tripless_zone_device_register(T6030_PMP_TEMP_ZONE, rep,
						   &apple_pmp_t6030_zone_ops, NULL);
	if (IS_ERR(tz)) {
		dev_warn(rep->dev, "cannot register the PMP temperature zone: %pe\n", tz);
		return;
	}
	devm_add_action_or_reset(rep->dev, apple_pmp_t6030_zone_unregister, tz);
}
#else
static void apple_pmp_t6030_zone_register(struct apple_pmp_report *rep)
{
}
#endif

static void apple_pmp_t6030_temp_unmap(void *data)
{
	iounmap(data);
}

/* Once the PMP runs: its temperatures as a hwmon device and a thermal zone. Never fatal. */
static void apple_pmp_t6030_temps_register(struct apple_pmp_report *rep)
{
	resource_size_t size;
	struct device *hwmon;
	unsigned int i;
	long value;
	int ret;

	if (!temps) {
		dev_info(rep->dev, "PMP temperatures not registered (pmp_report.temps=0)\n");
		return;
	}
	rep->temp_sram = apple_pmp_t6030_map(rep, "pmp", &size);
	if (!rep->temp_sram) {
		dev_warn(rep->dev, "cannot map the PMP SRAM for its temperatures\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(apple_pmp_t6030_temps); i++) {
		if (apple_pmp_t6030_temps[i].offset + T6030_PMP_TEMP_RECORD_SIZE > size) {
			iounmap(rep->temp_sram);
			rep->temp_sram = NULL;
			dev_warn(rep->dev, "PMP SRAM too small for its temperature records\n");
			return;
		}
	}
	if (devm_add_action_or_reset(rep->dev, apple_pmp_t6030_temp_unmap, rep->temp_sram)) {
		rep->temp_sram = NULL;
		return;
	}

	hwmon = devm_hwmon_device_register_with_info(rep->dev, "apple_pmp", rep,
						     &apple_pmp_t6030_temp_chip, NULL);
	if (IS_ERR(hwmon))
		dev_warn(rep->dev, "cannot register the PMP temperatures: %pe\n", hwmon);
	apple_pmp_t6030_zone_register(rep);

	ret = apple_pmp_t6030_temp_hotspot(rep, &value);
	if (ret)
		dev_info(rep->dev, "PMP temperatures registered; hotspot not readable yet (%d)\n", ret);
	else
		dev_info(rep->dev, "PMP temperatures registered; CPU hotspot %ld mC (zone %s)\n",
			 value, T6030_PMP_TEMP_ZONE);
}

static void apple_pmp_t6030_start(struct work_struct *work)
{
	struct apple_pmp_report *rep = container_of(work, struct apple_pmp_report, start_work);
	u64 status = 0, request, ack = 0;
	unsigned long deadline;
	int ret;

	/* 5: nothing may have changed since the requests were seeded */
	ret = apple_pmp_t6030_recheck(rep);
	if (ret) {
		apple_pmp_t6030_finish(rep, ret);
		return;
	}
	/* From here the PMP may run, and only a reboot stops it. */
	dev_info(rep->dev, "starting the PMP with request %#llx\n", rep->seed);
	ret = apple_pmp_t6030_enable(rep);
	if (ret) {
		dev_err(rep->dev, "T6030 PMP not started: enabling its nodes failed: %d\n", ret);
		apple_pmp_t6030_finish(rep, ret);
		return;
	}

	/* 6: bound means the firmware and its endpoint are up */
	deadline = jiffies + msecs_to_jiffies(PMP_START_BIND_TIMEOUT_MS);
	while (!apple_pmp_t6030_bound(rep) && time_before(jiffies, deadline))
		msleep(50);
	if (!apple_pmp_t6030_bound(rep)) {
		dev_err(rep->dev, "PMP driver did not bind within %d ms; display power not acknowledged\n",
			PMP_START_BIND_TIMEOUT_MS);
		apple_pmp_t6030_finish(rep, -ETIMEDOUT);
		return;
	}
	msleep(PMP_START_QUIET_MS);

	deadline = jiffies + msecs_to_jiffies(PMP_START_READY_TIMEOUT_MS);
	do {
		status = readq(rep->base + rep->offsets->status);
		if (status == PMP_REPORT_READY)
			break;
		msleep(50);
	} while (time_before(jiffies, deadline));
	if (status == PMP_REPORT_READY) {
		deadline = jiffies + msecs_to_jiffies(PMP_START_ACK_TIMEOUT_MS);
		do {
			ack = readq(rep->base + rep->offsets->actual);
			if ((ack & rep->startup_ack) == rep->startup_ack)
				break;
			msleep(20);
		} while (time_before(jiffies, deadline));
	}
	request = readq(rep->base + rep->offsets->tgt_read);

	if (status != PMP_REPORT_READY) {
		dev_err(rep->dev, "PMP started but not ready: PTD status %#llx request %#llx\n",
			status, request);
		ret = -ETIMEDOUT;
	} else if ((ack & rep->startup_ack) != rep->startup_ack) {
		dev_err(rep->dev, "PMP ready but did not acknowledge %#llx: request %#llx ack %#llx\n",
			rep->startup_ack, request, ack);
		ret = -EIO;
	} else {
		dev_info(rep->dev, "PMP ready: PTD status %#llx request %#llx ack %#llx\n",
			 status, request, ack);
		ret = 0;
		apple_pmp_t6030_temps_register(rep);
	}
	apple_pmp_t6030_finish(rep, ret);
}

/* Queues the start once both the requests are seeded and a consumer asked. */
static void apple_pmp_t6030_request_start(struct apple_pmp_report *rep)
{
	if (READ_ONCE(rep->seeded) && atomic_read(&rep->start_requested) &&
	    !atomic_xchg(&rep->start_queued, 1))
		queue_work(system_unbound_wq, &rep->start_work);
}

/*
 * apple_pmp_report_wait_ready() - wait until a PMP report entry is acknowledged
 * @entry: report entry node whose request must be acknowledged
 * @timeout: in jiffies
 *
 * For a report driver that starts the PMP itself: the first call starts it.
 * Returns 0 once the PMP runs and has acknowledged @entry, -EPROBE_DEFER
 * while the report driver has not probed yet, or the reason the PMP was not
 * started or is not ready.
 */
int apple_pmp_report_wait_ready(struct device_node *entry, unsigned long timeout)
{
	struct device_node *parent = of_get_parent(entry);
	struct platform_device *pdev;
	struct apple_pmp_report *rep;
	u64 status, ack;
	u32 id;
	int ret;

	if (!parent || of_property_read_u32(entry, "reg", &id) || id > 63) {
		of_node_put(parent);
		return -EINVAL;
	}
	pdev = of_find_device_by_node(parent);
	of_node_put(parent);
	if (!pdev)
		return -EPROBE_DEFER;

	rep = platform_get_drvdata(pdev);
	if (!rep) {
		put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}
	if (!rep->offsets->starts_pmp) {
		put_device(&pdev->dev);
		return -EINVAL;
	}

	atomic_set(&rep->start_requested, 1);
	/* Pairs with the barrier after seeding: one side sees the other's flag. */
	smp_mb__after_atomic();
	apple_pmp_t6030_request_start(rep);
	if (!wait_for_completion_timeout(&rep->started, timeout)) {
		ret = -ETIMEDOUT;
	} else if (rep->start_result) {
		ret = rep->start_result;
	} else if (!(rep->ack & BIT_ULL(id))) {
		ret = -EINVAL;
	} else {
		status = readq(rep->base + rep->offsets->status);
		ack = readq(rep->base + rep->offsets->actual);
		ret = status == PMP_REPORT_READY && (ack & BIT_ULL(id)) ? 0 : -EIO;
	}

	put_device(&pdev->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmp_report_wait_ready);

static int apple_pmp_report_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct apple_pmp_report *rep;
	u64 request;
	int ret;

	rep = devm_kzalloc(dev, sizeof(*rep), GFP_KERNEL);
	if (!rep)
		return -ENOMEM;

	rep->dev = dev;
	spin_lock_init(&rep->lock);
	rep->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rep->base))
		return PTR_ERR(rep->base);
	rep->offsets = of_device_get_match_data(dev);
	if (rep->offsets->starts_pmp) {
		init_completion(&rep->started);
		INIT_WORK(&rep->start_work, apple_pmp_t6030_start);
		ret = apple_pmp_t6030_check(rep);
		if (ret == -EPROBE_DEFER) {
			of_node_put(rep->pmp);
			return ret;
		}
		if (ret) {
			/* Stay bound, so consumers learn why and do not wait. */
			of_node_put(rep->pmp);
			rep->pmp = NULL;
			dev_set_drvdata(dev, rep);
			apple_pmp_t6030_finish(rep, ret);
			return 0;
		}
	}
	dev_set_drvdata(dev, rep);
	if (of_device_is_compatible(np, "apple,t8132-pmp-v2-report")) {
		ret = apple_pmp_temp_register(rep);
		if (ret)
			return ret;
	}
	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret && !rep->offsets->starts_pmp)
		return dev_err_probe(dev, ret, "failed to create child devices\n");

	if (rep->offsets->starts_pmp) {
		/* 4: the always-on entries have seeded their requests */
		request = readq(rep->base + rep->offsets->tgt_read);
		if (ret || request != rep->seed) {
			dev_err(dev, "T6030 PMP not started: request read back %#llx, expected %#llx (%d)\n",
				request, rep->seed, ret);
			apple_pmp_t6030_finish(rep, -EIO);
			return 0;
		}
		dev_info(dev, "request %#llx set; the PMP starts when a consumer asks for it\n",
			 request);
		WRITE_ONCE(rep->seeded, true);
		/* Pairs with the barrier in apple_pmp_report_wait_ready(). */
		smp_mb();
		apple_pmp_t6030_request_start(rep);
	}

	return 0;
}

static const struct apple_pmp_report_offsets apple_pmp_offsets_t600x = {
	.tgt_read = 0xf80,
	.tgt_write = 0x107c0,
	.actual = 0x1000,
	.status = 0x10,
};

static const struct apple_pmp_report_offsets apple_pmp_offsets_t602x = {
	.tgt_read = 0x2000,
	.tgt_write = 0x11000,
	.actual = 0x2080,
	.status = 0x10,
};

static const struct apple_pmp_report_offsets apple_pmp_offsets_t8112 = {
	.tgt_read = 0xa00,
	.tgt_write = 0x10500,
	.actual = 0xa40,
	.status = 0x10,
};

static const struct apple_pmp_report_offsets apple_pmp_offsets_t8132 = {
	.tgt_read = 0x1880,
	.tgt_write = 0x10c40,
	.actual = 0x1900,
	.status = 0x10,
};

static const struct apple_pmp_report_offsets apple_pmp_offsets_t6030 = {
	.tgt_read = 0x1180,
	.tgt_write = 0x108c0,
	.actual = 0x11c0,
	.status = 0x10,
	.starts_pmp = true,
};

static const struct of_device_id apple_pmp_report_of_match[] = {
	{ .compatible = "apple,t6000-pmp-v2-report", .data = &apple_pmp_offsets_t600x },
	{ .compatible = "apple,t6020-pmp-v2-report", .data = &apple_pmp_offsets_t602x },
	{ .compatible = "apple,t8112-pmp-v2-report", .data = &apple_pmp_offsets_t8112 },
	{ .compatible = "apple,t8132-pmp-v2-report", .data = &apple_pmp_offsets_t8132 },
	{ .compatible = "apple,t6030-pmp-v2-report", .data = &apple_pmp_offsets_t6030 },
	{}
};

static struct platform_driver apple_pmp_report_driver = {
	.probe = apple_pmp_report_probe,
	.driver = {
		.name = "apple-pmp-report",
		.of_match_table = apple_pmp_report_of_match,
	},
};

struct apple_pmp_report_entry {
	struct device *dev;
	struct generic_pm_domain genpd;
	u32 id;
	/* The PMP does not acknowledge this request. */
	bool no_ack;
};

#define genpd_to_apple_pmp_report_entry(_genpd) \
	container_of(_genpd, struct apple_pmp_report_entry, genpd)

static int apple_pmp_report_set_state(struct generic_pm_domain *genpd, bool enable)
{
	struct apple_pmp_report_entry *ent = genpd_to_apple_pmp_report_entry(genpd);
	struct apple_pmp_report *rep = dev_get_drvdata(ent->dev->parent);
	u64 bit_val = BIT_ULL(ent->id);
	u64 val;
	unsigned long flags;

	spin_lock_irqsave(&rep->lock, flags);
	val = readq(rep->base + rep->offsets->tgt_read);
	val &= ~bit_val;
	if (enable)
		val |= bit_val;
	writeq(val, rep->base + rep->offsets->tgt_write);
	spin_unlock_irqrestore(&rep->lock, flags);
	val = readq(rep->base + rep->offsets->status);
	if ((val & PMP_REPORT_READY) == 0 || ent->no_ack)
		return 0;
	return readq_poll_timeout_atomic(
		rep->base + rep->offsets->actual,
		val,
		!!(val & bit_val) == !!enable,
		100,
		50000);
}

static int apple_pmp_report_entry_power_on(struct generic_pm_domain *genpd)
{
	return apple_pmp_report_set_state(genpd, true);
}

static int apple_pmp_report_entry_power_off(struct generic_pm_domain *genpd)
{
	return apple_pmp_report_set_state(genpd, false);
}

static int apple_pmp_report_entry_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct apple_pmp_report_entry *ent;
	int ret;
	const char *name;
	struct of_phandle_iterator it;

	ent = devm_kzalloc(dev, sizeof(*ent), GFP_KERNEL);
	if (!ent)
		return -ENOMEM;

	ent->dev = dev;

	ret = of_property_read_u32(node, "reg", &ent->id);
	if (ret)
		return dev_err_probe(dev, ret, "missing reg property\n");

	ret = of_property_read_string(node, "label", &name);
	if (ret < 0)
		return dev_err_probe(dev, ret, "missing label property\n");

	ent->no_ack = of_property_read_bool(node, "apple,no-ack");
	if (of_property_read_bool(node, "apple,always-on")) {
		ent->genpd.flags |= GENPD_FLAG_ACTIVE_WAKEUP;
		apple_pmp_report_set_state(&ent->genpd, true);
	}

	ent->genpd.name = name;
	ent->genpd.power_on = apple_pmp_report_entry_power_on;
	ent->genpd.power_off = apple_pmp_report_entry_power_off;

	ret = pm_genpd_init(&ent->genpd, NULL, true);
	if (ret)
		return dev_err_probe(dev, ret, "pm_genpd_init failed\n");

	ret = of_genpd_add_provider_simple(node, &ent->genpd);
	if (ret)
		return dev_err_probe(dev, ret, "of_genpd_add_provider_simple failed\n");

	of_for_each_phandle(&it, ret, node, "power-domains", "#power-domain-cells", -1) {
		struct of_phandle_args parent, child;

		parent.np = it.node;
		parent.args_count = of_phandle_iterator_args(&it, parent.args, MAX_PHANDLE_ARGS);
		child.np = node;
		child.args_count = 0;
		ret = of_genpd_add_subdomain(&parent, &child);

		if (ret == -EPROBE_DEFER) {
			of_node_put(parent.np);
			goto err_remove;
		} else if (ret < 0) {
			dev_err(dev, "failed to add to parent domain: %d (%s -> %s)\n",
				ret, it.node->name, node->name);
			of_node_put(parent.np);
			goto err_remove;
		}
	}

	pm_genpd_remove_device(dev);

	return 0;
err_remove:
	of_genpd_del_provider(node);
	pm_genpd_remove(&ent->genpd);
	return ret;
}

static const struct of_device_id apple_pmp_report_entry_of_match[] = {
	{ .compatible = "apple,t6000-pmp-v2-report-entry" },
	{}
};

static struct platform_driver apple_pmp_report_entry_driver = {
	.probe = apple_pmp_report_entry_probe,
	.driver = {
		.name = "apple-pmp-report-entry",
		.of_match_table = apple_pmp_report_entry_of_match,
	},
};

MODULE_DEVICE_TABLE(of, apple_pmp_report_of_match);
MODULE_DEVICE_TABLE(of, apple_pmp_report_entry_of_match);

static int __init apple_pmp_report_init(void)
{
	platform_driver_register(&apple_pmp_report_entry_driver);
	platform_driver_register(&apple_pmp_report_driver);
	return 0;
}

static void __exit apple_pmp_report_exit(void)
{
	platform_driver_unregister(&apple_pmp_report_entry_driver);
	platform_driver_unregister(&apple_pmp_report_driver);
}

module_init(apple_pmp_report_init);
module_exit(apple_pmp_report_exit);

MODULE_DESCRIPTION("PMP power state reporting driver for Apple SoCs");
MODULE_LICENSE("Dual MIT/GPL");

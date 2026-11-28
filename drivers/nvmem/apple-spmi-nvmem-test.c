// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by apple-spmi-nvmem.c: the provider as the driver registers it, on a
 * register map in memory. Userspace cannot write the PMIC's registers through
 * the whole-device sysfs file, whatever force_ro says, while the kernel still
 * writes the cells its consumers declare.
 */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/string.h>

#include "internals.h"

struct apple_spmi_nvmem_test {
	u8 regs[0x10000];
};

static int apple_spmi_nvmem_test_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct apple_spmi_nvmem_test *t = context;

	*val = t->regs[reg];
	return 0;
}

static int apple_spmi_nvmem_test_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct apple_spmi_nvmem_test *t = context;

	t->regs[reg] = val;
	return 0;
}

/* The driver's register layout (apple_spmi_regmap_config), on the array above */
static const struct regmap_config apple_spmi_nvmem_test_regmap_config = {
	.reg_bits	= 16,
	.val_bits	= 8,
	.max_register	= 0xffff,
	.reg_read	= apple_spmi_nvmem_test_reg_read,
	.reg_write	= apple_spmi_nvmem_test_reg_write,
};

static struct nvmem_device *apple_spmi_nvmem_test_register(struct kunit *test,
							   struct apple_spmi_nvmem_test **regs)
{
	struct apple_spmi_nvmem_test *t;
	struct nvmem_device *nvmem;
	struct regmap *regmap;
	struct device *dev;

	dev = kunit_device_register(test, "apple-spmi-nvmem-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);

	regmap = devm_regmap_init(dev, NULL, t, &apple_spmi_nvmem_test_regmap_config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, regmap);

	nvmem = apple_spmi_nvmem_register(dev, regmap);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, nvmem);
	*regs = t;
	return nvmem;
}

/*
 * The whole-device file: root reads it, nobody writes it, not with read_only
 * cleared either, and force_ro is shown but cannot be stored.
 */
static void apple_spmi_nvmem_sysfs_read_only_test(struct kunit *test)
{
	u8 offset[6] = { 1, 2, 3, 4, 5, 6 }, out[6] = {};
	const struct attribute_group *group;
	const struct bin_attribute *raw;
	struct device_attribute *force_ro;
	struct apple_spmi_nvmem_test *t;
	struct nvmem_device *nvmem;
	struct kobject *kobj;
	int i;

	nvmem = apple_spmi_nvmem_test_register(test, &t);
	kobj = &nvmem->dev.kobj;
	KUNIT_ASSERT_NOT_NULL(test, nvmem->dev.groups);
	group = nvmem->dev.groups[0];
	raw = group->bin_attrs[0];
	KUNIT_ASSERT_STREQ(test, raw->attr.name, "nvmem");

	KUNIT_EXPECT_EQ(test, group->is_bin_visible(kobj, raw, 0), (umode_t)0400);

	t->regs[0x2100] = 0x5a;
	KUNIT_EXPECT_EQ(test, raw->write(NULL, kobj, raw, (char *)offset, 0x2100, sizeof(offset)),
			(ssize_t)-EPERM);
	KUNIT_EXPECT_EQ(test, t->regs[0x2100], 0x5a);

	for (i = 0; group->attrs[i]; i++)
		if (!strcmp(group->attrs[i]->name, "force_ro"))
			break;
	KUNIT_ASSERT_NOT_NULL(test, group->attrs[i]);
	KUNIT_EXPECT_EQ(test, group->is_visible(kobj, group->attrs[i], i), (umode_t)0444);
	force_ro = container_of(group->attrs[i], struct device_attribute, attr);
	KUNIT_EXPECT_EQ(test, force_ro->store(&nvmem->dev, force_ro, "0", 1), (ssize_t)-EPERM);

	/* As if read_only were cleared, by force_ro or otherwise */
	nvmem->read_only = false;
	KUNIT_EXPECT_EQ(test, group->is_bin_visible(kobj, raw, 0), (umode_t)0400);
	KUNIT_EXPECT_EQ(test, raw->write(NULL, kobj, raw, (char *)offset, 0x2100, sizeof(offset)),
			(ssize_t)-EPERM);
	KUNIT_EXPECT_EQ(test, t->regs[0x2100], 0x5a);

	KUNIT_EXPECT_EQ(test, raw->read(NULL, kobj, raw, (char *)out, 0x2100, sizeof(out)),
			(ssize_t)sizeof(out));
	KUNIT_EXPECT_EQ(test, out[0], 0x5a);
}

/*
 * The kernel's consumers still write their cells: a byte cell (the RTC offset)
 * and a bit-field cell sharing its byte (the panic count above the boot error
 * count).
 */
static void apple_spmi_nvmem_cell_write_test(struct kunit *test)
{
	struct nvmem_cell_info rtc_offset = {
		.name = "rtc-offset", .offset = 0x2100, .bytes = 6,
	};
	struct nvmem_cell_info panic_count = {
		.name = "panic-count", .offset = 0xf802, .bytes = 1, .bit_offset = 4, .nbits = 4,
	};
	u8 offset[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	struct apple_spmi_nvmem_test *t;
	struct nvmem_device *nvmem;
	u8 count = 3;

	nvmem = apple_spmi_nvmem_test_register(test, &t);

	KUNIT_EXPECT_EQ(test, nvmem_device_cell_write(nvmem, &rtc_offset, offset), 6);
	KUNIT_EXPECT_MEMEQ(test, &t->regs[0x2100], offset, sizeof(offset));

	t->regs[0xf802] = 0x05;
	KUNIT_EXPECT_EQ(test, nvmem_device_cell_write(nvmem, &panic_count, &count), 1);
	KUNIT_EXPECT_EQ(test, t->regs[0xf802], 0x35);
}

static struct kunit_case apple_spmi_nvmem_test_cases[] = {
	KUNIT_CASE(apple_spmi_nvmem_sysfs_read_only_test),
	KUNIT_CASE(apple_spmi_nvmem_cell_write_test),
	{}
};

static struct kunit_suite apple_spmi_nvmem_test_suite = {
	.name = "apple-spmi-nvmem",
	.test_cases = apple_spmi_nvmem_test_cases,
};

kunit_test_suite(apple_spmi_nvmem_test_suite);

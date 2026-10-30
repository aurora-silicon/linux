// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "ane_fw_platform.h"

static void ane_preload_ranges_test(struct kunit *test)
{
	const struct ane_fw_platform soc = {
		.preload_dva = { 0x10000000000, 0x100000c0000 },
		.preload_size = { 0xc0000, 0x2ac000 },
	};
	u64 phys[] = { 0x100000000, 0x200000000 };
	u64 size[] = { 0xc0000, 0x2ac000 };
	u64 dva[] = { 0x10000000000, 0x100000c0000 };

	KUNIT_EXPECT_TRUE(test, ane_fw_preload_ranges_valid(&soc, phys, size, dva));
	/* Physical placement may change independently on every boot. */
	phys[0] = 0x300000000;
	KUNIT_EXPECT_TRUE(test, ane_fw_preload_ranges_valid(&soc, phys, size, dva));
	phys[0] = phys[1] + SZ_16K;
	KUNIT_EXPECT_FALSE(test, ane_fw_preload_ranges_valid(&soc, phys, size, dva));
	phys[0] = phys[1] - size[0];
	KUNIT_EXPECT_TRUE(test, ane_fw_preload_ranges_valid(&soc, phys, size, dva));
	phys[0]++;
	KUNIT_EXPECT_FALSE(test, ane_fw_preload_ranges_valid(&soc, phys, size, dva));
	phys[0] = 0;
	KUNIT_EXPECT_FALSE(test, ane_fw_preload_ranges_valid(&soc, phys, size, dva));
	phys[0] = U64_MAX - SZ_16K + 1;
	KUNIT_EXPECT_FALSE(test, ane_fw_preload_ranges_valid(&soc, phys, size, dva));
	phys[0] = 0x100000000;
	size[1] -= SZ_16K;
	KUNIT_EXPECT_FALSE(test, ane_fw_preload_ranges_valid(&soc, phys, size, dva));
	size[1] = 0x2ac000;
	dva[0] = 0x100000c0000;
	dva[1] = 0x10000000000;
	KUNIT_EXPECT_FALSE(test, ane_fw_preload_ranges_valid(&soc, phys, size, dva));
}

static struct kunit_case ane_fw_platform_cases[] = {
	KUNIT_CASE(ane_preload_ranges_test),
	{}
};

static struct kunit_suite ane_fw_platform_suite = {
	.name = "ane-fw-platform",
	.test_cases = ane_fw_platform_cases,
};

kunit_test_suite(ane_fw_platform_suite);

MODULE_DESCRIPTION("KUnit tests for ANE firmware preload range validation");
MODULE_LICENSE("Dual MIT/GPL");

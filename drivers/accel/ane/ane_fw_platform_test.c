// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/platform_device.h>

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

static void ane_preload_physical_order_test(struct kunit *test)
{
	const struct ane_fw_platform platform = {
		.preload_dva = { SZ_1M, SZ_1M + SZ_16K },
		.preload_size = { SZ_16K, SZ_32K },
	};
	const u64 *size = platform.preload_size;
	const u64 *dva = platform.preload_dva;
	u64 phys[] = { SZ_2M + SZ_32K, SZ_2M };

	/* DATA can precede TEXT and touch its boundary in physical memory. */
	KUNIT_EXPECT_TRUE(test, ane_fw_preload_ranges_valid(&platform, phys, size, dva));
	phys[0] -= SZ_16K;
	KUNIT_EXPECT_FALSE(test, ane_fw_preload_ranges_valid(&platform, phys, size, dva));

	/* DATA's end must not wrap, even if its base is valid and aligned. */
	phys[0] = SZ_2M;
	phys[1] = U64_MAX - SZ_16K + 1;
	KUNIT_EXPECT_FALSE(test, ane_fw_preload_ranges_valid(&platform, phys, size, dva));
}

/*
 * H16 and H17 devices (any firmware platform) take the backend's sleep path,
 * never the rtclient one; only the H13/H14 rtclient (no platform) does.
 */
static void ane_platform_sleep_path_test(struct kunit *test)
{
	static const struct ane_fw_platform h16 = { .soc = 0x6041 }, h17 = { .soc = 0x8140 };

	KUNIT_EXPECT_EQ(test, ane_fw_sleep_path(&h16), ANE_SLEEP_FW_BACKEND);
	KUNIT_EXPECT_EQ(test, ane_fw_sleep_path(&h17), ANE_SLEEP_FW_BACKEND);
	KUNIT_EXPECT_EQ(test, ane_fw_sleep_path(NULL), ANE_SLEEP_RTCLIENT);
}

/*
 * Only the staged backend's platform (H17, Exclave-owned firmware) binds
 * the device for the backend; H16 keeps its own probe; the rtclient has no
 * platform. A bound H17 device still takes the backend's sleep path.
 */
static void ane_platform_staged_bind_test(struct kunit *test)
{
	static const struct ane_fw_platform h16 = { .soc = 0x6041, .owner = ANE_FW_OWNER_AP },
		h17 = { .soc = 0x8140, .owner = ANE_FW_OWNER_EXCLAVE };

	KUNIT_EXPECT_TRUE(test, ane_fw_platform_staged(&h17));
	KUNIT_EXPECT_FALSE(test, ane_fw_platform_staged(&h16));
	KUNIT_EXPECT_FALSE(test, ane_fw_platform_staged(NULL));
	KUNIT_EXPECT_EQ(test, ane_fw_sleep_path(&h17), ANE_SLEEP_FW_BACKEND);
}

#if IS_BUILTIN(CONFIG_DRM_ACCEL_ANE) && IS_BUILTIN(CONFIG_DRM_ACCEL_ANE_FW_KUNIT_TEST)
/* M3 (T8122) H15: three reg windows, five DAPF records at DART0 + 0x4000, cold sleep. */
static void ane_t8122_platform_test(struct kunit *test)
{
	struct platform_device *pdev = platform_device_alloc("ane-t8122", -1);
	u64 phys[] = { 0x100000000, 0x200000000 };

	KUNIT_ASSERT_NOT_NULL(test, pdev);
	KUNIT_EXPECT_EQ(test, ane_t8122_platform.soc, 0x8122U);
	KUNIT_EXPECT_EQ(test, ane_t8122_platform.chip_family, 15U);
	KUNIT_EXPECT_EQ(test, ane_t8122_platform.ane_type, 0xb0U);
	KUNIT_EXPECT_EQ(test, ane_t8122_platform.hwx_subtype, 6U);
	KUNIT_EXPECT_EQ(test, ane_t8122_platform.reg_count, 3U);
	KUNIT_EXPECT_STREQ(test, ane_t8122_platform.reg_names[2], "aux-0");
	KUNIT_EXPECT_FALSE(test, ane_t8122_platform.has_xl);
	KUNIT_EXPECT_EQ(test, ane_t8122_platform.dapf_dart, 0U);
	KUNIT_EXPECT_EQ(test, ane_t8122_platform.dapf_offset, 0x4000U);
	KUNIT_EXPECT_EQ(test, ane_t8122_platform.dapf_count, 5);
	KUNIT_EXPECT_EQ(test, ane_t8122_platform.power_words[0], 0x438U);
	KUNIT_EXPECT_TRUE(test, ane_t8122_platform.sleep_cold);
	KUNIT_EXPECT_TRUE(test, ane_fw_preload_ranges_valid(&ane_t8122_platform, phys,
							    ane_t8122_platform.preload_size,
							    ane_t8122_platform.preload_dva));
	KUNIT_EXPECT_EQ(test, ane_fw_platform_preflight(pdev, &ane_t8122_platform), -EINVAL);
	platform_device_put(pdev);
}

/* One engine: no aux-3, five DAPF records; a node without DT is refused. */
static void ane_t8132_platform_test(struct kunit *test)
{
	struct platform_device *pdev = platform_device_alloc("ane-t8132", -1);

	KUNIT_ASSERT_NOT_NULL(test, pdev);
	KUNIT_EXPECT_EQ(test, ane_t8132_platform.reg_count, 6U);
	KUNIT_EXPECT_STREQ(test, ane_t8132_platform.reg_names[5], "aux-4");
	KUNIT_EXPECT_FALSE(test, ane_t8132_platform.has_xl);
	KUNIT_EXPECT_TRUE(test, ane_h16_platform.has_xl);
	KUNIT_EXPECT_EQ(test, ane_t8132_platform.dapf_count, 5);
	KUNIT_EXPECT_EQ(test, ane_h16_platform.dapf_count, 4);
	KUNIT_EXPECT_EQ(test, ane_t8132_platform.power_words[0], 0x570U);
	/* No DT, resources or preload: refused before any of them is used. */
	KUNIT_EXPECT_EQ(test, ane_fw_platform_preflight(pdev, &ane_t8132_platform),
			-EINVAL);
	platform_device_put(pdev);
}

/*
 * T8132: the 26.6.2 and 27.2 builds each admit their own preload layout and
 * not the other's, and keep the M4 engine description.
 */
static void ane_t8132_builds_test(struct kunit *test)
{
	const struct ane_fw_platform *b25 = &ane_t8132_platform, *b27 = b25->next_build;
	const struct ane_fw_platform *builds[] = { b25, b27 };
	u64 phys[] = { 0x100000000, 0x200000000 };
	unsigned int i, j;

	KUNIT_ASSERT_NOT_NULL(test, b27);
	KUNIT_EXPECT_NULL(test, b27->next_build);
	KUNIT_EXPECT_EQ(test, b27->preload_size[0], 0xbc000ULL);
	KUNIT_EXPECT_EQ(test, b27->preload_size[1], 0x2b0000ULL);
	KUNIT_EXPECT_EQ(test, b27->preload_dva[1], 0x100000bc000ULL);
	for (i = 0; i < ARRAY_SIZE(builds); i++) {
		const struct ane_fw_platform *b = builds[i];

		KUNIT_EXPECT_EQ(test, b->soc, 0x8132U);
		KUNIT_EXPECT_EQ(test, b->owner, ANE_FW_OWNER_AP);
		KUNIT_EXPECT_EQ(test, b->dapf_count, 5);
		KUNIT_EXPECT_EQ(test, b->power_words[0], 0x570U);
		KUNIT_EXPECT_NOT_NULL(test, b->build_name);
		KUNIT_EXPECT_GT(test, b->h16_profile_count, 0U);
		for (j = 0; j < ARRAY_SIZE(builds); j++)
			KUNIT_EXPECT_EQ(test, ane_fw_preload_ranges_valid(builds[j], phys,
									  b->preload_size,
									  b->preload_dva),
					i == j);
	}
	/* Without a hand-off in DT the SoC's own data stays selected. */
	KUNIT_EXPECT_PTR_EQ(test, ane_fw_platform_build(NULL, b27), b27);
}

/*
 * T6050: the 27.0 and 27.2 builds each admit their own preload layout and
 * not the other's, and each has a code pin over two ordered TEXT sections.
 */
static void ane_t6050_builds_test(struct kunit *test)
{
	const struct ane_fw_platform *b27_0 = &ane_t6050_platform, *b27_2 = b27_0->next_build;
	const struct ane_fw_platform *builds[] = { b27_0, b27_2 };
	u64 phys[] = { 0x100000000, 0x200000000 };
	unsigned int i, j;

	KUNIT_ASSERT_NOT_NULL(test, b27_2);
	KUNIT_EXPECT_NULL(test, b27_2->next_build);
	KUNIT_EXPECT_NOT_NULL(test, b27_0->text_pin);
	for (i = 0; i < ARRAY_SIZE(builds); i++) {
		const struct ane_fw_platform *b = builds[i];

		KUNIT_EXPECT_EQ(test, b->soc, 0x6050U);
		KUNIT_EXPECT_NOT_NULL(test, b->text_code_pin);
		KUNIT_EXPECT_NOT_NULL(test, b->build_name);
		KUNIT_EXPECT_LT(test, (u64)b->text_env[0].offset + b->text_env[0].size,
				(u64)b->text_env[1].offset + 1);
		KUNIT_EXPECT_LE(test, (u64)b->text_env[1].offset + b->text_env[1].size,
				b->preload_size[0]);
		for (j = 0; j < ARRAY_SIZE(builds); j++)
			KUNIT_EXPECT_EQ(test, ane_fw_preload_ranges_valid(builds[j], phys,
									  b->preload_size,
									  b->preload_dva),
					i == j);
	}
}

/*
 * T8142: both firmware builds carry the AXI2AF bridge register updates with
 * aligned offsets inside the engine window, disjoint clear/set masks, and
 * non-empty transitions; other H17 platforms stay untouched.
 */
static void ane_t8142_bridge_ops_test(struct kunit *test)
{
	const struct ane_fw_platform *b27_0 = &ane_t8142_platform, *b27_2 = b27_0->next_build;
	const struct ane_fw_platform *builds[] = { b27_0, b27_2 };
	unsigned int i, j;

	KUNIT_ASSERT_NOT_NULL(test, b27_2);
	KUNIT_EXPECT_NULL(test, b27_2->next_build);
	KUNIT_EXPECT_EQ(test, ane_h17_platform.bridge_op_count, 0U);
	KUNIT_EXPECT_EQ(test, ane_t6050_platform.bridge_op_count, 0U);
	for (i = 0; i < ARRAY_SIZE(builds); i++) {
		const struct ane_fw_platform *b = builds[i];

		KUNIT_EXPECT_EQ(test, b->soc, 0x8142U);
		KUNIT_ASSERT_NOT_NULL(test, b->bridge_ops);
		KUNIT_ASSERT_EQ(test, b->bridge_op_count, 25U);
		for (j = 0; j < b->bridge_op_count; j++) {
			const struct ane_fw_bridge_op *op = &b->bridge_ops[j];

			KUNIT_EXPECT_TRUE(test, IS_ALIGNED(op->offset, 4));
			KUNIT_EXPECT_LE(test, op->offset + 4, b->reg_sizes[0]);
			KUNIT_EXPECT_EQ(test, op->clear & op->set, 0U);
			KUNIT_EXPECT_NE(test, op->clear | op->set, 0U);
		}
	}
	/* Preserves untouched reset bits and stays idempotent once applied. */
	KUNIT_EXPECT_EQ(test, ane_fw_bridge_op_apply(0x10000008U, &b27_0->bridge_ops[12]),
			0x90000010U);
	KUNIT_EXPECT_EQ(test, ane_fw_bridge_op_apply(0x90000010U, &b27_0->bridge_ops[12]),
			0x90000010U);
	KUNIT_EXPECT_EQ(test, ane_fw_bridge_op_apply(0x000413b6U, &b27_0->bridge_ops[16]),
			0x000413b1U);
	KUNIT_EXPECT_EQ(test, ane_fw_bridge_op_apply(0xffff0030U, &b27_0->bridge_ops[22]),
			0x00110071U);
	KUNIT_EXPECT_EQ(test, ane_fw_bridge_op_apply(0x00000001U, &b27_0->bridge_ops[24]),
			0x00000000U);
}
#endif

static struct kunit_case ane_fw_platform_cases[] = {
#if IS_BUILTIN(CONFIG_DRM_ACCEL_ANE) && IS_BUILTIN(CONFIG_DRM_ACCEL_ANE_FW_KUNIT_TEST)
	KUNIT_CASE(ane_t8122_platform_test),
	KUNIT_CASE(ane_t8132_platform_test),
	KUNIT_CASE(ane_t8132_builds_test),
	KUNIT_CASE(ane_t6050_builds_test),
	KUNIT_CASE(ane_t8142_bridge_ops_test),
#endif
	KUNIT_CASE(ane_platform_staged_bind_test),
	KUNIT_CASE(ane_platform_sleep_path_test),
	KUNIT_CASE(ane_preload_ranges_test),
	KUNIT_CASE(ane_preload_physical_order_test),
	{}
};

static struct kunit_suite ane_fw_platform_suite = {
	.name = "ane-fw-platform",
	.test_cases = ane_fw_platform_cases,
};

kunit_test_suite(ane_fw_platform_suite);

MODULE_DESCRIPTION("KUnit tests for ANE firmware preload range validation");
MODULE_LICENSE("Dual MIT/GPL");

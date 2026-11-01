// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "ave-recovery.h"

static void ave_recovery_limit_test(struct kunit *test)
{
	struct ave_recovery_history h = {};

	for (unsigned int i = 0; i < AVE_RECOVERY_ATTEMPTS; i++)
		KUNIT_EXPECT_TRUE(test, ave_recovery_allow(&h, i));
	KUNIT_EXPECT_FALSE(test, ave_recovery_allow(&h, 3));
	KUNIT_EXPECT_FALSE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW - 1));
	KUNIT_EXPECT_TRUE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW));
	KUNIT_EXPECT_FALSE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW));
	KUNIT_EXPECT_TRUE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW + 1));
	KUNIT_EXPECT_TRUE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW + 2));
	KUNIT_EXPECT_FALSE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW + 3));
}

static void ave_recovery_sliding_test(struct kunit *test)
{
	struct ave_recovery_history h = {};

	KUNIT_ASSERT_TRUE(test, ave_recovery_allow(&h, 0));
	KUNIT_ASSERT_TRUE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW / 2));
	KUNIT_ASSERT_TRUE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW - 1));
	KUNIT_EXPECT_TRUE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW));
	KUNIT_EXPECT_FALSE(test, ave_recovery_allow(&h, AVE_RECOVERY_WINDOW + 1));
	KUNIT_EXPECT_TRUE(test, ave_recovery_allow(&h, 2 * AVE_RECOVERY_WINDOW));
	KUNIT_EXPECT_TRUE(test, ave_recovery_allow(&h, 2 * AVE_RECOVERY_WINDOW));
	KUNIT_EXPECT_TRUE(test, ave_recovery_allow(&h, 2 * AVE_RECOVERY_WINDOW));
	KUNIT_EXPECT_FALSE(test, ave_recovery_allow(&h, 2 * AVE_RECOVERY_WINDOW));
}

static void ave_recovery_wrap_test(struct kunit *test)
{
	struct ave_recovery_history h = {};
	unsigned long start = ~0UL - AVE_RECOVERY_WINDOW / 2;

	for (unsigned int i = 0; i < AVE_RECOVERY_ATTEMPTS; i++)
		KUNIT_ASSERT_TRUE(test, ave_recovery_allow(&h, start));
	KUNIT_EXPECT_FALSE(test, ave_recovery_allow(&h, start + AVE_RECOVERY_WINDOW - 1));
	KUNIT_EXPECT_TRUE(test, ave_recovery_allow(&h, start + AVE_RECOVERY_WINDOW));
}

static void ave_recovery_domain_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, ave_domain_type_safe(IOMMU_DOMAIN_DMA));
	KUNIT_EXPECT_FALSE(test, ave_domain_type_safe(IOMMU_DOMAIN_DMA_FQ));
	KUNIT_EXPECT_FALSE(test, ave_domain_type_safe(IOMMU_DOMAIN_IDENTITY));
	KUNIT_EXPECT_FALSE(test, ave_domain_type_safe(IOMMU_DOMAIN_BLOCKED));
	KUNIT_EXPECT_FALSE(test, ave_domain_type_safe(IOMMU_DOMAIN_UNMANAGED));
	KUNIT_EXPECT_FALSE(test, ave_domain_type_safe(0));
}

static struct kunit_case ave_recovery_cases[] = {
	KUNIT_CASE(ave_recovery_domain_test),
	KUNIT_CASE(ave_recovery_limit_test),
	KUNIT_CASE(ave_recovery_sliding_test),
	KUNIT_CASE(ave_recovery_wrap_test),
	{}
};

static struct kunit_suite ave_recovery_suite = {
	.name = "apple-ave-recovery",
	.test_cases = ave_recovery_cases,
};

kunit_test_suite(ave_recovery_suite);

MODULE_LICENSE("GPL");

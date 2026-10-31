// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "pmp-ane.h"

struct ane_test {
	struct apple_pmp_ane ane;
	int requests;
	int releases;
	int request_error;
	int release_error;
	struct kunit *test;
};

static int ane_test_request(void *ctx, bool enable)
{
	struct ane_test *t = ctx;

	if (enable) {
		KUNIT_EXPECT_EQ(t->test, readl(t->ane.dvfs + PMP_ANE_DVFS_ON), 1U);
		KUNIT_EXPECT_EQ(t->test, readl(t->ane.dvfs + PMP_ANE_DVFS_CMD), 0x80000000U);
		t->requests++;
		t->ane.owned = true;
		return t->request_error;
	}
	t->releases++;
	if (t->release_error)
		return t->release_error;
	t->ane.owned = false;
	return 0;
}

static struct ane_test *ane_test_init(struct kunit *test)
{
	struct ane_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	if (!t)
		return NULL;
	t->test = test;
	t->ane.dvfs = (void __iomem *)kunit_kzalloc(test, 0x4000, GFP_KERNEL);
	t->ane.report = (void __iomem *)kunit_kzalloc(test, 0x20000, GFP_KERNEL);
	if (!t->ane.dvfs || !t->ane.report)
		return NULL;
	return t;
}

static void ane_level_cycle(struct kunit *test)
{
	struct ane_test *t = ane_test_init(test);

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_EQ(test, apple_pmp_ane_set(&t->ane, 5, ane_test_request, t), 0);
	KUNIT_EXPECT_EQ(test, t->ane.level, 5U);
	KUNIT_EXPECT_EQ(test, readl(t->ane.dvfs + PMP_ANE_DVFS_CMD), 0x80000005U);
	KUNIT_EXPECT_EQ(test, readq(t->ane.report + PMP_ANE_LEVEL_WRITE), PMP_ANE_LEVEL_TAG | 5);
	KUNIT_EXPECT_EQ(test, apple_pmp_ane_set(&t->ane, 5, ane_test_request, t), 0);
	KUNIT_EXPECT_EQ(test, t->requests, 1);
	KUNIT_ASSERT_EQ(test, apple_pmp_ane_set(&t->ane, 0, ane_test_request, t), 0);
	KUNIT_EXPECT_EQ(test, t->ane.level, 0U);
	KUNIT_EXPECT_FALSE(test, t->ane.owned);
	KUNIT_EXPECT_EQ(test, t->releases, 1);
	KUNIT_EXPECT_EQ(test, readl(t->ane.dvfs + PMP_ANE_DVFS_ON), 0U);
	KUNIT_EXPECT_EQ(test, readl(t->ane.dvfs + PMP_ANE_DVFS_CMD), 0x80000000U);
	KUNIT_EXPECT_EQ(test, readq(t->ane.report + PMP_ANE_LEVEL_WRITE), PMP_ANE_LEVEL_TAG);
	KUNIT_EXPECT_EQ(test, apple_pmp_ane_set(&t->ane, 0, ane_test_request, t), 0);
	KUNIT_EXPECT_EQ(test, t->releases, 1);
}

static void ane_level_busy(struct kunit *test)
{
	struct ane_test *t = ane_test_init(test);

	KUNIT_ASSERT_NOT_NULL(test, t);
	writel(1, t->ane.dvfs + PMP_ANE_DVFS_ON);
	KUNIT_EXPECT_EQ(test, apple_pmp_ane_set(&t->ane, 5, ane_test_request, t), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->requests, 0);
	writel(0, t->ane.dvfs + PMP_ANE_DVFS_ON);
	writeq(0xdead, t->ane.report + PMP_ANE_LEVEL_READ);
	KUNIT_EXPECT_EQ(test, apple_pmp_ane_set(&t->ane, 5, ane_test_request, t), -EBUSY);
	KUNIT_EXPECT_EQ(test, readl(t->ane.dvfs + PMP_ANE_DVFS_ON), 0U);
	KUNIT_EXPECT_EQ(test, t->requests, 0);
	KUNIT_EXPECT_EQ(test, apple_pmp_ane_set(&t->ane, 6, ane_test_request, t), -EINVAL);
}

static void ane_level_timeout(struct kunit *test)
{
	struct ane_test *t = ane_test_init(test);
	int i;

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->request_error = -ETIMEDOUT;
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ(test,
				apple_pmp_ane_set(&t->ane, 5, ane_test_request, t),
				-ETIMEDOUT);
		KUNIT_EXPECT_EQ(test, t->ane.level, 0U);
		KUNIT_EXPECT_EQ(test, readl(t->ane.dvfs + PMP_ANE_DVFS_ON), 0U);
		KUNIT_EXPECT_FALSE(test, t->ane.owned);
	}
	KUNIT_EXPECT_EQ(test, t->requests, 3);
	KUNIT_EXPECT_EQ(test, t->releases, 3);
}

static void ane_level_release_timeout(struct kunit *test)
{
	struct ane_test *t = ane_test_init(test);

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_EQ(test, apple_pmp_ane_set(&t->ane, 5, ane_test_request, t), 0);
	t->release_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, apple_pmp_ane_set(&t->ane, 0, ane_test_request, t), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->ane.level, 0U);
	KUNIT_EXPECT_EQ(test, t->ane.owned, true);
	KUNIT_EXPECT_EQ(test, readl(t->ane.dvfs + PMP_ANE_DVFS_ON), 0U);
	KUNIT_EXPECT_EQ(test, apple_pmp_ane_set(&t->ane, 5, ane_test_request, t), -EBUSY);
	t->release_error = 0;
	KUNIT_EXPECT_EQ(test, apple_pmp_ane_set(&t->ane, 0, ane_test_request, t), 0);
	KUNIT_EXPECT_FALSE(test, t->ane.owned);
	KUNIT_EXPECT_EQ(test, t->releases, 2);
}

static struct kunit_case ane_level_cases[] = {
	KUNIT_CASE(ane_level_cycle),
	KUNIT_CASE(ane_level_busy),
	KUNIT_CASE(ane_level_timeout),
	KUNIT_CASE(ane_level_release_timeout),
	{}
};

static struct kunit_suite ane_level_suite = {
	.name = "apple-pmp-ane",
	.test_cases = ane_level_cases,
};

kunit_test_suite(ane_level_suite);

MODULE_LICENSE("Dual MIT/GPL");

// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the buffer size checks of DRM_IOCTL_ANE_SUBMIT and
 * DRM_IOCTL_ANE_BO_INIT, and the fault-drain scratch page.
 */

#include <kunit/test.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/sizes.h>

#include "ane.h"

struct ane_submit_size_case {
	const char *desc;
	u64 tsk_size;
	u32 td_count;
	u32 td_size;
	size_t cmd_size;
	size_t btsp_size;
	int ret;
};

static const struct ane_submit_size_case ane_submit_size_cases[] = {
	{
		/* libane: the program in handles[0], one tile of bootstrap */
		.desc = "100 descriptors of 0x274 bytes",
		.tsk_size = 100 * 0x274, .td_count = 100, .td_size = 0x274,
		.cmd_size = SZ_64K + SZ_16K, .btsp_size = SZ_16K,
	},
	{
		.desc = "descriptors fill the task region",
		.tsk_size = 0x1000, .td_count = 0x400, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K,
	},
	{
		.desc = "one descriptor past the task region",
		.tsk_size = 0x1000, .td_count = 0x401, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K, .ret = -EINVAL,
	},
	{
		.desc = "65535 descriptors in one page",
		.tsk_size = 0x100, .td_count = 0xffff, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K, .ret = -EINVAL,
	},
	{
		.desc = "task region one byte short of the buffer",
		.tsk_size = SZ_16K - 1, .td_count = 1, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K,
	},
	{
		.desc = "task region as large as the buffer",
		.tsk_size = SZ_16K, .td_count = 1, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K, .ret = -EINVAL,
	},
	{
		.desc = "first descriptor fills the bootstrap buffer",
		.tsk_size = SZ_16K, .td_count = 1, .td_size = SZ_16K,
		.cmd_size = SZ_32K, .btsp_size = SZ_16K,
	},
	{
		.desc = "first descriptor one word past the bootstrap buffer",
		.tsk_size = SZ_16K + 4, .td_count = 1, .td_size = SZ_16K + 4,
		.cmd_size = SZ_32K, .btsp_size = SZ_16K, .ret = -EINVAL,
	},
	{
		.desc = "largest descriptor array below 4 GiB",
		.tsk_size = 0x3fffULL * 0x40000, .td_count = 0x3fff,
		.td_size = 0x40000, .cmd_size = SZ_4G, .btsp_size = 0x40000,
	},
	{
		.desc = "descriptor array of 4 GiB",
		.tsk_size = SZ_4G, .td_count = 0x4000, .td_size = 0x40000,
		.cmd_size = SZ_4G + SZ_16K, .btsp_size = 0x40000,
		.ret = -EINVAL,
	},
};

KUNIT_ARRAY_PARAM_DESC(ane_submit_size, ane_submit_size_cases, desc);

static void ane_submit_sizes_test(struct kunit *test)
{
	const struct ane_submit_size_case *c = test->param_value;
	struct drm_ane_submit args = {
		.tsk_size = c->tsk_size,
		.td_count = c->td_count,
		.td_size = c->td_size,
	};

	KUNIT_EXPECT_EQ(test, ane_submit_check_sizes(&args, c->cmd_size,
						     c->btsp_size), c->ret);
}

static struct kunit_case ane_submit_test_cases[] = {
	KUNIT_CASE_PARAM(ane_submit_sizes_test, ane_submit_size_gen_params),
	{}
};

static struct kunit_suite ane_submit_test_suite = {
	.name = "ane-submit",
	.test_cases = ane_submit_test_cases,
};

struct ane_recoverable_case {
	const char *desc;
	bool tm_retention;
	unsigned int unproven;
	int err;
	u32 finished;
	bool recoverable;
};

static const struct ane_recoverable_case ane_recoverable_cases[] = {
	{ "T8103 timeout without events", false, 0, -ETIMEDOUT, 0, true },
	{ "T8103 timeout without events after a recovery", false, 1,
	  -ETIMEDOUT, 0, false },
	{ "T600x timeout without events", true, 0, -ETIMEDOUT, 0, true },
	{ "T600x timeout after a recovery", true, 1, -ETIMEDOUT, 0, true },
	{ "T600x timeout, last recovery allowed", true,
	  ANE_TM_RECOVERIES_MAX - 1, -ETIMEDOUT, 0, true },
	{ "T600x timeout, too many recoveries in a row", true,
	  ANE_TM_RECOVERIES_MAX, -ETIMEDOUT, 0, false },
	{ "T600x timeout with one finish line", true, 0, -ETIMEDOUT, 1, true },
	{ "T600x task error with one finish line", true, 0, -EIO, 1, true },
	{ "T600x bad event count", true, 0, -EIO, 0, true },
	{ "T600x task error after a recovery", true, 1, -EIO, 2, true },
};

KUNIT_ARRAY_PARAM_DESC(ane_recoverable, ane_recoverable_cases, desc);

static void ane_recoverable_test(struct kunit *test)
{
	const struct ane_recoverable_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test, ane_tm_failure_recoverable(c->tm_retention,
							 c->unproven, c->err,
							 c->finished),
			c->recoverable);
}

static struct kunit_case ane_recovery_test_cases[] = {
	KUNIT_CASE_PARAM(ane_recoverable_test, ane_recoverable_gen_params),
	{}
};

static struct kunit_suite ane_recovery_test_suite = {
	.name = "ane-tm-recovery",
	.test_cases = ane_recovery_test_cases,
};

/* BO_INIT: the reservation and the map must agree for any page count. */
static void ane_bo_iova_size_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, ane_bo_iova_size(1, 14), (u64)SZ_16K);
	KUNIT_EXPECT_EQ(test, ane_bo_iova_size(0x3ffff, 14),
			(u64)SZ_4G - SZ_16K);
	KUNIT_EXPECT_EQ(test, ane_bo_iova_size(0x40000, 14), (u64)SZ_4G);
	KUNIT_EXPECT_EQ(test, ane_bo_iova_size(0x40001, 14),
			(u64)SZ_4G + SZ_16K);
	KUNIT_EXPECT_EQ(test, ane_bo_iova_size(U32_MAX, 12),
			(u64)U32_MAX << 12);
}

struct ane_bo_size_case {
	const char *desc;
	u64 size;
	u64 span;
	int ret;
};

/* The H13 allocator range: one page above zero up to 4 GiB. */
#define ANE_TEST_SPAN ((u64)SZ_4G - PAGE_SIZE)

static const struct ane_bo_size_case ane_bo_size_cases[] = {
	{ "zero bytes", 0, ANE_TEST_SPAN, -EINVAL },
	{ "one byte", 1, ANE_TEST_SPAN, 0 },
	{ "the whole range", ANE_TEST_SPAN, ANE_TEST_SPAN, 0 },
	{ "one byte past the range", ANE_TEST_SPAN + 1, ANE_TEST_SPAN, -E2BIG },
	{ "4 GiB", SZ_4G, ANE_TEST_SPAN, -E2BIG },
	{ "4 GiB and one page", (u64)SZ_4G + PAGE_SIZE, ANE_TEST_SPAN, -E2BIG },
	{ "page count past u32", (u64)PAGE_SIZE << 32, ANE_TEST_SPAN, -E2BIG },
	{ "largest u64", U64_MAX, ANE_TEST_SPAN, -E2BIG },
	{ "rounds up past a smaller range", SZ_16K + 1, SZ_16K + 1, -E2BIG },
	/* round_up() would wrap to 0 below these, for any range. */
	{ "largest u64, whole u64 range", U64_MAX, U64_MAX, -E2BIG },
	{ "one byte past the last whole page, whole u64 range",
	  U64_MAX - (PAGE_SIZE - 1) + 1, U64_MAX, -E2BIG },
	{ "last whole page, whole u64 range", U64_MAX - (PAGE_SIZE - 1),
	  U64_MAX, 0 },
};

KUNIT_ARRAY_PARAM_DESC(ane_bo_size, ane_bo_size_cases, desc);

static void ane_bo_check_size_test(struct kunit *test)
{
	const struct ane_bo_size_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test, ane_bo_check_size(c->size, c->span), c->ret);
}

/*
 * The fault-drain scratch page is readable by the job that faulted.
 * Dirty some pages and give them back to the allocator first, so an
 * unzeroed allocation would very likely hand one of them out again
 * (likely, not certain: the allocator may pick other pages).
 */
#define ANE_TEST_SCRATCH_PAGES 16

static void ane_dart_scratch_page_zeroed_test(struct kunit *test)
{
	struct page *pages[ANE_TEST_SCRATCH_PAGES];
	int i;

	/* The allocator zeroes every page itself: nothing to tell apart. */
	if (want_init_on_alloc(GFP_KERNEL))
		kunit_skip(test, "init_on_alloc zeroes every page");

	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
		memset(page_address(pages[i]), 0xa5, PAGE_SIZE);
	}
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);

	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = ane_dart_alloc_scratch_page();
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}
	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		KUNIT_EXPECT_TRUE_MSG(test,
				      !memchr_inv(page_address(pages[i]), 0,
						  PAGE_SIZE),
				      "scratch page %d is not zeroed", i);
		__free_page(pages[i]);
	}
}

static struct kunit_case ane_bo_test_cases[] = {
	KUNIT_CASE(ane_bo_iova_size_test),
	KUNIT_CASE_PARAM(ane_bo_check_size_test, ane_bo_size_gen_params),
	KUNIT_CASE(ane_dart_scratch_page_zeroed_test),
	{}
};

static struct kunit_suite ane_bo_test_suite = {
	.name = "ane-bo",
	.test_cases = ane_bo_test_cases,
};

kunit_test_suites(&ane_submit_test_suite, &ane_recovery_test_suite,
		  &ane_bo_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple Neural Engine SUBMIT checks");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

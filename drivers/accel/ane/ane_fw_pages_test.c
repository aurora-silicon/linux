// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>

#include "ane_fw_pages.h"

/* The largest block the rest fills, never above the last one that worked. */
static void ane_pages_order_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, ane_fw_pages_order(13, 7, 0), 3U);
	KUNIT_EXPECT_EQ(test, ane_fw_pages_order(5, 7, 0), 2U);
	KUNIT_EXPECT_EQ(test, ane_fw_pages_order(1, 7, 0), 0U);
	KUNIT_EXPECT_EQ(test, ane_fw_pages_order(4096, 7, 0), 7U);
	/* After a failure at order 3, a 13-page rest takes order 2 blocks. */
	KUNIT_EXPECT_EQ(test, ane_fw_pages_order(13, 2, 0), 2U);
	/* Never below the DART granule. */
	KUNIT_EXPECT_EQ(test, ane_fw_pages_order(4, 7, 2), 2U);
	KUNIT_EXPECT_EQ(test, ane_fw_pages_order(12, 1, 2), 2U);
}

/*
 * Every DART granule is physically contiguous and aligned, every page is an
 * order-0 page with its own reference, and the memory is zeroed.
 */
static void ane_pages_check(struct kunit *test, struct page **pages, size_t size,
			    unsigned int min_order)
{
	unsigned long block = 1UL << min_order, count = size >> PAGE_SHIFT;

	for (unsigned long i = 0; i < count; i++) {
		unsigned long first = page_to_pfn(pages[round_down(i, block)]);

		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
		KUNIT_EXPECT_TRUE(test, IS_ALIGNED(first, block));
		KUNIT_EXPECT_EQ(test, page_to_pfn(pages[i]), first + i % block);
		KUNIT_EXPECT_FALSE(test, PageCompound(pages[i]));
		KUNIT_EXPECT_EQ(test, page_count(pages[i]), 1);
		KUNIT_EXPECT_TRUE(test, mem_is_zero(page_address(pages[i]), PAGE_SIZE));
	}
}

/* A size that is no power of two takes blocks of several orders. */
static void ane_pages_mixed_test(struct kunit *test)
{
	unsigned int min_order = get_order(SZ_16K);
	size_t size = 13 * SZ_16K;
	struct page **pages;
	u8 *cpu;

	pages = ane_fw_pages_alloc(size, min_order, get_order(SZ_2M),
				   GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pages));
	ane_pages_check(test, pages, size, min_order);
	/* The CPU alias the backend makes: one contiguous view of every page. */
	cpu = vmap(pages, size >> PAGE_SHIFT, VM_MAP, PAGE_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, cpu);
	for (unsigned long i = 0; i < size >> PAGE_SHIFT; i++)
		cpu[i * PAGE_SIZE + 7] = i + 1;
	for (unsigned long i = 0; i < size >> PAGE_SHIFT; i++) {
		KUNIT_EXPECT_EQ(test, ((u8 *)page_address(pages[i]))[7], (u8)(i + 1));
		KUNIT_EXPECT_PTR_EQ(test, vmalloc_to_page(cpu + i * PAGE_SIZE), pages[i]);
	}
	vunmap(cpu);
	ane_fw_pages_free(pages, size);
}

/* The minimum block order follows the IOMMU domain's smallest page size. */
static void ane_pages_granule_test(struct kunit *test)
{
	struct page **pages;
	int min_order;

	KUNIT_EXPECT_EQ(test, ane_fw_pages_granule_order(0), -EINVAL);
	KUNIT_EXPECT_EQ(test, ane_fw_pages_granule_order(SZ_4K), get_order(SZ_4K));
	KUNIT_EXPECT_EQ(test, ane_fw_pages_granule_order(SZ_16K), get_order(SZ_16K));
	KUNIT_EXPECT_EQ(test, ane_fw_pages_granule_order(SZ_64K), get_order(SZ_64K));
	KUNIT_EXPECT_EQ(test, ane_fw_pages_granule_order(SZ_16K | SZ_2M), get_order(SZ_16K));
	KUNIT_EXPECT_EQ(test, ane_fw_pages_granule_order(SZ_4K | SZ_16K), get_order(SZ_4K));

	min_order = ane_fw_pages_granule_order(SZ_64K | SZ_2M);
	KUNIT_ASSERT_GE(test, min_order, 0);
	/* The helper enforces zeroing even when __GFP_ZERO is omitted. */
	pages = ane_fw_pages_alloc(3 * SZ_64K, min_order, get_order(SZ_2M),
				   GFP_KERNEL_ACCOUNT);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pages));
	ane_pages_check(test, pages, 3 * SZ_64K, min_order);
	ane_fw_pages_free(pages, 3 * SZ_64K);
}

/* A larger buffer, as a program region: granule blocks at the very least. */
static void ane_pages_large_test(struct kunit *test)
{
	unsigned int min_order = get_order(SZ_16K);
	size_t size = SZ_8M + 3 * SZ_16K;
	struct page **pages;

	pages = ane_fw_pages_alloc(size, min_order, get_order(SZ_2M),
				   GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pages));
	ane_pages_check(test, pages, size, min_order);
	ane_fw_pages_free(pages, size);
}

/* Orders out of the allocator's range are clamped, not tried (or warned about). */
static void ane_pages_clamp_test(struct kunit *test)
{
	unsigned int min_order = get_order(SZ_16K);
	struct page **pages;

	pages = ane_fw_pages_alloc(2 * SZ_16K, min_order, MAX_PAGE_ORDER + 4,
				   GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pages));
	ane_pages_check(test, pages, 2 * SZ_16K, min_order);
	ane_fw_pages_free(pages, 2 * SZ_16K);
	/* A maximum below the granule still allocates granules. */
	pages = ane_fw_pages_alloc(3 * SZ_16K, min_order, 0,
				   GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pages));
	ane_pages_check(test, pages, 3 * SZ_16K, min_order);
	ane_fw_pages_free(pages, 3 * SZ_16K);
}

static void ane_pages_invalid_test(struct kunit *test)
{
	unsigned int min_order = get_order(SZ_16K);

	KUNIT_EXPECT_EQ(test, PTR_ERR(ane_fw_pages_alloc(0, min_order, 7, GFP_KERNEL)),
			-EINVAL);
	/* Only whole DART granules. */
	KUNIT_EXPECT_EQ(test, PTR_ERR(ane_fw_pages_alloc(5 * PAGE_SIZE, 2, 7, GFP_KERNEL)),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(ane_fw_pages_alloc(SZ_16K, MAX_PAGE_ORDER + 1,
							 MAX_PAGE_ORDER + 1, GFP_KERNEL)),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(ane_fw_pages_alloc(SZ_16K, min_order, min_order,
							 GFP_KERNEL | __GFP_COMP)),
			-EINVAL);
	/* Freeing nothing is fine. */
	ane_fw_pages_free(NULL, SZ_16K);
}

static struct kunit_case ane_pages_cases[] = {
	KUNIT_CASE(ane_pages_order_test),
	KUNIT_CASE(ane_pages_granule_test),
	KUNIT_CASE(ane_pages_mixed_test),
	KUNIT_CASE(ane_pages_large_test),
	KUNIT_CASE(ane_pages_clamp_test),
	KUNIT_CASE(ane_pages_invalid_test),
	{}
};

static struct kunit_suite ane_pages_suite = {
	.name = "ane-fw-pages",
	.test_cases = ane_pages_cases,
};

kunit_test_suite(ane_pages_suite);

MODULE_DESCRIPTION("KUnit tests for the ANE discontiguous page backing");
MODULE_LICENSE("Dual MIT/GPL");

// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/page_ref.h>

#include "isp-buffer.h"
#include "isp-buffer-pages.h"

static struct isp_buflist_buffer preview_descriptor(void)
{
	/* Recorded completed1080p geometry/tag; addresses are synthetic leases. */
	return (struct isp_buflist_buffer) {
		.iovas = { 0x10008000000ULL, 0x100083f8000ULL },
		.flags = { 0x40000000, 0x40000000 },
		.num_planes = 2, .pool_type = 9, .tag = 0x1010,
	};
}

static void isp_buffer_retirement_test(struct kunit *test)
{
	struct isp_buffer_lease lease = {};
	struct isp_buflist_buffer descriptor = preview_descriptor();
	struct isp_buflist_buffer report = descriptor;

	report.iovas[0] = (u32)report.iovas[0];
	report.iovas[1] = (u32)report.iovas[1];
	KUNIT_ASSERT_TRUE(test, isp_buffer_submit(&lease, &descriptor));
	KUNIT_ASSERT_TRUE(test, isp_buffer_return(&lease, &report));
	KUNIT_EXPECT_FALSE(test, isp_buffer_reclaimable(&lease, false));
	KUNIT_EXPECT_FALSE(test, isp_buffer_submit(&lease, &descriptor));
	KUNIT_ASSERT_TRUE(test, isp_buffer_acknowledge(&lease));
	KUNIT_EXPECT_TRUE(test, isp_buffer_reclaimable(&lease, false));
	descriptor.tag++;
	KUNIT_ASSERT_TRUE(test, isp_buffer_submit(&lease, &descriptor));
	KUNIT_EXPECT_FALSE(test, isp_buffer_return(&lease, &report));
	KUNIT_EXPECT_EQ(test, lease.owner, ISP_BUFFER_SUBMITTED);
}

static void isp_buffer_full_record_test(struct kunit *test)
{
	struct isp_buflist_buffer descriptor = preview_descriptor();
	struct isp_buffer_lease lease = {};
	struct isp_buflist_buffer report;

	KUNIT_ASSERT_TRUE(test, isp_buffer_submit(&lease, &descriptor));
	for (unsigned int field = 0; field < 4; field++) {
		report = descriptor;
		switch (field) {
		case 0:
			report.iovas[1] += 0x4000;
			break;
		case 1:
			report.num_planes = 1;
			break;
		case 2:
			report.pool_type = 1;
			break;
		case 3:
			report.tag--;
			break;
		}
		KUNIT_EXPECT_FALSE(test, isp_buffer_return(&lease, &report));
		KUNIT_EXPECT_EQ(test, lease.owner, ISP_BUFFER_SUBMITTED);
	}
}

static void isp_buffer_alias_ambiguity_test(struct kunit *test)
{
	struct isp_buflist_buffer descriptor = preview_descriptor();
	struct isp_buffer_lease a = {}, b = {};
	struct isp_buffer_match match = {};

	KUNIT_ASSERT_TRUE(test, isp_buffer_submit(&a, &descriptor));
	descriptor.iovas[0] += 1ULL << 32;
	descriptor.iovas[1] += 1ULL << 32;
	KUNIT_ASSERT_TRUE(test, isp_buffer_submit(&b, &descriptor));
	isp_buffer_match_candidate(&match, &a, &descriptor);
	isp_buffer_match_candidate(&match, &b, &descriptor);
	KUNIT_EXPECT_PTR_EQ(test, isp_buffer_unique_match(&match), NULL);
}

static void isp_buffer_uncertainty_test(struct kunit *test)
{
	struct isp_buflist_buffer descriptor = preview_descriptor();
	struct isp_buffer_lease lease = {};

	KUNIT_ASSERT_TRUE(test, isp_buffer_submit(&lease, &descriptor));
	/* Losing the submission ACK does not return the lease to the host. */
	KUNIT_EXPECT_FALSE(test, isp_buffer_reclaimable(&lease, false));
	KUNIT_EXPECT_FALSE(test, isp_buffer_submit(&lease, &descriptor));
	KUNIT_EXPECT_TRUE(test, isp_buffer_reclaimable(&lease, true));
}

static void isp_buffer_legacy_tag_test(struct kunit *test)
{
	/* Legacy m1n1 native request example: metadata pool0/tag0x13f. */
	struct isp_buflist_buffer descriptor = {
		.iovas = { 0x056a8000 }, .flags = { 0x40000000 },
		.num_planes = 1, .pool_type = 0, .tag = 0x13f,
	};
	struct isp_buffer_lease lease = {};

	KUNIT_ASSERT_TRUE(test, isp_buffer_submit(&lease, &descriptor));
	KUNIT_EXPECT_TRUE(test, isp_buffer_return(&lease, &descriptor));
	KUNIT_EXPECT_TRUE(test, isp_buffer_acknowledge(&lease));
}

static void isp_buffer_modern_tag_test(struct kunit *test)
{
	struct isp_buflist_buffer descriptor = preview_descriptor();
	struct isp_buflist_buffer report;
	struct isp_buffer_lease lease = {};

	descriptor.tag = 1;
	descriptor.pad = 1; /* Modern u64 tag 0x100000001 at wire offset 0x38. */
	KUNIT_ASSERT_TRUE(test, isp_buffer_submit_tag(&lease, &descriptor, true));
	report = descriptor;
	report.pad = 2;
	KUNIT_EXPECT_FALSE(test, isp_buffer_return(&lease, &report));
	KUNIT_EXPECT_EQ(test, lease.owner, ISP_BUFFER_SUBMITTED);
	report.pad = 1;
	KUNIT_EXPECT_TRUE(test, isp_buffer_return(&lease, &report));
	KUNIT_EXPECT_TRUE(test, isp_buffer_acknowledge(&lease));
}

static void isp_buffer_pages_retained_test(struct kunit *test)
{
	struct isp_pinned_pages pins = {};
	struct sg_table table;
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, sg_alloc_table(&table, 1, GFP_KERNEL), 0);
	sg_set_page(table.sgl, page, PAGE_SIZE, 0);
	KUNIT_ASSERT_EQ(test, isp_pin_buffer_pages(&table, &pins), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), 2);
	/* Model vb2's normal allocation release while DMA is unretired. */
	put_page(page);
	sg_free_table(&table);
	KUNIT_EXPECT_EQ(test, page_ref_count(pins.pages[0]), 1);
	/* A witness reference lets us inspect the release without touching a
	 * freed page. Reclaim occurs only after the independent pins are put.
	 */
	get_page(page);
	isp_unpin_buffer_pages(&pins);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), 1);
	KUNIT_EXPECT_EQ(test, pins.count, 0);
	put_page(page);
}

static void isp_buffer_bad_sg_test(struct kunit *test)
{
	struct isp_pinned_pages pins = {};
	struct scatterlist sg;
	struct sg_table table = { .sgl = &sg, .orig_nents = 1 };
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, page);
	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, PAGE_SIZE - 1, 0);
	KUNIT_EXPECT_EQ(test, isp_pin_buffer_pages(&table, &pins), -EINVAL);
	KUNIT_EXPECT_EQ(test, page_ref_count(page), 1);
	KUNIT_EXPECT_PTR_EQ(test, pins.pages, NULL);
	put_page(page);
}

static struct kunit_case isp_buffer_cases[] = {
	KUNIT_CASE(isp_buffer_retirement_test),
	KUNIT_CASE(isp_buffer_full_record_test),
	KUNIT_CASE(isp_buffer_alias_ambiguity_test),
	KUNIT_CASE(isp_buffer_uncertainty_test),
	KUNIT_CASE(isp_buffer_legacy_tag_test),
	KUNIT_CASE(isp_buffer_modern_tag_test),
	KUNIT_CASE(isp_buffer_pages_retained_test),
	KUNIT_CASE(isp_buffer_bad_sg_test),
	{}
};

static struct kunit_suite isp_buffer_suite = {
	.name = "apple-isp-buffer-ownership",
	.test_cases = isp_buffer_cases,
};

kunit_test_suite(isp_buffer_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple ISP buffer ownership tests");

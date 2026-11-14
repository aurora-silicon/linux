// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <kunit/test.h>
#include <linux/module.h>

#include "iomfb.h"
#include "iomfb_v27_3_swap.h"

static void v27_3_tags_test(struct kunit *test)
{
	const struct dcp_method_entry *m = iomfb_v27_methods("27.3.0");
	const struct dcp_method_entry *old = iomfb_v27_methods("27.0.0");

	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_ASSERT_NOT_NULL(test, old);
	KUNIT_EXPECT_MEMEQ(test, m[dcpep_swap_start].tag, "A456", 4);
	KUNIT_EXPECT_MEMEQ(test, m[dcpep_swap_submit].tag, "A458", 4);
	KUNIT_EXPECT_MEMEQ(test, m[dcpep_set_display_device].tag, "A461", 4);
	KUNIT_EXPECT_MEMEQ(test, m[dcpep_pipe_set_476].tag, "A538", 4);
	KUNIT_EXPECT_MEMEQ(test, m[dcpep_pipe_query_478].tag, "A540", 4);
	/* the 27.0 table keeps its own IDs */
	KUNIT_EXPECT_MEMEQ(test, old[dcpep_swap_submit].tag, "A408", 4);
	/* nothing in the 27.3 table is left without an ID */
	KUNIT_EXPECT_NULL(test, iomfb_v27_missing_method(m, &(unsigned int){ 0 }));
}

static void v27_3_swap_request_test(struct kunit *test)
{
	u8 *src = kunit_kzalloc(test, DCP_V27_REQ_SIZE, GFP_KERNEL);
	u8 *dst = kunit_kzalloc(test, DCP_V27_3_REQ_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	src[DCP_V27_3_SWAP_ID_OFF] = 0x11;
	src[DCP_V27_SWAP_SIZE] = 0x22;
	src[DCP_V27_REQ_SIZE - 1] = 0x33;
	dcp_v27_3_swap_req(dst, src);
	KUNIT_EXPECT_EQ(test, dst[DCP_V27_3_SWAP_ID_OFF], 0);
	KUNIT_EXPECT_EQ(test, dst[DCP_V27_3_SWAP_ID_OFF + DCP_V27_3_SWAP_ID_SHIFT], 0x11);
	KUNIT_EXPECT_EQ(test, dst[DCP_V27_3_SWAP_SIZE], 0x22);
	KUNIT_EXPECT_EQ(test, dst[DCP_V27_3_REQ_SIZE - 1], 0x33);
}

static struct kunit_case v27_3_cases[] = {
	KUNIT_CASE(v27_3_tags_test),
	KUNIT_CASE(v27_3_swap_request_test),
	{}
};

static struct kunit_suite v27_3_suite = {
	.name = "apple-dcp-v27-3",
	.test_cases = v27_3_cases,
};

kunit_test_suite(v27_3_suite);

MODULE_LICENSE("Dual MIT/GPL");

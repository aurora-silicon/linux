// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple ProRes descriptor and table builders.
 *
 * Expected register words are the values the engine is known to run with
 * (m1n1 proxyclient experiments/prores.py, MIT).
 */
#include <kunit/test.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "apple-prores.h"

static void apr_test_qmat_regs(struct kunit *test)
{
	static const struct {
		enum apr_qtable table;
		bool chroma;
		unsigned int index;
		u32 value;
	} cases[] = {
		{ APR_QTABLE_XQ, false, 0, 0x802802 },
		{ APR_QTABLE_XQ, false, 15, 0x803802 },
		{ APR_QTABLE_XQ, false, 27, 0x804803 },
		{ APR_QTABLE_XQ, true, 23, 0x806805 },
		{ APR_QTABLE_XQ, true, 31, 0x807807 },
		{ APR_QTABLE_HQ, false, 0, 0x804804 },
		{ APR_QTABLE_HQ, true, 27, 0x807806 },
		{ APR_QTABLE_STANDARD, false, 2, 0x807806 },
		{ APR_QTABLE_STANDARD, false, 23, 0x80f80c },
		{ APR_QTABLE_STANDARD, true, 31, 0x815811 },
		{ APR_QTABLE_LT, false, 0, 0x805804 },
		{ APR_QTABLE_LT, false, 27, 0x82381c },
		{ APR_QTABLE_LT, true, 31, 0x829823 },
		{ APR_QTABLE_PROXY, false, 0, 0x807804 },
		{ APR_QTABLE_PROXY, false, 3, 0xfff83f },
		{ APR_QTABLE_PROXY, false, 6, 0x83f80e },
		{ APR_QTABLE_PROXY, true, 17, 0xfff80e },
		{ APR_QTABLE_PROXY, true, 24, 0xfff80d },
		{ APR_QTABLE_PROXY, true, 31, 0xffffff },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const u8 *qm = cases[i].chroma ? apr_qmat_chroma[cases[i].table] :
						 apr_qmat_luma[cases[i].table];

		KUNIT_EXPECT_EQ_MSG(test, apr_qmat_reg(qm, cases[i].index),
				    cases[i].value, "case %u", i);
	}
	KUNIT_EXPECT_EQ(test, apr_qmat_reg(apr_qmat_luma[0], 32), 0);
}

static void apr_test_dc_scale(struct kunit *test)
{
	static const struct { unsigned int index; u32 value; } cases[] = {
		{ 0, 0x401 }, { 1, 0x803 }, { 31, 0x803f }, { 62, 0xfc7d },
		{ 63, 0x1007f }, { 64, 0x11084 }, { 79, 0x200fc },
		{ 110, 0x3f1f4 }, { 111, 0x1fc },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ(test, apr_dc_scale_reg(cases[i].index), cases[i].value);
	KUNIT_EXPECT_EQ(test, apr_dc_scale_reg(APR_DC_SCALE_ENTRIES), 0);
}

static void apr_test_profiles(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, apr_profile_qtable(APR_PROFILE_PROXY), APR_QTABLE_PROXY);
	KUNIT_EXPECT_EQ(test, apr_profile_qtable(APR_PROFILE_LT), APR_QTABLE_LT);
	KUNIT_EXPECT_EQ(test, apr_profile_qtable(APR_PROFILE_STANDARD), APR_QTABLE_STANDARD);
	KUNIT_EXPECT_EQ(test, apr_profile_qtable(APR_PROFILE_HQ), APR_QTABLE_HQ);
	KUNIT_EXPECT_EQ(test, apr_profile_qtable(APR_PROFILE_4444), APR_QTABLE_HQ);
	KUNIT_EXPECT_EQ(test, apr_profile_qtable(APR_PROFILE_4444_XQ), APR_QTABLE_XQ);
	KUNIT_EXPECT_TRUE(test, apr_profile_is_444(APR_PROFILE_4444_XQ));
	KUNIT_EXPECT_FALSE(test, apr_profile_is_444(APR_PROFILE_HQ));
}

static struct apr_frame apr_test_frame(void)
{
	return (struct apr_frame){
		.width = 320, .height = 240,
		.input = APR_INPUT_422_16, .profile = APR_PROFILE_STANDARD,
		.luma_stride = 640, .chroma_stride = 640,
		.luma = 0x10000100000ULL, .chroma = 0x10000200000ULL,
		.output = 0x10000400000ULL, .output_size = SZ_1M,
		.primaries = 1, .transfer = 1, .matrix = 1,
	};
}

static void apr_test_desc_422(struct kunit *test)
{
	struct apr_frame f = apr_test_frame();
	u8 *d = kunit_kzalloc(test, APR_DESC_BYTES, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, d);
	KUNIT_ASSERT_EQ(test, apr_build_desc(&f, d), 0);
	/* Layout facts from the working T8140 job: flags, strides, header. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d), 0x363b);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x08), f.output);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x10), SZ_1M);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x18), 320);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x1a), 240);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x20), 10);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x22), 10);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x28), f.luma);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x38), f.chroma);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x48), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x58), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x70), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(d + 0x7c), 148);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(d + 0x84), 320);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(d + 0x86), 240);
	KUNIT_EXPECT_EQ(test, d[0x88], 0x80);
	KUNIT_EXPECT_EQ(test, d[0x89], 1);
	KUNIT_EXPECT_EQ(test, d[0x8b], 1);
	KUNIT_EXPECT_EQ(test, d[0x8f], 0x03);
	/* 20 macroblocks per row: two 8-wide slices and one 4-wide. */
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(d + 0x78), 45);
	KUNIT_EXPECT_EQ(test, d[0x7a], 0x30);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x110), 20);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x112), 15);
	KUNIT_EXPECT_EQ(test, d[0x90], 0);	/* matrices live in the engine */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x170), 0x23);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x174), 0);
}

static void apr_test_desc_444(struct kunit *test)
{
	struct apr_frame f = apr_test_frame();
	u8 *d = kunit_kzalloc(test, APR_DESC_BYTES, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, d);
	f.input = APR_INPUT_444_8;
	f.profile = APR_PROFILE_4444_XQ;
	f.luma_stride = 320;
	f.chroma_stride = 640;
	KUNIT_ASSERT_EQ(test, apr_build_desc(&f, d), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d), 0x363c);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x20), 5);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x22), 10);
	KUNIT_EXPECT_EQ(test, d[0x7f], 1);
	KUNIT_EXPECT_EQ(test, d[0x88], 0xc0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x170), 0x03);
}

static void apr_test_desc_rejects(struct kunit *test)
{
	u8 *d = kunit_kzalloc(test, APR_DESC_BYTES, GFP_KERNEL);
	struct apr_frame f;

	KUNIT_ASSERT_NOT_NULL(test, d);
	f = apr_test_frame();
	f.profile = APR_PROFILE_4444;		/* 4:2:2 input, 4:4:4 profile */
	KUNIT_EXPECT_EQ(test, apr_build_desc(&f, d), -EINVAL);
	f = apr_test_frame();
	f.luma_stride = 600;			/* not a multiple of 64 */
	KUNIT_EXPECT_EQ(test, apr_build_desc(&f, d), -EINVAL);
	f = apr_test_frame();
	f.chroma_stride = 576;			/* shorter than a row */
	KUNIT_EXPECT_EQ(test, apr_build_desc(&f, d), -EINVAL);
	f = apr_test_frame();
	f.width = 321;
	KUNIT_EXPECT_EQ(test, apr_build_desc(&f, d), -EINVAL);
	f = apr_test_frame();
	f.output = 0;
	KUNIT_EXPECT_EQ(test, apr_build_desc(&f, d), -EINVAL);
}

static void apr_test_slices(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, apr_slice_count(320, 240, 3), 45);
	/* 1920x1080: 120 MBs per row = 15 slices, 68 rows. */
	KUNIT_EXPECT_EQ(test, apr_slice_count(1920, 1080, 3), 15 * 68);
	/* 1366 wide: 86 MBs = 10 x 8 + 4 + 2 -> 12 slices per row. */
	KUNIT_EXPECT_EQ(test, apr_slice_count(1366, 16, 3), 12);
}

static void apr_test_output(struct kunit *test)
{
	u8 *out = kunit_kzalloc(test, SZ_4K, GFP_KERNEL);
	u32 bytes = 0;

	KUNIT_ASSERT_NOT_NULL(test, out);
	put_unaligned_be32(2000, out);
	memcpy(out + 4, "icpf", 4);
	KUNIT_EXPECT_EQ(test, apr_check_output(out, SZ_4K, &bytes), 0);
	KUNIT_EXPECT_EQ(test, bytes, 2000);
	KUNIT_EXPECT_EQ(test, apr_check_output(out, 1999, &bytes), -EIO);
	put_unaligned_be32(100, out);
	KUNIT_EXPECT_EQ(test, apr_check_output(out, SZ_4K, &bytes), -EIO);
	put_unaligned_be32(2000, out);
	out[4] = 'x';
	KUNIT_EXPECT_EQ(test, apr_check_output(out, SZ_4K, &bytes), -EIO);
}

static struct kunit_case apr_test_cases[] = {
	KUNIT_CASE(apr_test_qmat_regs),
	KUNIT_CASE(apr_test_dc_scale),
	KUNIT_CASE(apr_test_profiles),
	KUNIT_CASE(apr_test_desc_422),
	KUNIT_CASE(apr_test_desc_444),
	KUNIT_CASE(apr_test_desc_rejects),
	KUNIT_CASE(apr_test_slices),
	KUNIT_CASE(apr_test_output),
	{}
};

static struct kunit_suite apr_test_suite = {
	.name = "apple-prores",
	.test_cases = apr_test_cases,
};
kunit_test_suite(apr_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple ProRes encoder");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

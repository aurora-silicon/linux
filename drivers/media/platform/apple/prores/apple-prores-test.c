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
	KUNIT_EXPECT_EQ(test, d[0x7b], 2);	/* 422 starting quantiser */
	/* 320x240 422: 174762 and 196607 bytes over 300 macroblocks. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x110), 4640);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x112), 5224);
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
	KUNIT_EXPECT_EQ(test, apr_build_desc_v0(&f, d), -EINVAL);
	f = apr_test_frame();
	f.luma_stride = 600;			/* not a multiple of 64 */
	KUNIT_EXPECT_EQ(test, apr_build_desc(&f, d), -EINVAL);
	KUNIT_EXPECT_EQ(test, apr_build_desc_v0(&f, d), -EINVAL);
	f = apr_test_frame();
	f.chroma_stride = 576;			/* shorter than a row */
	KUNIT_EXPECT_EQ(test, apr_build_desc(&f, d), -EINVAL);
	KUNIT_EXPECT_EQ(test, apr_build_desc_v0(&f, d), -EINVAL);
	f = apr_test_frame();
	f.width = 321;
	KUNIT_EXPECT_EQ(test, apr_build_desc(&f, d), -EINVAL);
	KUNIT_EXPECT_EQ(test, apr_build_desc_v0(&f, d), -EINVAL);
	f = apr_test_frame();
	f.output = 0;
	KUNIT_EXPECT_EQ(test, apr_build_desc(&f, d), -EINVAL);
	KUNIT_EXPECT_EQ(test, apr_build_desc_v0(&f, d), -EINVAL);
}

/* Legacy words from MIT m1n1 f3ac615eb137 experiments/prores.py:607-644. */
static void apr_test_desc_v0(struct kunit *test)
{
	static const u32 legacy[] = {
		0xffffffff, 0x15100c0c, 0x80004020, 0x002c8080,
		0x00880080, 0x004e00c5, 0x009000d0, 0x00200122,
		0x00400200, 0x00400200, 0x00400200, 0x00400200,
		0x00400200, 0x00400200, 0x00400200, 0x00400200,
		0x00400200, 0x00400200, 0x00400200, 0x00400200,
		0x00400200, 0x00400200, 0x00400200, 0x00400200, 0x23,
	};
	struct apr_frame f = apr_test_frame();
	u8 *d = kunit_kmalloc(test, APR_DESC_BYTES + 1, GFP_KERNEL);
	unsigned int i;
	u32 budget;

	KUNIT_ASSERT_NOT_NULL(test, d);
	memset(d, 0xa5, APR_DESC_BYTES + 1);
	KUNIT_ASSERT_EQ(test, apr_build_desc_v0(&f, d), 0);
	KUNIT_EXPECT_EQ(test, d[APR_DESC_BYTES], 0xa5);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d), 0x363b);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 4), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 8), f.output);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x10), f.output_size);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x18), 320);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x1a), 240);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x20), 10);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x22), 10);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x28), f.luma);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x38), f.chroma);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(d + 0x58), 148);
	KUNIT_EXPECT_EQ(test, d[0x5b], 0);
	KUNIT_EXPECT_MEMEQ(test, d + 0x5c, "aurr", 4);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(d + 0x60), 320);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(d + 0x62), 240);
	KUNIT_EXPECT_EQ(test, d[0x64], 0x80);
	KUNIT_EXPECT_EQ(test, d[0x65], 0);
	KUNIT_EXPECT_EQ(test, d[0x66], f.primaries);
	KUNIT_EXPECT_EQ(test, d[0x67], f.transfer);
	KUNIT_EXPECT_EQ(test, d[0x68], f.matrix);
	KUNIT_EXPECT_EQ(test, d[0x69], 0);
	KUNIT_EXPECT_EQ(test, d[0x6a], 0);
	KUNIT_EXPECT_EQ(test, d[0x6b], 3);
	for (i = 0x6c; i < 0xee; i++)
		KUNIT_EXPECT_EQ_MSG(test, d[i], 0, "padding/slices %#x", i);
	KUNIT_EXPECT_EQ(test, d[0xee], 0x30);
	KUNIT_EXPECT_EQ(test, d[0xef], 2);
	/* Word 0 is the rate budget, checked against v1 below. */
	for (i = 1; i < ARRAY_SIZE(legacy); i++)
		KUNIT_EXPECT_EQ_MSG(test, get_unaligned_le32(d + 0xf0 + 4 * i),
				    legacy[i], "legacy word %u", i);
	budget = get_unaligned_le32(d + 0xf0);
	for (i = 0x154; i < APR_DESC_BYTES; i++)
		KUNIT_EXPECT_EQ_MSG(test, d[i], 0, "v0 trailing padding %#x", i);

	/* Reusing the buffer for v1 must clear the old frame header. */
	KUNIT_ASSERT_EQ(test, apr_build_desc(&f, d), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x58), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(d + 0x7c), 148);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x170), 0x23);
	/* v0 carries v1's target and maximum budget words. */
	KUNIT_EXPECT_EQ(test, budget, get_unaligned_le32(d + 0x110));
	KUNIT_EXPECT_NE(test, budget, 0xffffffff);
}

static void apr_test_desc_v0_profiles(struct kunit *test)
{
	struct apr_frame f = apr_test_frame();
	u8 *d = kunit_kmalloc(test, APR_DESC_BYTES, GFP_KERNEL);
	unsigned int profile;

	KUNIT_ASSERT_NOT_NULL(test, d);
	for (profile = 0; profile < APR_PROFILES; profile++) {
		bool is444 = apr_profile_is_444(profile);

		f.profile = profile;
		f.input = is444 ? APR_INPUT_444_8 : APR_INPUT_422_16;
		KUNIT_ASSERT_EQ(test, apr_build_desc_v0(&f, d), 0);
		KUNIT_EXPECT_EQ(test, d[0x5b], is444 ? 1 : 0);
		KUNIT_EXPECT_EQ(test, d[0x64], is444 ? 0xc0 : 0x80);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x150),
				apr_profile_qtable(profile) << 4 | 3);
		/* The v1 extension is never written into a v0 descriptor. */
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x170), 0);
	}
}

static void apr_test_budgets(struct kunit *test)
{
	/* Target, maximum and the Q3 per-macroblock words for 1080p and 2160p. */
	static const struct {
		enum apr_profile profile;
		u32 w, h, target, max;
		u16 q_target, q_max;
	} cases[] = {
		{ APR_PROFILE_PROXY, 1920, 1080, 189326, 212991, 176, 200 },
		{ APR_PROFILE_LT, 1920, 1080, 425984, 479232, 408, 456 },
		{ APR_PROFILE_STANDARD, 1920, 1080, 611669, 688127, 584, 664 },
		{ APR_PROFILE_HQ, 1920, 1080, 917504, 1032192, 888, 1000 },
		{ APR_PROFILE_4444, 1920, 1080, 1376256, 1548288, 1336, 1504 },
		{ APR_PROFILE_4444_XQ, 1920, 1080, 2064384, 2322432, 2008, 2264 },
		{ APR_PROFILE_PROXY, 3840, 2160, 757304, 851967, 176, 200 },
		{ APR_PROFILE_LT, 3840, 2160, 1711217, 1925119, 408, 464 },
		{ APR_PROFILE_STANDARD, 3840, 2160, 2457600, 2764800, 592, 672 },
		{ APR_PROFILE_HQ, 3840, 2160, 3684579, 4145151, 896, 1008 },
		{ APR_PROFILE_4444, 3840, 2160, 5526869, 6217727, 1352, 1520 },
		{ APR_PROFILE_4444_XQ, 3840, 2160, 8293944, 9330687, 2032, 2288 },
	};
	u8 *d = kunit_kzalloc(test, APR_DESC_BYTES, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, d);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct apr_frame f = apr_test_frame();
		bool is444 = apr_profile_is_444(cases[i].profile);
		u32 target, max;

		apr_frame_budget(cases[i].profile, cases[i].w, cases[i].h, &target, &max);
		KUNIT_EXPECT_EQ_MSG(test, target, cases[i].target, "case %u", i);
		KUNIT_EXPECT_EQ_MSG(test, max, cases[i].max, "case %u", i);
		f.profile = cases[i].profile;
		f.input = is444 ? APR_INPUT_444_8 : APR_INPUT_422_8;
		f.width = cases[i].w;
		f.height = cases[i].h;
		f.luma_stride = cases[i].w;
		f.chroma_stride = cases[i].w * (is444 ? 2 : 1);
		KUNIT_ASSERT_EQ(test, apr_build_desc(&f, d), 0);
		KUNIT_EXPECT_EQ_MSG(test, get_unaligned_le16(d + 0x110), cases[i].q_target,
				    "case %u", i);
		KUNIT_EXPECT_EQ_MSG(test, get_unaligned_le16(d + 0x112), cases[i].q_max,
				    "case %u", i);
	}
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

static const size_t apri_test_block_headers[APRI_T6041_BLOCKS] = {
	[APRI_SYSTEM_BLOCK] = 16,
	[APRI_CORE_BLOCK] = 16 + 8 + APRI_SYSTEM_COUNT * 12,
	[APRI_WRAPPER_BLOCK] = 16 + 8 + APRI_SYSTEM_COUNT * 12 +
			       8 + APRI_CORE_COUNT * 12,
};

static const size_t apri_test_entry_bases[APRI_T6041_BLOCKS] = {
	[APRI_SYSTEM_BLOCK] = 16 + 8,
	[APRI_CORE_BLOCK] = 16 + 8 + APRI_SYSTEM_COUNT * 12 + 8,
	[APRI_WRAPPER_BLOCK] = 16 + 8 + APRI_SYSTEM_COUNT * 12 +
			     8 + APRI_CORE_COUNT * 12 + 8,
};

static void apri_test_build_fixture(u8 *data)
{
	static const u32 counts[APRI_T6041_BLOCKS] = {
		[APRI_SYSTEM_BLOCK] = APRI_SYSTEM_COUNT,
		[APRI_CORE_BLOCK] = APRI_CORE_COUNT,
		[APRI_WRAPPER_BLOCK] = APRI_WRAPPER_COUNT,
	};
	u32 block, entry;

	memset(data, 0, APRI_T6041_FIRMWARE_SIZE);
	memcpy(data, "APRI", 4);
	put_unaligned_le32(1, data + 4);
	put_unaligned_le32(0x6041, data + 8);
	put_unaligned_le32(APRI_T6041_BLOCKS, data + 12);
	for (block = 0; block < APRI_T6041_BLOCKS; block++) {
		put_unaligned_le32(block + 1, data + apri_test_block_headers[block]);
		put_unaligned_le32(counts[block], data + apri_test_block_headers[block] + 4);
		for (entry = 0; entry < counts[block]; entry++) {
			size_t pos = apri_test_entry_bases[block] + entry * 12;

			put_unaligned_le32(entry * 4, data + pos);
			put_unaligned_le32(0xff, data + pos + 4);
			put_unaligned_le32(entry & 0xff, data + pos + 8);
		}
	}
}

static void apr_test_apri_valid(struct kunit *test)
{
	struct apr_init_data *init = kunit_kzalloc(test, sizeof(*init), GFP_KERNEL);
	u8 *data = kunit_kmalloc(test, APRI_T6041_FIRMWARE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, init);
	KUNIT_ASSERT_NOT_NULL(test, data);
	apri_test_build_fixture(data);
	ret = apr_parse_t6041_init(data, APRI_T6041_FIRMWARE_SIZE, init);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, init->blocks[APRI_SYSTEM_BLOCK].count,
			(u32)APRI_SYSTEM_COUNT);
	KUNIT_EXPECT_EQ(test, init->blocks[APRI_CORE_BLOCK].count,
			(u32)APRI_CORE_COUNT);
	KUNIT_EXPECT_EQ(test, init->blocks[APRI_WRAPPER_BLOCK].count,
			(u32)APRI_WRAPPER_COUNT);
	KUNIT_EXPECT_EQ(test,
			init->blocks[APRI_WRAPPER_BLOCK].entries[61].offset,
			(u32)61 * 4);
}

/* A T6050 container: T6041's three blocks, then the control block. */
static void apr_test_apri_t6050(struct kunit *test)
{
	static const u32 counts[APRI_T6050_BLOCKS] = {
		APRI_SYSTEM_COUNT, APRI_CORE_COUNT, APRI_WRAPPER_COUNT, APRI_CONTROL_COUNT,
	};
	struct apr_init_data *init = kunit_kzalloc(test, sizeof(*init), GFP_KERNEL);
	u8 *data = kunit_kzalloc(test, APRI_T6050_FIRMWARE_SIZE, GFP_KERNEL);
	size_t pos = 16;
	u32 block, entry;

	KUNIT_ASSERT_NOT_NULL(test, init);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, "APRI", 4);
	put_unaligned_le32(1, data + 4);
	put_unaligned_le32(0x6050, data + 8);
	put_unaligned_le32(APRI_T6050_BLOCKS, data + 12);
	for (block = 0; block < APRI_T6050_BLOCKS; block++) {
		put_unaligned_le32(block + 1, data + pos);
		put_unaligned_le32(counts[block], data + pos + 4);
		pos += 8;
		for (entry = 0; entry < counts[block]; entry++, pos += 12) {
			put_unaligned_le32(entry * 4, data + pos);
			put_unaligned_le32(0xffffffff, data + pos + 4);
			put_unaligned_le32(entry, data + pos + 8);
		}
	}
	KUNIT_ASSERT_EQ(test, pos, (size_t)APRI_T6050_FIRMWARE_SIZE);
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t6050, data, pos, init), 0);
	KUNIT_EXPECT_EQ(test, init->blocks[APRI_CONTROL_BLOCK].count,
			(u32)APRI_CONTROL_COUNT);
	KUNIT_EXPECT_EQ(test, init->blocks[APRI_CONTROL_BLOCK].entries[11].offset,
			(u32)11 * 4);
	/* Neither SoC takes the other's container. */
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t6041, data, pos, init), -EINVAL);
	put_unaligned_le32(0x6041, data + 8);
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t6050, data, pos, init), -EINVAL);
	put_unaligned_le32(0x6050, data + 8);
	/* A control row past its window. */
	put_unaligned_le32(0x4000, data + pos - 12);
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t6050, data, pos, init), -EINVAL);
}

/* A T8132 container: T6041's blocks with a 48-row wrapper table. */
static void apr_test_apri_t8132(struct kunit *test)
{
	static const u32 counts[APRI_T6041_BLOCKS] = {
		APRI_SYSTEM_COUNT, APRI_CORE_COUNT, APRI_T8132_WRAPPER_COUNT,
	};
	struct apr_init_data *init = kunit_kzalloc(test, sizeof(*init), GFP_KERNEL);
	u8 *data = kunit_kzalloc(test, APRI_T8132_FIRMWARE_SIZE, GFP_KERNEL);
	size_t pos = 16;
	u32 block, entry;

	KUNIT_ASSERT_NOT_NULL(test, init);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, "APRI", 4);
	put_unaligned_le32(1, data + 4);
	put_unaligned_le32(0x8132, data + 8);
	put_unaligned_le32(APRI_T6041_BLOCKS, data + 12);
	for (block = 0; block < APRI_T6041_BLOCKS; block++) {
		put_unaligned_le32(block + 1, data + pos);
		put_unaligned_le32(counts[block], data + pos + 4);
		pos += 8;
		for (entry = 0; entry < counts[block]; entry++, pos += 12) {
			put_unaligned_le32(entry * 4, data + pos);
			put_unaligned_le32(0xffffffff, data + pos + 4);
			put_unaligned_le32(entry, data + pos + 8);
		}
	}
	KUNIT_ASSERT_EQ(test, pos, (size_t)APRI_T8132_FIRMWARE_SIZE);
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t8132, data, pos, init), 0);
	KUNIT_EXPECT_EQ(test, init->blocks[APRI_WRAPPER_BLOCK].count,
			(u32)APRI_T8132_WRAPPER_COUNT);
	/* T6041 does not take it, nor T8132 a T6041 SoC field. */
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t6041, data, pos, init), -EINVAL);
	put_unaligned_le32(0x6041, data + 8);
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t8132, data, pos, init), -EINVAL);
	put_unaligned_le32(0x8132, data + 8);
	/* A wrapper row past its window. */
	put_unaligned_le32(0x1000000, data + pos - 12);
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t8132, data, pos, init), -EINVAL);
}

/* A T8142 container: T6050's blocks with a 38-row wrapper. */
static void apr_test_apri_t8142(struct kunit *test)
{
	static const u32 counts[APRI_T8142_BLOCKS] = {
		APRI_SYSTEM_COUNT, APRI_CORE_COUNT, APRI_T8142_WRAPPER_COUNT,
		APRI_CONTROL_COUNT,
	};
	struct apr_init_data *init = kunit_kzalloc(test, sizeof(*init), GFP_KERNEL);
	u8 *data = kunit_kzalloc(test, APRI_T8142_FIRMWARE_SIZE, GFP_KERNEL);
	size_t pos = 16;
	u32 block, entry;

	KUNIT_ASSERT_NOT_NULL(test, init);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, "APRI", 4);
	put_unaligned_le32(1, data + 4);
	put_unaligned_le32(0x8142, data + 8);
	put_unaligned_le32(APRI_T8142_BLOCKS, data + 12);
	for (block = 0; block < APRI_T8142_BLOCKS; block++) {
		put_unaligned_le32(block + 1, data + pos);
		put_unaligned_le32(counts[block], data + pos + 4);
		pos += 8;
		for (entry = 0; entry < counts[block]; entry++, pos += 12) {
			put_unaligned_le32(entry * 4, data + pos);
			put_unaligned_le32(0xffffffff, data + pos + 4);
			put_unaligned_le32(entry, data + pos + 8);
		}
	}
	KUNIT_ASSERT_EQ(test, pos, (size_t)APRI_T8142_FIRMWARE_SIZE);
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t8142, data, pos, init), 0);
	KUNIT_EXPECT_EQ(test, init->blocks[APRI_CONTROL_BLOCK].count,
			(u32)APRI_CONTROL_COUNT);
	KUNIT_EXPECT_EQ(test, init->blocks[APRI_CONTROL_BLOCK].entries[11].offset,
			(u32)11 * 4);
	/* T6050 refuses it by size, T8142 refuses T6050's SoC id. */
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t6050, data, pos, init), -EINVAL);
	put_unaligned_le32(0x6050, data + 8);
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t8142, data, pos, init), -EINVAL);
	put_unaligned_le32(0x8142, data + 8);
	/* A control row past its window. */
	put_unaligned_le32(0x4000, data + pos - 12);
	KUNIT_EXPECT_EQ(test, apr_parse_init(&apri_t8142, data, pos, init), -EINVAL);
}

static void apri_test_expect_rejected(struct kunit *test, const u8 *data,
				      size_t size, struct apr_init_data *init)
{
	KUNIT_EXPECT_EQ(test, apr_parse_t6041_init(data, size, init), -EINVAL);
}

static void apr_test_apri_header_rejects(struct kunit *test)
{
	struct apr_init_data *init = kunit_kzalloc(test, sizeof(*init), GFP_KERNEL);
	u8 *data = kunit_kmalloc(test, APRI_T6041_FIRMWARE_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, init);
	KUNIT_ASSERT_NOT_NULL(test, data);
	apri_test_build_fixture(data);
	data[0] = 'X';
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE, init);
	apri_test_build_fixture(data);
	put_unaligned_le32(2, data + 4);
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE, init);
	apri_test_build_fixture(data);
	put_unaligned_le32(0x6040, data + 8);
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE, init);
	apri_test_build_fixture(data);
	put_unaligned_le32(4, data + 12);
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE, init);
	apri_test_build_fixture(data);
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE - 1, init);
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE + 1, init);
}

static void apr_test_apri_shape_rejects(struct kunit *test)
{
	struct apr_init_data *init = kunit_kzalloc(test, sizeof(*init), GFP_KERNEL);
	u8 *data = kunit_kmalloc(test, APRI_T6041_FIRMWARE_SIZE, GFP_KERNEL);
	u8 *core_count;

	KUNIT_ASSERT_NOT_NULL(test, init);
	KUNIT_ASSERT_NOT_NULL(test, data);
	apri_test_build_fixture(data);
	put_unaligned_le32(1, data + apri_test_block_headers[APRI_CORE_BLOCK]);
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE, init);
	apri_test_build_fixture(data);
	put_unaligned_le32(4, data + apri_test_block_headers[APRI_CORE_BLOCK]);
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE, init);
	apri_test_build_fixture(data);
	core_count = data + apri_test_block_headers[APRI_CORE_BLOCK] + 4;
	put_unaligned_le32(APRI_CORE_COUNT - 1, core_count);
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE, init);
}

static void apr_test_apri_rows_reject(struct kunit *test, u32 block,
				      u32 entry, u32 field, u32 value)
{
	struct apr_init_data *init = kunit_kzalloc(test, sizeof(*init), GFP_KERNEL);
	u8 *data = kunit_kmalloc(test, APRI_T6041_FIRMWARE_SIZE, GFP_KERNEL);
	size_t pos;

	KUNIT_ASSERT_NOT_NULL(test, init);
	KUNIT_ASSERT_NOT_NULL(test, data);
	apri_test_build_fixture(data);
	pos = apri_test_entry_bases[block] + entry * 12 + field;
	put_unaligned_le32(value, data + pos);
	apri_test_expect_rejected(test, data, APRI_T6041_FIRMWARE_SIZE, init);
}

static void apr_test_apri_row_rejects(struct kunit *test)
{
	apr_test_apri_rows_reject(test, APRI_SYSTEM_BLOCK, 0, 0, 2);
	apr_test_apri_rows_reject(test, APRI_SYSTEM_BLOCK, 1, 0, 0);
	apr_test_apri_rows_reject(test, APRI_SYSTEM_BLOCK, 0, 0, 0x4000);
	apr_test_apri_rows_reject(test, APRI_CORE_BLOCK, 0, 0, 0x4000);
	apr_test_apri_rows_reject(test, APRI_WRAPPER_BLOCK, 0, 0, 0x1000000);
	apr_test_apri_rows_reject(test, APRI_SYSTEM_BLOCK, 0, 4, 0);
	apr_test_apri_rows_reject(test, APRI_SYSTEM_BLOCK, 2, 4, 1);
	/* A value bit outside its mask. */
	apr_test_apri_rows_reject(test, APRI_SYSTEM_BLOCK, 0, 8, 0x100);
}

static void apr_test_decode_desc(struct kunit *test)
{
	struct apr_decode_frame f = {
		.width = 320, .height = 240, .output = APR_INPUT_422_16,
		.luma_stride = 640, .chroma_stride = 640,
		.input = 0x10000100000ULL, .luma = 0x10000200000ULL,
		.chroma = 0x10000300000ULL, .job_id = 0x1234,
	};
	u8 *d = kunit_kmalloc(test, APR_DECODE_DESC_BYTES + 1, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, d);
	memset(d, 0xa5, APR_DECODE_DESC_BYTES + 1);
	KUNIT_ASSERT_EQ(test, apr_build_decode_desc(&f, d), 0);
	KUNIT_EXPECT_EQ(test, d[APR_DECODE_DESC_BYTES], 0xa5);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d), 0x1234120b);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 4), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 8), f.input);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x14), 320);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x16), 240);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x1c), 320);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x1e), 240);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x20), 10);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x22), 10);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x28), f.luma);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x30), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x38), f.chroma);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x40), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x48), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x90), 3);
	for (i = 0x94; i < APR_DECODE_DESC_BYTES; i++)
		KUNIT_EXPECT_EQ_MSG(test, d[i], 0, "reserved byte %#x", i);

	f.output = APR_INPUT_444_16;
	f.chroma_stride = 1280;
	f.bottom_field_first = true;
	KUNIT_ASSERT_EQ(test, apr_build_decode_desc(&f, d), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d), 0x1234520f);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(d + 0x22), 20);
	f.luma_stride = 600;
	KUNIT_EXPECT_EQ(test, apr_build_decode_desc(&f, d), -EINVAL);
}

static u8 *apr_test_bitstream_size(struct kunit *test, u32 width, u32 height,
				   bool interlaced, size_t *bytes)
{
	u32 slices = apr_slice_count(width, interlaced ?
				     DIV_ROUND_UP(height, 2) : height, 3);
	u32 picture_size = 8 + 2 * slices + 6 * slices;
	u32 pictures = interlaced ? 2 : 1;
	u32 frame_size = 28 + pictures * picture_size;
	u8 *frame = kunit_kzalloc(test, frame_size, GFP_KERNEL);
	u32 offset = 28;
	unsigned int i, p;

	if (!frame)
		return NULL;
	put_unaligned_be32(frame_size, frame);
	memcpy(frame + 4, "icpf", 4);
	put_unaligned_be16(20, frame + 8);
	put_unaligned_be16(0, frame + 10);
	memcpy(frame + 12, "kunt", 4);
	put_unaligned_be16(width, frame + 16);
	put_unaligned_be16(height, frame + 18);
	frame[20] = 2 << 6 | (interlaced ? 1 << 2 : 0);
	frame[22] = 1;
	frame[23] = 1;
	frame[24] = 1;
	for (p = 0; p < pictures; p++) {
		u8 *picture = frame + offset;

		picture[0] = 8 << 3;
		put_unaligned_be32(picture_size, picture + 1);
		put_unaligned_be16(slices, picture + 5);
		picture[7] = 3 << 4;
		for (i = 0; i < slices; i++) {
			put_unaligned_be16(6, picture + 8 + 2 * i);
			/* A 6-byte slice header with empty Y and Cb data. */
			picture[8 + 2 * slices + 6 * i] = 6 << 3;
		}
		offset += picture_size;
	}
	*bytes = frame_size;
	return frame;
}

static u8 *apr_test_bitstream(struct kunit *test, bool interlaced, size_t *bytes)
{
	return apr_test_bitstream_size(test, 320, 240, interlaced, bytes);
}

static void apr_test_parse_frame(struct kunit *test)
{
	struct apr_prores_info info;
	size_t bytes;
	u8 *frame = apr_test_bitstream(test, false, &bytes);

	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_EQ(test, apr_parse_frame(frame, bytes, &info), 0);
	KUNIT_EXPECT_EQ(test, info.frame_size, (u32)bytes);
	KUNIT_EXPECT_EQ(test, info.width, 320U);
	KUNIT_EXPECT_EQ(test, info.height, 240U);
	KUNIT_EXPECT_EQ(test, info.chroma, (u8)2);
	KUNIT_EXPECT_EQ(test, info.pictures, (u8)1);
	KUNIT_EXPECT_EQ(test, info.slices[0], 45U);
	KUNIT_EXPECT_EQ(test, info.picture_offset[0], 28U);

	put_unaligned_be32(bytes - 1, frame);
	KUNIT_EXPECT_EQ(test, apr_parse_frame(frame, bytes, &info), -EINVAL);
	put_unaligned_be32(bytes, frame);
	frame[4] = 'x';
	KUNIT_EXPECT_EQ(test, apr_parse_frame(frame, bytes, &info), -EINVAL);
}

static void apr_test_parse_interlaced(struct kunit *test)
{
	struct apr_prores_info info;
	size_t bytes;
	u8 *frame = apr_test_bitstream(test, true, &bytes);

	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_EQ(test, apr_parse_frame(frame, bytes, &info), 0);
	KUNIT_EXPECT_EQ(test, info.frame_type, (u8)1);
	KUNIT_EXPECT_EQ(test, info.pictures, (u8)2);
	KUNIT_EXPECT_EQ(test, info.slices[0], 24U);
	KUNIT_EXPECT_EQ(test, info.slices[1], 24U);
	KUNIT_EXPECT_EQ(test, info.picture_offset[1],
			info.picture_offset[0] + info.picture_size[0]);
}

/* 1080p has 1020 slices: the table is read in several blocks. */
static void apr_test_parse_many_slices(struct kunit *test)
{
	struct apr_prores_info info;
	size_t bytes;
	u8 *frame = apr_test_bitstream_size(test, 1920, 1080, false, &bytes);
	u8 *table;

	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_EQ(test, apr_parse_frame(frame, bytes, &info), 0);
	KUNIT_EXPECT_EQ(test, info.slices[0], 1020U);
	table = frame + 28 + 8;
	/* A bad size in the last, partial block is still found. */
	put_unaligned_be16(5, table + 2 * 1019);
	KUNIT_EXPECT_EQ(test, apr_parse_frame(frame, bytes, &info), -EINVAL);
	put_unaligned_be16(6, table + 2 * 1019);
	/* So is one just past the first block. */
	put_unaligned_be16(0, table + 2 * 128);
	KUNIT_EXPECT_EQ(test, apr_parse_frame(frame, bytes, &info), -EINVAL);
	put_unaligned_be16(6, table + 2 * 128);
	/* And slices that together overrun the picture. */
	put_unaligned_be16(7, table + 2 * 500);
	KUNIT_EXPECT_EQ(test, apr_parse_frame(frame, bytes, &info), -EINVAL);
	put_unaligned_be16(6, table + 2 * 500);
	KUNIT_EXPECT_EQ(test, apr_parse_frame(frame, bytes, &info), 0);
}

/*
 * The decode descriptor has no input length, so the engine reads as far as the
 * frame's own sizes say. Each of these frames points past its payload and
 * must be rejected before a descriptor is built. Every frame is parsed from an
 * allocation exactly as long as the payload, so an over-read is a KASAN error.
 *
 * Progressive fixture: frame header at 8, picture at 28 (8-byte header,
 * 45-entry slice table, 45 slices of 6 bytes), 396 bytes in all.
 */
#define APR_TP_PIC	28U
#define APR_TP_SLICES	45U
#define APR_TP_TABLE	(APR_TP_PIC + 8)
#define APR_TP_DATA	(APR_TP_TABLE + 2 * APR_TP_SLICES)
#define APR_TP_SIZE	(8 + 8 * APR_TP_SLICES)
#define APR_TP_BYTES	(APR_TP_PIC + APR_TP_SIZE)

/* Parse the first @bytes of @frame from a buffer exactly that long. */
static int apr_test_parse_at(struct kunit *test, const u8 *frame, size_t bytes)
{
	struct apr_prores_info info;
	u8 *copy = kunit_kmalloc(test, bytes, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, copy);
	memcpy(copy, frame, bytes);
	return apr_parse_frame(copy, bytes, &info);
}

/* A progressive fixture with @extra zero bytes of room after it. */
static u8 *apr_test_room(struct kunit *test, size_t extra, size_t *bytes)
{
	u8 *frame = apr_test_bitstream(test, false, bytes);
	u8 *room;

	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_EQ(test, *bytes, (size_t)APR_TP_BYTES);
	room = kunit_kzalloc(test, *bytes + extra, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, room);
	memcpy(room, frame, *bytes);
	return room;
}

static void apr_test_oob_exact_fit(struct kunit *test)
{
	size_t bytes;
	u8 *frame = apr_test_room(test, 1, &bytes);
	struct apr_prores_info info;
	u8 *last = frame + APR_TP_TABLE + 2 * (APR_TP_SLICES - 1);

	/* Header, table and slices fill the picture and the frame exactly. */
	KUNIT_ASSERT_EQ(test, apr_parse_frame(frame, bytes, &info), 0);
	KUNIT_EXPECT_EQ(test, info.picture_offset[0] + info.picture_size[0],
			info.frame_size);
	KUNIT_EXPECT_EQ(test, info.picture_size[0], APR_TP_SIZE);

	/* One more byte in the last slice, the picture and the frame fits too. */
	put_unaligned_be16(7, last);
	put_unaligned_be32(APR_TP_SIZE + 1, frame + APR_TP_PIC + 1);
	put_unaligned_be32(APR_TP_BYTES + 1, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes + 1), 0);

	/* An encoder may leave the deprecated slice count at 0. */
	put_unaligned_be16(0, frame + APR_TP_PIC + 5);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes + 1), 0);
}

static void apr_test_oob_frame_size(struct kunit *test)
{
	size_t bytes;
	u8 *frame = apr_test_room(test, 4096, &bytes);

	/* The frame claims more bytes than were queued. */
	put_unaligned_be32(bytes + 1, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be32(bytes + 4096, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be32(U32_MAX, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);

	/* bytesused one short of an otherwise valid frame. */
	put_unaligned_be32(bytes, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes - 1), -EINVAL);

	/* Truncated by one byte with the frame size rewritten to match. */
	put_unaligned_be32(bytes - 1, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes - 1), -EINVAL);

	/* Shorter than any frame header. */
	put_unaligned_be32(27, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, 27), -EINVAL);
}

static void apr_test_oob_frame_header(struct kunit *test)
{
	size_t bytes;
	u8 *frame = apr_test_room(test, 0, &bytes);

	/* Frame header one byte past the frame, then filling it entirely. */
	put_unaligned_be16(bytes - 8 + 1, frame + 8);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be16(bytes - 8, frame + 8);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be16(U16_MAX, frame + 8);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);

	/* Quantisation matrices flagged but not inside the frame header. */
	put_unaligned_be16(20, frame + 8);
	frame[8 + 19] = 3;
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	frame[8 + 19] = 0;

	/* Picture header shorter than its fixed fields. */
	frame[APR_TP_PIC] = 7 << 3;
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
}

static void apr_test_oob_picture_size(struct kunit *test)
{
	size_t bytes;
	u8 *frame = apr_test_room(test, 0, &bytes);
	u8 *size = frame + APR_TP_PIC + 1;

	/* The picture runs one byte, 64 KiB and 4 GiB past the payload. */
	put_unaligned_be32(APR_TP_SIZE + 1, size);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be32(APR_TP_SIZE + SZ_64K, size);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be32(U32_MAX, size);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);

	/* Its slices must fit a picture one byte shorter. */
	put_unaligned_be32(APR_TP_SIZE - 1, size);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);

	/* Pictures must account for every byte of the frame. */
	frame = apr_test_room(test, 1, &bytes);
	put_unaligned_be32(bytes + 1, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes + 1), -EINVAL);
}

static void apr_test_oob_slice_table(struct kunit *test)
{
	size_t bytes;
	u8 *frame = apr_test_room(test, 0, &bytes);
	u32 table_end = 8 + 2 * APR_TP_SLICES;

	/* The slice table ends one byte past the picture (and the frame). */
	put_unaligned_be32(table_end - 1, frame + APR_TP_PIC + 1);
	put_unaligned_be32(APR_TP_PIC + table_end - 1, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, APR_TP_PIC + table_end - 1),
			-EINVAL);

	/* The in-band slice count asks for one more table entry. */
	frame = apr_test_room(test, 0, &bytes);
	put_unaligned_be16(APR_TP_SLICES + 1, frame + APR_TP_PIC + 5);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be16(U16_MAX, frame + APR_TP_PIC + 5);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);

	/*
	 * Smaller slices mean more of them (75 at 320x240): the table the
	 * engine walks grows with log2_slice even with no in-band count.
	 */
	put_unaligned_be16(0, frame + APR_TP_PIC + 5);
	frame[APR_TP_PIC + 7] = 2 << 4;
	KUNIT_EXPECT_EQ(test, apr_slice_count(320, 240, 2), 75U);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
}

static void apr_test_oob_slice_sizes(struct kunit *test)
{
	size_t bytes;
	u8 *frame = apr_test_room(test, 0, &bytes);
	u8 *table = frame + APR_TP_TABLE;

	/* The last slice ends one byte past the picture and the payload. */
	put_unaligned_be16(7, table + 2 * (APR_TP_SLICES - 1));
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be16(U16_MAX, table + 2 * (APR_TP_SLICES - 1));
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);

	/* A large early slice pushes every later one past the end. */
	put_unaligned_be16(6, table + 2 * (APR_TP_SLICES - 1));
	put_unaligned_be16(U16_MAX, table);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);

	/* A slice too short for its own header. */
	put_unaligned_be16(5, table);
	put_unaligned_be16(7, table + 2);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
}

static void apr_test_oob_slice_headers(struct kunit *test)
{
	size_t bytes;
	u8 *frame = apr_test_room(test, 0, &bytes);
	u8 *last = frame + APR_TP_DATA + 6 * (APR_TP_SLICES - 1);

	/* Coded Y and Cb data run one byte past the last slice. */
	put_unaligned_be16(1, last + 2);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be16(0, last + 2);
	put_unaligned_be16(U16_MAX, last + 4);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be16(0, last + 4);

	/* A slice header longer than the slice. */
	last[0] = 7 << 3;
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	last[0] = 6 << 3;
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), 0);

	/* With alpha the Cr size is in the header too: 6 bytes is too short. */
	frame[8 + 12] = 3 << 6;
	frame[8 + 17] = 1;
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
}

static void apr_test_oob_second_field(struct kunit *test)
{
	size_t bytes;
	u8 *frame = apr_test_bitstream(test, true, &bytes);
	struct apr_prores_info info;
	u32 second;

	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_EQ(test, apr_parse_frame(frame, bytes, &info), 0);
	second = info.picture_offset[1];

	/* The bottom field runs one byte past the payload. */
	put_unaligned_be32(info.picture_size[1] + 1, frame + second + 1);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be32(U32_MAX, frame + second + 1);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be32(info.picture_size[1], frame + second + 1);

	/* Its last slice ends one byte past the payload. */
	put_unaligned_be16(7, frame + second + 8 + 2 * (info.slices[1] - 1));
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
	put_unaligned_be16(6, frame + second + 8 + 2 * (info.slices[1] - 1));

	/* The frame stops one byte into, and right at, the bottom field. */
	put_unaligned_be32(second + 1, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, second + 1), -EINVAL);
	put_unaligned_be32(second, frame);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, second), -EINVAL);
	put_unaligned_be32(bytes, frame);

	/* The top field grows by a byte, so the bottom one is misplaced. */
	put_unaligned_be32(info.picture_size[0] + 1, frame + info.picture_offset[0] + 1);
	KUNIT_EXPECT_EQ(test, apr_test_parse_at(test, frame, bytes), -EINVAL);
}

static void apr_test_ring_accounting(struct kunit *test)
{
	u8 *ring = kunit_kzalloc(test, APR_RING_BYTES, GFP_KERNEL);
	u8 *desc = kunit_kmalloc(test, APR_ENCODE_DESC_BYTES, GFP_KERNEL);
	u32 ptr = APR_RING_BYTES - 0x100;
	u32 next;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, ring);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	next = apr_ring_advance(0, APR_DECODE_DESC_BYTES, APR_RING_BYTES);
	KUNIT_EXPECT_EQ(test, next, 0x100U);
	next = apr_ring_advance(0x100, APR_ENCODE_DESC_BYTES, APR_RING_BYTES);
	KUNIT_EXPECT_EQ(test, next, 0x280U);
	next = apr_ring_advance(ptr, APR_ENCODE_DESC_BYTES, APR_RING_BYTES);
	KUNIT_EXPECT_EQ(test, next, APR_RING_GENERATION | 0x80U);
	next = apr_ring_advance(APR_RING_GENERATION | APR_RING_BYTES - 0x80,
				APR_DECODE_DESC_BYTES, APR_RING_BYTES);
	KUNIT_EXPECT_EQ(test, next, 0x80U);
	for (i = 0; i < APR_ENCODE_DESC_BYTES; i++)
		desc[i] = i;
	apr_ring_write(ring, APR_RING_BYTES, ptr, desc, APR_ENCODE_DESC_BYTES);
	KUNIT_EXPECT_MEMEQ(test, ring + ptr, desc, 0x100);
	KUNIT_EXPECT_MEMEQ(test, ring, desc + 0x100, 0x80);
}

static void apr_test_ring_space(struct kunit *test)
{
	const u32 r = APR_RING_BYTES, g = APR_RING_GENERATION;

	KUNIT_EXPECT_EQ(test, apr_ring_distance(0, 0x100, r), 0x100U);
	KUNIT_EXPECT_EQ(test, apr_ring_distance(0x100, 0x100, r), 0U);
	/* Across the end of the ring, in either generation. */
	KUNIT_EXPECT_EQ(test, apr_ring_distance(r - 0x80, g | 0x80, r), 0x100U);
	KUNIT_EXPECT_EQ(test, apr_ring_distance(g | (r - 0x80), 0x80, r), 0x100U);
	/* Same offset, other generation: a whole ring apart. */
	KUNIT_EXPECT_EQ(test, apr_ring_distance(0x100, g | 0x100, r), r);

	KUNIT_EXPECT_EQ(test, apr_ring_free(0, 0, r), r);
	KUNIT_EXPECT_EQ(test, apr_ring_free(0, 0x800, r), r - 0x800);
	KUNIT_EXPECT_EQ(test, apr_ring_free(r - 0x80, g | 0x80, r), r - 0x100);
	KUNIT_EXPECT_EQ(test, apr_ring_free(g | 0x100, 0x100, r), 0U);
	/* The deepest ring the driver builds still has room. */
	KUNIT_EXPECT_GT(test, apr_ring_free(0, APR_MAX_INFLIGHT *
					    APR_ENCODE_DESC_BYTES, r),
			(u32)APR_ENCODE_DESC_BYTES);
}

static void apr_test_ring_tail(struct kunit *test)
{
	const u32 r = APR_RING_BYTES, g = APR_RING_GENERATION;

	KUNIT_EXPECT_TRUE(test, apr_ring_passed(0, 0x100, 0x100, r));
	KUNIT_EXPECT_TRUE(test, apr_ring_passed(0, 0x100, 0x300, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_passed(0, 0x100, 0x80, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_passed(0, 0x100, 0, r));
	KUNIT_EXPECT_TRUE(test, apr_ring_passed(r - 0x80, g | 0x80, g | 0x80, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_passed(r - 0x80, g | 0x80, r - 0x40, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_passed(g | (r - 0x80), 0x80, 0x40, r));
	KUNIT_EXPECT_TRUE(test, apr_ring_passed(g | (r - 0x80), 0x80, 0x80, r));

	KUNIT_EXPECT_TRUE(test, apr_ring_tail_valid(0x100, 0x100, 0x400, r));
	KUNIT_EXPECT_TRUE(test, apr_ring_tail_valid(0x100, 0x400, 0x400, r));
	KUNIT_EXPECT_TRUE(test, apr_ring_tail_valid(0x100, 0x280, 0x400, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_tail_valid(0x100, 0x500, 0x400, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_tail_valid(0x100, 0x80, 0x400, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_tail_valid(0x100, g | 0x200, 0x400, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_tail_valid(0, r, 0x400, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_tail_valid(0, BIT(25), 0x400, r));
	KUNIT_EXPECT_TRUE(test, apr_ring_tail_valid(r - 0x80, g | 0x40, g | 0x80, r));
	KUNIT_EXPECT_FALSE(test, apr_ring_tail_valid(r - 0x80, 0x40, g | 0x80, r));
}

static void apr_test_inflight(struct kunit *test)
{
	const u32 r = APR_RING_BYTES;
	u32 ends[APR_MAX_INFLIGHT], done, head, tail;
	unsigned int i, retired;

	KUNIT_EXPECT_FALSE(test, apr_jobs_full(0, 0));
	KUNIT_EXPECT_FALSE(test, apr_jobs_full(APR_MAX_INFLIGHT - 1, 0));
	KUNIT_EXPECT_TRUE(test, apr_jobs_full(APR_MAX_INFLIGHT, 0));
	/* Encode stays alone at the end of the ring. */
	KUNIT_EXPECT_TRUE(test, apr_jobs_full(1, 1));
	KUNIT_EXPECT_TRUE(test, apr_jobs_full(3, 1));

	/*
	 * Ring space and tail arithmetic for a full ring straddling the wrap.
	 * Only the lone encode retires on the tail; decode jobs retire on
	 * their ids (apr_test_job_id).
	 */
	done = r - 3 * APR_DECODE_DESC_BYTES - 0x80;
	head = done;
	for (i = 0; i < APR_MAX_INFLIGHT; i++) {
		KUNIT_ASSERT_GT(test, apr_ring_free(done, head, r),
				(u32)APR_DECODE_DESC_BYTES);
		head = apr_ring_advance(head, APR_DECODE_DESC_BYTES, r);
		ends[i] = head;
	}
	KUNIT_EXPECT_EQ(test, apr_ring_distance(done, head, r),
			(u32)(APR_MAX_INFLIGHT * APR_DECODE_DESC_BYTES));
	/* The tail stops inside the fifth descriptor: four jobs are done. */
	tail = apr_ring_advance(ends[3], 0x40, r);
	KUNIT_ASSERT_TRUE(test, apr_ring_tail_valid(done, tail, head, r));
	for (retired = 0; retired < APR_MAX_INFLIGHT; retired++) {
		if (!apr_ring_passed(done, ends[retired], tail, r))
			break;
		done = ends[retired];
	}
	KUNIT_EXPECT_EQ(test, retired, 4U);
	KUNIT_EXPECT_EQ(test, done, ends[3]);
	/* Then the engine drains the ring. */
	tail = head;
	KUNIT_ASSERT_TRUE(test, apr_ring_tail_valid(done, tail, head, r));
	for (; retired < APR_MAX_INFLIGHT; retired++) {
		if (!apr_ring_passed(done, ends[retired], tail, r))
			break;
		done = ends[retired];
	}
	KUNIT_EXPECT_EQ(test, retired, (unsigned int)APR_MAX_INFLIGHT);
	KUNIT_EXPECT_EQ(test, done, head);
	KUNIT_EXPECT_EQ(test, apr_ring_free(done, head, r), r);
}

static void apr_test_job_id(struct kunit *test)
{
	/* Ids are never 0, which ST1 reads after a reset. */
	KUNIT_EXPECT_EQ(test, apr_next_job_id(1), 2);
	KUNIT_EXPECT_EQ(test, apr_next_job_id(U16_MAX), 1);
	KUNIT_EXPECT_FALSE(test, apr_job_id_done(0, 1));
	KUNIT_EXPECT_TRUE(test, apr_job_id_done(1, 1));
	KUNIT_EXPECT_TRUE(test, apr_job_id_done(5, 3));
	KUNIT_EXPECT_FALSE(test, apr_job_id_done(3, 5));
	/* Across the wrap: 65535 finishes before 1 and 2. */
	KUNIT_EXPECT_TRUE(test, apr_job_id_done(1, U16_MAX));
	KUNIT_EXPECT_FALSE(test, apr_job_id_done(U16_MAX, 1));
	KUNIT_EXPECT_FALSE(test, apr_job_id_done(U16_MAX - 1, 2));

	/* ST1 = 0 is no progress, never "everything in 0x8001..0xffff done". */
	KUNIT_EXPECT_EQ(test, apr_job_id_check(0, 9), APR_DONE_ID_NONE);
	KUNIT_EXPECT_EQ(test, apr_job_id_check(0, 0x8005), APR_DONE_ID_NONE);
	KUNIT_EXPECT_TRUE(test, apr_job_id_done(0, 0x8001));
	/* Ids 1..8 outstanding: ids already issued, finished or not. */
	KUNIT_EXPECT_EQ(test, apr_job_id_check(1, 9), APR_DONE_ID_ISSUED);
	KUNIT_EXPECT_EQ(test, apr_job_id_check(8, 9), APR_DONE_ID_ISSUED);
	/* The other half of the id space reads as old ids: nothing retires. */
	for (u32 d = 0x8009; d <= U16_MAX; d += 0x7ff) {
		KUNIT_EXPECT_EQ(test, apr_job_id_check(d, 9), APR_DONE_ID_ISSUED);
		for (u16 id = 1; id < 9; id++)
			KUNIT_EXPECT_FALSE(test, apr_job_id_done(d, id));
	}
	KUNIT_EXPECT_EQ(test, apr_job_id_check(U16_MAX, 9), APR_DONE_ID_ISSUED);
	KUNIT_EXPECT_FALSE(test, apr_job_id_done(U16_MAX, 1));
	/* An id not issued yet: the ring is hung, nothing retires. */
	KUNIT_EXPECT_EQ(test, apr_job_id_check(9, 9), APR_DONE_ID_FUTURE);
	KUNIT_EXPECT_EQ(test, apr_job_id_check(10, 9), APR_DONE_ID_FUTURE);
	KUNIT_EXPECT_EQ(test, apr_job_id_check(0x8008, 9), APR_DONE_ID_FUTURE);
	/* First job after a reset: id 1 not issued until next_id moves on. */
	KUNIT_EXPECT_EQ(test, apr_job_id_check(1, 1), APR_DONE_ID_FUTURE);

	/* Eight jobs across the wrap: 0xfffc..0xffff, then 1..4 (0 skipped). */
	{
		u16 ids[APR_MAX_INFLIGHT], next = 0xfffc;
		unsigned int i, retired = 0;

		for (i = 0; i < APR_MAX_INFLIGHT; i++) {
			ids[i] = next;
			next = apr_next_job_id(next);
		}
		KUNIT_EXPECT_EQ(test, ids[4], 1);
		KUNIT_EXPECT_EQ(test, next, 5);
		/* ST1 skips ahead to 2: six jobs finished, in ring order. */
		KUNIT_EXPECT_EQ(test, apr_job_id_check(2, next), APR_DONE_ID_ISSUED);
		while (retired < APR_MAX_INFLIGHT &&
		       apr_job_id_done(2, ids[retired]))
			retired++;
		KUNIT_EXPECT_EQ(test, retired, 6U);
		/* The same id again retires nothing more. */
		while (retired < APR_MAX_INFLIGHT &&
		       apr_job_id_done(2, ids[retired]))
			retired++;
		KUNIT_EXPECT_EQ(test, retired, 6U);
		/* A stale id from before the wrap retires nothing more either. */
		KUNIT_EXPECT_FALSE(test, apr_job_id_done(0xfffe, ids[6]));
		KUNIT_EXPECT_EQ(test, apr_job_id_check(4, next), APR_DONE_ID_ISSUED);
		KUNIT_EXPECT_TRUE(test, apr_job_id_done(4, ids[7]));
		/* 5 = next_id: not issued, hung. */
		KUNIT_EXPECT_EQ(test, apr_job_id_check(5, next), APR_DONE_ID_FUTURE);
		KUNIT_EXPECT_EQ(test, apr_job_id_check(0x8004, next), APR_DONE_ID_FUTURE);
		KUNIT_EXPECT_EQ(test, apr_job_id_check(0x8005, next), APR_DONE_ID_ISSUED);
	}
}

static struct kunit_case apr_test_cases[] = {
	KUNIT_CASE(apr_test_qmat_regs),
	KUNIT_CASE(apr_test_dc_scale),
	KUNIT_CASE(apr_test_profiles),
	KUNIT_CASE(apr_test_desc_422),
	KUNIT_CASE(apr_test_desc_444),
	KUNIT_CASE(apr_test_desc_rejects),
	KUNIT_CASE(apr_test_desc_v0),
	KUNIT_CASE(apr_test_desc_v0_profiles),
	KUNIT_CASE(apr_test_budgets),
	KUNIT_CASE(apr_test_slices),
	KUNIT_CASE(apr_test_output),
	KUNIT_CASE(apr_test_apri_valid),
	KUNIT_CASE(apr_test_apri_t6050),
	KUNIT_CASE(apr_test_apri_t8132),
	KUNIT_CASE(apr_test_apri_t8142),
	KUNIT_CASE(apr_test_apri_header_rejects),
	KUNIT_CASE(apr_test_apri_shape_rejects),
	KUNIT_CASE(apr_test_apri_row_rejects),
	KUNIT_CASE(apr_test_decode_desc),
	KUNIT_CASE(apr_test_parse_frame),
	KUNIT_CASE(apr_test_parse_interlaced),
	KUNIT_CASE(apr_test_parse_many_slices),
	KUNIT_CASE(apr_test_oob_exact_fit),
	KUNIT_CASE(apr_test_oob_frame_size),
	KUNIT_CASE(apr_test_oob_frame_header),
	KUNIT_CASE(apr_test_oob_picture_size),
	KUNIT_CASE(apr_test_oob_slice_table),
	KUNIT_CASE(apr_test_oob_slice_sizes),
	KUNIT_CASE(apr_test_oob_slice_headers),
	KUNIT_CASE(apr_test_oob_second_field),
	KUNIT_CASE(apr_test_ring_accounting),
	KUNIT_CASE(apr_test_ring_space),
	KUNIT_CASE(apr_test_ring_tail),
	KUNIT_CASE(apr_test_inflight),
	KUNIT_CASE(apr_test_job_id),
	{}
};

static struct kunit_suite apr_test_suite = {
	.name = "apple-prores",
	.test_cases = apr_test_cases,
};
kunit_test_suite(apr_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple ProRes codec");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

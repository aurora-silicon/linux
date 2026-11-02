// SPDX-License-Identifier: GPL-2.0-only
/* Synthetic tables only; no firmware or encoder required. */
#include <kunit/test.h>
#include <linux/module.h>

#include "ave-temp.h"

static struct ave_temp_inputs inputs(void)
{
	return (struct ave_temp_inputs) {
		.tcr = 0x40808126, .ttbr = 0x8000efff0ULL,
		.text_iova = 0x800000000ULL, .data_iova = 0x8000d0000ULL,
		.data_link = 0x8000d0000ULL, .text_size = 0xd0000,
		.data_size = 0x128000, .page_shift = 14, .page_size = 0x4000,
		.index_bits = 11, .final_va_bits = 31, .final_mode = 3,
	};
}

static void put(__le64 *t, u32 off, u64 value)
{
	t[(off - 0x18000) / 8] = cpu_to_le64(value);
}

/* Independent fixture: 52 TEXT leaves, 74 DATA leaves, two roots. */
static __le64 *tables(struct kunit *test)
{
	__le64 *t = kunit_kzalloc(test, 0x8000, GFP_KERNEL);

	if (!t)
		return NULL;
	for (u32 page = 0; page < 52; page++)
		put(t, 0x18000 + page * 8, 0x800000483ULL + page * 0x4000ULL);
	for (u32 page = 52; page < 126; page++)
		put(t, 0x18000 + page * 8, 0x800000403ULL + page * 0x4000ULL);
	put(t, 0x1fff0, 0x8000e8403ULL);
	put(t, 0x1fff8, 0x8000ec403ULL);
	return t;
}

static void ave_temp_good_test(struct kunit *test)
{
	struct ave_temp_inputs in = inputs();
	struct ave_temp_result result = {};
	struct ave_temp_walk w;
	__le64 *t = tables(test), *before;

	KUNIT_ASSERT_NOT_NULL(test, t);
	before = kunit_kzalloc(test, 0x8000, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, before);
	memcpy(before, t, 0x8000);
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_OK);
	KUNIT_EXPECT_TRUE(test, w.translated && w.executable && !w.writable);
	KUNIT_EXPECT_EQ(test, w.level, 3U);
	KUNIT_EXPECT_EQ(test, w.offset, 0x18000U);
	w = ave_temp_walk(t, &in, 0xf0000, 0x8000f0000ULL, false);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_OK);
	KUNIT_EXPECT_TRUE(test, w.translated && w.writable);
	KUNIT_EXPECT_EQ(test, w.offset, 0x181e0U);
	ave_temp_compare(t, &in, &result);
	KUNIT_EXPECT_TRUE(test, result.model_valid);
	KUNIT_EXPECT_EQ(test, result.compared, 4096U);
	KUNIT_EXPECT_EQ(test, result.mismatches, 0U);
	KUNIT_EXPECT_MEMEQ(test, t, before, 0x8000);
}

static void ave_temp_sparse_test(struct kunit *test)
{
	struct ave_temp_inputs in = inputs();
	struct ave_temp_result result = {};
	struct ave_temp_walk w;
	__le64 *t = kunit_kzalloc(test, 0x8000, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	put(t, 0x18000, 0x800000483ULL);
	put(t, 0x1fff0, 0x8000e8403ULL);
	put(t, 0x1fff8, 0x8000ec403ULL);
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_OK);
	w = ave_temp_walk(t, &in, 0xf0000, 0x8000f0000ULL, false);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_INVALID);
	KUNIT_EXPECT_EQ(test, w.offset, 0x181e0U);
	KUNIT_EXPECT_EQ(test, w.descriptor, 0ULL);
	ave_temp_compare(t, &in, &result);
	KUNIT_EXPECT_EQ(test, result.mismatches, 125U);
	KUNIT_EXPECT_EQ(test, result.first_offset, 0x18008U);
	KUNIT_EXPECT_EQ(test, result.first_expected, 0x800004483ULL);
}

static void ave_temp_permissions_test(struct kunit *test)
{
	struct ave_temp_inputs in = inputs();
	struct ave_temp_walk w;
	__le64 *t = tables(test);

	KUNIT_ASSERT_NOT_NULL(test, t);
	put(t, 0x18000, 0x800000483ULL | BIT_ULL(53));
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_PERMISSION);
	KUNIT_EXPECT_FALSE(test, w.executable);
	put(t, 0x18000, 0x800000483ULL | BIT_ULL(54));
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_OK); /* UXN restricts EL0 */
	put(t, 0x181e0, 0x8000f0483ULL);
	w = ave_temp_walk(t, &in, 0xf0000, 0x8000f0000ULL, false);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_PERMISSION);
	KUNIT_EXPECT_FALSE(test, w.writable);
	put(t, 0x1fff0, 0x8000e8403ULL | BIT_ULL(59));
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_PERMISSION);
	KUNIT_EXPECT_EQ(test, w.offset, 0x1fff0U);
	put(t, 0x1fff0, 0x8000e8403ULL | BIT_ULL(62));
	w = ave_temp_walk(t, &in, 0xf0000, 0x8000f0000ULL, false);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_PERMISSION);
	KUNIT_EXPECT_EQ(test, w.offset, 0x1fff0U);
}

static void ave_temp_bad_leaf_test(struct kunit *test)
{
	struct ave_temp_inputs in = inputs();
	struct ave_temp_walk w;
	__le64 *t = tables(test);
	const u64 bad[] = { 0, 0x800000481ULL, 0x800000083ULL,
		0x800004483ULL, 0x800001483ULL, 0x1000800000483ULL };
	const enum ave_temp_fault fault[] = {
		AVE_TEMP_INVALID, AVE_TEMP_TYPE, AVE_TEMP_ACCESS_FLAG,
		AVE_TEMP_OUTPUT, AVE_TEMP_ADDRESS, AVE_TEMP_ADDRESS,
	};

	KUNIT_ASSERT_NOT_NULL(test, t);
	for (unsigned int i = 0; i < ARRAY_SIZE(bad); i++) {
		put(t, 0x18000, bad[i]);
		w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
		KUNIT_EXPECT_EQ(test, w.fault, fault[i]);
		KUNIT_EXPECT_EQ(test, w.offset, 0x18000U);
		KUNIT_EXPECT_EQ(test, w.descriptor, bad[i]);
	}
}

static void ave_temp_root_test(struct kunit *test)
{
	struct ave_temp_inputs in = inputs();
	struct ave_temp_walk w;
	__le64 *t = tables(test);

	KUNIT_ASSERT_NOT_NULL(test, t);
	put(t, 0x1fff0, 0);
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_INVALID);
	KUNIT_EXPECT_EQ(test, w.level, 2U);
	KUNIT_EXPECT_EQ(test, w.offset, 0x1fff0U);
	put(t, 0x1fff0, 0x8000f0403ULL); /* child just above window */
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_TABLE_RANGE);
	KUNIT_EXPECT_EQ(test, w.offset, 0x1fff0U);
	put(t, 0x1fff0, 0x8000ec403ULL); /* in-window wrong child */
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_OUTPUT);
	KUNIT_EXPECT_EQ(test, w.offset, 0x1fff0U);
	put(t, 0x1fff8, 0x8000ec403ULL);
	put(t, 0x1c008, 0x802004403ULL);
	w = ave_temp_walk(t, &in, 0x2004001, 0x802004001ULL, false);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_OK); /* second root index */
}

static void ave_temp_block_test(struct kunit *test)
{
	struct ave_temp_inputs in = inputs();
	struct ave_temp_walk w;
	__le64 *t = tables(test);

	KUNIT_ASSERT_NOT_NULL(test, t);
	put(t, 0x1fff0, 0x800000481ULL); /* L2 32 MiB block */
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_OK);
	KUNIT_EXPECT_EQ(test, w.level, 2U);
	put(t, 0x1fff0, 0x800004481ULL);
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_ADDRESS);
}

static void ave_temp_geometry_test(struct kunit *test)
{
	struct ave_temp_inputs in = inputs();
	struct ave_temp_walk w;
	__le64 *t = tables(test);

	KUNIT_ASSERT_NOT_NULL(test, t);
	w = ave_temp_walk(t, &in, BIT_ULL(26), 0, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_VA_RANGE);
	in.tcr ^= BIT_ULL(14);
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_GEOMETRY);
	in = inputs();
	in.ttbr &= ~0x3fffULL; /* silently rounding root down is wrong */
	w = ave_temp_walk(t, &in, 0x574, 0x800000574ULL, true);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_GEOMETRY);
	in = inputs();
	in.tcr |= 2ULL << 32; /* IPS doesn't change temporary input geometry */
	KUNIT_EXPECT_TRUE(test, ave_temp_geometry(&in));
}

static void ave_temp_ledger_test(struct kunit *test)
{
	struct ave_temp_inputs in = inputs();
	struct ave_temp_result result = {};
	__le64 *t = tables(test);

	KUNIT_ASSERT_NOT_NULL(test, t);
	put(t, 0x1e000, 1); /* unused words must be compared too */
	ave_temp_compare(t, &in, &result);
	KUNIT_EXPECT_EQ(test, result.compared, 4096U);
	KUNIT_EXPECT_EQ(test, result.mismatches, 1U);
	KUNIT_EXPECT_EQ(test, result.first_offset, 0x1e000U);
	KUNIT_EXPECT_EQ(test, result.first_observed, 1ULL);
	KUNIT_EXPECT_EQ(test, result.first_expected, 0ULL);
	in.pt_virt = 0x100000;
	result = (struct ave_temp_result) {};
	ave_temp_compare(t, &in, &result);
	KUNIT_EXPECT_FALSE(test, result.model_valid);
	KUNIT_EXPECT_EQ(test, result.compared, 0U);
	in = inputs();
	in.data_link += 0x4000;
	KUNIT_EXPECT_FALSE(test, ave_temp_model_inputs(&in));
}

static void ave_temp_relocated_model_test(struct kunit *test)
{
	struct ave_temp_inputs in = inputs();
	struct ave_temp_result result = {};
	struct ave_temp_walk w;
	__le64 *t = tables(test);

	KUNIT_ASSERT_NOT_NULL(test, t);
	/* Loader inputs, not fixed retained-run IOVAs, drive the model. */
	in.text_iova += 0x40000000;
	in.data_iova += 0x40000000;
	in.data_link = in.data_iova;
	in.ttbr = in.data_iova + 0x1fff0;
	for (u32 page = 0; page < 52; page++)
		put(t, 0x18000 + page * 8, 0x840000483ULL + page * 0x4000ULL);
	for (u32 page = 52; page < 126; page++)
		put(t, 0x18000 + page * 8, 0x840000403ULL + page * 0x4000ULL);
	put(t, 0x1fff0, 0x8400e8403ULL);
	put(t, 0x1fff8, 0x8400ec403ULL);
	ave_temp_compare(t, &in, &result);
	KUNIT_EXPECT_TRUE(test, result.model_valid);
	KUNIT_EXPECT_EQ(test, result.mismatches, 0U);
	w = ave_temp_walk(t, &in, 0xf0000, 0x8400f0000ULL, false);
	KUNIT_EXPECT_EQ(test, w.fault, AVE_TEMP_OK);
}

static struct kunit_case ave_temp_cases[] = {
	KUNIT_CASE(ave_temp_good_test),
	KUNIT_CASE(ave_temp_sparse_test),
	KUNIT_CASE(ave_temp_permissions_test),
	KUNIT_CASE(ave_temp_bad_leaf_test),
	KUNIT_CASE(ave_temp_root_test),
	KUNIT_CASE(ave_temp_block_test),
	KUNIT_CASE(ave_temp_geometry_test),
	KUNIT_CASE(ave_temp_ledger_test),
	KUNIT_CASE(ave_temp_relocated_model_test),
	{}
};

static struct kunit_suite ave_temp_suite = {
	.name = "apple-ave-temp",
	.test_cases = ave_temp_cases,
};

kunit_test_suite(ave_temp_suite);
MODULE_DESCRIPTION("Apple AVE read-only temporary stage-1 walker tests");
MODULE_LICENSE("GPL");

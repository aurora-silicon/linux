// SPDX-License-Identifier: GPL-2.0-only
/* Decode emitted operands without executing TLBI instructions. */
#include <kunit/test.h>
#include <kunit/test-bug.h>
#include <linux/mm.h>
#include <asm/tlb.h>

struct tlbi_test_state {
	u64 next;
	u64 end;
	u64 stride;
	u16 asid;
	u32 level;
	unsigned int shift;
	unsigned int single_ops;
	unsigned int range_ops;
	bool lpa2;
};

static unsigned int test_tg(unsigned int shift)
{
	return shift == 12 ? 1 : shift == 14 ? 2 : 3;
}

static void record_single(u64 arg)
{
	struct kunit *test = kunit_get_current_test();
	struct tlbi_test_state *state = test->priv;
	u64 address = (arg & GENMASK_ULL(43, 0)) << 12;
	u64 hint = 0;

	if (alternative_has_cap_unlikely(ARM64_HAS_ARMv8_4_TTL) && state->level <= 3)
		hint = (test_tg(state->shift) << 2) | state->level;
	KUNIT_EXPECT_EQ(test, address, state->next);
	KUNIT_EXPECT_EQ(test, arg >> 48, (u64)state->asid);
	KUNIT_EXPECT_EQ(test, FIELD_GET(TLBI_TTL_MASK, arg), hint);
	state->next += state->stride;
	state->single_ops++;
	KUNIT_EXPECT_LE(test, state->next, state->end);
}

static void record_range(u64 arg)
{
	struct kunit *test = kunit_get_current_test();
	struct tlbi_test_state *state = test->priv;
	u64 address = FIELD_GET(TLBIR_BADDR_MASK, arg) << (state->lpa2 ? 16 : state->shift);
	u64 count = (FIELD_GET(TLBIR_NUM_MASK, arg) + 1) <<
		    (5 * FIELD_GET(TLBIR_SCALE_MASK, arg) + 1);

	KUNIT_EXPECT_EQ(test, address, state->next);
	KUNIT_EXPECT_EQ(test, arg >> 48, (u64)state->asid);
	KUNIT_EXPECT_EQ(test, FIELD_GET(TLBIR_TG_MASK, arg), (u64)test_tg(state->shift));
	KUNIT_EXPECT_EQ(test, FIELD_GET(TLBIR_TTL_MASK, arg),
			state->level > 3 ? 0ULL : (u64)state->level);
	if (state->lpa2)
		KUNIT_EXPECT_TRUE(test, IS_ALIGNED(address, SZ_64K));
	state->next += count << state->shift;
	state->range_ops++;
	KUNIT_EXPECT_LE(test, state->next, state->end);
}

static bool range_supported(void)
{
	return true;
}

static bool range_unsupported(void)
{
	return false;
}

static void test_flush(struct kunit *test, unsigned int shift, u64 start,
		       unsigned long pages, u64 stride, u32 level, bool lpa2,
		       bool has_range)
{
	struct tlbi_test_state state = {
		.next = start,
		.end = start + (pages << shift),
		.stride = stride,
		.asid = 0xa55a,
		.level = level,
		.shift = shift,
		.lpa2 = lpa2,
	};

	test->priv = &state;
	__flush_tlb_range_op_granule(record_single, record_range, start, pages,
				   stride, state.asid, level, lpa2, shift,
				   has_range ? range_supported : range_unsupported);
	KUNIT_EXPECT_EQ(test, state.next, state.end);
	if (has_range) {
		KUNIT_EXPECT_LE(test, state.single_ops + state.range_ops, 20U);
		if (pages >= 32)
			KUNIT_EXPECT_GT(test, state.range_ops, 0U);
	} else {
		KUNIT_EXPECT_EQ(test, state.range_ops, 0U);
		KUNIT_EXPECT_EQ(test, state.single_ops, (unsigned int)((pages << shift) / stride));
	}
	test->priv = NULL;
}

static void user4k_tlbi_operands_test(struct kunit *test)
{
	static const unsigned long counts[] = {
		0, 1, 2, 3, 31, 32, 33, 63, 64, 65, 2047, 2048, 2049,
		65535, 65536, 65537, MAX_TLBI_RANGE_PAGES - 1, MAX_TLBI_RANGE_PAGES,
	};
	static const u32 levels[] = { 2, 3, TLBI_TTL_UNKNOWN };
	unsigned int shift, i, j;

	/* Force both algorithms using recording callbacks, independent of CPU caps. */
	for (shift = 12; shift <= 16; shift += 2) {
		u64 size = 1UL << shift;

		for (j = 0; j < ARRAY_SIZE(levels); j++) {
			for (i = 0; i < ARRAY_SIZE(counts); i++) {
				unsigned long pages = counts[i];

				test_flush(test, shift, SZ_4M + size, pages, size,
					   levels[j], false, true);
				if (shift != 16)
					test_flush(test, shift, SZ_4M + size, pages, size,
						   levels[j], true, true);
				if (pages <= 65)
					test_flush(test, shift, SZ_4M + size, pages, size,
						   levels[j], false, false);
			}
		}
		/* Preserve a larger leaf stride when range instructions are absent. */
		test_flush(test, shift, SZ_1G, 3 * 512, 512 * size, 2, false, false);
		/* Last virtual page must not lose high address bits. */
		test_flush(test, shift, (1ULL << 48) - size, 1, size, 3, false, false);
	}
}

static void user4k_tlb_gather_test(struct kunit *test)
{
	struct mm_struct *mm = kunit_kzalloc(test, sizeof(*mm), GFP_KERNEL);
	pte_t entries[3] = {};
	unsigned int small;

	KUNIT_ASSERT_NOT_NULL(test, mm);
	for (small = 0; small < 2; small++) {
		struct mmu_gather tlb = { .mm = mm, .start = ULONG_MAX };
		unsigned long size = small ? SZ_4K : PAGE_SIZE;
		unsigned long address = SZ_4M + size;

		mm->context.flags = small ? MMCF_USER_4K : 0;
		KUNIT_EXPECT_EQ(test, tlb_get_unmap_size(&tlb), size);
		tlb_remove_tlb_entries(&tlb, entries, 3, address);
		KUNIT_EXPECT_EQ(test, tlb.start, address);
		KUNIT_EXPECT_EQ(test, tlb.end, address + 3 * size);
		KUNIT_EXPECT_EQ(test, tlb_get_unmap_size(&tlb), size);
		KUNIT_EXPECT_EQ(test, tlb_get_level(&tlb), 3);
		tlb.cleared_pmds = 1;
		KUNIT_EXPECT_EQ(test, tlb_get_unmap_size(&tlb), size);
		KUNIT_EXPECT_EQ(test, tlb_get_level(&tlb), TLBI_TTL_UNKNOWN);
		tlb.cleared_ptes = 0;
		KUNIT_EXPECT_EQ(test, tlb_get_unmap_size(&tlb), small ? SZ_2M : PMD_SIZE);
		tlb.cleared_pmds = 0;
		tlb.cleared_puds = 1;
		KUNIT_EXPECT_EQ(test, tlb_get_unmap_size(&tlb), small ? SZ_1G : PUD_SIZE);
		tlb.cleared_puds = 0;
		tlb.cleared_p4ds = 1;
		KUNIT_EXPECT_EQ(test, tlb_get_unmap_size(&tlb), small ? 1UL << 39 : P4D_SIZE);
		tlb.freed_tables = 1;
		KUNIT_EXPECT_EQ(test, tlb_get_level(&tlb), TLBI_TTL_UNKNOWN);
	}
}

static struct kunit_case user4k_tlb_test_cases[] = {
	KUNIT_CASE(user4k_tlbi_operands_test),
	KUNIT_CASE(user4k_tlb_gather_test),
	{}
};

static struct kunit_suite user4k_tlb_test_suite = {
	.name = "arm64-user4k-tlb",
	.test_cases = user4k_tlb_test_cases,
};

kunit_test_suite(user4k_tlb_test_suite);

MODULE_LICENSE("GPL");

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Read-only DART2 (T8110 layout, 16 KiB pages) table decoding for AVE
 * diagnostics. Diagnostic only: the Apple DART driver owns these tables;
 * this never writes a table, a register or a TLB.
 *
 * Copyright The Aurora Silicon Contributors
 */
#ifndef _APPLE_AVE_DART_PTE_H_
#define _APPLE_AVE_DART_PTE_H_

#include <linux/bits.h>
#include <linux/math.h>
#include <linux/types.h>

#define AVE_DART_PAGE_SHIFT	14
#define AVE_DART_LEVEL_BITS	11	/* 2048 eight-byte entries per 16 KiB table */

/* DART2 leaf and table entries */
#define AVE_DART2_VALID		BIT_ULL(0)
#define AVE_DART2_NO_CACHE	BIT_ULL(1)
#define AVE_DART2_NO_WRITE	BIT_ULL(2)
#define AVE_DART2_NO_READ	BIT_ULL(3)
#define AVE_DART2_PADDR		GENMASK_ULL(37, 10)
#define AVE_DART2_PADDR_SHIFT	4

/* T8110 TTBR register: valid in bit 0, table PA >> 14 from bit 2 */
#define AVE_DART_TTBR_VALID	BIT(0)

struct ave_dart_leaf {
	bool valid, r, w, cache;
	u64 pa;
};

static inline u64 ave_dart_ttbr_pa(u32 ttbr)
{
	return (u64)(ttbr >> 2) << AVE_DART_PAGE_SHIFT;
}

static inline u64 ave_dart2_entry_pa(u64 pte)
{
	return (pte & AVE_DART2_PADDR) << AVE_DART2_PADDR_SHIFT;
}

static inline struct ave_dart_leaf ave_dart2_leaf(u64 pte)
{
	return (struct ave_dart_leaf){
		.valid = pte & AVE_DART2_VALID,
		.r = !(pte & AVE_DART2_NO_READ),
		.w = !(pte & AVE_DART2_NO_WRITE),
		.cache = !(pte & AVE_DART2_NO_CACHE),
		.pa = ave_dart2_entry_pa(pte),
	};
}

/* Table levels below the TTBR for an input address width (as io-pgtable-dart) */
static inline unsigned int ave_dart_levels(unsigned int va_width)
{
	unsigned int bits = va_width > AVE_DART_PAGE_SHIFT ? va_width - AVE_DART_PAGE_SHIFT : 0;
	unsigned int levels = DIV_ROUND_UP(bits, AVE_DART_LEVEL_BITS);

	return levels < 2 ? 2 : levels;
}

/* Index into the table of @level (0 is the leaf table) */
static inline unsigned int ave_dart_index(u64 iova, unsigned int level)
{
	return (iova >> (AVE_DART_PAGE_SHIFT + AVE_DART_LEVEL_BITS * level)) &
	       ((1U << AVE_DART_LEVEL_BITS) - 1);
}

/* The address bits the walk of @levels covers; above them must be zero */
static inline bool ave_dart_iova_in_range(u64 iova, unsigned int levels)
{
	unsigned int bits = AVE_DART_PAGE_SHIFT + AVE_DART_LEVEL_BITS * levels;

	return bits >= 64 || !(iova >> bits);
}

#endif

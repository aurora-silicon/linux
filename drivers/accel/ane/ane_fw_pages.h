/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_PAGES_H__
#define __ANE_FW_PAGES_H__

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/gfp_types.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/mm_types.h>
#include <linux/types.h>
#include <asm/page.h>

/*
 * Minimum page allocation order for an IOMMU domain's smallest granule, or
 * -EINVAL if @pgsize_bitmap has no page size set.
 */
static inline int ane_fw_pages_granule_order(unsigned long pgsize_bitmap)
{
	if (!pgsize_bitmap)
		return -EINVAL;
	return get_order(1UL << __ffs(pgsize_bitmap));
}

/*
 * The order of the next block of a discontiguous buffer with @remaining
 * pages to go, at most @order (the last order that worked) and at least
 * @min_order: the largest power of two the rest still fills. @remaining is
 * a nonzero multiple of 1 << @min_order.
 */
static inline unsigned int ane_fw_pages_order(unsigned long remaining, unsigned int order,
					      unsigned int min_order)
{
	return max(min_t(unsigned int, order, ilog2(remaining)), min_order);
}

struct page **ane_fw_pages_alloc(size_t size, unsigned int min_order,
				 unsigned int max_order, gfp_t gfp);
void ane_fw_pages_free(struct page **pages, size_t size);

#endif

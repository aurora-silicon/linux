// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Discontiguous backing for ANE buffers. The engine sees memory only through
 * its DARTs, so a buffer needs to be contiguous in the engine's address
 * space only: it is built from blocks of at least 1 << min_order pages (the
 * DART granule), each physically contiguous and naturally aligned, and the
 * caller maps them at consecutive DVAs. A large program then loads without a
 * contiguous allocation the size of the program (CMA, or the buddy
 * allocator's largest order).
 */
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "ane_fw_pages.h"

/**
 * ane_fw_pages_alloc - zeroed pages for a buffer the engine sees through its DARTs
 * @size: bytes, a nonzero multiple of PAGE_SIZE << @min_order
 * @min_order: order of the smallest block: the DART granule
 * @max_order: order of the largest block to try
 * @gfp: allocation flags of the smallest blocks (__GFP_ACCOUNT also charges
 *       the page array)
 *
 * Blocks start at the largest order the remaining size fills, as dma-iommu
 * builds its buffers, and fall back to smaller ones; an order that failed is
 * not tried again for this buffer. Only the smallest blocks reclaim as @gfp
 * permits: the larger ones are opportunistic (__GFP_NORETRY). No failure
 * warns: userspace chooses the size.
 *
 * Every block is split into order-0 pages, so each page has its own
 * reference, as vm_insert_pages() and vmap() want; pages[i * (1 << min_order)]
 * starts a physically contiguous block of at least 1 << min_order pages,
 * naturally aligned.
 *
 * Return: the page array (kvfree()d by ane_fw_pages_free()), or an ERR_PTR.
 */
struct page **ane_fw_pages_alloc(size_t size, unsigned int min_order,
				 unsigned int max_order, gfp_t gfp)
{
	unsigned long count = size >> PAGE_SHIFT, done = 0;
	unsigned int order;
	struct page **pages;

	if (!size || min_order > MAX_PAGE_ORDER ||
	    !IS_ALIGNED(size, PAGE_SIZE << min_order) || (gfp & __GFP_COMP))
		return ERR_PTR(-EINVAL);
	gfp |= __GFP_ZERO | __GFP_NOWARN;
	pages = kvcalloc(count, sizeof(*pages),
			 GFP_KERNEL | (gfp & __GFP_ACCOUNT) | __GFP_NOWARN);
	if (!pages)
		return ERR_PTR(-ENOMEM);
	order = clamp(max_order, min_order, (unsigned int)MAX_PAGE_ORDER);
	while (done < count) {
		struct page *page;

		order = ane_fw_pages_order(count - done, order, min_order);
		page = alloc_pages(order > min_order ?
				   (gfp & ~__GFP_RETRY_MAYFAIL) | __GFP_NORETRY : gfp,
				   order);
		if (!page) {
			if (order == min_order)
				goto fail;
			order--;
			continue;
		}
		if (order)
			split_page(page, order);
		for (unsigned long i = 0; i < 1UL << order; i++)
			pages[done + i] = page + i;
		done += 1UL << order;
	}
	return pages;

fail:
	ane_fw_pages_free(pages, done << PAGE_SHIFT);
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL_GPL(ane_fw_pages_alloc);

/* Free the first @size bytes of pages and the array; NULL is a no-op. */
void ane_fw_pages_free(struct page **pages, size_t size)
{
	if (!pages)
		return;
	for (unsigned long i = 0; i < size >> PAGE_SHIFT; i++)
		__free_page(pages[i]);
	kvfree(pages);
}
EXPORT_SYMBOL_GPL(ane_fw_pages_free);

MODULE_DESCRIPTION("Discontiguous page backing for Apple Neural Engine buffers");
MODULE_LICENSE("Dual MIT/GPL");

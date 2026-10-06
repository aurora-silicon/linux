// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/copypage.c
 *
 * Copyright (C) 2002 Deep Blue Solutions Ltd, All Rights Reserved.
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include <asm/page.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/mte.h>

void copy_highpage(struct page *to, struct page *from)
{
	void *kto = page_address(to);
	void *kfrom = page_address(from);
	struct folio *src = page_folio(from);
	struct folio *dst = page_folio(to);
	unsigned int i, nr_pages;

	copy_page(kto, kfrom);

	if (kasan_hw_tags_enabled())
		page_kasan_tag_reset(to);

	if (!system_supports_mte())
		return;

	if (folio_test_hugetlb(src)) {
		if (!folio_test_hugetlb_mte_tagged(src) ||
		    from != folio_page(src, 0))
			return;

		folio_try_hugetlb_mte_tagging(dst);

		/*
		 * Populate tags for all subpages.
		 *
		 * Don't assume the first page is head page since
		 * huge page copy may start from any subpage.
		 */
		nr_pages = folio_nr_pages(src);
		for (i = 0; i < nr_pages; i++) {
			kfrom = page_address(folio_page(src, i));
			kto = page_address(folio_page(dst, i));
			mte_copy_page_tags(kto, kfrom);
		}
		folio_set_hugetlb_mte_tagged(dst);
	} else if (page_mte_tagged(from)) {
		/*
		 * Most of the time it's a new page that shouldn't have been
		 * tagged yet. However, folio migration can end up reusing the
		 * same page without untagging it. Ignore the warning if the
		 * page is already tagged.
		 */
		try_page_mte_tagging(to);

		mte_copy_page_tags(kto, kfrom);
		set_page_mte_tagged(to);
	}
}
EXPORT_SYMBOL(copy_highpage);

void copy_user_highpage(struct page *to, struct page *from,
			unsigned long vaddr, struct vm_area_struct *vma)
{
	copy_highpage(to, from);
	flush_dcache_page(to);
}
EXPORT_SYMBOL_GPL(copy_user_highpage);

#ifdef CONFIG_MM_SUBPAGE
void clear_user_subpage_range(struct page *page, unsigned int offset, unsigned int size)
{
	void *addr = page_address(page) + offset;

	VM_BUG_ON(!is_power_of_2(size) || size < SZ_4K || size > PAGE_SIZE);
	VM_BUG_ON(!IS_ALIGNED(offset, size) || offset > PAGE_SIZE - size);
	memset(addr, 0, size);
	if (system_supports_mte() && page_mte_tagged(page))
		mte_clear_subpage_tags_range(addr, size);
	flush_dcache_page(page);
}

int copy_user_subpage_range(struct page *to, unsigned int to_offset,
			   struct page *from, unsigned int from_offset, unsigned int size)
{
	void *kto = page_address(to), *kfrom = page_address(from);

	VM_BUG_ON(!is_power_of_2(size) || size < SZ_4K || size > PAGE_SIZE);
	VM_BUG_ON(!IS_ALIGNED(to_offset, size) || to_offset > PAGE_SIZE - size);
	VM_BUG_ON(!IS_ALIGNED(from_offset, size) || from_offset > PAGE_SIZE - size);
	if (PageHWPoison(from) || copy_mc_to_kernel(kto + to_offset, kfrom + from_offset, size))
		return -EHWPOISON;
	if (kasan_hw_tags_enabled())
		page_kasan_tag_reset(to);
	if (system_supports_mte()) {
		if (page_mte_tagged(from)) {
			if (try_page_mte_tagging(to))
				mte_clear_page_tags(kto);
			mte_copy_subpage_tags_range(kto + to_offset, kfrom + from_offset, size);
			set_page_mte_tagged(to);
		} else if (page_mte_tagged(to)) {
			mte_clear_subpage_tags_range(kto + to_offset, size);
		}
	}
	flush_dcache_page(to);
	return 0;
}
/* Existing 4K fault and ownership callers keep their fixed-size contract. */
void clear_user_subpage(struct page *page, unsigned int offset)
{
	clear_user_subpage_range(page, offset, SZ_4K);
}

int copy_user_subpage(struct page *to, unsigned int to_offset,
		      struct page *from, unsigned int from_offset)
{
	return copy_user_subpage_range(to, to_offset, from, from_offset, SZ_4K);
}
#endif

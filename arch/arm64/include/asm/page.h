/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/page.h
 *
 * Copyright (C) 1995-2003 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_PAGE_H
#define __ASM_PAGE_H

#include <asm/page-def.h>

#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
/* A shared zero mapping must cover the largest supported user granule. */
#define ARCH_ZERO_PAGE_SIZE (1UL << 16)
#endif

#ifndef __ASSEMBLER__

#include <linux/personality.h> /* for READ_IMPLIES_EXEC */
#include <linux/types.h> /* for gfp_t */
#include <asm/pgtable-types.h>

struct page;
struct vm_area_struct;

extern void copy_page(void *to, const void *from);
extern void clear_page(void *to);

void copy_user_highpage(struct page *to, struct page *from,
			unsigned long vaddr, struct vm_area_struct *vma);
#define __HAVE_ARCH_COPY_USER_HIGHPAGE

#ifdef CONFIG_MM_SUBPAGE
void clear_user_subpage_range(struct page *page, unsigned int offset,
			     unsigned int size);
int copy_user_subpage_range(struct page *to, unsigned int to_offset,
			   struct page *from, unsigned int from_offset,
			   unsigned int size);
void clear_user_subpage(struct page *page, unsigned int offset);
int copy_user_subpage(struct page *to, unsigned int to_offset,
		      struct page *from, unsigned int from_offset);
#endif

void copy_highpage(struct page *to, struct page *from);
#define __HAVE_ARCH_COPY_HIGHPAGE

struct folio *vma_alloc_zeroed_movable_folio_order(struct vm_area_struct *vma,
					 unsigned long vaddr, unsigned int order);
#define vma_alloc_zeroed_movable_folio_order vma_alloc_zeroed_movable_folio_order
static inline struct folio *vma_alloc_zeroed_movable_folio(struct vm_area_struct *vma,
						unsigned long vaddr)
{
	return vma_alloc_zeroed_movable_folio_order(vma, vaddr, 0);
}
#define vma_alloc_zeroed_movable_folio vma_alloc_zeroed_movable_folio

bool tag_clear_highpages(struct page *to, int numpages, bool clear_pages);
#define __HAVE_ARCH_TAG_CLEAR_HIGHPAGES

#define copy_user_page(to, from, vaddr, pg)	copy_page(to, from)

#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
/* A table may occupy a 4K fragment of a native 16K physical page. */
typedef pte_t *pgtable_t;
#define pgtable_to_page(table) virt_to_page(table)
#define pgtable_from_page(page) ((page) ? (pgtable_t)page_address(page) : NULL)
#else
typedef struct page *pgtable_t;
#endif

int pfn_is_map_memory(unsigned long pfn);

#include <asm/memory.h>

#endif /* !__ASSEMBLER__ */

#ifdef CONFIG_ARM64_MTE
#define VMA_DATA_DEFAULT_FLAGS	append_vma_flags(VMA_DATA_FLAGS_TSK_EXEC, \
						 VMA_MTE_ALLOWED_BIT)
#else
#define VMA_DATA_DEFAULT_FLAGS	VMA_DATA_FLAGS_TSK_EXEC
#endif

#include <asm-generic/getorder.h>

#endif

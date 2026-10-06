/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_USER_PTE_H
#define __ASM_USER_PTE_H

/* Included after native leaf accessors, before their batched setters. */
#define ARM64_USER4K_TABLE_ADDR_MASK GENMASK_ULL(47, 12)

static inline bool arm64_mm_user4k(const struct mm_struct *mm)
{
	return mm && arm64_context_user4k(&mm->context);
}

static inline bool arm64_mm_alt_granule(const struct mm_struct *mm)
{
	return mm && arm64_context_page_shift(&mm->context) != PAGE_SHIFT;
}

static inline u64 arm64_user_leaf_addr_mask(const struct mm_struct *mm)
{
	return ARM64_USER4K_TABLE_ADDR_MASK & mm_page_mask(mm);
}

/* PFNs still identify native pages; leaf physical addresses identify slots. */
static inline phys_addr_t pte_phys_mm(const struct mm_struct *mm, pte_t pte)
{
	if (arm64_mm_alt_granule(mm))
		return pte_val(pte) & arm64_user_leaf_addr_mask(mm);
	return __pte_to_phys(pte);
}
#define pte_phys_mm pte_phys_mm

static inline pte_t phys_pte_mm(const struct mm_struct *mm, phys_addr_t phys,
			       pgprot_t prot)
{
	if (arm64_mm_alt_granule(mm)) {
		VM_BUG_ON(phys & ~arm64_user_leaf_addr_mask(mm));
		return __pte(phys | pgprot_val(prot));
	}
	return pfn_pte(phys >> PAGE_SHIFT, prot);
}
#define phys_pte_mm phys_pte_mm

static inline pgprot_t pte_pgprot_mm(const struct mm_struct *mm, pte_t pte)
{
	if (arm64_mm_alt_granule(mm))
		return __pgprot(pte_val(pte) & ~arm64_user_leaf_addr_mask(mm));
	return pte_pgprot(pte);
}
#define pte_pgprot_mm pte_pgprot_mm


#ifdef CONFIG_MM_SUBPAGE
/* Slot bits belong to the PTE, not the native swap-cache index. */
#define ARM64_SWP_SUBPAGE_MASK (GENMASK_ULL(5, 4) | GENMASK_ULL(63, 62))

static inline unsigned int pte_swp_subpage_offset(pte_t pte)
{
	return ((((pte_val(pte) >> 4) & 3) |
		 ((pte_val(pte) >> 60) & 12)) << 12);
}

static inline pte_t pte_swp_set_subpage_offset(pte_t pte, unsigned int offset)
{
	VM_BUG_ON(!IS_ALIGNED(offset, SZ_4K) || offset >= PAGE_SIZE);
	return __pte((pte_val(pte) & ~ARM64_SWP_SUBPAGE_MASK) |
		     (((u64)(offset >> 12) & 3) << 4) |
		     (((u64)(offset >> 12) & 12) << 60));
}
#endif

static inline pte_t pte_advance_pfn_mm(const struct mm_struct *mm, pte_t pte,
				     unsigned long nr)
{
	if (arm64_mm_alt_granule(mm))
		return phys_pte_mm(mm, pte_phys_mm(mm, pte) + (nr << mm_page_shift(mm)),
				   pte_pgprot_mm(mm, pte));
	return pte_advance_pfn(pte, nr);
}

#define pte_advance_pfn_mm pte_advance_pfn_mm

#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
unsigned int arm64_ptep_page_shift(pte_t *ptep);
#else
static inline unsigned int arm64_ptep_page_shift(pte_t *ptep)
{
	return PAGE_SHIFT;
}
#endif

static inline unsigned int arm64_cont_ptes(unsigned int shift)
{
	/* Architectural contiguous group lengths for 4K, 16K and 64K. */
	return shift == 12 ? 16 : shift == 14 ? 128 : 32;
}

static inline unsigned int cont_ptes_mm(const struct mm_struct *mm)
{
	return arm64_cont_ptes(mm_page_shift(mm));
}

static inline unsigned long cont_pte_size_mm(const struct mm_struct *mm)
{
	return (unsigned long)cont_ptes_mm(mm) << mm_page_shift(mm);
}

static inline unsigned long pte_cont_addr_end_mm(const struct mm_struct *mm,
						unsigned long addr, unsigned long end)
{
	unsigned long size = cont_pte_size_mm(mm);
	unsigned long boundary = (addr + size) & ~(size - 1);

	return boundary - 1 < end - 1 ? boundary : end;
}

#endif

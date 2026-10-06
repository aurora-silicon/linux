/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_USER_PGTABLE_H
#define __ASM_USER_PGTABLE_H

/* Included by asm/pgtable.h after the native accessors. */
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL

/* Alternative tables use 48-bit VA; 64K folds the PUD level. */
static inline unsigned int arm64_user_level_shift(const struct mm_struct *mm,
						 unsigned int level)
{
	unsigned int shift = mm_page_shift(mm);

	if (shift == 16 && !level)
		level = 1;
	return shift + (3 - level) * (shift - 3);
}

static inline unsigned long arm64_user_table_index(const struct mm_struct *mm,
					 unsigned long addr, unsigned int level)
{
	unsigned int shift = arm64_user_level_shift(mm, level);
	unsigned int bits = level ? mm_page_shift(mm) - 3 : 48 - shift;

	return (addr >> shift) & ((1UL << bits) - 1);
}

#define p4d_none_mm(mm, p4d) \
	(arm64_mm_alt_granule(mm) ? (!mm_pud_folded(mm) && !p4d_val(p4d)) : p4d_none(p4d))
#define p4d_bad_mm(mm, p4d) \
	(arm64_mm_alt_granule(mm) ? (!mm_pud_folded(mm) && ((p4d_val(p4d) & P4D_TYPE_MASK) != P4D_TYPE_TABLE)) : p4d_bad(p4d))
#define p4d_present_mm(mm, p4d) (!p4d_none_mm(mm, p4d))
static inline void p4d_clear_mm(struct mm_struct *mm, p4d_t *p4d)
{
	if (arm64_mm_alt_granule(mm)) {
		if (!mm_pud_folded(mm))
			set_p4d(p4d, __p4d(0));
	} else {
		p4d_clear(p4d);
	}
}
#define p4d_clear_mm p4d_clear_mm


/* pgtable_t preserves the fragment offset; pmd_page() remains its owner. */
#define pmd_pgtable(pmd) ((pgtable_t)__va(pmd_val(pmd) & ARM64_USER4K_TABLE_ADDR_MASK))

static inline void *arm64_user4k_table(u64 descriptor)
{
	/* Unlike native __pXd_to_phys(), retain sub-native table address bits. */
	return __va(descriptor & ARM64_USER4K_TABLE_ADDR_MASK);
}

static inline pgd_t *pgd_offset_pgd_mm(struct mm_struct *mm, pgd_t *pgd,
				       unsigned long addr)
{
	if (arm64_mm_alt_granule(mm))
		return pgd + arm64_user_table_index(mm, addr, 0);
	return pgd + ((addr >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1));
}

#define pgd_offset_pgd_mm pgd_offset_pgd_mm

static inline pgd_t *pgd_offset_mm(struct mm_struct *mm, unsigned long addr)
{
	return pgd_offset_pgd_mm(mm, mm->pgd, addr);
}

#define pgd_offset(mm, addr) pgd_offset_mm(mm, addr)

static inline p4d_t *p4d_offset_mm(struct mm_struct *mm, pgd_t *pgd,
				   unsigned long addr)
{
	if (arm64_mm_alt_granule(mm))
		return (p4d_t *)pgd;
	return p4d_offset(pgd, addr);
}

#define p4d_offset_mm p4d_offset_mm

static inline pud_t *pud_offset_mm(struct mm_struct *mm, p4d_t *p4d,
				   unsigned long addr)
{
	if (arm64_mm_alt_granule(mm)) {
		if (mm_pud_folded(mm))
			return (pud_t *)p4d;
		return (pud_t *)arm64_user4k_table(p4d_val(READ_ONCE(*p4d))) +
			arm64_user_table_index(mm, addr, 1);
	}
	return pud_offset(p4d, addr);
}

#define pud_offset_mm pud_offset_mm

static inline pmd_t *pud_pgtable_mm(struct mm_struct *mm, pud_t pud)
{
	if (arm64_mm_alt_granule(mm))
		return arm64_user4k_table(pud_val(pud));
	return pud_pgtable(pud);
}

#define pud_pgtable_mm pud_pgtable_mm

static inline pmd_t *pmd_offset_mm(struct mm_struct *mm, pud_t *pud,
				   unsigned long addr)
{
	if (arm64_mm_alt_granule(mm))
		return (pmd_t *)arm64_user4k_table(pud_val(READ_ONCE(*pud))) +
			arm64_user_table_index(mm, addr, 2);
	return (pmd_t *)pud_pgtable(READ_ONCE(*pud)) +
		((addr >> PMD_SHIFT) & (PTRS_PER_PMD - 1));
}

#define pmd_offset_mm pmd_offset_mm

static inline pte_t *pte_offset_kernel_mm(struct mm_struct *mm, pmd_t *pmd,
					  unsigned long addr)
{
	if (arm64_mm_alt_granule(mm))
		return (pte_t *)arm64_user4k_table(pmd_val(READ_ONCE(*pmd))) +
			arm64_user_table_index(mm, addr, 3);
	return (pte_t *)pmd_page_vaddr(READ_ONCE(*pmd)) +
		((addr >> PAGE_SHIFT) & (PTRS_PER_PTE - 1));
}

#define pte_offset_kernel_mm pte_offset_kernel_mm
#define __pte_map_mm(mm, pmd, addr) pte_offset_kernel_mm(mm, pmd, addr)

#define pmd_shift_mm(mm) (arm64_mm_alt_granule(mm) ? arm64_user_level_shift(mm, 2) : PMD_SHIFT)
#define pud_shift_mm(mm) (arm64_mm_alt_granule(mm) ? arm64_user_level_shift(mm, 1) : PUD_SHIFT)
#define p4d_shift_mm(mm) (arm64_mm_alt_granule(mm) ? arm64_user_level_shift(mm, 0) : P4D_SHIFT)
#define pmd_size_mm(mm) (1UL << pmd_shift_mm(mm))
#define pud_size_mm(mm) (1UL << pud_shift_mm(mm))
#define p4d_size_mm(mm) (1UL << p4d_shift_mm(mm))
#define pgd_size_mm(mm) (arm64_mm_alt_granule(mm) ? (1UL << arm64_user_level_shift(mm, 0)) : PGDIR_SIZE)
#define pte_table_bytes_mm(mm) (arm64_mm_alt_granule(mm) ? mm_page_size(mm) : PTRS_PER_PTE * sizeof(pte_t))
#define pmd_table_bytes_mm(mm) (arm64_mm_alt_granule(mm) ? mm_page_size(mm) : PTRS_PER_PMD * sizeof(pmd_t))
#define pud_table_bytes_mm(mm) (arm64_mm_alt_granule(mm) ? mm_page_size(mm) : PTRS_PER_PUD * sizeof(pud_t))
#define pmd_mask_mm(mm) (~(pmd_size_mm(mm) - 1))
#define pud_mask_mm(mm) (~(pud_size_mm(mm) - 1))
#define p4d_mask_mm(mm) (~(p4d_size_mm(mm) - 1))
#define pgd_mask_mm(mm) (~(pgd_size_mm(mm) - 1))

static inline unsigned long arm64_user4k_addr_end(unsigned long addr,
						  unsigned long end, unsigned int shift)
{
	unsigned long size = 1UL << shift;
	unsigned long boundary = (addr + size) & ~(size - 1);

	return boundary - 1 < end - 1 ? boundary : end;
}

/* Native fallbacks retain their architecture-specific folding semantics. */
#define pgd_addr_end_mm(mm, addr, end) \
	(arm64_mm_alt_granule(mm) ? arm64_user4k_addr_end(addr, end, arm64_user_level_shift(mm, 0)) : pgd_addr_end(addr, end))
#define p4d_addr_end_mm(mm, addr, end) \
	(arm64_mm_alt_granule(mm) ? (end) : p4d_addr_end(addr, end))
#define pud_addr_end_mm(mm, addr, end) \
	(arm64_mm_alt_granule(mm) ? arm64_user4k_addr_end(addr, end, arm64_user_level_shift(mm, 1)) : pud_addr_end(addr, end))
#define pmd_addr_end_mm(mm, addr, end) \
	(arm64_mm_alt_granule(mm) ? arm64_user4k_addr_end(addr, end, arm64_user_level_shift(mm, 2)) : pmd_addr_end(addr, end))

#endif /* CONFIG_ARM64_USER4K_EXPERIMENTAL */
#endif

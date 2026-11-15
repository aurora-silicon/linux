/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/pgalloc.h
 *
 * Copyright (C) 2000-2001 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_PGALLOC_H
#define __ASM_PGALLOC_H

#include <asm/pgtable-hwdef.h>
#include <asm/processor.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#define __HAVE_ARCH_PGD_FREE
#define __HAVE_ARCH_PUD_FREE
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
#define __HAVE_ARCH_PTE_ALLOC_ONE
#define __HAVE_ARCH_PTE_FREE
#define __HAVE_ARCH_PMD_ALLOC_ONE
#define __HAVE_ARCH_PMD_FREE
#define __HAVE_ARCH_PUD_ALLOC_ONE
#endif
#include <asm-generic/pgalloc.h>
#include <asm/user-pgalloc.h>

#define PGD_SIZE	(PTRS_PER_PGD * sizeof(pgd_t))

#if CONFIG_PGTABLE_LEVELS > 2

static inline void __pud_populate(pud_t *pudp, phys_addr_t pmdp, pudval_t prot)
{
	set_pud(pudp, __pud(__phys_to_pud_val(pmdp) | prot));
}

static inline void pud_populate(struct mm_struct *mm, pud_t *pudp, pmd_t *pmdp)
{
	pudval_t pudval = PUD_TYPE_TABLE | PUD_TABLE_AF;

	pudval |= (mm == &init_mm) ? PUD_TABLE_UXN : PUD_TABLE_PXN;
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
	if (arm64_mm_alt_granule(mm)) {
		set_pud(pudp, __pud(__pa(pmdp) | pudval));
		return;
	}
#endif
	__pud_populate(pudp, __pa(pmdp), pudval);
}
#else
static inline void __pud_populate(pud_t *pudp, phys_addr_t pmdp, pudval_t prot)
{
	BUILD_BUG();
}
#endif	/* CONFIG_PGTABLE_LEVELS > 2 */

#if CONFIG_PGTABLE_LEVELS > 3

static inline void __p4d_populate(p4d_t *p4dp, phys_addr_t pudp, p4dval_t prot)
{
	if (pgtable_l4_enabled())
		set_p4d(p4dp, __p4d(__phys_to_p4d_val(pudp) | prot));
}

static inline void p4d_populate(struct mm_struct *mm, p4d_t *p4dp, pud_t *pudp)
{
	p4dval_t p4dval = P4D_TYPE_TABLE | P4D_TABLE_AF;

	p4dval |= (mm == &init_mm) ? P4D_TABLE_UXN : P4D_TABLE_PXN;
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
	if (arm64_mm_alt_granule(mm)) {
		set_p4d(p4dp, __p4d(__pa(pudp) | p4dval));
		return;
	}
#endif
	__p4d_populate(p4dp, __pa(pudp), p4dval);
}

static inline void pud_free(struct mm_struct *mm, pud_t *pud)
{
	if (mm_pud_folded(mm))
		return;
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
	arm64_pgtable_free(pud);
#else
	__pud_free(mm, pud);
#endif
}
#else
static inline void __p4d_populate(p4d_t *p4dp, phys_addr_t pudp, p4dval_t prot)
{
	BUILD_BUG();
}
#endif	/* CONFIG_PGTABLE_LEVELS > 3 */

#if CONFIG_PGTABLE_LEVELS > 4

static inline void __pgd_populate(pgd_t *pgdp, phys_addr_t p4dp, pgdval_t prot)
{
	if (pgtable_l5_enabled())
		set_pgd(pgdp, __pgd(__phys_to_pgd_val(p4dp) | prot));
}

static inline void pgd_populate(struct mm_struct *mm, pgd_t *pgdp, p4d_t *p4dp)
{
	pgdval_t pgdval = PGD_TYPE_TABLE | PGD_TABLE_AF;

	pgdval |= (mm == &init_mm) ? PGD_TABLE_UXN : PGD_TABLE_PXN;
	__pgd_populate(pgdp, __pa(p4dp), pgdval);
}

#else
static inline void __pgd_populate(pgd_t *pgdp, phys_addr_t p4dp, pgdval_t prot)
{
	BUILD_BUG();
}
#endif	/* CONFIG_PGTABLE_LEVELS > 4 */

extern pgd_t *pgd_alloc(struct mm_struct *mm);
extern void pgd_free(struct mm_struct *mm, pgd_t *pgdp);

static inline void __pmd_populate(pmd_t *pmdp, phys_addr_t ptep,
				  pmdval_t prot)
{
	set_pmd(pmdp, __pmd(__phys_to_pmd_val(ptep) | prot));
}

/*
 * Populate the pmdp entry with a pointer to the pte.  This pmd is part
 * of the mm address space.
 */
static inline void
pmd_populate_kernel(struct mm_struct *mm, pmd_t *pmdp, pte_t *ptep)
{
	VM_BUG_ON(mm && mm != &init_mm);
	__pmd_populate(pmdp, __pa(ptep),
		       PMD_TYPE_TABLE | PMD_TABLE_AF | PMD_TABLE_UXN);
}

static inline void
pmd_populate(struct mm_struct *mm, pmd_t *pmdp, pgtable_t ptep)
{
	VM_BUG_ON(mm == &init_mm);
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
	if (arm64_mm_alt_granule(mm)) {
		set_pmd(pmdp, __pmd(__pa(ptep) | PMD_TYPE_TABLE |
				    PMD_TABLE_AF | PMD_TABLE_PXN));
		return;
	}
	__pmd_populate(pmdp, __pa(ptep),
#else
	__pmd_populate(pmdp, page_to_phys(ptep),
#endif
		       PMD_TYPE_TABLE | PMD_TABLE_AF | PMD_TABLE_PXN);
}

#endif

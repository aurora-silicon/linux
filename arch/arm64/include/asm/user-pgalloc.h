/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_USER_PGALLOC_H
#define __ASM_USER_PGALLOC_H

#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL

enum arm64_user4k_pt_level {
	USER4K_PT_PTE,
	USER4K_PT_PMD,
	USER4K_PT_PUD,
};

/* Explicit geometry entry point; callers must keep each mm's granule fixed. */
void *arm64_user_pt_alloc_granule(struct mm_struct *mm,
		enum arm64_user4k_pt_level level, unsigned int shift);
void *arm64_user4k_pt_alloc(struct mm_struct *mm, enum arm64_user4k_pt_level level);
void arm64_user4k_pt_free(void *table);
void arm64_user4k_pt_cache_destroy(struct mm_struct *mm);
void arm64_pgtable_free(void *table);
void arm64_pte_free_defer(struct mm_struct *mm, pgtable_t table);

static inline pgtable_t pte_alloc_one_noprof(struct mm_struct *mm)
{
	if (arm64_mm_alt_granule(mm))
		return arm64_user_pt_alloc_granule(mm, USER4K_PT_PTE, mm_page_shift(mm));
	return __pte_alloc_one_noprof(mm, GFP_PGTABLE_USER);
}
#define pte_alloc_one(...) alloc_hooks(pte_alloc_one_noprof(__VA_ARGS__))

static inline void pte_free(struct mm_struct *mm, pgtable_t table)
{
	arm64_pgtable_free(table);
}

static inline pmd_t *pmd_alloc_one_noprof(struct mm_struct *mm, unsigned long addr)
{
	if (arm64_mm_alt_granule(mm))
		return arm64_user_pt_alloc_granule(mm, USER4K_PT_PMD, mm_page_shift(mm));
	return __pmd_alloc_one_noprof(mm, addr);
}
#define pmd_alloc_one(...) alloc_hooks(pmd_alloc_one_noprof(__VA_ARGS__))

static inline void pmd_free(struct mm_struct *mm, pmd_t *table)
{
	arm64_pgtable_free(table);
}

static inline pud_t *pud_alloc_one_noprof(struct mm_struct *mm, unsigned long addr)
{
	if (arm64_mm_alt_granule(mm))
		return arm64_user_pt_alloc_granule(mm, USER4K_PT_PUD, mm_page_shift(mm));
	return __pud_alloc_one_noprof(mm, addr);
}
#define pud_alloc_one(...) alloc_hooks(pud_alloc_one_noprof(__VA_ARGS__))

#define pte_free_defer arm64_pte_free_defer

#endif
#endif

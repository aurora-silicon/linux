// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2021, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#include <kunit/visibility.h>
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/page_table_check.h>
#include <linux/swap.h>
#include <linux/leafops.h>

#undef pr_fmt
#define pr_fmt(fmt)	"page_table_check: " fmt

#ifdef CONFIG_MM_SUBPAGE
#define PAGE_TABLE_CHECK_SLOTS (PAGE_SIZE / SZ_4K)
#else
#define PAGE_TABLE_CHECK_SLOTS 1
#endif

struct page_table_check_slot {
	atomic_t anon_map_count;
	atomic_t file_map_count;
};

struct page_table_check {
	struct page_table_check_slot slots[PAGE_TABLE_CHECK_SLOTS];
};

static bool __page_table_check_enabled __initdata =
				IS_ENABLED(CONFIG_PAGE_TABLE_CHECK_ENFORCED);

DEFINE_STATIC_KEY_TRUE(page_table_check_disabled);
EXPORT_SYMBOL(page_table_check_disabled);

static int __init early_page_table_check_param(char *buf)
{
	return kstrtobool(buf, &__page_table_check_enabled);
}

early_param("page_table_check", early_page_table_check_param);

static bool __init need_page_table_check(void)
{
	return __page_table_check_enabled;
}

static void __init init_page_table_check(void)
{
	if (!__page_table_check_enabled)
		return;
	static_branch_disable(&page_table_check_disabled);
}

struct page_ext_operations page_table_check_ops = {
	.size = sizeof(struct page_table_check),
	.need = need_page_table_check,
	.init = init_page_table_check,
	.need_shared_flags = false,
};

static struct page_table_check *get_page_table_check(struct page_ext *page_ext)
{
	BUG_ON(!page_ext);
	return page_ext_data(page_ext, &page_table_check_ops);
}

#if IS_ENABLED(CONFIG_KUNIT)
/* Diagnostic snapshot for private test allocations, not a synchronization API. */
int page_table_check_get_counts(struct page *page, unsigned int offset,
			       int *anon, int *file)
{
	struct page_ext *ext;
	struct page_table_check *ptc;
	unsigned int slot = 0;

	if (static_branch_likely(&page_table_check_disabled))
		return -EOPNOTSUPP;
	if (offset >= PAGE_SIZE)
		return -EINVAL;
#ifdef CONFIG_MM_SUBPAGE
	slot = offset / SZ_4K;
#endif
	ext = page_ext_get(page);
	if (!ext)
		return -ENOENT;
	ptc = get_page_table_check(ext);
	*anon = atomic_read(&ptc->slots[slot].anon_map_count);
	*file = atomic_read(&ptc->slots[slot].file_map_count);
	page_ext_put(ext);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(page_table_check_get_counts);
#endif

static void page_table_check_update(struct page_table_check_slot *slot,
				    bool anon, bool clear, bool rw)
{
	if (anon) {
		BUG_ON(atomic_read(&slot->file_map_count));
		if (clear)
			BUG_ON(atomic_dec_return(&slot->anon_map_count) < 0);
		else
			BUG_ON(atomic_inc_return(&slot->anon_map_count) > 1 && rw);
	} else {
		BUG_ON(atomic_read(&slot->anon_map_count));
		if (clear)
			BUG_ON(atomic_dec_return(&slot->file_map_count) < 0);
		else
			BUG_ON(atomic_inc_return(&slot->file_map_count) < 0);
	}
}

#ifdef CONFIG_MM_SUBPAGE
static void page_table_check_subpage(phys_addr_t phys, unsigned long size,
				     bool clear, bool rw)
{
	unsigned long pfn = PHYS_PFN(phys);
	struct page_table_check *ptc;
	struct page_ext *ext;
	struct page *page;
	unsigned int first = offset_in_page(phys) / SZ_4K, i;

	if (!pfn_valid(pfn))
		return;
	page = pfn_to_page(pfn);
	BUG_ON(PageSlab(page));
	ext = page_ext_get(page);
	ptc = get_page_table_check(ext);
	for (i = first; i < first + size / SZ_4K; i++)
		page_table_check_update(&ptc->slots[i], PageAnon(page), clear, rw);
	page_ext_put(ext);
}
#endif

/*
 * An entry is removed from the page table, decrement the counters for that page
 * verify that it is of correct type and counters do not become negative.
 */
static void page_table_check_clear(unsigned long pfn, unsigned long pgcnt)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;
	struct page *page;
	bool anon;

	if (!pfn_valid(pfn))
		return;

	page = pfn_to_page(pfn);
	BUG_ON(PageSlab(page));
	anon = PageAnon(page);

	rcu_read_lock();
	for_each_page_ext(page, pgcnt, page_ext, iter) {
		struct page_table_check *ptc = get_page_table_check(page_ext);

		for (unsigned int i = 0; i < PAGE_TABLE_CHECK_SLOTS; i++)
			page_table_check_update(&ptc->slots[i], anon, true, false);
	}
	rcu_read_unlock();
}

/*
 * A new entry is added to the page table, increment the counters for that page
 * verify that it is of correct type and is not being mapped with a different
 * type to a different process.
 */
static void page_table_check_set(unsigned long pfn, unsigned long pgcnt,
				 bool rw)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;
	struct page *page;
	bool anon;

	if (!pfn_valid(pfn))
		return;

	page = pfn_to_page(pfn);
	BUG_ON(PageSlab(page));
	anon = PageAnon(page);

	rcu_read_lock();
	for_each_page_ext(page, pgcnt, page_ext, iter) {
		struct page_table_check *ptc = get_page_table_check(page_ext);

		for (unsigned int i = 0; i < PAGE_TABLE_CHECK_SLOTS; i++)
			page_table_check_update(&ptc->slots[i], anon, false, rw);
	}
	rcu_read_unlock();
}

/*
 * page is on free list, or is being allocated, verify that counters are zeroes
 * crash if they are not.
 */
void __page_table_check_zero(struct page *page, unsigned int order)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;

	BUG_ON(PageSlab(page));

	rcu_read_lock();
	for_each_page_ext(page, 1 << order, page_ext, iter) {
		struct page_table_check *ptc = get_page_table_check(page_ext);

		for (unsigned int i = 0; i < PAGE_TABLE_CHECK_SLOTS; i++) {
			BUG_ON(atomic_read(&ptc->slots[i].anon_map_count));
			BUG_ON(atomic_read(&ptc->slots[i].file_map_count));
		}
	}
	rcu_read_unlock();
}

void __page_table_check_pte_clear(struct mm_struct *mm, unsigned long addr,
				  pte_t pte)
{
	if (&init_mm == mm)
		return;

	if (!pte_user_accessible_page(mm, addr, pte) || pte_special(pte))
		return;
#ifdef CONFIG_MM_SUBPAGE
	if (mm_page_size(mm) < PAGE_SIZE) {
		page_table_check_subpage(pte_phys_mm(mm, pte), mm_page_size(mm), true, false);
		return;
	}
#endif
	page_table_check_clear(pte_pfn(pte), mm_pte_native_pages(mm));
}
EXPORT_SYMBOL(__page_table_check_pte_clear);

static inline bool page_table_check_huge_zero_pmd(pmd_t pmd)
{
	unsigned long pfn = pmd_pfn(pmd);

	if (!pfn_valid(pfn))
		return false;

	return is_huge_zero_folio(page_folio(pfn_to_page(pfn)));
}

void __page_table_check_pmd_clear(struct mm_struct *mm, unsigned long addr,
				  pmd_t pmd)
{
	if (&init_mm == mm)
		return;

	if (pmd_user_accessible_page(mm, addr, pmd) &&
	    !page_table_check_huge_zero_pmd(pmd))
		page_table_check_clear(pmd_pfn(pmd), PMD_SIZE >> PAGE_SHIFT);
}
EXPORT_SYMBOL(__page_table_check_pmd_clear);

void __page_table_check_pud_clear(struct mm_struct *mm, unsigned long addr,
				  pud_t pud)
{
	if (&init_mm == mm)
		return;

	if (pud_user_accessible_page(mm, addr, pud))
		page_table_check_clear(pud_pfn(pud), PUD_SIZE >> PAGE_SHIFT);
}
EXPORT_SYMBOL(__page_table_check_pud_clear);

/* Whether the swap entry cached writable information */
static inline bool softleaf_cached_writable(softleaf_t entry)
{
	return softleaf_is_device_private_write(entry) ||
		softleaf_is_migration_write(entry);
}

static void page_table_check_pte_flags(pte_t pte)
{
	if (pte_present(pte)) {
		WARN_ON_ONCE(pte_uffd_wp(pte) && pte_write(pte));
	} else if (pte_swp_uffd_wp(pte)) {
		const softleaf_t entry = softleaf_from_pte(pte);

		WARN_ON_ONCE(softleaf_cached_writable(entry));
	}
}

void __page_table_check_ptes_set(struct mm_struct *mm, unsigned long addr,
				 pte_t *ptep, pte_t pte, unsigned int nr)
{
	unsigned int i;

	if (&init_mm == mm)
		return;

	page_table_check_pte_flags(pte);

	for (i = 0; i < nr; i++)
		__page_table_check_pte_clear(mm, addr + mm_page_size(mm) * i, ptep_get(ptep + i));
	if (!pte_user_accessible_page(mm, addr, pte) || pte_special(pte))
		return;
#ifdef CONFIG_MM_SUBPAGE
	if (mm_page_size(mm) < PAGE_SIZE) {
		phys_addr_t phys = pte_phys_mm(mm, pte);

		for (i = 0; i < nr; i++)
			page_table_check_subpage(phys + i * mm_page_size(mm), mm_page_size(mm), false,
						pte_write(pte));
		return;
	}
#endif
	page_table_check_set(pte_pfn(pte), nr * mm_pte_native_pages(mm), pte_write(pte));
}
EXPORT_SYMBOL(__page_table_check_ptes_set);

static inline void page_table_check_pmd_flags(pmd_t pmd)
{
	if (pmd_present(pmd)) {
		if (pmd_uffd_wp(pmd))
			WARN_ON_ONCE(pmd_write(pmd));
	} else if (pmd_swp_uffd_wp(pmd)) {
		const softleaf_t entry = softleaf_from_pmd(pmd);

		WARN_ON_ONCE(softleaf_cached_writable(entry));
	}
}

void __page_table_check_pmds_set(struct mm_struct *mm, unsigned long addr,
		pmd_t *pmdp, pmd_t pmd, unsigned int nr)
{
	unsigned long stride = PMD_SIZE >> PAGE_SHIFT;
	unsigned int i;

	if (&init_mm == mm)
		return;

	page_table_check_pmd_flags(pmd);

	for (i = 0; i < nr; i++)
		__page_table_check_pmd_clear(mm, addr + PMD_SIZE * i, *(pmdp + i));
	if (pmd_user_accessible_page(mm, addr, pmd) &&
	    !page_table_check_huge_zero_pmd(pmd))
		page_table_check_set(pmd_pfn(pmd), stride * nr, pmd_write(pmd));
}
EXPORT_SYMBOL(__page_table_check_pmds_set);

void __page_table_check_puds_set(struct mm_struct *mm, unsigned long addr,
		pud_t *pudp, pud_t pud,	unsigned int nr)
{
	unsigned long stride = PUD_SIZE >> PAGE_SHIFT;
	unsigned int i;

	if (&init_mm == mm)
		return;

	for (i = 0; i < nr; i++)
		__page_table_check_pud_clear(mm, addr + PUD_SIZE * i, *(pudp + i));
	if (pud_user_accessible_page(mm, addr, pud))
		page_table_check_set(pud_pfn(pud), stride * nr, pud_write(pud));
}
EXPORT_SYMBOL(__page_table_check_puds_set);

void __page_table_check_pte_clear_range(struct mm_struct *mm,
					unsigned long addr,
					pmd_t pmd)
{
	if (&init_mm == mm)
		return;

	if (!pmd_bad(pmd) && !pmd_leaf(pmd)) {
		pte_t *ptep = pte_offset_map_mm(mm, &pmd, addr);
		unsigned long nr = pte_table_bytes_mm(mm) / sizeof(pte_t);
		unsigned long i;

		if (WARN_ON(!ptep))
			return;
		for (i = 0; i < nr; i++) {
			__page_table_check_pte_clear(mm, addr, ptep_get(ptep));
			addr += mm_page_size(mm);
			ptep++;
		}
		pte_unmap(ptep - nr);
	}
}

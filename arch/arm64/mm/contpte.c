// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#include <linux/mm.h>
#include <linux/efi.h>
#include <linux/export.h>
#include <asm/tlbflush.h>

static inline unsigned int contpte_nr(pte_t *ptep)
{
	return arm64_cont_ptes(arm64_ptep_page_shift(ptep));
}

static inline phys_addr_t contpte_phys(pte_t pte, unsigned int shift)
{
	if (IS_ENABLED(CONFIG_ARM64_USER4K_EXPERIMENTAL) && shift != PAGE_SHIFT)
		return pte_val(pte) & (ARM64_USER4K_TABLE_ADDR_MASK & ~((1ULL << shift) - 1));
	return __pte_to_phys(pte);
}

static inline pgprot_t contpte_prot(pte_t pte, unsigned int shift)
{
	if (IS_ENABLED(CONFIG_ARM64_USER4K_EXPERIMENTAL) && shift != PAGE_SHIFT)
		return __pgprot(pte_val(pte) & ~(ARM64_USER4K_TABLE_ADDR_MASK & ~((1ULL << shift) - 1)));
	return pte_pgprot(pte);
}

static inline bool mm_is_user(struct mm_struct *mm)
{
	/*
	 * Don't attempt to apply the contig bit to kernel mappings, because
	 * dynamically adding/removing the contig bit can cause page faults.
	 * These racing faults are ok for user space, since they get serialized
	 * on the PTL. But kernel mappings can't tolerate faults.
	 */
	if (unlikely(mm_is_efi(mm)))
		return false;
	return mm != &init_mm;
}

static inline pte_t *contpte_align_down(pte_t *ptep)
{
	return PTR_ALIGN_DOWN(ptep, sizeof(*ptep) * contpte_nr(ptep));
}

static inline pte_t *contpte_align_addr_ptep(struct mm_struct *mm, unsigned long *start,
					     unsigned long *end, pte_t *ptep,
					     unsigned int nr)
{
	/*
	 * Note: caller must ensure these nr PTEs are consecutive (present)
	 * PTEs that map consecutive pages of the same large folio within a
	 * single VMA and a single page table.
	 */
	if (pte_cont(__ptep_get(ptep + nr - 1)))
		*end = ALIGN(*end, cont_pte_size_mm(mm));

	if (pte_cont(__ptep_get(ptep))) {
		*start = ALIGN_DOWN(*start, cont_pte_size_mm(mm));
		ptep = contpte_align_down(ptep);
	}

	return ptep;
}

static void contpte_try_unfold_partial(struct mm_struct *mm, unsigned long addr,
					pte_t *ptep, unsigned int nr)
{
	/*
	 * Unfold any partially covered contpte block at the beginning and end
	 * of the range.
	 */

	if (ptep != contpte_align_down(ptep) || nr < cont_ptes_mm(mm))
		contpte_try_unfold(mm, addr, ptep, __ptep_get(ptep));

	if (ptep + nr != contpte_align_down(ptep + nr)) {
		unsigned long last_addr = addr + mm_page_size(mm) * (nr - 1);
		pte_t *last_ptep = ptep + nr - 1;

		contpte_try_unfold(mm, last_addr, last_ptep,
				   __ptep_get(last_ptep));
	}
}

static void contpte_convert(struct mm_struct *mm, unsigned long addr,
			    pte_t *ptep, pte_t pte)
{
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);
	unsigned long start_addr;
	pte_t *start_ptep;
	int i;

	start_ptep = ptep = contpte_align_down(ptep);
	start_addr = addr = ALIGN_DOWN(addr, cont_pte_size_mm(mm));
	pte = phys_pte_mm(mm, ALIGN_DOWN(pte_phys_mm(mm, pte), cont_pte_size_mm(mm)),
			  pte_pgprot_mm(mm, pte));

	for (i = 0; i < cont_ptes_mm(mm); i++, ptep++, addr += mm_page_size(mm)) {
		pte_t ptent = __ptep_get_and_clear(mm, addr, ptep);

		if (pte_dirty(ptent))
			pte = pte_mkdirty(pte);

		if (pte_young(ptent))
			pte = pte_mkyoung(pte);
	}

	/*
	 * On eliding the __tlb_flush_range() under BBML2+noabort:
	 *
	 * NOTE: Instead of using N=16 as the contiguous block length, we use
	 *       N=4 for clarity.
	 *
	 * NOTE: 'n' and 'c' are used to denote the "contiguous bit" being
	 *       unset and set, respectively.
	 *
	 * We worry about two cases where contiguous bit is used:
	 *  - When folding N smaller non-contiguous ptes as 1 contiguous block.
	 *  - When unfolding a contiguous block into N smaller non-contiguous ptes.
	 *
	 * Currently, the BBML0 folding case looks as follows:
	 *
	 *  0) Initial page-table layout:
	 *
	 *   +----+----+----+----+
	 *   |RO,n|RO,n|RO,n|RW,n| <--- last page being set as RO
	 *   +----+----+----+----+
	 *
	 *  1) Aggregate AF + dirty flags using __ptep_get_and_clear():
	 *
	 *   +----+----+----+----+
	 *   |  0 |  0 |  0 |  0 |
	 *   +----+----+----+----+
	 *
	 *  2) __flush_tlb_range():
	 *
	 *   |____ tlbi + dsb ____|
	 *
	 *  3) __set_ptes() to repaint contiguous block:
	 *
	 *   +----+----+----+----+
	 *   |RO,c|RO,c|RO,c|RO,c|
	 *   +----+----+----+----+
	 *
	 *  4) The kernel will eventually __flush_tlb() for changed page:
	 *
	 *                  |____| <--- tlbi + dsb
	 *
	 * As expected, the intermediate tlbi+dsb ensures that other PEs
	 * only ever see an invalid (0) entry, or the new contiguous TLB entry.
	 * The final tlbi+dsb will always throw away the newly installed
	 * contiguous TLB entry, which is a micro-optimisation opportunity,
	 * but does not affect correctness.
	 *
	 * In the BBML2 case, the change is avoiding the intermediate tlbi+dsb.
	 * This means a few things, but notably other PEs will still "see" any
	 * stale cached TLB entries. This could lead to a "contiguous bit
	 * misprogramming" issue until the final tlbi+dsb of the changed page,
	 * which would clear out both the stale (RW,n) entry and the new (RO,c)
	 * contiguous entry installed in its place.
	 *
	 * What this is saying, is the following:
	 *
	 *  +----+----+----+----+
	 *  |RO,n|RO,n|RO,n|RW,n| <--- old page tables, all non-contiguous
	 *  +----+----+----+----+
	 *
	 *  +----+----+----+----+
	 *  |RO,c|RO,c|RO,c|RO,c| <--- new page tables, all contiguous
	 *  +----+----+----+----+
	 *   /\
	 *   ||
	 *
	 *  If both the old single (RW,n) and new contiguous (RO,c) TLB entries
	 *  are present, and a write is made to this address, do we fault or
	 *  is the write permitted (via amalgamation)?
	 *
	 * The relevant Arm ARM DDI 0487L.a requirements are RNGLXZ and RJQQTC,
	 * and together state that when BBML1 or BBML2 are implemented, either
	 * a TLB conflict abort is raised (which we expressly forbid), or will
	 * "produce an OA, access permissions, and memory attributes that are
	 * consistent with any of the programmed translation table values".
	 *
	 * That is to say, will either raise a TLB conflict, or produce one of
	 * the cached TLB entries, but never amalgamate.
	 *
	 * Thus, as the page tables are only considered "consistent" after
	 * the final tlbi+dsb (which evicts both the single stale (RW,n) TLB
	 * entry as well as the new contiguous (RO,c) TLB entry), omitting the
	 * initial tlbi+dsb is correct.
	 *
	 * It is also important to note that at the end of the BBML2 folding
	 * case, we are still left with potentially all N TLB entries still
	 * cached (the N-1 non-contiguous ptes, and the single contiguous
	 * block). However, over time, natural TLB pressure will cause the
	 * non-contiguous pte TLB entries to be flushed, leaving only the
	 * contiguous block TLB entry. This means that omitting the tlbi+dsb is
	 * not only correct, but also keeps our eventual performance benefits.
	 *
	 * For the unfolding case, BBML0 looks as follows:
	 *
	 *  0) Initial page-table layout:
	 *
	 *   +----+----+----+----+
	 *   |RW,c|RW,c|RW,c|RW,c| <--- last page being set as RO
	 *   +----+----+----+----+
	 *
	 *  1) Aggregate AF + dirty flags using __ptep_get_and_clear():
	 *
	 *   +----+----+----+----+
	 *   |  0 |  0 |  0 |  0 |
	 *   +----+----+----+----+
	 *
	 *  2) __flush_tlb_range():
	 *
	 *   |____ tlbi + dsb ____|
	 *
	 *  3) __set_ptes() to repaint as non-contiguous:
	 *
	 *   +----+----+----+----+
	 *   |RW,n|RW,n|RW,n|RW,n|
	 *   +----+----+----+----+
	 *
	 *  4) Update changed page permissions:
	 *
	 *   +----+----+----+----+
	 *   |RW,n|RW,n|RW,n|RO,n| <--- last page permissions set
	 *   +----+----+----+----+
	 *
	 *  5) The kernel will eventually __flush_tlb() for changed page:
	 *
	 *                  |____| <--- tlbi + dsb
	 *
	 * For BBML2, we again remove the intermediate tlbi+dsb. Here, there
	 * are no issues, as the final tlbi+dsb covering the changed page is
	 * guaranteed to remove the original large contiguous (RW,c) TLB entry,
	 * as well as the intermediate (RW,n) TLB entry; the next access will
	 * install the new (RO,n) TLB entry and the page tables are only
	 * considered "consistent" after the final tlbi+dsb, so software must
	 * be prepared for this inconsistency prior to finishing the mm dance
	 * regardless.
	 */

	if (!system_supports_bbml2_noabort())
		__flush_tlb_range(&vma, start_addr, addr, mm_page_size(mm), 3,
				  TLBF_NOWALKCACHE);

	__set_ptes(mm, start_addr, start_ptep, pte, cont_ptes_mm(mm));
}

void __contpte_try_fold(struct mm_struct *mm, unsigned long addr,
			pte_t *ptep, pte_t pte)
{
	/*
	 * We have already checked that the virtual and pysical addresses are
	 * correctly aligned for a contpte mapping in contpte_try_fold() so the
	 * remaining checks are to ensure that the contpte range is fully
	 * covered by a single folio, and ensure that all the ptes are valid
	 * with contiguous PFNs and matching prots. We ignore the state of the
	 * access and dirty bits for the purpose of deciding if its a contiguous
	 * range; the folding process will generate a single contpte entry which
	 * has a single access and dirty bit. Those 2 bits are the logical OR of
	 * their respective bits in the constituent pte entries. In order to
	 * ensure the contpte range is covered by a single folio, we must
	 * recover the folio from the pfn, but special mappings don't have a
	 * folio backing them. Fortunately contpte_try_fold() already checked
	 * that the pte is not special - we never try to fold special mappings.
	 * Note we can't use vm_normal_page() for this since we don't have the
	 * vma.
	 */

	unsigned long folio_start, folio_end;
	unsigned long cont_start, cont_end;
	pte_t expected_pte, subpte;
	struct folio *folio;
	struct page *page;
	unsigned long pfn;
	pte_t *orig_ptep;
	pgprot_t prot;

	int i;

	if (!mm_is_user(mm))
		return;

	page = pte_page(pte);
	folio = page_folio(page);
	folio_start = addr - (page - &folio->page) * PAGE_SIZE;
	if (arm64_mm_alt_granule(mm))
		folio_start -= pte_phys_mm(mm, pte) & ~PAGE_MASK;
	folio_end = folio_start + folio_nr_pages(folio) * PAGE_SIZE;
	cont_start = ALIGN_DOWN(addr, cont_pte_size_mm(mm));
	cont_end = cont_start + cont_pte_size_mm(mm);

	if (folio_start > cont_start || folio_end < cont_end)
		return;

	pfn = ALIGN_DOWN(pte_phys_mm(mm, pte) >> mm_page_shift(mm), cont_ptes_mm(mm));
	prot = pte_pgprot_mm(mm, pte_mkold(pte_mkclean(pte)));
	expected_pte = phys_pte_mm(mm, pfn << mm_page_shift(mm), prot);
	orig_ptep = ptep;
	ptep = contpte_align_down(ptep);

	for (i = 0; i < cont_ptes_mm(mm); i++) {
		subpte = pte_mkold(pte_mkclean(__ptep_get(ptep)));
		if (!pte_same(subpte, expected_pte))
			return;
		expected_pte = pte_advance_pfn_mm(mm, expected_pte, 1);
		ptep++;
	}

	pte = pte_mkcont(pte);
	contpte_convert(mm, addr, orig_ptep, pte);
}
EXPORT_SYMBOL_GPL(__contpte_try_fold);

void __contpte_try_unfold(struct mm_struct *mm, unsigned long addr,
			pte_t *ptep, pte_t pte)
{
	/*
	 * We have already checked that the ptes are contiguous in
	 * contpte_try_unfold(), so just check that the mm is user space.
	 */
	if (!mm_is_user(mm))
		return;

	pte = pte_mknoncont(pte);
	contpte_convert(mm, addr, ptep, pte);
}
EXPORT_SYMBOL_GPL(__contpte_try_unfold);

pte_t contpte_ptep_get(pte_t *ptep, pte_t orig_pte)
{
	/*
	 * Gather access/dirty bits, which may be populated in any of the ptes
	 * of the contig range. We are guaranteed to be holding the PTL, so any
	 * contiguous range cannot be unfolded or otherwise modified under our
	 * feet.
	 */

	unsigned int nr = contpte_nr(ptep);
	pte_t pte;
	int i;

	ptep = contpte_align_down(ptep);

	for (i = 0; i < nr; i++, ptep++) {
		pte = __ptep_get(ptep);

		if (pte_dirty(pte)) {
			orig_pte = pte_mkdirty(orig_pte);
			for (; i < nr; i++, ptep++) {
				pte = __ptep_get(ptep);
				if (pte_young(pte)) {
					orig_pte = pte_mkyoung(orig_pte);
					break;
				}
			}
			break;
		}

		if (pte_young(pte)) {
			orig_pte = pte_mkyoung(orig_pte);
			i++;
			ptep++;
			for (; i < nr; i++, ptep++) {
				pte = __ptep_get(ptep);
				if (pte_dirty(pte)) {
					orig_pte = pte_mkdirty(orig_pte);
					break;
				}
			}
			break;
		}
	}

	return orig_pte;
}
EXPORT_SYMBOL_GPL(contpte_ptep_get);

static inline bool contpte_is_consistent(pte_t pte, unsigned long pfn,
					unsigned int shift,
					pgprot_t orig_prot)
{
	pgprot_t prot = contpte_prot(pte_mkold(pte_mkclean(pte)), shift);

	return pte_valid_cont(pte) && (contpte_phys(pte, shift) >> shift) == pfn &&
			pgprot_val(prot) == pgprot_val(orig_prot);
}

pte_t contpte_ptep_get_lockless(pte_t *orig_ptep)
{
	/*
	 * The ptep_get_lockless() API requires us to read and return *orig_ptep
	 * so that it is self-consistent, without the PTL held, so we may be
	 * racing with other threads modifying the pte. Usually a READ_ONCE()
	 * would suffice, but for the contpte case, we also need to gather the
	 * access and dirty bits from across all ptes in the contiguous block,
	 * and we can't read all of those neighbouring ptes atomically, so any
	 * contiguous range may be unfolded/modified/refolded under our feet.
	 * Therefore we ensure we read a _consistent_ contpte range by checking
	 * that all ptes in the range are valid and have CONT_PTE set, that all
	 * pfns are contiguous and that all pgprots are the same (ignoring
	 * access/dirty). If we find a pte that is not consistent, then we must
	 * be racing with an update so start again. If the target pte does not
	 * have CONT_PTE set then that is considered consistent on its own
	 * because it is not part of a contpte range.
	 */

	unsigned int shift = arm64_ptep_page_shift(orig_ptep);
	unsigned int nr = arm64_cont_ptes(shift);
	pgprot_t orig_prot;
	unsigned long pfn;
	pte_t orig_pte;
	pte_t *ptep;
	pte_t pte;
	int i;

retry:
	orig_pte = __ptep_get(orig_ptep);

	if (!pte_valid_cont(orig_pte))
		return orig_pte;

	orig_prot = contpte_prot(pte_mkold(pte_mkclean(orig_pte)), shift);
	ptep = contpte_align_down(orig_ptep);
	pfn = (contpte_phys(orig_pte, shift) >> shift) - (orig_ptep - ptep);

	for (i = 0; i < nr; i++, ptep++, pfn++) {
		pte = __ptep_get(ptep);

		if (!contpte_is_consistent(pte, pfn, shift, orig_prot))
			goto retry;

		if (pte_dirty(pte)) {
			orig_pte = pte_mkdirty(orig_pte);
			for (; i < nr; i++, ptep++, pfn++) {
				pte = __ptep_get(ptep);

				if (!contpte_is_consistent(pte, pfn, shift, orig_prot))
					goto retry;

				if (pte_young(pte)) {
					orig_pte = pte_mkyoung(orig_pte);
					break;
				}
			}
			break;
		}

		if (pte_young(pte)) {
			orig_pte = pte_mkyoung(orig_pte);
			i++;
			ptep++;
			pfn++;
			for (; i < nr; i++, ptep++, pfn++) {
				pte = __ptep_get(ptep);

				if (!contpte_is_consistent(pte, pfn, shift, orig_prot))
					goto retry;

				if (pte_dirty(pte)) {
					orig_pte = pte_mkdirty(orig_pte);
					break;
				}
			}
			break;
		}
	}

	return orig_pte;
}
EXPORT_SYMBOL_GPL(contpte_ptep_get_lockless);

void contpte_set_ptes(struct mm_struct *mm, unsigned long addr,
					pte_t *ptep, pte_t pte, unsigned int nr)
{
	unsigned long next;
	unsigned long end;
	unsigned long pfn;
	pgprot_t prot;

	/*
	 * The set_ptes() spec guarantees that when nr > 1, the initial state of
	 * all ptes is not-present. Therefore we never need to unfold or
	 * otherwise invalidate a range before we set the new ptes.
	 * contpte_set_ptes() should never be called for nr < 2.
	 */
	VM_WARN_ON(nr == 1);

	if (!mm_is_user(mm))
		return __set_ptes(mm, addr, ptep, pte, nr);

	end = addr + (nr << mm_page_shift(mm));
	pfn = pte_phys_mm(mm, pte) >> mm_page_shift(mm);
	prot = pte_pgprot_mm(mm, pte);

	do {
		next = pte_cont_addr_end_mm(mm, addr, end);
		nr = (next - addr) >> mm_page_shift(mm);
		pte = phys_pte_mm(mm, pfn << mm_page_shift(mm), prot);

		if (((addr | next | (pfn << mm_page_shift(mm))) & (cont_pte_size_mm(mm) - 1)) == 0)
			pte = pte_mkcont(pte);
		else
			pte = pte_mknoncont(pte);

		__set_ptes(mm, addr, ptep, pte, nr);

		addr = next;
		ptep += nr;
		pfn += nr;

	} while (addr != end);
}
EXPORT_SYMBOL_GPL(contpte_set_ptes);

void contpte_clear_full_ptes(struct mm_struct *mm, unsigned long addr,
				pte_t *ptep, unsigned int nr, int full)
{
	contpte_try_unfold_partial(mm, addr, ptep, nr);
	__clear_full_ptes(mm, addr, ptep, nr, full);
}
EXPORT_SYMBOL_GPL(contpte_clear_full_ptes);

pte_t contpte_get_and_clear_full_ptes(struct mm_struct *mm,
				unsigned long addr, pte_t *ptep,
				unsigned int nr, int full)
{
	contpte_try_unfold_partial(mm, addr, ptep, nr);
	return __get_and_clear_full_ptes(mm, addr, ptep, nr, full);
}
EXPORT_SYMBOL_GPL(contpte_get_and_clear_full_ptes);

bool contpte_test_and_clear_young_ptes(struct vm_area_struct *vma,
		unsigned long addr, pte_t *ptep, unsigned int nr)
{
	struct mm_struct *mm = vma->vm_mm;

	/*
	 * ptep_clear_flush_young() technically requires us to clear the access
	 * flag for a _single_ pte. However, the core-mm code actually tracks
	 * access/dirty per folio, not per page. And since we only create a
	 * contig range when the range is covered by a single folio, we can get
	 * away with clearing young for the whole contig range here, so we avoid
	 * having to unfold.
	 *
	 * The 'nr' means consecutive (present) PTEs that map consecutive pages
	 * of the same large folio in a single VMA and a single page table.
	 */

	unsigned long end = addr + nr * mm_page_size(mm);
	bool young = false;

	ptep = contpte_align_addr_ptep(vma->vm_mm, &addr, &end, ptep, nr);
	for (; addr != end; ptep++, addr += mm_page_size(mm))
		young |= __ptep_test_and_clear_young(vma, addr, ptep);

	return young;
}
EXPORT_SYMBOL_GPL(contpte_test_and_clear_young_ptes);

bool contpte_clear_flush_young_ptes(struct vm_area_struct *vma,
		unsigned long addr, pte_t *ptep, unsigned int nr)
{
	struct mm_struct *mm = vma->vm_mm;

	bool young;

	young = contpte_test_and_clear_young_ptes(vma, addr, ptep, nr);

	if (young) {
		unsigned long end = addr + nr * mm_page_size(mm);

		contpte_align_addr_ptep(vma->vm_mm, &addr, &end, ptep, nr);
		/*
		 * See comment in __ptep_clear_flush_young(); same rationale for
		 * eliding the trailing DSB applies here.
		 */
		__flush_tlb_range(vma, addr, end, mm_page_size(mm), 3,
				  TLBF_NOWALKCACHE | TLBF_NOSYNC);
	}

	return young;
}
EXPORT_SYMBOL_GPL(contpte_clear_flush_young_ptes);

void contpte_wrprotect_ptes(struct mm_struct *mm, unsigned long addr,
					pte_t *ptep, unsigned int nr)
{
	/*
	 * If wrprotecting an entire contig range, we can avoid unfolding. Just
	 * set wrprotect and wait for the later mmu_gather flush to invalidate
	 * the tlb. Until the flush, the page may or may not be wrprotected.
	 * After the flush, it is guaranteed wrprotected. If it's a partial
	 * range though, we must unfold, because we can't have a case where
	 * CONT_PTE is set but wrprotect applies to a subset of the PTEs; this
	 * would cause it to continue to be unpredictable after the flush.
	 */

	contpte_try_unfold_partial(mm, addr, ptep, nr);
	__wrprotect_ptes(mm, addr, ptep, nr);
}
EXPORT_SYMBOL_GPL(contpte_wrprotect_ptes);

void contpte_clear_young_dirty_ptes(struct vm_area_struct *vma,
				    unsigned long addr, pte_t *ptep,
				    unsigned int nr, cydp_t flags)
{
	struct mm_struct *mm = vma->vm_mm;

	/*
	 * We can safely clear access/dirty without needing to unfold from
	 * the architectures perspective, even when contpte is set. If the
	 * range starts or ends midway through a contpte block, we can just
	 * expand to include the full contpte block. While this is not
	 * exactly what the core-mm asked for, it tracks access/dirty per
	 * folio, not per page. And since we only create a contpte block
	 * when it is covered by a single folio, we can get away with
	 * clearing access/dirty for the whole block.
	 */
	unsigned long start = addr;
	unsigned long end = start + nr * mm_page_size(mm);

	ptep = contpte_align_addr_ptep(vma->vm_mm, &start, &end, ptep, nr);
	__clear_young_dirty_ptes(vma, start, ptep, (end - start) >> mm_page_shift(mm), flags);
}
EXPORT_SYMBOL_GPL(contpte_clear_young_dirty_ptes);

static bool contpte_all_subptes_match_access_flags(struct mm_struct *mm,
						  pte_t *ptep, pte_t entry)
{
	pte_t *cont_ptep = contpte_align_down(ptep);
	/*
	 * PFNs differ per sub-PTE. Match only bits consumed by
	 * __ptep_set_access_flags(): AF, DIRTY and write permission.
	 */
	const pteval_t cmp_mask = PTE_RDONLY | PTE_AF | PTE_WRITE | PTE_DIRTY;
	pteval_t entry_cmp = pte_val(entry) & cmp_mask;
	int i;

	for (i = 0; i < cont_ptes_mm(mm); i++) {
		pteval_t pte_cmp = pte_val(__ptep_get(cont_ptep + i)) & cmp_mask;

		if (pte_cmp != entry_cmp)
			return false;
	}

	return true;
}

int contpte_ptep_set_access_flags(struct vm_area_struct *vma,
					unsigned long addr, pte_t *ptep,
					pte_t entry, int dirty)
{
	struct mm_struct *mm = vma->vm_mm;

	unsigned long start_addr;
	pte_t orig_pte;
	int i;

	/*
	 * Check whether all sub-PTEs in the CONT block already match the
	 * requested access flags/write permission, using raw per-PTE values
	 * rather than the gathered ptep_get() view.
	 *
	 * __ptep_set_access_flags() can update AF, dirty and write
	 * permission, but only to make the mapping more permissive.
	 *
	 * ptep_get() gathers AF/dirty state across the whole CONT block,
	 * which is correct for a CPU with FEAT_HAFDBS. But page-table
	 * walkers that evaluate each descriptor individually (e.g. a CPU
	 * without DBM support, or an SMMU without HTTU, or with HA/HD
	 * disabled in CD.TCR) can keep faulting on the target sub-PTE if
	 * only a sibling has been updated. Gathering can therefore cause
	 * false no-ops when only a sibling has been updated:
	 *  - write faults: target still has PTE_RDONLY (needs PTE_RDONLY cleared)
	 *  - read faults:  target still lacks PTE_AF
	 *
	 * Per Arm ARM (DDI 0487) D8.7.1, any sub-PTE in a CONT range may
	 * become the effective cached translation, so all entries must have
	 * consistent attributes. Check the full CONT block before returning
	 * no-op, and when any sub-PTE mismatches, proceed to update the whole
	 * range.
	 */
	if (contpte_all_subptes_match_access_flags(mm, ptep, entry))
		return 0;

	/*
	 * Use raw target pte (not gathered) for write-bit unfold decision.
	 */
	orig_pte = pte_mknoncont(__ptep_get(ptep));

	/*
	 * We can fix up access/dirty bits without having to unfold the contig
	 * range. But if the write bit is changing, we must unfold.
	 */
	if (pte_write(orig_pte) == pte_write(entry)) {
		/*
		 * For HW access management, we technically only need to update
		 * the flag on a single pte in the range. But for SW access
		 * management, we need to update all the ptes to prevent extra
		 * faults. Avoid per-page tlb flush in __ptep_set_access_flags()
		 * and instead flush the whole range at the end.
		 */
		ptep = contpte_align_down(ptep);
		start_addr = addr = ALIGN_DOWN(addr, cont_pte_size_mm(mm));

		/*
		 * We are not advancing entry because __ptep_set_access_flags()
		 * only consumes access flags from entry. And since we have checked
		 * for the whole contpte block and returned early, pte_same()
		 * within __ptep_set_access_flags() is likely false.
		 */
		for (i = 0; i < cont_ptes_mm(mm); i++, ptep++, addr += mm_page_size(mm))
			__ptep_set_access_flags(vma, addr, ptep, entry, 0);

		if (dirty)
			__flush_tlb_range(vma, start_addr,
					  start_addr + cont_pte_size_mm(mm),
					  mm_page_size(mm), 3,
					  TLBF_NOWALKCACHE | TLBF_NOBROADCAST);
	} else {
		__contpte_try_unfold(vma->vm_mm, addr, ptep, orig_pte);
		__ptep_set_access_flags(vma, addr, ptep, entry, dirty);
	}

	return 1;
}
EXPORT_SYMBOL_GPL(contpte_ptep_set_access_flags);

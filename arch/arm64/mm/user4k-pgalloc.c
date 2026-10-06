// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware tables have immutable granule/owner metadata. Smaller tables share
 * a native page; larger tables own a compound allocation. Only unissued slots
 * in a shared native page are cached by the mm.
 */
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/pgalloc.h>
#include <linux/rcupdate.h>
#include <asm/tlb.h>

unsigned int arm64_ptep_page_shift(pte_t *ptep)
{
	struct ptdesc *pt = virt_to_ptdesc(ptep);

	/* Table owners retain geometry through their last RCU reader. */
	return test_bit(PT_fragmented, &pt->pt_flags.f) ? pt->pt_frag_shift : PAGE_SHIFT;
}
EXPORT_SYMBOL_GPL(arm64_ptep_page_shift);

static void fragment_owner_free(struct ptdesc *pt)
{
	clear_bit(PT_fragmented, &pt->pt_flags.f);
	pagetable_dtor_free(pt);
}

static void fragment_owner_free_rcu(struct rcu_head *rcu)
{
	fragment_owner_free(container_of(rcu, struct ptdesc, pt_rcu_head));
}

static void fragment_put(struct ptdesc *pt, unsigned int nr)
{
	if (!atomic_sub_and_test(nr, &pt->pt_frag_refcount))
		return;
	if (test_and_clear_bit(PT_frag_defer, &pt->pt_flags.f))
		call_rcu(&pt->pt_rcu_head, fragment_owner_free_rcu);
	else
		fragment_owner_free(pt);
}

void *arm64_user_pt_alloc_granule(struct mm_struct *mm,
		 enum arm64_user4k_pt_level level, unsigned int shift)
{
	struct ptdesc *pt;
	void *table, *next;
	bool constructed;
	unsigned int frag_size, nr_fragments, order;

	if (shift != 12 && shift != 14 && shift != 16)
		return NULL;
	frag_size = 1U << shift;
	order = shift > PAGE_SHIFT ? shift - PAGE_SHIFT : 0;
	nr_fragments = (PAGE_SIZE << order) >> shift;
	if (WARN_ON_ONCE((unsigned int)level >= ARRAY_SIZE(mm->context.user4k_pt_frag)))
		return NULL;

	spin_lock(&mm->page_table_lock);
	table = mm->context.user4k_pt_frag[level];
	if (table) {
		/* An mm's geometry is immutable; never consume another granule. */
		if (virt_to_ptdesc(table)->pt_frag_shift != shift) {
			spin_unlock(&mm->page_table_lock);
			return NULL;
		}
		next = table + frag_size;
		mm->context.user4k_pt_frag[level] =
			((unsigned long)next & ~PAGE_MASK) ? next : NULL;
	}
	spin_unlock(&mm->page_table_lock);
	if (table)
		return table;

	pt = pagetable_alloc(GFP_PGTABLE_USER, order);
	if (!pt)
		return NULL;
	switch (level) {
	case USER4K_PT_PTE:
		constructed = pagetable_pte_ctor(mm, pt);
		break;
	case USER4K_PT_PMD:
		constructed = pagetable_pmd_ctor(mm, pt);
		break;
	case USER4K_PT_PUD:
		pagetable_pud_ctor(pt);
		constructed = true;
		break;
	default:
		constructed = false;
	}
	if (!constructed) {
		pagetable_free(pt);
		return NULL;
	}
	pt->pt_frag_shift = shift;
	set_bit(PT_fragmented, &pt->pt_flags.f);
	atomic_set(&pt->pt_frag_refcount, 1);
	table = ptdesc_address(pt);
	spin_lock(&mm->page_table_lock);
	if (nr_fragments > 1 && !mm->context.user4k_pt_frag[level]) {
		atomic_set(&pt->pt_frag_refcount, nr_fragments);
		mm->context.user4k_pt_frag[level] = table + frag_size;
	}
	/* A racing allocation may own the cache; this page then has one user. */
	spin_unlock(&mm->page_table_lock);
	return table;
}

void *arm64_user4k_pt_alloc(struct mm_struct *mm, enum arm64_user4k_pt_level level)
{
	return arm64_user_pt_alloc_granule(mm, level, 12);
}

void arm64_user4k_pt_free(void *table)
{
	struct ptdesc *pt = virt_to_ptdesc(table);

	VM_BUG_ON(!test_bit(PT_fragmented, &pt->pt_flags.f));
	VM_BUG_ON(!IS_ALIGNED((unsigned long)table, 1UL << pt->pt_frag_shift));
	VM_BUG_ON(atomic_read(&pt->pt_frag_refcount) <= 0);
	fragment_put(pt, 1);
}

void arm64_pgtable_free(void *table)
{
	struct ptdesc *pt = virt_to_ptdesc(table);

	if (test_bit(PT_fragmented, &pt->pt_flags.f))
		arm64_user4k_pt_free(table);
	else {
		VM_BUG_ON(!PAGE_ALIGNED(table));
		pagetable_dtor_free(pt);
	}
}

void arm64_user4k_pt_cache_destroy(struct mm_struct *mm)
{
	unsigned int level;

	/* The mm is private or dying; no allocator may still access its cache. */
	for (level = 0; level < ARRAY_SIZE(mm->context.user4k_pt_frag); level++) {
		void *next = mm->context.user4k_pt_frag[level];
		struct ptdesc *pt;
		unsigned int issued, shift;

		if (!next)
			continue;
		mm->context.user4k_pt_frag[level] = NULL;
		pt = virt_to_ptdesc(next);
		shift = pt->pt_frag_shift;
		issued = ((unsigned long)next & ~PAGE_MASK) >> shift;
		fragment_put(pt, (PAGE_SIZE >> shift) - issued);
	}
}

void arm64_pte_free_defer(struct mm_struct *mm, pgtable_t table)
{
	struct ptdesc *pt = virt_to_ptdesc(table);

	if (test_bit(PT_fragmented, &pt->pt_flags.f)) {
		/* No freed slot is reused. Delay the final owner release for RCU. */
		set_bit(PT_frag_defer, &pt->pt_flags.f);
		arm64_user4k_pt_free(table);
	} else {
		call_rcu(&pt->pt_rcu_head, fragment_owner_free_rcu);
	}
}

void __tlb_remove_table(void *token)
{
	if ((unsigned long)token & 1)
		arm64_user4k_pt_free((void *)((unsigned long)token & ~1UL));
	else
		pagetable_dtor_free(token);
}

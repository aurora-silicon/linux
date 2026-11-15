// SPDX-License-Identifier: GPL-2.0-only
/* Smaller hardware tables share native owners, including safely retired slots. */
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/pgalloc.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <asm/tlb.h>

struct fragment_pool {
	spinlock_t lock;
	refcount_t refs;
	struct list_head available;
	unsigned int shift;
	bool closed;
};

struct fragment_slot {
	struct rcu_head rcu;
	void *table;
};

struct fragment_owner {
	struct fragment_pool *pool;
	struct ptdesc *pt;
	struct list_head available;
	unsigned long free_mask;
	unsigned long dirty_mask;
	struct fragment_slot slots[];
};

unsigned int arm64_ptep_page_shift(pte_t *ptep)
{
	struct ptdesc *pt = virt_to_ptdesc(ptep);

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

static void fragment_pool_put(struct fragment_pool *pool)
{
	if (refcount_dec_and_test(&pool->refs)) {
		WARN_ON_ONCE(!list_empty(&pool->available));
		kfree(pool);
	}
}

/* Active table allocation already holds the mm alive. Its cache reference
 * remains until destroy_context, after all allocators have stopped. Owners
 * independently retain the pool for callbacks after that point.
 */
static struct fragment_pool *fragment_pool_get(struct mm_struct *mm,
		enum arm64_user4k_pt_level level, unsigned int shift)
{
	struct fragment_pool *pool, *new;

	pool = smp_load_acquire(&mm->context.user4k_pt_frag[level]);
	if (pool)
		return pool->shift == shift ? pool : NULL;
	new = kzalloc(sizeof(*new), GFP_KERNEL | __GFP_ACCOUNT);
	if (!new)
		return NULL;
	spin_lock_init(&new->lock);
	refcount_set(&new->refs, 1);
	INIT_LIST_HEAD(&new->available);
	new->shift = shift;
	pool = cmpxchg(&mm->context.user4k_pt_frag[level], NULL, new);
	if (!pool)
		return new;
	fragment_pool_put(new);
	return pool->shift == shift ? pool : NULL;
}

static struct ptdesc *fragment_pt_alloc(struct mm_struct *mm,
		enum arm64_user4k_pt_level level, unsigned int shift)
{
	struct ptdesc *pt;
	bool constructed;

	pt = pagetable_alloc(GFP_PGTABLE_USER, shift > PAGE_SHIFT ? shift - PAGE_SHIFT : 0);
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
	return pt;
}

/* The caller holds pool->lock; its active mm retains the pool. */
static void *fragment_take(struct fragment_pool *pool, bool *zero)
{
	struct fragment_owner *owner;
	unsigned int slot;

	if (pool->closed || list_empty(&pool->available))
		return NULL;
	owner = list_first_entry(&pool->available, struct fragment_owner, available);
	slot = __ffs(owner->free_mask);
	owner->free_mask &= ~BIT(slot);
	*zero = owner->dirty_mask & BIT(slot);
	owner->dirty_mask &= ~BIT(slot);
	atomic_inc(&owner->pt->pt_frag_refcount);
	if (!owner->free_mask)
		list_del_init(&owner->available);
	return owner->slots[slot].table;
}

void *arm64_user_pt_alloc_granule(struct mm_struct *mm,
		enum arm64_user4k_pt_level level, unsigned int shift)
{
	struct fragment_pool *pool;
	struct fragment_owner *owner;
	struct ptdesc *pt;
	void *table;
	unsigned int nr, i;
	unsigned long flags;
	bool zero = false;

	if (shift != 12 && shift != 14 && shift != 16)
		return NULL;
	if (WARN_ON_ONCE((unsigned int)level >= ARRAY_SIZE(mm->context.user4k_pt_frag)))
		return NULL;
	if (shift >= PAGE_SHIFT) {
		pt = fragment_pt_alloc(mm, level, shift);
		return pt ? ptdesc_address(pt) : NULL;
	}
	pool = fragment_pool_get(mm, level, shift);
	if (!pool)
		return NULL;
	spin_lock_irqsave(&pool->lock, flags);
	table = fragment_take(pool, &zero);
	spin_unlock_irqrestore(&pool->lock, flags);
	if (table)
		goto out;

	nr = PAGE_SIZE >> shift;
	owner = kzalloc(struct_size(owner, slots, nr), GFP_KERNEL | __GFP_ACCOUNT);
	if (!owner)
		goto out;
	pt = fragment_pt_alloc(mm, level, shift);
	if (!pt) {
		kfree(owner);
		goto out;
	}
	owner->pool = pool;
	owner->pt = pt;
	INIT_LIST_HEAD(&owner->available);
	owner->free_mask = GENMASK(nr - 1, 1);
	for (i = 0; i < nr; i++)
		owner->slots[i].table = ptdesc_address(pt) + (i << shift);
	/* This word is unused by arm64 page-table constructors and walkers. */
	pt->__page_mapping = (unsigned long)owner;

	spin_lock_irqsave(&pool->lock, flags);
	/* Reuse supply published while allocation slept, including retired slots. */
	table = fragment_take(pool, &zero);
	if (!table && !pool->closed) {
		refcount_inc(&pool->refs); /* Native owner's lifetime reference. */
		list_add_tail(&owner->available, &pool->available);
		table = owner->slots[0].table;
		owner = NULL;
	}
	spin_unlock_irqrestore(&pool->lock, flags);
	if (owner) {
		pt->__page_mapping = 0;
		fragment_owner_free(pt);
		kfree(owner);
	}
out:
	/* Clear only recycled slots that are actually allocated again. The issued
	 * reference excludes other allocators and keeps this owner alive here.
	 */
	if (table && zero)
		memset(table, 0, 1UL << shift);
	return table;
}

void *arm64_user4k_pt_alloc(struct mm_struct *mm, enum arm64_user4k_pt_level level)
{
	return arm64_user_pt_alloc_granule(mm, level, 12);
}

/* Only unpublished tables or tables past the required walker grace arrive here. */
void arm64_user4k_pt_free(void *table)
{
	struct ptdesc *pt = virt_to_ptdesc(table);
	struct fragment_owner *owner;
	struct fragment_pool *pool;
	unsigned int slot;
	unsigned long flags;
	bool last;

	VM_BUG_ON(!test_bit(PT_fragmented, &pt->pt_flags.f));
	VM_BUG_ON(!IS_ALIGNED((unsigned long)table, 1UL << pt->pt_frag_shift));
	VM_BUG_ON(atomic_read(&pt->pt_frag_refcount) <= 0);
	if (pt->pt_frag_shift >= PAGE_SHIFT) {
		if (test_and_clear_bit(PT_frag_defer, &pt->pt_flags.f))
			call_rcu(&pt->pt_rcu_head, fragment_owner_free_rcu);
		else
			fragment_owner_free(pt);
		return;
	}
	owner = (void *)pt->__page_mapping;
	pool = owner->pool;
	slot = ((unsigned long)table & ~PAGE_MASK) >> pt->pt_frag_shift;
	spin_lock_irqsave(&pool->lock, flags);
	VM_BUG_ON(owner->free_mask & BIT(slot));
	owner->free_mask |= BIT(slot);
	owner->dirty_mask |= BIT(slot);
	last = atomic_dec_and_test(&pt->pt_frag_refcount);
	if (last)
		list_del_init(&owner->available);
	else if (!pool->closed && list_empty(&owner->available))
		list_add_tail(&owner->available, &pool->available);
	spin_unlock_irqrestore(&pool->lock, flags);
	if (last) {
		pt->__page_mapping = 0;
		fragment_owner_free(pt);
		kfree(owner);
		fragment_pool_put(pool);
	}
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
	struct fragment_pool *pool;
	unsigned int level;
	unsigned long flags;

	/* No allocator may still access this mm; deferred owners are independent. */
	for (level = 0; level < ARRAY_SIZE(mm->context.user4k_pt_frag); level++) {
		pool = mm->context.user4k_pt_frag[level];
		if (!pool)
			continue;
		mm->context.user4k_pt_frag[level] = NULL;
		spin_lock_irqsave(&pool->lock, flags);
		pool->closed = true;
		spin_unlock_irqrestore(&pool->lock, flags);
		fragment_pool_put(pool);
	}
}

static void fragment_slot_free_rcu(struct rcu_head *rcu)
{
	struct fragment_slot *slot = container_of(rcu, struct fragment_slot, rcu);

	arm64_user4k_pt_free(slot->table);
}

void arm64_pte_free_defer(struct mm_struct *mm, pgtable_t table)
{
	struct ptdesc *pt = virt_to_ptdesc(table);

	if (test_bit(PT_fragmented, &pt->pt_flags.f)) {
		if (pt->pt_frag_shift < PAGE_SHIFT) {
			struct fragment_owner *owner = (void *)pt->__page_mapping;
			unsigned int slot = ((unsigned long)table & ~PAGE_MASK) >>
					    pt->pt_frag_shift;

			/* Pending callbacks retain their issued references and contents. */
			call_rcu(&owner->slots[slot].rcu, fragment_slot_free_rcu);
		} else {
			set_bit(PT_frag_defer, &pt->pt_flags.f);
			arm64_user4k_pt_free(table);
		}
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

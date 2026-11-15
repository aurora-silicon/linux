/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MM_USER_SUBPAGE_INTERNAL_H
#define _MM_USER_SUBPAGE_INTERNAL_H

#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/refcount.h>
#include <linux/mm_granule.h>

struct mm_subpage_owner;
struct rmap_walk_control;
struct anon_vma;

struct mm_subpage {
	struct mm_subpage_owner *owner;
	refcount_t refs;
	atomic_t mapcount;
	unsigned int index;
	u8 shift;
	unsigned int pins;
	bool exclusive;
	bool lazyfree;
	bool rmap_bound;
};

/* Logical identity is independent of the physical slot after relocation. */
struct mm_subpage_rmap {
	struct anon_vma *root;
	struct vm_page_offset pos;
	unsigned int offset;
};

struct subpage_retired_root;
struct mm_subpage_move {
	struct mm_subpage *slot;
	struct mm_subpage_rmap map;
	struct subpage_retired_root *retired;
	bool swap;
	bool root_changed;
	bool committed;
};

/* Folio and both PTE locks stay held across prepare, commit and finish. */
int mm_subpage_move_prepare(struct mm_subpage_move *move, struct mm_subpage *slot,
		struct vm_area_struct *src, unsigned long src_address,
		struct vm_area_struct *dst, unsigned long dst_address, bool swap);
/* Present source PTE must be cleared and its TLB invalidated before commit. */
int mm_subpage_move_commit(struct mm_subpage_move *move);
void mm_subpage_move_finish(struct mm_subpage_move *move);

int mm_subpage_bind_rmap(struct mm_subpage *slot, struct vm_area_struct *vma,
			unsigned long address);
bool mm_subpage_match_rmap(struct mm_subpage *slot, struct vm_area_struct *vma,
			  unsigned long address);
/* Return -1 for ordinary linear rmap; otherwise return referenced identities. */
int mm_subpage_snapshot_rmap(struct folio *folio, struct mm_subpage_rmap *maps);
bool mm_subpage_address_in_vma(const struct folio *folio,
		const struct vm_area_struct *vma, unsigned long *address);
bool rmap_walk_subpages(struct folio *folio, struct rmap_walk_control *rwc,
			bool locked);

struct anon_vma *mm_subpage_anon_root(const struct mm_subpage *subpage);

/* Folio lock held; called once before publishing anonymous folio mapping. */
void mm_subpage_hold_anon_root(struct mm_subpage *subpage, struct anon_vma *root,
			       bool droppable);
bool mm_subpage_is_droppable(const struct mm_subpage *subpage);

#endif

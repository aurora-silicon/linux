/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MM_SUBPAGE_H
#define _LINUX_MM_SUBPAGE_H

#include <linux/gfp_types.h>
#include <linux/types.h>
#include <linux/mm_granule.h>

#define MM_SUBPAGE_SHIFT 12
#define MM_SUBPAGE_SIZE (1UL << MM_SUBPAGE_SHIFT)
#define MM_SUBPAGES_PER_PAGE (PAGE_SIZE / MM_SUBPAGE_SIZE)

struct mm_subpage_mapcounts {
	unsigned int slots[MM_SUBPAGES_PER_PAGE];
};

struct folio;
struct page;
struct mm_struct;
struct mm_subpage;
struct mm_subpage_pool;
struct page_ext_operations;
struct vm_area_struct;

extern struct page_ext_operations mm_subpage_ext_ops;

#ifdef CONFIG_MM_SUBPAGE
/* Per-native-page shmem validity; caller holds the containing folio lock. */
unsigned long mm_subpage_shmem_missing(struct page *page);
void mm_subpage_shmem_set_missing(struct page *page, unsigned long missing);
#endif

/*
 * A pool is one caller-defined allocation/policy/charge domain. The caller
 * supplies fresh, unmapped native folios using its normal NUMA and memcg
 * allocation path. No mm, VMA, rmap, swap or folio-private field is repurposed.
 * Fault paths transfer slot references to PTEs through the rmap helpers below.
 *
 * Each caller must hold a pool reference through pool operations. close()
 * stops allocation and releases unused backing; it does not consume that
 * reference. Existing slot references outlive close() and the originating mm.
 */
struct mm_subpage_pool *mm_subpage_pool_create(gfp_t gfp);
struct mm_subpage_pool *mm_subpage_pool_create_granule(gfp_t gfp, unsigned int shift);
void mm_subpage_pool_get(struct mm_subpage_pool *pool);
void mm_subpage_pool_put(struct mm_subpage_pool *pool);
void mm_subpage_pool_close(struct mm_subpage_pool *pool);
#ifdef CONFIG_MM_SUBPAGE
/* Optional per-mm pool: default policy, one memory node, matching memcg. */
struct mm_subpage_pool *mm_subpage_cow_pool_get(struct mm_struct *mm,
					     struct vm_area_struct *vma, unsigned long address);
void mm_subpage_cow_pool_exit(struct mm_struct *mm);
bool mm_subpage_cow_folio_matches(struct mm_subpage_pool *pool, struct folio *folio);
bool mm_subpage_cow_wait_busy_at(struct mm_subpage_pool *pool, unsigned int offset);
/* Exact physical-slot mask, one owner and folio lock for the entire batch.
 * slots is indexed by physical offset >> pool shift; only mask bits are set.
 * Success returns initialized owned slots; errors leave slots untouched.
 */
int mm_subpage_alloc_mask_locked(struct mm_subpage_pool *pool, unsigned long mask,
				struct mm_subpage **slots);
int mm_subpage_pool_add_folio_mask(struct mm_subpage_pool *pool, struct folio *folio,
				 unsigned long mask, gfp_t gfp);
bool mm_subpage_cow_wait_busy_mask(struct mm_subpage_pool *pool, unsigned long mask);
/* Return an initialized slot with its folio locked; may wait without a pool lock. */
struct mm_subpage *mm_subpage_alloc_at_locked(struct mm_subpage_pool *pool,
					    unsigned int offset);
struct mm_subpage *mm_subpage_copy_at_locked(struct mm_subpage_pool *pool,
			const struct mm_subpage *source, unsigned int offset);
#else
static inline void mm_subpage_cow_pool_exit(struct mm_struct *mm) { }
#endif

/* Success consumes the folio reference. On error the caller retains it. */
int mm_subpage_pool_add_folio(struct mm_subpage_pool *pool, struct folio *folio,
			      gfp_t gfp);
int mm_subpage_pool_add_folio_at(struct mm_subpage_pool *pool, struct folio *folio,
				 unsigned int offset, gfp_t gfp);
unsigned long mm_subpage_pool_backing_pages(struct mm_subpage_pool *pool);

/* -EAGAIN requires more compatible backing; -ESHUTDOWN means pool closed. */
struct mm_subpage *mm_subpage_alloc(struct mm_subpage_pool *pool);
struct mm_subpage *mm_subpage_copy(struct mm_subpage_pool *pool,
				 const struct mm_subpage *source);
/* Exact physical byte offset within a native folio; no other slot is substituted. */
struct mm_subpage *mm_subpage_alloc_at(struct mm_subpage_pool *pool, unsigned int offset);
struct mm_subpage *mm_subpage_copy_at(struct mm_subpage_pool *pool,
				    const struct mm_subpage *source, unsigned int offset);

/*
 * Reattach a slot to an uptodate, locked order-0 swap-cache folio, without
 * zeroing or copying its bytes. Caller holds a native folio reference and
 * retains it. The returned slot owns an additional reference. -EAGAIN means
 * ownership changed concurrently; -ENOMEM means metadata allocation failed.
 */
struct mm_subpage *mm_subpage_restore(struct folio *folio, unsigned int offset, gfp_t gfp);
struct mm_subpage *mm_subpage_restore_granule(struct folio *folio, unsigned int offset,
					   unsigned int shift, gfp_t gfp);

/* PTE lock held, one file-backed alternative leaf per call. Native mms are no-ops. */
#ifdef CONFIG_MM_SUBPAGE
void __mm_subpage_file_map_add(phys_addr_t phys, unsigned long size);
void __mm_subpage_file_map_del(phys_addr_t phys, unsigned long size);
static inline void mm_subpage_file_map_add(struct vm_area_struct *vma, phys_addr_t phys)
{
	if (mm_page_size(vma->vm_mm) < PAGE_SIZE)
		__mm_subpage_file_map_add(phys, mm_page_size(vma->vm_mm));
}
static inline void mm_subpage_file_map_del(struct vm_area_struct *vma, phys_addr_t phys)
{
	if (mm_page_size(vma->vm_mm) < PAGE_SIZE)
		__mm_subpage_file_map_del(phys, mm_page_size(vma->vm_mm));
}
/* Fill 4K coverage counts; return alternative leaf count. Statistics only. */
unsigned int mm_subpage_file_mapcounts(struct page *page,
				     struct mm_subpage_mapcounts *counts);
#else
static inline void mm_subpage_file_map_add(struct vm_area_struct *vma, phys_addr_t phys) { }
static inline void mm_subpage_file_map_del(struct vm_area_struct *vma, phys_addr_t phys) { }
#endif

/* Both folios locked; preallocate destination metadata before removing PTEs. */
#ifdef CONFIG_MM_SUBPAGE
int mm_subpage_migrate_prepare(struct folio *src, struct folio *dst);
bool mm_subpage_migrating(struct folio *folio);
void mm_subpage_migrate_save_lazyfree(struct mm_subpage *subpage);
/* Infallible metadata reuse after prepare; caller still holds the folio lock. */
struct mm_subpage *mm_subpage_migrate_restore(struct folio *folio, unsigned int offset);
void mm_subpage_migrate_finish(struct folio *folio);
#else
static inline int mm_subpage_migrate_prepare(struct folio *src, struct folio *dst)
{
	return 0;
}
static inline bool mm_subpage_migrating(struct folio *folio)
{
	return false;
}
static inline void mm_subpage_migrate_finish(struct folio *folio) { }
#endif

/*
 * get() requires a live reference. A mapping reference must not be put until
 * its PTE is removed and TLB invalidation completes. Every pin/alias holds
 * its own reference: the slot becomes reusable only after the final put.
 * Every live slot reference also holds one reference to the native folio.
 * copy() requires the caller to stabilise the source contents (as for COW).
 * A failed data copy returns -EHWPOISON without publishing the destination.
 */
void mm_subpage_get(struct mm_subpage *subpage);
void mm_subpage_put(struct mm_subpage *subpage);
/*
 * Acquire the live identity containing a 4K-aligned physical address, or NULL.
 * Interior quarters of a larger identity return that same identity.
 * The caller must stabilise the physical allocation, e.g. with a live PTE
 * under its lock or a folio reference. A lockless walker additionally needs
 * GUP's TLB/free ordering; RCU protects metadata, not arbitrary physical PFNs.
 * This is not a mapping identity check: a lockless PTE reader must revalidate
 * its PTE after acquisition, just as for speculative folio references.
 */
struct mm_subpage *mm_subpage_get_from_phys(phys_addr_t phys);
/*
 * Optional shared sets for asynchronous consumers of native byte spans.
 * add() requires the source slot identities to remain live for the call.
 * The pointer being modified is exclusively owned by the caller; get() shares
 * a read-only set, and mutation uses copy-on-write. Duplicate ranges add no
 * references. prune() requires a stable, side-effect-free membership predicate.
 * Allocation failure leaves the original set intact. put() accepts NULL.
 */
struct mm_subpage_refs;
#ifdef CONFIG_MM_SUBPAGE
struct mm_subpage_refs *mm_subpage_refs_get(struct mm_subpage_refs *refs);
void mm_subpage_refs_put(struct mm_subpage_refs *refs);
int mm_subpage_refs_add(struct mm_subpage_refs **refs, struct page *page,
			unsigned int offset, unsigned int len, gfp_t gfp);
int mm_subpage_refs_prune(struct mm_subpage_refs **refs,
			 bool (*keep)(const struct mm_subpage *, void *),
			 void *context, gfp_t gfp);
#else
static inline struct mm_subpage_refs *mm_subpage_refs_get(struct mm_subpage_refs *refs)
{
	return refs;
}
static inline void mm_subpage_refs_put(struct mm_subpage_refs *refs) { }
static inline int mm_subpage_refs_add(struct mm_subpage_refs **refs, struct page *page,
				     unsigned int offset, unsigned int len, gfp_t gfp)
{
	return 0;
}
static inline int mm_subpage_refs_prune(struct mm_subpage_refs **refs,
			bool (*keep)(const struct mm_subpage *, void *), void *context, gfp_t gfp)
{
	return 0;
}
#endif

/*
 * Anonymous rmap attachment: caller holds the folio lock and PTE lock, has
 * prepared vma->anon_vma, and transfers an existing slot reference to its PTE.
 * A folio may cover only one native linear index and anon_vma root; its slot
 * offset must match the VMA's linear byte offset. Removal needs the PTE lock.
 * These helpers do not install PTEs, update RSS, enqueue on LRU, or drop refs.
 */
int mm_subpage_add_anon_rmap(struct mm_subpage *subpage, struct vm_area_struct *vma,
			    unsigned long address);
int mm_subpage_add_new_anon_rmap(struct mm_subpage *subpage, struct vm_area_struct *vma,
			    unsigned long address);
void mm_subpage_remove_anon_rmap(struct mm_subpage *subpage, struct vm_area_struct *vma);
/* Both PTE locks and source write_protect_seq held; both mappings become R/O. */
int mm_subpage_dup_anon_rmap(struct mm_subpage *subpage,
		struct vm_area_struct *dst, struct vm_area_struct *src, unsigned long address);
/*
 * Per-slot anonymous exclusivity. Setting requires the folio and PTE locks,
 * and proof of a fresh private slot or an exclusive migration/swap entry.
 * Sharing requires the PTE lock and fails while a typed pin exists.
 */
void mm_subpage_set_exclusive(struct mm_subpage *subpage);
bool mm_subpage_is_exclusive(const struct mm_subpage *subpage);
/*
 * Folio and PTE locked; caller proves one remaining swap PTE for this slot.
 * Rmap has been attached, but its PTE is not published yet. Reject old GET
 * references or present aliases before recovering lost exclusive ownership.
 */
bool mm_subpage_try_reuse_swap(struct mm_subpage *subpage);
/* Present PTE plus caller lookup ref; caller proves no swapped aliases. */
bool mm_subpage_try_reuse_anon(struct mm_subpage *subpage);
/* PTE locked, caller holds one GET; retire the PTE/TLB before dropping its ref. */
bool mm_subpage_can_zap(struct mm_subpage *subpage);
bool mm_subpage_try_share(struct mm_subpage *subpage);
/* Folio/PTE locked, caller owns a lookup reference. */
bool mm_subpage_mark_lazyfree(struct mm_subpage *subpage);
bool mm_subpage_all_lazyfree(struct mm_subpage *subpage);
bool mm_subpage_discardable(struct mm_subpage *subpage);
bool mm_subpage_is_lazyfree(const struct mm_subpage *subpage);
void mm_subpage_cancel_lazyfree(struct mm_subpage *subpage);
/*
 * Caller holds a live slot reference and its PTE lock. Success takes an
 * additional slot reference plus a native FOLL_PIN reference. The pin keeps
 * the exact fragment occupied after unmap; release only with subpage_unpin.
 * This is not a GUP walker and does not validate user addresses/permissions.
 */
int mm_subpage_pin(struct mm_subpage *subpage);
void mm_subpage_unpin(struct mm_subpage *subpage);
unsigned int mm_subpage_pincount(const struct mm_subpage *subpage);
#ifdef CONFIG_MM_SUBPAGE
/* A referenced/pinned folio range; never infer offset from the user VA. */
struct user_page_fragment {
	struct folio *folio;
	struct mm_subpage *subpage;
	unsigned long offset;
	unsigned int length;
	bool pinned;
};

/*
 * Takes/releases mmap_lock internally. Returns the number of records filled,
 * or -errno if none. A positive partial result is valid; sum record lengths
 * to advance the user address. Each record stops at the target mm's granule
 * or a native physical-page boundary, whichever comes first.
 * Caller owns the target mm lifetime. Release either kind of record with
 * release_user_fragments(); copying a record does not acquire a reference.
 * FOLL_LONGTERM pins migrate unpinnable backing before returning success.
 * Device/PFN mappings are not accepted by this interface.
 */
long pin_user_fragments_remote(struct mm_struct *mm, unsigned long start,
		unsigned long length, unsigned int flags,
		struct user_page_fragment *fragments, unsigned long capacity);
long get_user_fragments_remote(struct mm_struct *mm, unsigned long start,
		unsigned long length, unsigned int flags,
		struct user_page_fragment *fragments, unsigned long capacity);
/*
 * Single-record GET with a stable VMA. Caller holds mmap_lock throughout;
 * this function never releases it. Returns 1 or -errno (0 for zero length).
 * FOLL_NOWAIT is not supported. The returned VMA is valid until unlock.
 */
int get_user_fragment_vma_remote(struct mm_struct *mm, unsigned long start,
		unsigned long length, unsigned int flags,
		struct user_page_fragment *fragment, struct vm_area_struct **vma);
/* Single-record PIN under the caller's mmap_lock, retained throughout.
 * Same stable-VMA/partial-record contract as GET above. FOLL_LONGTERM and
 * FOLL_NOWAIT are rejected: long-term migration can require dropping locks.
 */
int pin_user_fragment_vma_remote(struct mm_struct *mm, unsigned long start,
		unsigned long length, unsigned int flags,
		struct user_page_fragment *fragment, struct vm_area_struct **vma);
/* Current mm, one aligned fragment; sparse zero pages return a negative errno. */
int get_dump_fragment(unsigned long addr, struct user_page_fragment *fragment);
/* Dirty a retained reference without releasing its allocation identity. */
void mark_user_fragment_dirty(struct user_page_fragment *fragment);
void release_user_fragments(struct user_page_fragment *fragments,
		unsigned long count, bool dirty);
#endif

int mm_subpage_mapcount(const struct mm_subpage *subpage);
/* Borrowed folio: a raw folio reference alone does not keep a slot occupied. */
struct folio *mm_subpage_folio(const struct mm_subpage *subpage);
unsigned int mm_subpage_shift(const struct mm_subpage *subpage);
unsigned int mm_subpage_size(const struct mm_subpage *subpage);
unsigned int mm_subpage_offset(const struct mm_subpage *subpage);
phys_addr_t mm_subpage_phys(const struct mm_subpage *subpage);

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Alternative user granules need an ownership record in addition to a physical
 * address. A device translation cannot retain a subpage allocation identity.
 * Keep those records until the final domain interval has gone away. The native
 * PFN reader remains in pages.c; file and dma-buf suppliers retain its geometry.
 */
#include <linux/highmem.h>
#include <linux/mm_subpage.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

#include "iommufd_private.h"
#include "io_pagetable.h"

static phys_addr_t fragment_phys(const struct user_page_fragment *fragment)
{
	return PFN_PHYS(folio_pfn(fragment->folio)) + fragment->offset;
}

/* Charge each native physical page once, even when several slots pin it. */
static int fragment_store(struct iopt_pages *pages, unsigned long index,
			  struct user_page_fragment *fragment)
{
	unsigned long pfn = PHYS_PFN(fragment_phys(fragment));
	unsigned long count = xa_to_value(xa_load(&pages->fragment_pages, pfn));
	struct user_page_fragment *record;
	int rc;

	record = kmemdup(fragment, sizeof(*record), GFP_KERNEL_ACCOUNT);
	if (!record)
		return -ENOMEM;
	rc = xa_reserve(&pages->fragments, index, GFP_KERNEL_ACCOUNT);
	if (rc)
		goto out_free;
	rc = xa_err(xa_store(&pages->fragment_pages, pfn, xa_mk_value(count + 1),
			     GFP_KERNEL_ACCOUNT));
	if (rc)
		goto out_release;
	/* xa_reserve() made this infallible. Ownership transfers here. */
	xa_store(&pages->fragments, index, record, GFP_NOWAIT);
	if (!count)
		pages->npinned++;
	*fragment = (struct user_page_fragment) {};
	return 0;

out_release:
	xa_release(&pages->fragments, index);
out_free:
	kfree(record);
	return rc;
}

/* No allocation on teardown, and callers have already unmapped all devices. */
static void fragments_release_unused(struct iopt_pages *pages,
				     unsigned long first, unsigned long last)
{
	struct user_page_fragment *record;
	unsigned long index = first;

	lockdep_assert_held(&pages->mutex);
	xa_for_each_range(&pages->fragments, index, record, first, last) {
		unsigned long pfn, count;

		if (interval_tree_iter_first(&pages->domains_itree, index, index) ||
		    interval_tree_iter_first(&pages->access_itree, index, index))
			continue;
		pfn = PHYS_PFN(fragment_phys(record));
		count = xa_to_value(xa_load(&pages->fragment_pages, pfn));
		if (WARN_ON(!count))
			continue;
		if (count == 1) {
			xa_erase(&pages->fragment_pages, pfn);
			pages->npinned--;
		} else {
			xa_store(&pages->fragment_pages, pfn, xa_mk_value(count - 1),
				 GFP_NOWAIT);
		}
		xa_erase(&pages->fragments, index);
		release_user_fragments(record, 1, pages->writable);
		kfree(record);
		cond_resched();
	}
	if (pages->npinned < pages->last_npinned)
		iopt_pages_update_pinned(pages, pages->last_npinned - pages->npinned,
					 false, NULL);
}

/* Reuse existing records even if the original VA was unmapped or replaced. */
static int fragments_acquire(struct iopt_pages *pages, unsigned long first,
			     unsigned long last)
{
	unsigned long size = 1UL << pages->page_shift, index = first;
	unsigned long capacity = PAGE_SIZE / sizeof(struct user_page_fragment);
	struct user_page_fragment *batch = NULL;
	bool mm_live = false;
	int rc = 0;

	lockdep_assert_held(&pages->mutex);
	while (index <= last) {
		unsigned long next = index, end, i;
		long count;

		if (xa_load(&pages->fragments, index)) {
			index++;
			continue;
		}
		if (!mm_live) {
			if (!mmget_not_zero(pages->source_mm)) {
				rc = -EFAULT;
				goto out;
			}
			mm_live = true;
			batch = kmalloc(PAGE_SIZE, GFP_KERNEL_ACCOUNT);
			if (!batch) {
				rc = -ENOMEM;
				goto out;
			}
		}
		/* Stop before any cached pin; never replace its ownership record. */
		end = xa_find(&pages->fragments, &next, last, XA_PRESENT) ? next : last + 1;
		end = min(end, index + capacity);
		count = pin_user_fragments_remote(pages->source_mm,
			(unsigned long)pages->uptr + (index << pages->page_shift),
			(end - index) << pages->page_shift,
			FOLL_LONGTERM | (pages->writable ? FOLL_WRITE : 0), batch, capacity);
		if (count <= 0) {
			rc = count ?: -EFAULT;
			goto out;
		}
		for (i = 0; i < count; i++) {
			/* Each aligned logical index must have exactly one full pin. */
			if (WARN_ON(batch[i].length != size ||
				    !IS_ALIGNED(fragment_phys(&batch[i]), size)))
				rc = -EFAULT;
			else
				rc = fragment_store(pages, index, &batch[i]);
			if (rc) {
				release_user_fragments(batch + i, count - i, false);
				goto out;
			}
			index++;
		}
		cond_resched();
	}
	if (pages->npinned != pages->last_npinned) {
		if (iommufd_should_fail())
			rc = -ENOMEM;
		else
			rc = iopt_pages_update_pinned(pages,
				pages->npinned - pages->last_npinned, true, NULL);
	}
out:
	kfree(batch);
	if (mm_live)
		mmput(pages->source_mm);
	if (rc)
		fragments_release_unused(pages, first, last);
	return rc;
}

static void fragments_unmap(struct iommu_domain *domain, unsigned long iova,
			    unsigned long length)
{
	if (length)
		WARN_ON(iommu_unmap(domain, iova, length) != length);
}

/* All pins are present. Merge physically consecutive fragments before mapping. */
static int fragments_map(struct iopt_area *area, struct iopt_pages *pages,
			 struct iommu_domain *domain)
{
	unsigned long size = 1UL << pages->page_shift;
	unsigned long index = iopt_area_index(area), last = iopt_area_last_index(area);
	unsigned long remaining = iopt_area_length(area), done = 0;
	unsigned long offset = area->page_offset;
	unsigned long min_size = 1UL << __ffs(domain->pgsize_bitmap);
	int rc;

	while (remaining) {
		struct user_page_fragment *record = xa_load(&pages->fragments, index);
		phys_addr_t phys = fragment_phys(record) + offset;
		unsigned long bytes = min(size - offset, remaining);

		index++;
		/* Legacy splitting forbids mappings larger than a native page. */
		while (index <= last && bytes < remaining &&
		       (!area->iopt->disable_large_pages || bytes < PAGE_SIZE)) {
			record = xa_load(&pages->fragments, index);
			if (fragment_phys(record) != phys + bytes)
				break;
			bytes += min(size, remaining - bytes);
			index++;
		}
		/* Never widen a device mapping to include an unowned neighbour. */
		if (!IS_ALIGNED(phys | (iopt_area_iova(area) + done) | bytes, min_size)) {
			rc = -EINVAL;
			goto out_unmap;
		}
		rc = iommu_map(domain, iopt_area_iova(area) + done, phys, bytes,
			       area->iommu_prot, GFP_KERNEL_ACCOUNT);
		if (rc)
			goto out_unmap;
		done += bytes;
		remaining -= bytes;
		offset = 0;
		cond_resched();
	}
	return 0;

out_unmap:
	fragments_unmap(domain, iopt_area_iova(area), done);
	return rc;
}

int iopt_fragments_fill_domain(struct iopt_area *area, struct iopt_pages *pages,
			       struct iommu_domain *domain)
{
	int rc;

	lockdep_assert_held(&pages->mutex);
	rc = fragments_acquire(pages, iopt_area_index(area), iopt_area_last_index(area));
	if (rc)
		return rc;
	rc = fragments_map(area, pages, domain);
	if (rc)
		fragments_release_unused(pages, iopt_area_index(area), iopt_area_last_index(area));
	return rc;
}

int iopt_fragments_fill_domains(struct iopt_area *area, struct iopt_pages *pages)
{
	struct iommu_domain *domain;
	unsigned long index, failed;
	int rc;

	guard(mutex)(&pages->mutex);
	rc = fragments_acquire(pages, iopt_area_index(area), iopt_area_last_index(area));
	if (rc)
		return rc;
	xa_for_each(&area->iopt->domains, index, domain) {
		rc = fragments_map(area, pages, domain);
		if (rc)
			goto out_unmap;
	}
	area->storage_domain = xa_load(&area->iopt->domains, 0);
	interval_tree_insert(&area->pages_node, &pages->domains_itree);
	return 0;

out_unmap:
	failed = index;
	xa_for_each(&area->iopt->domains, index, domain) {
		if (index == failed)
			break;
		fragments_unmap(domain, iopt_area_iova(area), iopt_area_length(area));
	}
	fragments_release_unused(pages, iopt_area_index(area), iopt_area_last_index(area));
	return rc;
}

void iopt_fragments_unfill_domain(struct iopt_area *area, struct iopt_pages *pages,
				 struct iommu_domain *domain)
{
	lockdep_assert_held(&pages->mutex);
	fragments_unmap(domain, iopt_area_iova(area), iopt_area_length(area));
	fragments_release_unused(pages, iopt_area_index(area), iopt_area_last_index(area));
}

int iopt_fragments_rw(struct iopt_pages *pages, unsigned long start_byte,
		      void *data, unsigned long length, unsigned int flags)
{
	unsigned long size = 1UL << pages->page_shift;
	unsigned long first = start_byte >> pages->page_shift;
	unsigned long last = (start_byte + length - 1) >> pages->page_shift;
	unsigned long index, offset = start_byte & (size - 1);
	int rc;

	guard(mutex)(&pages->mutex);
	rc = fragments_acquire(pages, first, last);
	if (rc)
		return rc;
	for (index = first; index <= last; index++) {
		struct user_page_fragment *record = xa_load(&pages->fragments, index);
		unsigned long bytes = min(size - offset, length);
		void *addr = kmap_local_folio(record->folio, record->offset + offset);

		if (flags & IOMMUFD_ACCESS_RW_WRITE)
			memcpy(addr, data, bytes);
		else
			memcpy(data, addr, bytes);
		kunmap_local(addr);
		data += bytes;
		length -= bytes;
		offset = 0;
	}
	fragments_release_unused(pages, first, last);
	return 0;
}

/* The retained typed records, not the output page array, own the pins. */
static int fragments_register_access(struct iopt_area *area, unsigned long first,
				     unsigned long last, bool lock_area)
{
	struct iopt_pages *pages = area->pages;
	struct iopt_pages_access *access;
	struct interval_tree_node *node;

	lockdep_assert_held(&pages->mutex);
	for (node = interval_tree_iter_first(&pages->access_itree, first, last);
	     node; node = interval_tree_iter_next(node, first, last)) {
		if (node->start == first && node->last == last) {
			access = container_of(node, struct iopt_pages_access, node);
			access->users++;
			goto out_count;
		}
	}
	access = kzalloc_obj(*access, GFP_KERNEL_ACCOUNT);
	if (!access)
		return -ENOMEM;
	access->node.start = first;
	access->node.last = last;
	access->users = 1;
	interval_tree_insert(&access->node, &pages->access_itree);
out_count:
	area->num_accesses++;
	if (lock_area)
		area->num_locks++;
	return 0;
}

/* Register a physical byte span without discarding fragment ownership. */
int iopt_fragments_add_phys_access(struct iopt_area *area, unsigned long start_byte,
				   unsigned long length, phys_addr_t *out_phys)
{
	struct iopt_pages *pages = area->pages;
	unsigned long size = 1UL << pages->page_shift;
	unsigned long first = start_byte >> pages->page_shift;
	unsigned long last = (start_byte + length - 1) >> pages->page_shift;
	struct user_page_fragment *record;
	unsigned long index;
	phys_addr_t base;
	int rc;

	guard(mutex)(&pages->mutex);
	rc = fragments_acquire(pages, first, last);
	if (rc)
		return rc;
	record = xa_load(&pages->fragments, first);
	base = fragment_phys(record);
	for (index = first + 1; index <= last; index++) {
		record = xa_load(&pages->fragments, index);
		if (fragment_phys(record) !=
		    base + ((index - first) << pages->page_shift)) {
			rc = -EFAULT;
			goto out_release;
		}
		cond_resched();
	}
	rc = fragments_register_access(area, first, last, true);
	if (rc)
		goto out_release;
	*out_phys = base + (start_byte & (size - 1));
	return 0;

out_release:
	fragments_release_unused(pages, first, last);
	return rc;
}

/*
 * A native page pointer has no fragment offset. Require an aligned physical
 * base and consecutive fragments within each output page; different output
 * pages may be scattered. Only the requested logical interval is retained.
 */
int iopt_fragments_add_access(struct iopt_area *area, unsigned long first,
			      unsigned long last, struct page **out_pages,
			      bool lock_area)
{
	struct iopt_pages *pages = area->pages;
	unsigned long per_page = 1UL << (PAGE_SHIFT - pages->page_shift);
	unsigned long index;
	phys_addr_t base = 0;
	int rc;

	if (first & (per_page - 1))
		return -EINVAL;
	guard(mutex)(&pages->mutex);
	rc = fragments_acquire(pages, first, last);
	if (rc)
		return rc;
	for (index = first; index <= last; index++) {
		struct user_page_fragment *record = xa_load(&pages->fragments, index);
		unsigned long offset = (index - first) & (per_page - 1);
		phys_addr_t phys = fragment_phys(record);

		if (!offset) {
			base = phys;
			if (!IS_ALIGNED(base, PAGE_SIZE)) {
				rc = -EINVAL;
				goto out_release;
			}
			*out_pages++ = pfn_to_page(PHYS_PFN(base));
		} else if (phys != base + (offset << pages->page_shift)) {
			rc = -EINVAL;
			goto out_release;
		}
		cond_resched();
	}
	rc = fragments_register_access(area, first, last, lock_area);
	if (!rc)
		return 0;
out_release:
	fragments_release_unused(pages, first, last);
	return rc;
}

void iopt_fragments_unfill_access(struct iopt_pages *pages, unsigned long first,
				 unsigned long last)
{
	fragments_release_unused(pages, first, last);
}

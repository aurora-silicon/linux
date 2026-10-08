// SPDX-License-Identifier: GPL-2.0-only
/* Included by type1: retained byte ranges, rather than IOMMU PFNs, own pins. */
#ifdef CONFIG_MM_SUBPAGE
struct vfio_fragment {
	struct rb_node node;
	unsigned long start;
	struct user_page_fragment pin;
};

struct vfio_fragment_provider {
	struct rb_root records;
	struct xarray physical_pages;
	unsigned long npinned;
};

static phys_addr_t vfio_fragment_phys(const struct user_page_fragment *pin)
{
	return PFN_PHYS(folio_pfn(pin->folio)) + pin->offset;
}

/* Returns a containing range, or the first range after the requested byte. */
static struct vfio_fragment *vfio_fragment_find(struct vfio_dma *dma,
					      unsigned long offset)
{
	struct rb_node *node = dma->fragments->records.rb_node;
	struct vfio_fragment *next = NULL;

	while (node) {
		struct vfio_fragment *record = rb_entry(node, struct vfio_fragment, node);

		if (offset < record->start) {
			next = record;
			node = node->rb_left;
		} else if (offset - record->start >= record->pin.length) {
			node = node->rb_right;
		} else {
			return record;
		}
	}
	return next;
}

static int vfio_fragments_init(struct vfio_dma *dma, size_t length)
{
	struct vfio_fragment_provider *provider;

	if (mm_page_size(dma->mm) == PAGE_SIZE &&
	    IS_ALIGNED(dma->vaddr | dma->iova | length, PAGE_SIZE))
		return 0;
	provider = kzalloc_obj(*provider, GFP_KERNEL_ACCOUNT);
	if (!provider)
		return -ENOMEM;
	provider->records = RB_ROOT;
	xa_init(&provider->physical_pages);
	dma->fragments = provider;
	return 0;
}

static int vfio_fragment_store(struct vfio_dma *dma, unsigned long start,
			       struct user_page_fragment *pin)
{
	struct vfio_fragment_provider *provider = dma->fragments;
	struct rb_node **link = &provider->records.rb_node, *parent = NULL;
	unsigned long pfn = PHYS_PFN(vfio_fragment_phys(pin));
	unsigned long count = xa_to_value(xa_load(&provider->physical_pages, pfn));
	struct vfio_fragment *record;
	int ret;

	record = kmalloc_obj(*record, GFP_KERNEL_ACCOUNT);
	if (!record)
		return -ENOMEM;
	ret = xa_err(xa_store(&provider->physical_pages, pfn, xa_mk_value(count + 1),
			     GFP_KERNEL_ACCOUNT));
	if (ret) {
		kfree(record);
		return ret;
	}
	while (*link) {
		struct vfio_fragment *cur = rb_entry(*link, struct vfio_fragment, node);

		parent = *link;
		link = start < cur->start ? &(*link)->rb_left : &(*link)->rb_right;
	}
	record->start = start;
	record->pin = *pin;
	rb_link_node(&record->node, parent, link);
	rb_insert_color(&record->node, &provider->records);
	if (!count)
		provider->npinned++;
	*pin = (struct user_page_fragment) {};
	return 0;
}

/* These descriptors borrow existing native GUP pins until owner commit. */
static int vfio_fragment_native_pin(unsigned long pfn, struct user_page_fragment *pin)
{
	struct page *page;

	if (!pfn_valid(pfn))
		return -EOPNOTSUPP;
	page = pfn_to_page(pfn);
	if (PageReserved(page))
		return -EOPNOTSUPP;
	*pin = (struct user_page_fragment) {
		.folio = page_folio(page),
		.offset = page_to_phys(page) - PFN_PHYS(folio_pfn(page_folio(page))),
		.length = PAGE_SIZE,
		.pinned = true,
	};
	return 0;
}

/* No PIN, GET or account reference was acquired while preparing metadata. */
static void vfio_fragments_abort_native(struct vfio_fragment_provider *provider)
{
	struct rb_node *node;

	while ((node = rb_first(&provider->records))) {
		struct vfio_fragment *record = rb_entry(node, struct vfio_fragment, node);

		rb_erase(node, &provider->records);
		kfree(record);
	}
	xa_destroy(&provider->physical_pages);
	kfree(provider);
}

static struct vfio_fragment_provider *
vfio_fragments_prepare_native(struct vfio_iommu *iommu, struct vfio_dma *dma)
{
	struct vfio_fragment_provider *provider;
	struct vfio_dma stage = {};
	struct vfio_domain *domain = NULL;
	struct rb_node *node;
	int ret;

	if (mm_page_size(dma->mm) != PAGE_SIZE || dma->has_rsvd)
		return ERR_PTR(-EOPNOTSUPP);
	if (dma->iommu_mapped) {
		if (list_empty(&iommu->domain_list))
			return ERR_PTR(-EFAULT);
		domain = list_first_entry(&iommu->domain_list, struct vfio_domain, next);
	}
	provider = kzalloc_obj(*provider, GFP_KERNEL_ACCOUNT);
	if (!provider)
		return ERR_PTR(-ENOMEM);
	provider->records = RB_ROOT;
	xa_init(&provider->physical_pages);
	stage.fragments = provider;
	/* One descriptor owns each original native GUP pin, including aliases. */
	if (domain) {
		for (unsigned long offset = 0; offset < dma->size; offset += PAGE_SIZE) {
			phys_addr_t phys = iommu_iova_to_phys(domain->domain, dma->iova + offset);
			struct user_page_fragment pin;

			if (!phys || !IS_ALIGNED(phys, PAGE_SIZE)) {
				ret = -EFAULT;
				goto out;
			}
			ret = vfio_fragment_native_pin(PHYS_PFN(phys), &pin);
			if (!ret)
				ret = vfio_fragment_store(&stage, offset, &pin);
			if (ret)
				goto out;
			cond_resched();
		}
	}
	for (node = rb_first(&dma->pfn_list); node; node = rb_next(node)) {
		struct vfio_pfn *vpfn = rb_entry(node, struct vfio_pfn, node);
		struct user_page_fragment pin;

		ret = vfio_fragment_native_pin(vpfn->pfn, &pin);
		if (ret)
			goto out;
		if (!domain) {
			/* Without a domain, the external pin is the cached buffer. */
			ret = vfio_fragment_store(&stage, vpfn->iova - dma->iova, &pin);
		} else {
			unsigned long count = xa_to_value(xa_load(&provider->physical_pages,
								  vpfn->pfn));

			/* The external GUP reference may identify a different page. */
			ret = xa_err(xa_store(&provider->physical_pages, vpfn->pfn,
					     xa_mk_value(count + 1), GFP_KERNEL_ACCOUNT));
			if (!ret && !count)
				provider->npinned++;
		}
		if (ret)
			goto out;
	}
	return provider;
out:
	vfio_fragments_abort_native(provider);
	return ERR_PTR(ret);
}

static unsigned long vfio_fragments_native_pages(struct vfio_fragment_provider *provider)
{
	return provider->npinned;
}

static void vfio_fragments_commit_native(struct vfio_dma *dma,
					 struct vfio_fragment_provider *provider)
{
	struct rb_node *node;

	/* Mapped legacy external consumers own separate native GUP references. */
	if (dma->iommu_mapped) {
		for (node = rb_first(&dma->pfn_list); node; node = rb_next(node))
			rb_entry(node, struct vfio_pfn, node)->native_pin = true;
	}
	dma->fragments = provider;
}

static void vfio_fragments_put_native_pin(struct vfio_dma *dma, unsigned long pfn)
{
	struct vfio_fragment_provider *provider = dma->fragments;
	unsigned long count = xa_to_value(xa_load(&provider->physical_pages, pfn));
	struct user_page_fragment pin;

	if (WARN_ON(!count || vfio_fragment_native_pin(pfn, &pin)))
		return;
	if (count == 1) {
		xa_erase(&provider->physical_pages, pfn);
		provider->npinned--;
	} else {
		xa_store(&provider->physical_pages, pfn, xa_mk_value(count - 1), GFP_NOWAIT);
	}
	release_user_fragments(&pin, 1, dma->prot & IOMMU_WRITE);
	if (provider->npinned < dma->locked_vm)
		vfio_lock_acct(dma, -(long)(dma->locked_vm - provider->npinned), true);
}

static bool vfio_fragments_external_overlap(struct vfio_dma *dma,
					    dma_addr_t start, unsigned long length)
{
	struct rb_node *node = dma->pfn_list.rb_node;

	while (node) {
		struct vfio_pfn *vpfn = rb_entry(node, struct vfio_pfn, node);

		if (start + length <= vpfn->iova)
			node = node->rb_left;
		else if (start >= vpfn->iova + vpfn->length)
			node = node->rb_right;
		else
			return true;
	}
	return false;
}

/* Device unmaps and their IOTLB synchronization must precede this call. */
static void vfio_fragments_release_range(struct vfio_dma *dma, unsigned long start,
					 unsigned long length)
{
	struct vfio_fragment_provider *provider = dma->fragments;
	struct vfio_fragment *first = vfio_fragment_find(dma, start);
	struct rb_node *node = first ? &first->node : NULL;
	unsigned long end = start + length;

	if (dma->iommu_mapped)
		return;
	while (node) {
		struct vfio_fragment *record = rb_entry(node, struct vfio_fragment, node);
		dma_addr_t iova = dma->iova + record->start;
		unsigned long pfn, count;

		if (record->start >= end)
			break;
		node = rb_next(node);
		/* Native-page consumers retain all records overlapping their page. */
		if (vfio_fragments_external_overlap(dma, iova, record->pin.length))
			continue;
		pfn = PHYS_PFN(vfio_fragment_phys(&record->pin));
		count = xa_to_value(xa_load(&provider->physical_pages, pfn));
		if (WARN_ON(!count))
			continue;
		if (count == 1) {
			xa_erase(&provider->physical_pages, pfn);
			provider->npinned--;
		} else {
			xa_store(&provider->physical_pages, pfn, xa_mk_value(count - 1),
				 GFP_NOWAIT);
		}
		rb_erase(&record->node, &provider->records);
		release_user_fragments(&record->pin, 1, dma->prot & IOMMU_WRITE);
		kfree(record);
		cond_resched();
	}
	if (provider->npinned < dma->locked_vm)
		vfio_lock_acct(dma, -(long)(dma->locked_vm - provider->npinned), true);
}

static void vfio_fragments_release_unused(struct vfio_dma *dma)
{
	vfio_fragments_release_range(dma, 0, dma->size);
}

/* Cached records survive source VA replacement and the last source mm user. */
static int vfio_fragments_acquire(struct vfio_dma *dma, unsigned long start,
				  unsigned long length)
{
	unsigned long end = start + length, original = start;
	unsigned long capacity = PAGE_SIZE / sizeof(struct user_page_fragment);
	struct user_page_fragment *batch = NULL;
	bool live = false;
	int ret = 0;

	while (start < end) {
		struct vfio_fragment *record = vfio_fragment_find(dma, start);
		unsigned long stop = end;
		long count;

		if (record && record->start <= start) {
			start = record->start + record->pin.length;
			continue;
		}
		if (record)
			stop = min(stop, record->start);
		if (!live) {
			if (!mmget_not_zero(dma->mm)) {
				ret = -EFAULT;
				goto out;
			}
			live = true;
			batch = kmalloc(PAGE_SIZE, GFP_KERNEL_ACCOUNT);
			if (!batch) {
				ret = -ENOMEM;
				goto out;
			}
		}
		count = pin_user_fragments_remote(dma->mm, dma->vaddr + start,
			stop - start, FOLL_LONGTERM | (dma->prot & IOMMU_WRITE ? FOLL_WRITE : 0),
			batch, capacity);
		if (count <= 0) {
			ret = count ?: -EFAULT;
			goto out;
		}
		for (unsigned long i = 0; i < count; i++) {
			unsigned long bytes = batch[i].length;

			if (WARN_ON(!bytes || bytes > stop - start ||
				    offset_in_page(vfio_fragment_phys(&batch[i])) >
									PAGE_SIZE - bytes))
				ret = -EFAULT;
			else
				ret = vfio_fragment_store(dma, start, &batch[i]);
			if (ret) {
				release_user_fragments(batch + i, count - i, false);
				goto out;
			}
			start += bytes;
		}
		cond_resched();
	}
	if (dma->fragments->npinned > dma->locked_vm)
		ret = vfio_lock_acct(dma, dma->fragments->npinned - dma->locked_vm, false);
out:
	kfree(batch);
	if (live)
		mmput(dma->mm);
	if (ret)
		vfio_fragments_release_range(dma, original, length);
	return ret;
}

/* Merge physical byte runs while retaining each original pin identity. */
static int vfio_fragments_map_domain(struct vfio_dma *dma, struct iommu_domain *domain)
{
	unsigned long done = 0, min_size = 1UL << __ffs(domain->pgsize_bitmap);
	int ret = 0;

	while (done < dma->size) {
		struct vfio_fragment *record = vfio_fragment_find(dma, done);
		unsigned long bytes, limit = dma->size - done;
		phys_addr_t phys;

		if (WARN_ON(!record || record->start > done)) {
			ret = -EFAULT;
			goto out;
		}
		if (disable_hugepages)
			limit = min(limit, PAGE_SIZE);
		phys = vfio_fragment_phys(&record->pin) + done - record->start;
		bytes = min(record->pin.length - (done - record->start), limit);
		while (bytes < limit) {
			record = vfio_fragment_find(dma, done + bytes);
			if (!record || record->start != done + bytes ||
			    vfio_fragment_phys(&record->pin) != phys + bytes)
				break;
			bytes += min_t(unsigned long, record->pin.length, limit - bytes);
		}
		if (!IS_ALIGNED((dma->iova + done) | phys | bytes, min_size)) {
			ret = -EINVAL;
			goto out;
		}
		ret = iommu_map(domain, dma->iova + done, phys, bytes,
				dma->prot | IOMMU_CACHE, GFP_KERNEL_ACCOUNT);
		if (ret)
			goto out;
		done += bytes;
		cond_resched();
	}
out:
	if (ret && done)
		iommu_unmap(domain, dma->iova, done);
	return ret;
}

static int vfio_fragments_map_all(struct vfio_iommu *iommu, struct vfio_dma *dma)
{
	struct vfio_domain *domain;
	int ret;

	ret = vfio_fragments_acquire(dma, 0, dma->size);
	if (ret)
		return ret;
	list_for_each_entry(domain, &iommu->domain_list, next) {
		ret = vfio_fragments_map_domain(dma, domain->domain);
		if (ret)
			goto unwind;
	}
	dma->iommu_mapped = true;
	return 0;
unwind:
	list_for_each_entry_continue_reverse(domain, &iommu->domain_list, next)
		iommu_unmap(domain->domain, dma->iova, dma->size);
	vfio_fragments_release_unused(dma);
	return ret;
}

static void vfio_fragments_unmap(struct vfio_iommu *iommu, struct vfio_dma *dma)
{
	struct vfio_domain *domain;

	if (dma->iommu_mapped) {
		list_for_each_entry(domain, &iommu->domain_list, next)
			WARN_ON(iommu_unmap(domain->domain, dma->iova, dma->size) != dma->size);
		dma->iommu_mapped = false;
	}
	vfio_fragments_release_unused(dma);
}

static void vfio_fragments_destroy(struct vfio_dma *dma)
{
	if (!dma->fragments)
		return;
	vfio_fragments_release_unused(dma);
	if (WARN_ON(!RB_EMPTY_ROOT(&dma->fragments->records)))
		return;
	xa_destroy(&dma->fragments->physical_pages);
	kfree(dma->fragments);
	dma->fragments = NULL;
}

/* The output page has no offset field: validate every byte and its native offset. */
static int vfio_fragments_external(struct vfio_dma *dma, dma_addr_t iova,
				    unsigned long length, unsigned long *pfn)
{
	unsigned long first = iova - dma->iova, done = 0;
	phys_addr_t phys = 0;
	int ret;

	ret = vfio_fragments_acquire(dma, first, length);
	if (ret)
		return ret;
	while (done < length) {
		struct vfio_fragment *record = vfio_fragment_find(dma, first + done);
		phys_addr_t next;
		unsigned long offset;

		if (WARN_ON(!record || record->start > first + done)) {
			ret = -EFAULT;
			goto out;
		}
		offset = first + done - record->start;
		next = vfio_fragment_phys(&record->pin) + offset;
		if (!done)
			phys = next;
		if (offset_in_page(phys) != offset_in_page(iova) || next != phys + done) {
			ret = -EINVAL;
			goto out;
		}
		done += min_t(unsigned long, record->pin.length - offset, length - done);
	}
	*pfn = PHYS_PFN(phys);
	ret = vfio_add_to_pfn_list(dma, iova, *pfn, length);
out:
	if (ret)
		vfio_fragments_release_range(dma, first, length);
	return ret;
}

/* CPU-only missing ranges take GET references, not new long-term DMA pins. */
static int vfio_fragments_rw(struct vfio_dma *dma, unsigned long offset, void *data,
			     unsigned long length, bool write, size_t *copied)
{
	bool live = false;
	int ret = 0;

	while (length) {
		struct vfio_fragment *record = vfio_fragment_find(dma, offset);
		struct user_page_fragment temp = {}, *pin;
		unsigned long in_record = 0, bytes = length;
		void *addr;

		if (record && record->start <= offset) {
			pin = &record->pin;
			in_record = offset - record->start;
		} else {
			long count;

			if (record)
				bytes = min(bytes, record->start - offset);
			if (!live) {
				if (!mmget_not_zero(dma->mm)) {
					ret = -EFAULT;
					break;
				}
				live = true;
			}
			count = get_user_fragments_remote(dma->mm, dma->vaddr + offset, bytes,
							 write ? FOLL_WRITE : 0, &temp, 1);
			if (count <= 0) {
				ret = count ?: -EFAULT;
				break;
			}
			pin = &temp;
		}
		bytes = min_t(unsigned long, bytes, pin->length - in_record);
		addr = kmap_local_folio(pin->folio, pin->offset + in_record);
		if (write)
			memcpy(addr, data, bytes);
		else
			memcpy(data, addr, bytes);
		kunmap_local(addr);
		if (write && record && record->start <= offset)
			mark_user_fragment_dirty(pin);
		release_user_fragments(&temp, 1, write);
		data += bytes;
		offset += bytes;
		length -= bytes;
		*copied += bytes;
	}
	if (live)
		mmput(dma->mm);
	return ret;
}
#else
static struct vfio_fragment_provider *
vfio_fragments_prepare_native(struct vfio_iommu *iommu, struct vfio_dma *dma)
{
	return ERR_PTR(-EOPNOTSUPP);
}
static void vfio_fragments_abort_native(struct vfio_fragment_provider *provider)
{
}
static unsigned long vfio_fragments_native_pages(struct vfio_fragment_provider *provider)
{
	return 0;
}
static void vfio_fragments_commit_native(struct vfio_dma *dma,
					 struct vfio_fragment_provider *provider)
{
}
static void vfio_fragments_put_native_pin(struct vfio_dma *dma, unsigned long pfn)
{
}
static int vfio_fragments_init(struct vfio_dma *dma, size_t length)
{
	return 0;
}
static void vfio_fragments_release_range(struct vfio_dma *dma, unsigned long start,
					 unsigned long length)
{
}
static void vfio_fragments_release_unused(struct vfio_dma *dma)
{
}
static int vfio_fragments_acquire(struct vfio_dma *dma, unsigned long start,
				  unsigned long length)
{
	return -EOPNOTSUPP;
}
static int vfio_fragments_map_domain(struct vfio_dma *dma, struct iommu_domain *domain)
{
	return -EOPNOTSUPP;
}
static int vfio_fragments_map_all(struct vfio_iommu *iommu, struct vfio_dma *dma)
{
	return -EOPNOTSUPP;
}
static void vfio_fragments_unmap(struct vfio_iommu *iommu, struct vfio_dma *dma)
{
}
static void vfio_fragments_destroy(struct vfio_dma *dma)
{
}
static int vfio_fragments_external(struct vfio_dma *dma, dma_addr_t iova,
				    unsigned long length, unsigned long *pfn)
{
	return -EOPNOTSUPP;
}
static int vfio_fragments_rw(struct vfio_dma *dma, unsigned long offset, void *data,
			     unsigned long length, bool write, size_t *copied)
{
	return -EOPNOTSUPP;
}
#endif

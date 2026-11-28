/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Included after the retained DART and allocation helpers in apple-dart.c. */

static int apple_dart_dcp_shared_pair(struct device *dev, struct device *piodma,
				      struct apple_dart_dcp **firmware,
				      struct apple_dart_dcp **frontend)
{
	struct apple_dart_domain *a, *b;
	int ret;

	if (!dev || !piodma || dev == piodma || piodma->parent != dev ||
	    dev_is_dma_coherent(dev) || dev_is_dma_coherent(piodma))
		return -ENODEV;
	ret = __apple_dart_dcp_check(dev);
	if (ret)
		return ret;
	ret = __apple_dart_dcp_check(piodma);
	if (ret)
		return ret;
	a = to_dart_domain(iommu_get_domain_for_dev(dev));
	b = to_dart_domain(iommu_get_domain_for_dev(piodma));
	if (a->dcp->sid != 23 || b->dcp->sid != 16 || a->dcp->dart == b->dcp->dart)
		return -ENODEV;
	/* No pair on an external display DART either of whose streams is retired. */
	if (b->dcp->dart->dispext && apple_dart_dispext_retired(b->dcp->dart))
		return -EIO;
	*firmware = a->dcp;
	*frontend = b->dcp;
	return 0;
}

static int apple_dart_dcp_shared_verify(struct apple_dart_dcp *dcp,
					struct apple_dart_dcp_shared *object)
{
	for (size_t offset = 0; offset < object->size; offset += PAGE_SIZE) {
		u64 dva = object->dva + offset;
		unsigned int root = (dva >> 25) & 2047, leaf = (dva >> 14) & 2047;

		if (apple_dart_dcp_translate(dcp, dva) != object->phys + offset ||
		    test_bit(root, dcp->foreign) || !test_bit(root, dcp->claimed) ||
		    !dcp_dart_pte_frontend(READ_ONCE(dcp->table[root][leaf])))
			return -EIO;
	}
	return 0;
}

static int __apple_dart_dcp_shared_alloc(struct device *dev, struct device *piodma,
					 size_t size, u64 *phys, u64 *dva)
{
	struct apple_dart_dcp *a, *b;
	struct apple_dart_dcp_shared *object;
	unsigned int root;
	unsigned long flags;
	size_t mapped = 0;
	int ret;

	if (!phys || !dva || !size || size > SZ_16M)
		return -EINVAL;
	ret = apple_dart_dcp_shared_pair(dev, piodma, &a, &b);
	if (ret)
		return ret;
	size = PAGE_ALIGN(size);
	/* Always SID23 then SID16; ordinary RTKit allocation holds SID23 only. */
	mutex_lock(&a->alloc_lock);
	mutex_lock_nested(&b->alloc_lock, SINGLE_DEPTH_NESTING);
	ret = -EIO;
	if (READ_ONCE(a->failed) || READ_ONCE(b->failed))
		goto unlock;
	ret = -ENOSPC;
	if (a->shared_count >= 64 || a->allocated > SZ_64M - size || b->allocated > SZ_64M - size)
		goto unlock;
	for (root = 0; root < DCP_DART_ENTRIES; root++)
		if (!test_bit(root, a->claimed) && !test_bit(root, b->claimed) &&
		    !a->entry[root] && !b->entry[root] &&
		    !READ_ONCE(a->root[root]) && !READ_ONCE(b->root[root]))
			break;
	if (root == DCP_DART_ENTRIES)
		goto unlock;
	/* Serialize the claim against explicit generic map/unmap callers too. */
	spin_lock_irqsave(&a->lock, flags);
	spin_lock_nested(&b->lock, SINGLE_DEPTH_NESTING);
	if (a->entry[root] || b->entry[root] || READ_ONCE(a->root[root]) || READ_ONCE(b->root[root])) {
		spin_unlock(&b->lock);
		spin_unlock_irqrestore(&a->lock, flags);
		ret = -EBUSY;
		goto unlock;
	}
	/* Failed attempts retain both claims, even before any page is published. */
	set_bit(root, a->claimed);
	set_bit(root, b->claimed);
	set_bit(root, a->shared_roots);
	set_bit(root, b->shared_roots);
	spin_unlock(&b->lock);
	spin_unlock_irqrestore(&a->lock, flags);
	ret = -ENOMEM;
	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		goto unlock;
	object->backing = alloc_pages_exact(size, GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN);
	if (!object->backing) {
		kfree(object);
		goto unlock;
	}
	object->phys = virt_to_phys(object->backing);
	object->dva = DCP_DART_ALIAS + root * DCP_DART_SLICE;
	object->size = size;
	object->frontend = b;
	if ((object->phys & (PAGE_SIZE - 1)) || object->phys >= BIT_ULL(42) ||
	    size > BIT_ULL(42) - object->phys) {
		free_pages_exact(object->backing, size);
		kfree(object);
		ret = -ERANGE;
		goto unlock;
	}
	/* The cached direct alias is never touched again. Firmware owns the bytes. */
	arch_dma_prep_coherent(virt_to_page(object->backing), size);
	list_add_tail(&object->link, &a->shared);
	a->shared_count++;
	a->allocated += size;
	b->allocated += size;
	/* Backing and paired claims are permanently owned before the first PTE. */
	ret = apple_dart_dcp_map_owned(a, object->dva, object->phys, PAGE_SIZE,
				       size / PAGE_SIZE, IOMMU_READ | IOMMU_WRITE,
				       GFP_KERNEL, &mapped, true);
	if (!ret && mapped != size)
		ret = -EIO;
	if (!ret)
		ret = apple_dart_domain_flush_tlb_range(a->domain, false, 0, 0);
	if (!ret)
		ret = apple_dart_dcp_shared_verify(a, object);
	if (!ret)
		ret = __apple_dart_dcp_check(dev);
	if (!ret)
		ret = __apple_dart_dcp_check(piodma);
	if (ret) {
		apple_dart_dcp_fail(a, "shared buffer publication failed: %d", ret);
		apple_dart_dcp_fail(b, "shared buffer publication failed: %d", ret);
		dev_err(dev, "Retaining shared DCP backing after SID23 publication failure: %d\n", ret);
		goto unlock;
	}
	*phys = object->phys;
	*dva = object->dva;
unlock:
	mutex_unlock(&b->alloc_lock);
	mutex_unlock(&a->alloc_lock);
	return ret;
}

#define apple_dart_dcp_dispext_fail(dart, fmt, ...)				\
	do {									\
		struct apple_dart *__dd = (dart);				\
										\
		for (unsigned int __i = 0; __i < 2; __i++)			\
			if (__dd->frontend[__i])				\
				apple_dart_dcp_fail(__dd->frontend[__i], fmt,	\
						    ##__VA_ARGS__);		\
	} while (0)

/*
 * J713 external DCP: a shared buffer on the coprocessor's own DART
 * only, SID23, as the firmware's D451 asks: the display DART comes in only
 * if the firmware maps the buffer there (D201). Its root slot is claimed in
 * SID23 alone, so apple_dart_dcp_shared_map() refuses it (frontend NULL).
 * Up to 16 MiB, as the pair; backing and translation are retained until reboot.
 */
int apple_dart_dcp_shared_alloc_link(struct device *dev, size_t size, u64 *phys, u64 *dva)
{
	struct apple_dart_dcp_shared *object;
	struct apple_dart_dcp *a;
	struct apple_dart_domain *dd;
	unsigned int root;
	unsigned long flags;
	size_t mapped = 0;
	int ret;

	if (!dev || dev_is_dma_coherent(dev))
		return -ENODEV;
	if (!phys || !dva || !size || size > SZ_16M)
		return -EINVAL;
	ret = apple_dart_dcp_check(dev);
	if (ret)
		return ret;
	dd = to_dart_domain(iommu_get_domain_for_dev(dev));
	a = dd->dcp;
	if (a->sid != 23)
		return -ENODEV;
	size = PAGE_ALIGN(size);
	mutex_lock(&a->alloc_lock);
	ret = -EIO;
	if (READ_ONCE(a->failed))
		goto unlock;
	ret = -ENOSPC;
	if (a->shared_count >= 64 || a->allocated > SZ_64M - size)
		goto unlock;
	for (root = 0; root < DCP_DART_ENTRIES; root++)
		if (!test_bit(root, a->claimed) && !a->entry[root] && !READ_ONCE(a->root[root]))
			break;
	if (root == DCP_DART_ENTRIES)
		goto unlock;
	spin_lock_irqsave(&a->lock, flags);
	if (a->entry[root] || READ_ONCE(a->root[root])) {
		spin_unlock_irqrestore(&a->lock, flags);
		ret = -EBUSY;
		goto unlock;
	}
	/* A failed attempt retains its claim, even before any page is published. */
	set_bit(root, a->claimed);
	set_bit(root, a->shared_roots);
	spin_unlock_irqrestore(&a->lock, flags);
	ret = -ENOMEM;
	object = kzalloc_obj(*object);
	if (!object)
		goto unlock;
	object->backing = alloc_pages_exact(size, GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN);
	if (!object->backing) {
		kfree(object);
		goto unlock;
	}
	object->phys = virt_to_phys(object->backing);
	object->dva = DCP_DART_ALIAS + root * DCP_DART_SLICE;
	object->size = size;
	object->frontend = NULL;
	if ((object->phys & (PAGE_SIZE - 1)) || object->phys >= BIT_ULL(42) ||
	    size > BIT_ULL(42) - object->phys) {
		free_pages_exact(object->backing, size);
		kfree(object);
		ret = -ERANGE;
		goto unlock;
	}
	/* The cached direct alias is never touched again. Firmware owns the bytes. */
	arch_dma_prep_coherent(virt_to_page(object->backing), size);
	list_add_tail(&object->link, &a->shared);
	a->shared_count++;
	a->allocated += size;
	ret = apple_dart_dcp_map_owned(a, object->dva, object->phys, PAGE_SIZE,
				       size / PAGE_SIZE, IOMMU_READ | IOMMU_WRITE,
				       GFP_KERNEL, &mapped, true);
	if (!ret && mapped != size)
		ret = -EIO;
	if (!ret)
		ret = apple_dart_domain_flush_tlb_range(a->domain, false, 0, 0);
	if (!ret)
		ret = apple_dart_dcp_shared_verify(a, object);
	if (!ret)
		ret = apple_dart_dcp_check(dev);
	if (ret) {
		apple_dart_dcp_fail(a, "SID23 shared buffer publication failed: %d", ret);
		dev_err(dev, "Retaining SID23 shared backing after publication failure: %d\n", ret);
		goto unlock;
	}
	*phys = object->phys;
	*dva = object->dva;
unlock:
	mutex_unlock(&a->alloc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_shared_alloc_link);

/*
 * J713 external DCP: map a D451 buffer's backing (@phys, @size) in
 * stream @sid (0 scanout, 16 PIODMA) of the display DART @piodma is on, for
 * the firmware's D201, at a DVA of its own (*@dva). Mappings fill a claimed
 * 32-MiB slice one after the other (SID0: a slice of the arena fixed at
 * admission; SID16: any free slice, so the pair allocator never takes it);
 * a new slice is claimed only when one is full. Retained until reboot.
 */
static int __apple_dart_dcp_dispext_map(struct device *piodma, unsigned int sid, u64 phys,
					size_t size, u64 *dva)
{
	struct apple_dart_dcp *pio, *dcp;
	unsigned int root, count;
	unsigned long flags;
	u64 base;
	int ret;

	if (!piodma || !dva || !size || size > DCP_DART_SLICE || (sid != 0 && sid != 16) ||
	    ((phys | size) & (PAGE_SIZE - 1)) || phys >= BIT_ULL(42) || size > BIT_ULL(42) - phys)
		return -EINVAL;
	ret = __apple_dart_dcp_check(piodma);
	if (ret)
		return ret;
	pio = to_dart_domain(iommu_get_domain_for_dev(piodma))->dcp;
	if (pio->sid != 16 || !pio->dart->dispext)
		return -ENODEV;
	dcp = sid ? pio : pio->dart->frontend[0];
	if (!dcp)
		return -ENODEV;
	count = size / PAGE_SIZE;
	mutex_lock(&dcp->alloc_lock);
	ret = -EIO;
	if (apple_dart_dispext_retired(dcp->dart))
		goto unlock;
	if (!apple_dart_dcp_registers(dcp))
		goto unlock;
	if (apple_dart_fault_pending(dcp->dart)) {
		apple_dart_dcp_dispext_fail(dcp->dart, "a DART fault is pending (mapping)");
		goto unlock;
	}
	ret = -ENOSPC;
	if (dcp->allocated > SZ_64M - size)
		goto unlock;
	if (dcp->sub_root < 0 || dcp->sub_used > DCP_DART_SLICE - size) {
		spin_lock_irqsave(&dcp->lock, flags);
		for (root = 0; root < DCP_DART_ENTRIES; root++)
			if ((sid || test_bit(root, dcp->arena)) && !test_bit(root, dcp->claimed) &&
			    !test_bit(root, dcp->foreign) && !dcp->entry[root] &&
			    !READ_ONCE(dcp->root[root]))
				break;
		if (root != DCP_DART_ENTRIES) {
			set_bit(root, dcp->claimed);
			/* Private map only, never generic unmap. */
			set_bit(root, dcp->shared_roots);
		}
		spin_unlock_irqrestore(&dcp->lock, flags);
		if (root == DCP_DART_ENTRIES)
			goto unlock;
		dcp->sub_root = root;
		dcp->sub_used = 0;
	}
	root = dcp->sub_root;
	base = DCP_DART_ALIAS + root * DCP_DART_SLICE + dcp->sub_used;
	/* Used before the first possible PTE: a failure never reuses the range. */
	dcp->sub_used += size;
	dcp->allocated += size;
	for (unsigned int i = 0; i < count; i++) {
		size_t mapped = 0;

		ret = apple_dart_dcp_map_owned(dcp, base + i * PAGE_SIZE, phys + i * PAGE_SIZE,
					       PAGE_SIZE, 1, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL,
					       &mapped, true);
		if (!ret && mapped != PAGE_SIZE)
			ret = -EIO;
		if (ret)
			goto pin_failure;
	}
	ret = sid ? apple_dart_dcp_shared_flush(dcp) : apple_dart_dcp_flush(dcp);
	if (!ret)
		ret = __apple_dart_dcp_check(piodma);
	for (unsigned int i = 0; !ret && i < count; i++) {
		unsigned int leaf = ((base + i * PAGE_SIZE) >> 14) & 2047;
		u64 pte = READ_ONCE(dcp->table[root][leaf]);

		if (apple_dart_dcp_translate(dcp, base + i * PAGE_SIZE) != phys + i * PAGE_SIZE ||
		    pte != dcp->expected[root][leaf] || !dcp_dart_pte_frontend(pte))
			ret = -EIO;
	}
	if (ret)
		goto pin_failure;
	*dva = base;
	goto unlock;
pin_failure:
	apple_dart_dcp_dispext_fail(dcp->dart, "external display mapping failed: %d", ret);
	dev_err(piodma, "retaining a SID%u mapping after its failure: %d\n", sid, ret);
unlock:
	mutex_unlock(&dcp->alloc_lock);
	return ret;
}

/*
 * J713 external DCP: whether stream @sid (0 or 16) of the display DART
 * @piodma is on is adopted and not retired (a failed mapping, a lost state,
 * a sleep). Its flags only: no register is read.
 */
int apple_dart_dcp_dispext_stream_ok(struct device *piodma, unsigned int sid)
{
	struct iommu_domain *domain = piodma ? iommu_get_domain_for_dev(piodma) : NULL;
	struct apple_dart_dcp *pio, *dcp;

	if (!domain || domain->owner != &apple_dart_iommu_ops || (sid != 0 && sid != 16))
		return -ENODEV;
	pio = to_dart_domain(domain)->dcp;
	if (!pio || pio->sid != 16 || !pio->dart->dispext)
		return -ENODEV;
	dcp = sid ? pio : pio->dart->frontend[0];
	if (!dcp)
		return -ENODEV;
	/* Either frontend failed retires both streams (apple_dart_dcp_dispext_fail()). */
	if (apple_dart_dispext_retired(pio->dart))
		return -EIO;
	/* Its power not confirmed since sleep: usable later, not now. */
	return apple_dart_pm_unconfirmed(pio->dart) ? -EAGAIN : 0;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_dispext_stream_ok);

/* Find @slices (1..DCP_DART_DISPEXT_MAX_SLICES) contiguous free SID0 arena slices. */
static int apple_dart_dcp_arena_find(struct apple_dart_dcp *dcp, unsigned int slices)
{
	unsigned int root, s;

	lockdep_assert_held(&dcp->lock);
	if (!slices || slices > DCP_DART_DISPEXT_MAX_SLICES)
		return -EINVAL;
	for (root = 0; root + slices <= DCP_DART_ENTRIES; root++) {
		for (s = 0; s < slices; s++) {
			unsigned int r = root + s;

			if (!test_bit(r, dcp->arena) || test_bit(r, dcp->claimed) ||
			    test_bit(r, dcp->foreign) || dcp->entry[r] || READ_ONCE(dcp->root[r]))
				break;
		}
		if (s == slices)
			return root;
	}
	return -ENOSPC;
}

/* Find and claim @slices (1..DCP_DART_DISPEXT_MAX_SLICES) contiguous free SID0 arena slices. */
static int apple_dart_dcp_arena_claim(struct apple_dart_dcp *dcp, unsigned int slices, bool window)
{
	int root = apple_dart_dcp_arena_find(dcp, slices);

	if (root < 0)
		return root;
	for (unsigned int s = 0; s < slices; s++) {
		set_bit(root + s, dcp->claimed);
		/* Private map only, never generic unmap. */
		set_bit(root + s, dcp->shared_roots);
		if (window)
			set_bit(root + s, dcp->window);
	}
	if (window)
		dcp->window_slices[root] = slices;
	return root;
}

/*
 * J713 external DCP: a host-written scanout surface in SID0 of the
 * display DART @piodma is on: zeroed pages behind a Normal-NC CPU mapping
 * (the return value), in one or two contiguous 32-MiB slices of the SID0
 * arena at *@dva. Retained until reboot, like every display-visible allocation.
 */
static void *__apple_dart_dcp_dispext_scanout_alloc(struct device *piodma, size_t size, u64 *dva)
{
	struct apple_dart_dcp_allocation *allocation;
	struct apple_dart_dcp *pio, *dcp;
	unsigned int slices, count, allocated = 0;
	unsigned long flags;
	void *result;
	int root, ret;

	if (!piodma || !dva || !size || size > DCP_DART_DISPEXT_MAX_SIZE ||
	    dev_is_dma_coherent(piodma))
		return ERR_PTR(-EINVAL);
	ret = __apple_dart_dcp_check(piodma);
	if (ret)
		return ERR_PTR(ret);
	pio = to_dart_domain(iommu_get_domain_for_dev(piodma))->dcp;
	if (pio->sid != 16 || !pio->dart->dispext)
		return ERR_PTR(-ENODEV);
	dcp = pio->dart->frontend[0];
	if (!dcp)
		return ERR_PTR(-ENODEV);
	size = PAGE_ALIGN(size);
	slices = DIV_ROUND_UP(size, DCP_DART_SLICE);
	count = size / PAGE_SIZE;
	mutex_lock(&dcp->alloc_lock);
	result = ERR_PTR(-EIO);
	if (apple_dart_dispext_retired(dcp->dart))
		goto unlock;
	if (!apple_dart_dcp_registers(dcp))
		goto unlock;
	if (apple_dart_fault_pending(dcp->dart)) {
		apple_dart_dcp_dispext_fail(dcp->dart, "a DART fault is pending (scanout surface)");
		goto unlock;
	}
	/* Bounded by the external scanout budget (fits 4K and 5K/5K2K surfaces). */
	result = ERR_PTR(-ENOSPC);
	if (dcp->allocated > DCP_DART_DISPEXT_SCANOUT_MAX - size)
		goto unlock;
	/* Fail fast without allocating pages if contiguous slices are unavailable. */
	spin_lock_irqsave(&dcp->lock, flags);
	root = apple_dart_dcp_arena_find(dcp, slices);
	spin_unlock_irqrestore(&dcp->lock, flags);
	if (root < 0) {
		result = ERR_PTR(root);
		goto unlock;
	}
	/* Complete all fallible host allocations before claiming arena slices. */
	result = ERR_PTR(-ENOMEM);
	allocation = kzalloc(struct_size(allocation, pages, count), GFP_KERNEL);
	if (!allocation)
		goto unlock;
	allocation->size = size;
	for (allocated = 0; allocated < count; allocated++) {
		allocation->pages[allocated] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!allocation->pages[allocated])
			goto free_unpublished;
		arch_dma_prep_coherent(allocation->pages[allocated], PAGE_SIZE);
	}
	/* Only this Normal-NC mapping may access payload bytes after cache cleaning. */
	allocation->cpu = dma_common_pages_remap(allocation->pages, size,
						 dma_pgprot(piodma, PAGE_KERNEL, 0),
						 __builtin_return_address(0));
	if (!allocation->cpu)
		goto free_unpublished;
	spin_lock_irqsave(&dcp->lock, flags);
	root = apple_dart_dcp_arena_claim(dcp, slices, false);
	spin_unlock_irqrestore(&dcp->lock, flags);
	if (root < 0) {
		result = ERR_PTR(root);
		goto free_unpublished;
	}
	allocation->dva = DCP_DART_ALIAS + (u64)root * DCP_DART_SLICE;
	list_add_tail(&allocation->link, &dcp->allocations);
	dcp->allocated += size;
	/* Every outcome after the first possible PTE retains all backing and claims. */
	for (unsigned int i = 0; i < count; i++) {
		size_t mapped = 0;

		ret = apple_dart_dcp_map_owned(dcp, allocation->dva + (u64)i * PAGE_SIZE,
					       page_to_phys(allocation->pages[i]), PAGE_SIZE, 1,
					       IOMMU_READ | IOMMU_WRITE, GFP_KERNEL, &mapped, true);
		if (!ret && mapped != PAGE_SIZE)
			ret = -EIO;
		if (ret)
			goto pin_failure;
	}
	ret = apple_dart_dcp_flush(dcp);
	if (!ret)
		ret = __apple_dart_dcp_check(piodma);
	for (unsigned int i = 0; !ret && i < count; i++) {
		u64 addr = allocation->dva + (u64)i * PAGE_SIZE;
		unsigned int r = (addr >> 25) & 2047;
		unsigned int leaf = (addr >> 14) & 2047;
		u64 pte = READ_ONCE(dcp->table[r][leaf]);

		if (apple_dart_dcp_translate(dcp, addr) != page_to_phys(allocation->pages[i]) ||
		    pte != dcp->expected[r][leaf] || !dcp_dart_pte_frontend(pte))
			ret = -EIO;
	}
	for (unsigned int s = 0; !ret && s < slices; s++)
		if (READ_ONCE(dcp->root[root + s]) != dcp->entry[root + s])
			ret = -EIO;
	if (ret)
		goto pin_failure;
	*dva = allocation->dva;
	result = allocation->cpu;
	goto unlock;
pin_failure:
	apple_dart_dcp_dispext_fail(dcp->dart, "SID0 scanout surface mapping failed: %d", ret);
	dev_err(piodma, "retaining %zu SID0 scanout bytes after their mapping failure: %d\n",
		size, ret);
	result = ERR_PTR(ret);
	goto unlock;
free_unpublished:
	if (allocation->cpu)
		dma_common_free_remap(allocation->cpu, size);
	while (allocated)
		__free_page(allocation->pages[--allocated]);
	kfree(allocation);
unlock:
	mutex_unlock(&dcp->alloc_lock);
	return result;
}

/*
 * A hardware qualification failure of an external display DART (a pending
 * fault, registers not as adopted, a failed check) retires the whole DART:
 * both frontends, SID0 and SID16 (PIODMA).
 */

/* The SID16 (PIODMA) frontend of an external display DART, or NULL. */
static struct apple_dart_dcp *apple_dart_dcp_dispext_pio(struct device *piodma)
{
	struct iommu_domain *domain = piodma ? iommu_get_domain_for_dev(piodma) : NULL;
	struct apple_dart_dcp *pio;

	if (!domain || domain->owner != &apple_dart_iommu_ops)
		return NULL;
	pio = to_dart_domain(domain)->dcp;
	if (!pio || pio->sid != 16 || !pio->dart->dispext)
		return NULL;
	return pio;
}

/*
 * The SID0 frontend of an external display DART, from its PIODMA
 * child, once that child's check (registers, roots, ownership) passed; a
 * failed check retires the whole DART.
 */
static struct apple_dart_dcp *apple_dart_dcp_dispext_sid0(struct device *piodma)
{
	struct apple_dart_dcp *pio = apple_dart_dcp_dispext_pio(piodma);
	int ret;

	if (!pio)
		return NULL;
	/* Unconfirmed power since sleep (-EAGAIN) retires nothing: it waits. */
	ret = __apple_dart_dcp_check(piodma);
	if (ret == -EAGAIN)
		return NULL;
	if (ret) {
		apple_dart_dcp_dispext_fail(pio->dart, "the PIODMA check failed: %d", ret);
		return NULL;
	}
	return pio->dart->frontend[0];
}

/*
 * J713 external DCP: with the DART's power confirmed on by the caller
 * (its firmware last powered it up, under the lock that serialises that), both
 * frontends as adopted and no fault pending; else the whole DART is retired.
 * Register reads only: PROTECT, both TTBRs and TCRs, ENABLE_STREAMS and the
 * exception/error state.
 */
static int __apple_dart_dcp_dispext_qualify(struct device *piodma)
{
	struct apple_dart_dcp *pio = apple_dart_dcp_dispext_pio(piodma);
	struct apple_dart_dcp *sid0 = pio ? pio->dart->frontend[0] : NULL;

	if (!sid0)
		return -ENODEV;
	if (apple_dart_dispext_retired(pio->dart))
		return -EIO;
	/* A mismatch retires the whole DART, see apple_dart_dcp_registers(). */
	if (!apple_dart_dcp_registers(pio) || !apple_dart_dcp_registers(sid0))
		return -EIO;
	if (apple_dart_fault_pending(pio->dart)) {
		apple_dart_dcp_dispext_fail(pio->dart, "a DART fault is pending");
		return -EIO;
	}
	return 0;
}

/*
 * J713 external DCP: claim one or two contiguous free SID0 arena slices (not
 * yet used by a surface, firmware-owned or holding a table) covering @size
 * bytes as a remappable host window at *@dva, until reboot. -ENOSPC without
 * contiguous slices.
 */
static int __apple_dart_dcp_dispext_window_claim(struct device *piodma, size_t size, u64 *dva)
{
	struct apple_dart_dcp *dcp;
	unsigned int slices;
	unsigned long flags;
	int root, ret;

	if (!dva || !size || size > DCP_DART_DISPEXT_MAX_SIZE)
		return -EINVAL;
	dcp = apple_dart_dcp_dispext_sid0(piodma);
	if (!dcp)
		return -ENODEV;
	slices = DIV_ROUND_UP(size, DCP_DART_SLICE);
	mutex_lock(&dcp->alloc_lock);
	/* A register mismatch retires the whole DART, see apple_dart_dcp_registers(). */
	if (apple_dart_dispext_retired(dcp->dart) || !apple_dart_dcp_registers(dcp)) {
		ret = -EIO;
		goto unlock;
	}
	spin_lock_irqsave(&dcp->lock, flags);
	root = apple_dart_dcp_arena_claim(dcp, slices, true);
	if (root >= 0) {
		*dva = DCP_DART_ALIAS + (u64)root * DCP_DART_SLICE;
		ret = 0;
	} else {
		ret = root;
	}
	spin_unlock_irqrestore(&dcp->lock, flags);
unlock:
	mutex_unlock(&dcp->alloc_lock);
	return ret;
}

/* The window root at @dva and its slice count in *@slices, or -1 if it is none. */
static int apple_dart_dcp_window_root(struct apple_dart_dcp *dcp, u64 dva, unsigned int *slices)
{
	unsigned int root = (dva >> 25) & 2047;
	unsigned int n;

	if (!apple_dart_dcp_iova(dva) || (dva & (DCP_DART_SLICE - 1)) ||
	    dva != DCP_DART_ALIAS + (u64)root * DCP_DART_SLICE)
		return -1;
	n = dcp->window_slices[root];
	if (!n || n > DCP_DART_DISPEXT_MAX_SLICES || root + n > DCP_DART_ENTRIES)
		return -1;
	for (unsigned int s = 0; s < n; s++) {
		unsigned int r = root + s;

		if (!test_bit(r, dcp->window) || !test_bit(r, dcp->claimed) ||
		    !test_bit(r, dcp->arena) || test_bit(r, dcp->foreign))
			return -1;
	}
	*slices = n;
	return root;
}

/*
 * J713 external DCP: map @count caller-owned pages (@phys, 16 KiB each)
 * at the start of the empty window @dva; TLB flush, then every PTE read back.
 * On any error the window and the pages stay as they are: the caller keeps
 * the pages until reboot.
 */
static int __apple_dart_dcp_dispext_window_map(struct device *piodma, u64 dva,
					       const phys_addr_t *phys, unsigned int count)
{
	struct apple_dart_dcp *dcp = apple_dart_dcp_dispext_sid0(piodma);
	unsigned int slices = 0;
	int root, ret;

	if (!dcp)
		return -ENODEV;
	if (!phys || !count || count > DCP_DART_DISPEXT_MAX_SLICES * DCP_DART_ENTRIES)
		return -EINVAL;
	for (unsigned int i = 0; i < count; i++)
		if ((phys[i] & (DCP_DART_PAGE - 1)) || phys[i] >> 42)
			return -EINVAL;
	mutex_lock(&dcp->alloc_lock);
	root = apple_dart_dcp_window_root(dcp, dva, &slices);
	ret = -EINVAL;
	if (root < 0 || count > slices * DCP_DART_ENTRIES)
		goto unlock;
	ret = -EIO;
	if (apple_dart_dispext_retired(dcp->dart))
		goto unlock;
	if (!apple_dart_dcp_registers(dcp) || apple_dart_fault_pending(dcp->dart))
		goto failed;
	ret = -EBUSY;
	for (unsigned int s = 0; s < slices; s++)
		for (unsigned int i = 0; dcp->expected[root + s] && i < DCP_DART_ENTRIES; i++)
			if (dcp->expected[root + s][i])
				goto unlock;
	for (unsigned int i = 0; i < count; i++) {
		size_t mapped = 0;

		ret = apple_dart_dcp_map_owned(dcp, dva + (u64)i * DCP_DART_PAGE, phys[i],
					       DCP_DART_PAGE, 1, IOMMU_READ | IOMMU_WRITE,
					       GFP_KERNEL, &mapped, true);
		if (!ret && mapped != DCP_DART_PAGE)
			ret = -EIO;
		if (ret)
			goto failed;
	}
	ret = apple_dart_dcp_flush(dcp);
	if (!ret)
		ret = __apple_dart_dcp_check(piodma);
	for (unsigned int i = 0; !ret && i < count; i++) {
		u64 addr = dva + (u64)i * DCP_DART_PAGE;
		unsigned int r = (addr >> 25) & 2047;
		unsigned int leaf = (addr >> 14) & 2047;
		u64 pte = READ_ONCE(dcp->table[r][leaf]);

		if (apple_dart_dcp_translate(dcp, addr) != phys[i] ||
		    pte != dcp->expected[r][leaf] || !dcp_dart_pte_frontend(pte))
			ret = -EIO;
	}
	for (unsigned int s = 0; !ret && s < slices; s++)
		if (READ_ONCE(dcp->root[root + s]) != dcp->entry[root + s])
			ret = -EIO;
	if (!ret)
		goto unlock;
failed:
	apple_dart_dcp_dispext_fail(dcp->dart, "window map at %#llx failed: %d", dva, ret);
	dev_err(piodma, "SID0 window %#llx: mapping %u pages failed: %d; pages retained\n",
		dva, count, ret);
unlock:
	mutex_unlock(&dcp->alloc_lock);
	return ret;
}

/*
 * J713 external DCP: clear every PTE of the window @dva, TLB flush,
 * then read every one back as invalid. The number of pages unmapped, or an
 * error, after which the window's pages must be kept until reboot (a stale
 * translation may survive).
 */
static int __apple_dart_dcp_dispext_window_unmap(struct device *piodma, u64 dva)
{
	struct apple_dart_dcp *dcp = apple_dart_dcp_dispext_sid0(piodma);
	unsigned int slices = 0;
	unsigned long flags;
	bool any_table = false;
	int root, ret, cleared = 0;

	if (!dcp)
		return -ENODEV;
	mutex_lock(&dcp->alloc_lock);
	root = apple_dart_dcp_window_root(dcp, dva, &slices);
	ret = -EINVAL;
	if (root < 0)
		goto unlock;
	for (unsigned int s = 0; s < slices; s++)
		if (dcp->table[root + s])
			any_table = true;
	ret = 0;
	if (!any_table)
		goto unlock;
	ret = -EIO;
	if (apple_dart_dispext_retired(dcp->dart))
		goto unlock;
	if (!apple_dart_dcp_registers(dcp) || apple_dart_fault_pending(dcp->dart))
		goto failed;
	spin_lock_irqsave(&dcp->lock, flags);
	for (unsigned int s = 0; s < slices; s++) {
		unsigned int r = root + s;

		if (READ_ONCE(dcp->root[r]) != dcp->entry[r]) {
			spin_unlock_irqrestore(&dcp->lock, flags);
			goto failed;
		}
		if (!dcp->table[r])
			continue;
		for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++)
			if (READ_ONCE(dcp->table[r][i]) != dcp->expected[r][i]) {
				spin_unlock_irqrestore(&dcp->lock, flags);
				goto failed;
			}
	}
	for (unsigned int s = 0; s < slices; s++) {
		unsigned int r = root + s;

		if (!dcp->table[r])
			continue;
		for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++) {
			if (!dcp->expected[r][i])
				continue;
			WRITE_ONCE(dcp->table[r][i], 0);
			dcp->expected[r][i] = 0;
			cleared++;
		}
	}
	dma_wmb();
	spin_unlock_irqrestore(&dcp->lock, flags);
	ret = apple_dart_dcp_flush(dcp);
	if (!ret)
		ret = __apple_dart_dcp_check(piodma);
	for (unsigned int s = 0; !ret && s < slices; s++) {
		unsigned int r = root + s;

		if (!dcp->table[r])
			continue;
		for (unsigned int i = 0; !ret && i < DCP_DART_ENTRIES; i++) {
			u64 addr = dva + ((u64)s * DCP_DART_ENTRIES + i) * DCP_DART_PAGE;

			if (READ_ONCE(dcp->table[r][i]) || apple_dart_dcp_translate(dcp, addr))
				ret = -EIO;
		}
	}
	if (!ret) {
		ret = cleared;
		goto unlock;
	}
failed:
	if (!ret)
		ret = -EIO;
	apple_dart_dcp_dispext_fail(dcp->dart, "window unmap at %#llx failed: %d", dva, ret);
	dev_err(piodma, "SID0 window %#llx: unmap failed: %d; pages retained\n", dva, ret);
unlock:
	mutex_unlock(&dcp->alloc_lock);
	return ret;
}

static int __apple_dart_dcp_shared_map(struct device *dev, struct device *piodma,
				       u64 phys, u64 dva, size_t size)
{
	struct apple_dart_dcp *a, *b;
	struct apple_dart_dcp_shared *object, *found = NULL;
	size_t mapped = 0;
	int ret = apple_dart_dcp_shared_pair(dev, piodma, &a, &b);

	if (ret)
		return ret;
	mutex_lock(&a->alloc_lock);
	mutex_lock_nested(&b->alloc_lock, SINGLE_DEPTH_NESTING);
	list_for_each_entry(object, &a->shared, link)
		if (object->dva == dva) {
			found = object;
			break;
		}
	ret = -EINVAL;
	if (!found || found->frontend != b || found->phys != phys || found->size != size || found->mapped)
		goto unlock;
	ret = apple_dart_dcp_shared_verify(a, found);
	if (ret)
		goto failed;
	ret = apple_dart_dcp_map_owned(b, dva, phys, PAGE_SIZE, size / PAGE_SIZE,
				       IOMMU_READ | IOMMU_WRITE, GFP_KERNEL, &mapped, true);
	if (!ret && mapped != size)
		ret = -EIO;
	if (!ret)
		ret = apple_dart_dcp_shared_flush(b);
	if (!ret)
		ret = apple_dart_dcp_shared_verify(b, found);
	if (!ret)
		ret = __apple_dart_dcp_check(dev);
	if (!ret)
		ret = __apple_dart_dcp_check(piodma);
	if (ret)
		goto failed;
	found->mapped = true;
	WRITE_ONCE(b->shared_mapping, true);
	goto unlock;
failed:
	apple_dart_dcp_fail(a, "SID16 shared mapping failed: %d", ret);
	apple_dart_dcp_fail(b, "SID16 shared mapping failed: %d", ret);
	dev_err(dev, "Retaining shared DCP backing after SID16 publication failure: %d\n", ret);
unlock:
	mutex_unlock(&b->alloc_lock);
	mutex_unlock(&a->alloc_lock);
	return ret;
}

/*
 * J713 external DCP: the external display DART operations above, each
 * inside one use of the DART's registers (apple_dart_pm_get()): with
 * its power not confirmed since sleep, -EAGAIN before any step, nothing
 * published, nothing retired. A sleep waits for a use under way to end, so
 * no step of it (its flushes included) runs unconfirmed.
 */
static struct apple_dart *apple_dart_dcp_dispext_gate(struct device *piodma)
{
	struct apple_dart_dcp *pio = apple_dart_dcp_dispext_pio(piodma);

	return pio ? pio->dart : NULL;
}

#define APPLE_DART_DISPEXT_GATED(piodma, call)					\
	({									\
		struct apple_dart *__gd = apple_dart_dcp_dispext_gate(piodma);	\
		typeof(call) __ret;						\
										\
		if (__gd && !apple_dart_pm_get(__gd)) {			\
			__ret = (typeof(call))-EAGAIN;				\
		} else {							\
			__ret = (call);						\
			if (__gd)						\
				apple_dart_pm_put(__gd);			\
		}								\
		__ret;								\
	})

int apple_dart_dcp_shared_alloc(struct device *dev, struct device *piodma,
				size_t size, u64 *phys, u64 *dva)
{
	return APPLE_DART_DISPEXT_GATED(piodma,
		__apple_dart_dcp_shared_alloc(dev, piodma, size, phys, dva));
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_shared_alloc);

int apple_dart_dcp_shared_map(struct device *dev, struct device *piodma,
			      u64 phys, u64 dva, size_t size)
{
	return APPLE_DART_DISPEXT_GATED(piodma,
		__apple_dart_dcp_shared_map(dev, piodma, phys, dva, size));
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_shared_map);

int apple_dart_dcp_dispext_map(struct device *piodma, unsigned int sid, u64 phys, size_t size,
			       u64 *dva)
{
	return APPLE_DART_DISPEXT_GATED(piodma,
		__apple_dart_dcp_dispext_map(piodma, sid, phys, size, dva));
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_dispext_map);

void *apple_dart_dcp_dispext_scanout_alloc(struct device *piodma, size_t size, u64 *dva)
{
	struct apple_dart *dart = apple_dart_dcp_dispext_gate(piodma);
	void *ret;

	if (dart && !apple_dart_pm_get(dart))
		return ERR_PTR(-EAGAIN);
	ret = __apple_dart_dcp_dispext_scanout_alloc(piodma, size, dva);
	if (dart)
		apple_dart_pm_put(dart);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_dispext_scanout_alloc);

int apple_dart_dcp_dispext_qualify(struct device *piodma)
{
	return APPLE_DART_DISPEXT_GATED(piodma, __apple_dart_dcp_dispext_qualify(piodma));
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_dispext_qualify);

int apple_dart_dcp_dispext_window_claim(struct device *piodma, size_t size, u64 *dva)
{
	return APPLE_DART_DISPEXT_GATED(piodma,
		__apple_dart_dcp_dispext_window_claim(piodma, size, dva));
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_dispext_window_claim);

int apple_dart_dcp_dispext_window_map(struct device *piodma, u64 dva, const phys_addr_t *phys,
				      unsigned int count)
{
	return APPLE_DART_DISPEXT_GATED(piodma,
		__apple_dart_dcp_dispext_window_map(piodma, dva, phys, count));
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_dispext_window_map);

int apple_dart_dcp_dispext_window_unmap(struct device *piodma, u64 dva)
{
	return APPLE_DART_DISPEXT_GATED(piodma, __apple_dart_dcp_dispext_window_unmap(piodma, dva));
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_dispext_window_unmap);

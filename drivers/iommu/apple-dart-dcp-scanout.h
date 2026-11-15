/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Retained SID0 copy surfaces, confined to the arena fixed at admission. */
void *apple_dart_dcp_scanout_alloc(struct device *dev, size_t size, dma_addr_t *dva)
{
	struct apple_dart_dcp_allocation *allocation;
	struct apple_dart_dcp *dcp;
	struct device_node *node;
	unsigned int root, count, allocated = 0;
	unsigned long flags;
	int ret;
	void *result;
	bool match;

	if (!dev || !dva || !size || size > SZ_32M)
		return ERR_PTR(-EINVAL);
	node = of_find_node_by_path("disp0");
	match = node && node == dev->of_node &&
		apple_dart_j713_scanout_node(node);
	of_node_put(node);
	if (!match || dev_is_dma_coherent(dev))
		return ERR_PTR(-ENODEV);
	ret = apple_dart_dcp_check(dev);
	if (ret)
		return ERR_PTR(ret);
	dcp = to_dart_domain(iommu_get_domain_for_dev(dev))->dcp;
	if (dcp->sid != 0)
		return ERR_PTR(-ENODEV);
	size = PAGE_ALIGN(size);
	count = size / PAGE_SIZE;
	mutex_lock(&dcp->alloc_lock);
	result = ERR_PTR(-ENOSPC);
	if (READ_ONCE(dcp->failed) || dcp->allocated > SZ_64M - size)
		goto unlock;
	/* Claim under the PTE lock too: generic callers cannot race publication. */
	spin_lock_irqsave(&dcp->lock, flags);
	for (root = 0; root < DCP_DART_ENTRIES; root++)
		if (test_bit(root, dcp->arena) && !test_bit(root, dcp->claimed) &&
		    !test_bit(root, dcp->foreign) && !dcp->entry[root] && !READ_ONCE(dcp->root[root]))
			break;
	if (root != DCP_DART_ENTRIES) {
		set_bit(root, dcp->claimed);
		set_bit(root, dcp->shared_roots); /* Private map only, never generic unmap. */
	}
	spin_unlock_irqrestore(&dcp->lock, flags);
	if (root == DCP_DART_ENTRIES)
		goto unlock;
	result = ERR_PTR(-ENOMEM);
	allocation = kzalloc(struct_size(allocation, pages, count), GFP_KERNEL);
	if (!allocation)
		goto unlock;
	allocation->size = size;
	allocation->dva = DCP_DART_ALIAS + root * DCP_DART_SLICE;
	for (allocated = 0; allocated < count; allocated++) {
		allocation->pages[allocated] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!allocation->pages[allocated])
			goto free_unpublished;
		arch_dma_prep_coherent(allocation->pages[allocated], PAGE_SIZE);
	}
	/* Only this Normal-NC mapping may access payload bytes after cache cleaning. */
	allocation->cpu = dma_common_pages_remap(allocation->pages, size,
						 dma_pgprot(dev, PAGE_KERNEL, 0),
						 __builtin_return_address(0));
	if (!allocation->cpu)
		goto free_unpublished;
	list_add_tail(&allocation->link, &dcp->allocations);
	dcp->allocated += size;
	/* Every outcome after the first possible PTE retains all backing and claims. */
	for (unsigned int i = 0; i < count; i++) {
		size_t mapped = 0;

		ret = apple_dart_dcp_map_owned(dcp, allocation->dva + i * PAGE_SIZE,
				page_to_phys(allocation->pages[i]), PAGE_SIZE, 1,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL, &mapped, true);
		if (!ret && mapped != PAGE_SIZE)
			ret = -EIO;
		if (ret)
			goto pin_failure;
	}
	ret = apple_dart_dcp_flush(dcp);
	if (!ret)
		ret = apple_dart_dcp_check(dev);
	if (ret)
		goto pin_failure;
	for (unsigned int i = 0; i < count; i++) {
		if (apple_dart_dcp_translate(dcp, allocation->dva + i * PAGE_SIZE) !=
		    page_to_phys(allocation->pages[i]) ||
		    !dcp_dart_pte_frontend(READ_ONCE(dcp->table[root][i]))) {
			ret = -EIO;
			goto pin_failure;
		}
	}
	*dva = allocation->dva;
	result = allocation->cpu;
	goto unlock;
pin_failure:
	apple_dart_dcp_fail(dcp, "scanout mapping failed: %d", ret);
	dev_err(dev, "Retaining %zu scanout bytes after SID0 mapping failure: %d\n", size, ret);
	result = ERR_PTR(ret);
	goto unlock;
free_unpublished:
	while (allocated)
		__free_page(allocation->pages[--allocated]);
	kfree(allocation);
unlock:
	mutex_unlock(&dcp->alloc_lock);
	return result;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_scanout_alloc);

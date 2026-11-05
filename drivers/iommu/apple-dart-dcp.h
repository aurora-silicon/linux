/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Included by apple-dart.c. J713 display coprocessors: the internal DCP's
 * firmware and frontend DARTs, and the external DCPs' firmware DARTs.
 * Foreign tables stay live.
 */
#include <linux/mm.h>
#include <linux/of_reserved_mem.h>
#include "apple-dart-dcp-format.h"

struct apple_dart_dcp {
	struct apple_dart *dart;
	struct apple_dart_domain *domain;
	struct device *owner;
	spinlock_t lock;
	struct mutex alloc_lock;
	struct list_head allocations;
	struct list_head shared;
	size_t allocated;
	unsigned int shared_count;
	struct dcp_adt_tables firmware;
	u64 *root;
	u64 *table[DCP_DART_ENTRIES];
	u64 *expected[DCP_DART_ENTRIES];
	u64 entry[DCP_DART_ENTRIES];
	struct resource leaf_regions[64];
	unsigned int nr_leaf_regions;
	struct resource excluded_regions[128];
	unsigned int nr_excluded_regions;
	DECLARE_BITMAP(foreign, DCP_DART_ENTRIES);
	DECLARE_BITMAP(claimed, DCP_DART_ENTRIES);
	DECLARE_BITMAP(shared_roots, DCP_DART_ENTRIES);
	DECLARE_BITMAP(arena, DCP_DART_ENTRIES); /* SID0 retained surfaces only. */
	unsigned long retained; /* SID0 pages pinned after an uncertain unmap. */
	u32 ttbr;
	unsigned int sid;
	bool failed;
	bool shared_mapping;
	bool shared_flushing;
};

struct apple_dart_dcp_shared {
	struct list_head link;
	struct apple_dart_dcp *frontend;
	void *backing;
	u64 phys, dva;
	size_t size;
	bool mapped;
};

struct apple_dart_dcp_allocation {
	struct list_head link;
	void *cpu;
	dma_addr_t dva;
	size_t size;
	struct page *pages[];
};

/* SID0 slices for the retained copy surfaces; generic DMA gets the other free ones. */
#define DCP_DART_SCANOUT_SLOTS	4

static bool apple_dart_has_borrowed(struct apple_dart *dart)
{
	return dart->dcp || dart->frontend[0] || dart->frontend[1];
}

static struct apple_dart_dcp *apple_dart_borrowed_sid(struct apple_dart *dart, unsigned int sid)
{
	return sid == 23 ? dart->dcp : sid == 0 ? dart->frontend[0] :
		sid == 16 ? dart->frontend[1] : NULL;
}

static struct apple_dart_dcp *apple_dart_borrowed_cfg(struct apple_dart_master_cfg *cfg)
{
	struct apple_dart *dart = cfg->stream_maps[0].dart;

	if (cfg->stream_maps[1].dart ||
	    bitmap_weight(cfg->stream_maps[0].sidmap, dart->num_streams) != 1)
		return NULL;
	return apple_dart_borrowed_sid(dart,
		find_first_bit(cfg->stream_maps[0].sidmap, dart->num_streams));
}

static bool apple_dart_dcp_registers(struct apple_dart_dcp *dcp)
{
	struct apple_dart *dart = dcp->dart;
	bool valid;

	valid = !READ_ONCE(dcp->failed) &&
		(readl(dart->regs + DART_T8110_PROTECT) & DART_T8110_PROTECT_TTBR_TCR) &&
		readl(dart->regs + DART_TTBR(dart, dcp->sid, 0)) == dcp->ttbr &&
		readl(dart->regs + DART_TCR(dart, dcp->sid)) == DART_T8110_TCR_TRANSLATE_ENABLE &&
		(readl(dart->regs + DART_T8110_ENABLE_STREAMS) & BIT(dcp->sid));
	if (!valid)
		WRITE_ONCE(dcp->failed, true);
	return valid;
}

static int apple_dart_dcp_read_adt(struct apple_dart *dart, struct dcp_adt_tables *tables,
				 struct dcp_adt_frontend *frontend)
{
	struct device_node *node, *found = NULL;
	struct reserved_mem *rmem;
	struct resource res;
	void *data;
	int ret;

	for_each_compatible_node(node, NULL, "phram") {
		const char *label;

		if (of_property_read_string(node, "label", &label) || strcmp(label, "adt"))
			continue;
		if (found) {
			of_node_put(found);
			of_node_put(node);
			return -EINVAL;
		}
		found = of_node_get(node);
	}
	if (!found)
		return -ENODEV;
	ret = of_address_to_resource(found, 0, &res);
	rmem = of_reserved_mem_lookup(found);
	if (ret || !rmem || rmem->base != res.start || rmem->size != resource_size(&res) ||
	    !resource_size(&res) || resource_size(&res) > SZ_2M) {
		ret = -EINVAL;
		goto out_node;
	}
	data = memremap(res.start, resource_size(&res), MEMREMAP_WB);
	if (!data) {
		ret = -ENOMEM;
		goto out_node;
	}
	ret = frontend ? dcp_adt_parse_frontend(data, resource_size(&res), tables, frontend) :
		dcp_adt_parse_instance(data, resource_size(&res), dart->dcp_instance, tables);
	memunmap(data);
out_node:
	of_node_put(found);
	return ret;
}

static int apple_dart_dcp_excluded(struct apple_dart *dart, u64 start, u64 end)
{
	struct device_node *node, *reserved;
	struct resource res;

	/* Inspect every final DT RAM tuple, not only Linux's currently free pages. */
	for_each_node_by_type(node, "memory") {
		int cells = of_n_addr_cells(node) + of_n_size_cells(node);
		int count = of_property_count_u32_elems(node, "reg");

		if (cells <= 0 || count <= 0 || count % cells) {
			of_node_put(node);
			return -EINVAL;
		}
		for (int i = 0; i < count / cells; i++) {
			if (of_address_to_resource(node, i, &res)) {
				of_node_put(node);
				return -EINVAL;
			}
			if (start <= res.end && res.start < end) {
				of_node_put(node);
				return -EBUSY;
			}
		}
	}
	if (region_intersects(start, end - start, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT)
		return -EBUSY;
	for (u64 pa = start; pa < end; pa += PAGE_SIZE) {
#ifdef CONFIG_ARM64
		if (pfn_is_map_memory(PHYS_PFN(pa)))
			return -EBUSY;
#else
		if (pfn_valid(PHYS_PFN(pa)))
			return -EBUSY;
#endif
	}
	/* Reserved DT regions outside System RAM need not appear in /proc/iomem. */
	reserved = of_find_node_by_path("/reserved-memory");
	for_each_child_of_node(reserved, node) {
		int cells = of_n_addr_cells(node) + of_n_size_cells(node);
		int count;

		if (!of_device_is_available(node) || !of_property_present(node, "reg"))
			continue;
		count = of_property_count_u32_elems(node, "reg");
		if (cells <= 0 || count <= 0 || count % cells) {
			of_node_put(node);
			of_node_put(reserved);
			return -EINVAL;
		}
		for (int i = 0; i < count / cells; i++) {
			if (of_address_to_resource(node, i, &res)) {
				of_node_put(node);
				of_node_put(reserved);
				return -EINVAL;
			}
			if (start <= res.end && res.start < end) {
				of_node_put(node);
				of_node_put(reserved);
				return -EBUSY;
			}
		}
	}
	of_node_put(reserved);
	/* Includes conflicting non-RAM owners; kept until reboot. */
	return apple_dart_retained_request_region(dart, start, end - start,
						"J713 DCP inherited tables");
}

/* Fixed at admission, so every reservation query reports the same SID0 slices. */
static int apple_dart_dcp_arena(struct apple_dart_dcp *dcp)
{
	unsigned int slots = 0;

	for (unsigned int i = 0; i < DCP_DART_ENTRIES && slots < DCP_DART_SCANOUT_SLOTS; i++) {
		if (dcp->entry[i] || test_bit(i, dcp->foreign))
			continue;
		set_bit(i, dcp->arena);
		slots++;
	}
	return slots == DCP_DART_SCANOUT_SLOTS ? 0 : -ENOSPC;
}

/* Snapshot only this consumer's real no-map reservations, before table admission. */
static int apple_dart_dcp_leaf_regions(struct apple_dart_dcp *dcp)
{
	struct device_node *consumer, *node;
	const char *alias = dcp->sid == 23 ? dcp->dart->dcp_instance->dcp :
		dcp->sid == 0 ? "disp0" : "disp0_piodma";
	int count, ret = 0;

	consumer = of_find_node_by_path(alias);
	if (!consumer)
		return -ENODEV;
	count = of_property_count_u32_elems(consumer, "memory-region");
	if (count <= 0 || count > ARRAY_SIZE(dcp->leaf_regions)) {
		ret = -EINVAL;
		goto out;
	}
	for (int i = 0; i < count; i++) {
		struct resource res;
		struct reserved_mem *rmem;

		node = of_parse_phandle(consumer, "memory-region", i);
		if (!node) { ret = -EINVAL; break; }
		rmem = of_reserved_mem_lookup(node);
		if (!of_property_read_bool(node, "no-map") ||
		    of_address_to_resource(node, 0, &res) || !rmem ||
		    rmem->base != res.start || rmem->size != resource_size(&res) ||
		    !resource_size(&res))
			ret = -EINVAL;
		of_node_put(node);
		if (ret)
			break;
		dcp->leaf_regions[dcp->nr_leaf_regions++] = res;
	}
out:
	of_node_put(consumer);
	return ret;
}

/* Snapshot all reserved tuples at admission, never walk OF under the IRQ lock. */
static int apple_dart_dcp_excluded_regions(struct apple_dart_dcp *dcp)
{
	struct device_node *reserved, *node;
	int ret = 0;

	reserved = of_find_node_by_path("/reserved-memory");
	for_each_child_of_node(reserved, node) {
		int cells = of_n_addr_cells(node) + of_n_size_cells(node);
		int count;

		if (!of_device_is_available(node) || !of_property_present(node, "reg"))
			continue;
		count = of_property_count_u32_elems(node, "reg");
		if (cells <= 0 || count <= 0 || count % cells) {
			ret = -EINVAL;
			goto out_node;
		}
		for (int i = 0; i < count / cells; i++) {
			if (dcp->nr_excluded_regions == ARRAY_SIZE(dcp->excluded_regions)) {
				ret = -E2BIG;
				goto out_node;
			}
			ret = of_address_to_resource(node, i,
				&dcp->excluded_regions[dcp->nr_excluded_regions]);
			if (ret)
				goto out_node;
			/* Empty loader placeholders wrap end to start - 1; they reserve
			 * no bytes and must never exclude the whole address space.
			 */
			if (!resource_size(&dcp->excluded_regions[dcp->nr_excluded_regions]))
				continue;
			dcp->nr_excluded_regions++;
		}
	}
	goto out;
out_node:
	of_node_put(node);
out:
	of_node_put(reserved);
	return ret;
}

/* Foreign leaves must never DMA into ordinary Linux memory. Consumer no-map
 * reservations may sit under a broad System RAM resource, but must really have
 * been removed from the linear map. Other consumers' reserved carveouts are
 * forbidden even when /proc/iomem does not label them System RAM. Host-owned
 * later mappings use expected[].
 */
static bool apple_dart_dcp_leaf_safe(const struct apple_dart_dcp *dcp, u64 leaf)
{
	u64 pa;

	if (!leaf)
		return true;
	pa = dcp_dart_pte_phys(leaf);
	if (dcp_dart_pte_check(leaf, true) || pa > U64_MAX - DCP_DART_PAGE)
		return false;
#ifdef CONFIG_ARM64
	if (pfn_is_map_memory(PHYS_PFN(pa)))
		return false;
#else
	if (pfn_valid(PHYS_PFN(pa)))
		return false;
#endif
	for (unsigned int i = 0; i < dcp->nr_leaf_regions; i++)
		if (pa >= dcp->leaf_regions[i].start &&
		    pa + DCP_DART_PAGE - 1 <= dcp->leaf_regions[i].end)
			return true;
	for (unsigned int i = 0; i < dcp->nr_excluded_regions; i++)
		if (pa <= dcp->excluded_regions[i].end &&
		    dcp->excluded_regions[i].start < pa + DCP_DART_PAGE)
			return false;
	return region_intersects(pa, DCP_DART_PAGE, IORESOURCE_SYSTEM_RAM,
				 IORES_DESC_NONE) == REGION_DISJOINT;
}

static int apple_dart_dcp_import(struct apple_dart *dart, unsigned int sid,
				 struct dcp_adt_tables bounds, struct apple_dart_dcp **result)
{
	struct apple_dart_dcp *dcp;
	u64 root_pa;
	u8 *storage;
	u32 tcr, enabled;
	int ret;

	tcr = readl(dart->regs + DART_TCR(dart, sid));
	enabled = readl(dart->regs + DART_T8110_ENABLE_STREAMS);
	if (!dart->locked || dart->hw->type != DART_T8110 || dart->num_streams != 32 ||
	    PAGE_SIZE != SZ_16K || dart->pgsize != SZ_16K || dart->ias != 42 || dart->oas != 42 ||
	    dart->dma_min != DCP_DART_ALIAS ||
	    dart->dma_max != DCP_DART_ALIAS + DCP_DART_APERTURE - 1 ||
	    tcr != DART_T8110_TCR_TRANSLATE_ENABLE || !(enabled & BIT(sid)))
		return dev_err_probe(dart->dev, -ENODEV,
			"DCP admission: lock=%d type=%u streams=%u page=%u IAS/OAS=%u/%u range=%pad..%pad TCR%u=%08x enabled=%08x\n",
			dart->locked, dart->hw->type, dart->num_streams, dart->pgsize,
			dart->ias, dart->oas, &dart->dma_min, &dart->dma_max, sid, tcr, enabled);
	ret = apple_dart_dcp_excluded(dart, bounds.start, bounds.end);
	if (ret)
		return dev_err_probe(dart->dev, ret, "DCP table storage is not excluded memory\n");
	dcp = apple_dart_retained_alloc(dart, sizeof(*dcp));
	if (!dcp)
		return -ENOMEM;
	dcp->dart = dart;
	dcp->sid = sid;
	dcp->firmware = bounds;
	ret = apple_dart_dcp_excluded_regions(dcp);
	if (ret)
		return dev_err_probe(dart->dev, ret, "invalid reserved-memory leaf exclusions\n");
	ret = apple_dart_dcp_leaf_regions(dcp);
	if (ret)
		return dev_err_probe(dart->dev, ret, "DCP consumer reservations are invalid\n");
	spin_lock_init(&dcp->lock);
	mutex_init(&dcp->alloc_lock);
	INIT_LIST_HEAD(&dcp->allocations);
	INIT_LIST_HEAD(&dcp->shared);
	dcp->ttbr = readl(dart->regs + DART_TTBR(dart, sid, 0));
	root_pa = ((u64)dcp->ttbr >> 2) << 14;
	if ((dcp->ttbr & 3) != 1 || (sid != 23 && root_pa != bounds.start) ||
	    !dcp_dart_contains(bounds.start, bounds.end, root_pa, DCP_DART_PAGE))
		return dev_err_probe(dart->dev, -EINVAL,
			"DCP admission: TTBR%u=%08x root=%llx outside ADT [%llx,%llx) or invalid flags\n",
			sid, dcp->ttbr, root_pa, bounds.start, bounds.end);
	storage = apple_dart_retained_memremap(dart, bounds.start, bounds.end - bounds.start);
	if (IS_ERR(storage))
		return PTR_ERR(storage);
	if (!storage)
		return -ENOMEM;
	dcp->root = (u64 *)(storage + root_pa - bounds.start);
	for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++) {
		u64 pte = READ_ONCE(dcp->root[i]);
		u64 pa = dcp_dart_pte_phys(pte);

		if (dcp_dart_pte_check(pte, false))
			return dev_err_probe(dart->dev, -EINVAL,
				"DCP admission: root[%u]=%016llx has unsupported encoding\n",
				i, pte);
		dcp->entry[i] = pte;
		if (!pte)
			continue;
		if (pa == root_pa || !dcp_dart_contains(bounds.start, bounds.end, pa, DCP_DART_PAGE))
			return dev_err_probe(dart->dev, -EINVAL,
				"DCP admission: root[%u]=%016llx child=%llx outside ADT [%llx,%llx) or aliases root\n",
				i, pte, pa, bounds.start, bounds.end);
		for (unsigned int j = 0; j < i; j++)
			if (dcp->entry[j] && dcp_dart_pte_phys(dcp->entry[j]) == pa)
				return dev_err_probe(dart->dev, -EINVAL,
					"DCP admission: roots %u and %u alias child=%llx\n",
					j, i, pa);
		dcp->table[i] = (u64 *)(storage + pa - bounds.start);
		set_bit(i, dcp->foreign);
		if (sid != 23) {
			dcp->expected[i] = apple_dart_retained_alloc(dart, DCP_DART_PAGE);
			if (!dcp->expected[i])
				return -ENOMEM;
		}
		for (unsigned int j = 0; j < DCP_DART_ENTRIES; j++) {
			u64 leaf = READ_ONCE(dcp->table[i][j]);

			if (dcp_dart_pte_check(leaf, true))
				return dev_err_probe(dart->dev, -EINVAL,
					"DCP admission: leaf[%u][%u]=%016llx has unsupported encoding\n",
					i, j, leaf);
			if (!apple_dart_dcp_leaf_safe(dcp, leaf))
				return dev_err_probe(dart->dev, -EBUSY,
					"DCP inherited SID%u leaf[%u][%u] targets Linux RAM: %016llx\n",
					sid, i, j, leaf);
			if (sid != 23)
				dcp->expected[i][j] = leaf;
		}
	}
	/* Refuse an unstable root instead of trying to repair it. */
	for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++)
		if (READ_ONCE(dcp->root[i]) != dcp->entry[i])
			return dev_err_probe(dart->dev, -EBUSY,
				"DCP admission: root[%u] changed during import\n", i);
	if (sid != 23)
		for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++)
			if (dcp->table[i])
				for (unsigned int j = 0; j < DCP_DART_ENTRIES; j++)
					if (READ_ONCE(dcp->table[i][j]) != dcp->expected[i][j])
						return -EBUSY;
	if (!apple_dart_dcp_registers(dcp))
		return -EBUSY;
	if (sid == 0 && apple_dart_dcp_arena(dcp))
		return dev_err_probe(dart->dev, -ENOSPC,
			"DCP admission: no free SID0 slices for retained surfaces\n");
	*result = dcp;
	dev_info(dart->dev, "Preserving DCP SID%u root %pa and %u foreign 32-MiB slots; no writes\n",
		 sid, &root_pa, bitmap_weight(dcp->foreign, DCP_DART_ENTRIES));
	return 0;
}

static int apple_dart_dcp_prepare(struct apple_dart *dart)
{
	struct dcp_adt_tables tables;
	int ret = apple_dart_dcp_read_adt(dart, &tables, NULL);

	if (ret)
		return dev_err_probe(dart->dev, ret, "cannot validate existing ADT table handoff\n");
	return apple_dart_dcp_import(dart, 23, tables, &dart->dcp);
}

static bool apple_dart_dcp_iova(u64 iova)
{
	return iova >= DCP_DART_ALIAS && iova - DCP_DART_ALIAS < DCP_DART_APERTURE;
}

static phys_addr_t apple_dart_dcp_translate(struct apple_dart_dcp *dcp, u64 iova)
{
	unsigned int root = (iova >> 25) & 2047;
	unsigned int leaf = (iova >> 14) & 2047;
	u64 pte;
	phys_addr_t phys = 0;
	unsigned long flags;

	if (!apple_dart_dcp_iova(iova))
		return 0;
	spin_lock_irqsave(&dcp->lock, flags);
	/* DMA teardown still needs the host-owned physical address after quarantine.
	 * expected[] owns that provenance independently of failed hardware state.
	 */
	if (test_bit(root, dcp->claimed) && dcp->expected[root]) {
		pte = dcp->expected[root][leaf];
		if (pte)
			phys = dcp_dart_pte_phys(pte) | (iova & (DCP_DART_PAGE - 1));
		goto out;
	}
	if (dcp->failed || !apple_dart_dcp_registers(dcp))
		goto out;
	if (READ_ONCE(dcp->root[root]) != dcp->entry[root]) {
		dcp->failed = true;
		goto out;
	}
	if (!dcp->table[root])
		goto out;
	pte = READ_ONCE(dcp->table[root][leaf]);
	if (dcp_dart_pte_check(pte, true) ||
	    (test_bit(root, dcp->foreign) && !apple_dart_dcp_leaf_safe(dcp, pte)) ||
	    ((dcp->sid != 23 || !test_bit(root, dcp->foreign)) &&
	     pte != dcp->expected[root][leaf])) {
		dcp->failed = true;
		goto out;
	}
	if (!pte)
		goto out;
	phys = dcp_dart_pte_phys(pte) | (iova & (DCP_DART_PAGE - 1));
out:
	spin_unlock_irqrestore(&dcp->lock, flags);
	return phys;
}

/* The old loader name selects the same common Apple DRM SID0 consumer. */
static bool apple_dart_j713_scanout_node(struct device_node *node)
{
	return of_device_is_compatible(node, "apple,t8132-display-subsystem") ||
		of_device_is_compatible(node, "apple,t8132-display-diagnostics");
}

static int apple_dart_frontend_validate(struct apple_dart_dcp *dcp,
				       struct device_node *consumer)
{
	struct device_node *alias;
	struct of_phandle_args spec;
	u32 wanted = dcp->sid == 0 ? BIT(4) | BIT(6) : BIT(5);
	u32 seen = 0;
	unsigned long flags;
	int nr, ret;

	if (!apple_dart_dcp_registers(dcp))
		return -EBUSY;
	/* Read-only admission must not hide leaf drift behind stable root pointers. */
	ret = 0;
	spin_lock_irqsave(&dcp->lock, flags);
	for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++) {
		if (READ_ONCE(dcp->root[i]) != dcp->entry[i]) {
			WRITE_ONCE(dcp->failed, true);
			ret = -EBUSY;
			break;
		}
		if (dcp->table[i])
			for (unsigned int j = 0; j < DCP_DART_ENTRIES; j++)
				if (READ_ONCE(dcp->table[i][j]) != dcp->expected[i][j]) {
					WRITE_ONCE(dcp->failed, true);
					ret = -EBUSY;
					goto snapshot_done;
				}
	}
snapshot_done:
	spin_unlock_irqrestore(&dcp->lock, flags);
	if (ret)
		return ret;

	alias = of_find_node_by_path(dcp->sid == 0 ? "disp0" : "disp0_piodma");
	ret = alias && consumer == alias && of_device_is_available(alias) &&
		of_property_read_bool(alias, "apple,j713-inherited-mappings") ? 0 : -ENODEV;
	of_node_put(alias);
	if (ret)
		return ret;
	if (dcp->sid == 0 && !apple_dart_j713_scanout_node(consumer))
		return -ENODEV;
	if (of_count_phandle_with_args(consumer, "iommus", "#iommu-cells") != 1)
		return -EINVAL;
	ret = of_parse_phandle_with_args(consumer, "iommus", "#iommu-cells", 0, &spec);
	if (ret)
		return ret;
	ret = spec.np == dcp->dart->dev->of_node && spec.args_count == 1 &&
		spec.args[0] == dcp->sid ? 0 : -EINVAL;
	of_node_put(spec.np);
	if (ret)
		return ret;
	nr = of_property_count_u32_elems(consumer, "memory-region");
	if (nr != (dcp->sid == 0 ? 2 : 1))
		return -EINVAL;
	for (int i = 0; i < nr; i++) {
		struct device_node *node = of_parse_phandle(consumer, "memory-region", i);
		struct reserved_mem *rmem;
		struct resource res;
		const __be32 *maps;
		unsigned int matches = 0;
		int bytes;

		if (!node)
			return -EINVAL;
		rmem = of_reserved_mem_lookup(node);
		ret = -EINVAL;
		if (!of_device_is_compatible(node, "apple,asc-mem") ||
		    !of_property_read_bool(node, "no-map") ||
		    of_property_count_u32_elems(node, "reg") != 4 ||
		    of_address_to_resource(node, 0, &res) || !rmem ||
		    rmem->base != res.start || rmem->size != resource_size(&res))
			goto put_node;
		maps = of_get_property(node, "iommu-addresses", &bytes);
		if (!maps || bytes <= 0 || bytes % (5 * sizeof(*maps)))
			goto put_node;
		for (int j = 0; j < bytes / sizeof(*maps); j += 5) {
			struct device_node *owner = of_find_node_by_phandle(be32_to_cpu(maps[j]));
			const struct dcp_adt_segment *segment;
			u64 iova = of_read_number(maps + j + 1, 2);
			u64 size = of_read_number(maps + j + 3, 2);
			bool ours = owner == consumer;
			bool cells = owner && of_n_addr_cells(owner) == 2 && of_n_size_cells(owner) == 2;
			u32 bit;

			of_node_put(owner);
			/* Validate other owners' tuple extents too, before skipping them. */
			if (!cells || size != resource_size(&res) ||
			    ((res.start | size | iova) & (DCP_DART_PAGE - 1)) ||
			    !dcp_dart_contains(DCP_DART_ALIAS, DCP_DART_ALIAS + DCP_DART_APERTURE,
					       iova, size))
				goto put_node;
			segment = dcp_adt_segment_match(&dcp->firmware, res.start, iova, size);
			if (!segment || segment->kind != 2)
				goto put_node;
			if (!ours)
				continue;
			bit = BIT(segment - dcp->firmware.segment);
			if (++matches != 1 || !(wanted & bit) || (seen & bit))
				goto put_node;
			seen |= bit;
			for (u64 off = 0; off < size; off += DCP_DART_PAGE) {
				unsigned int root = ((iova + off) >> 25) & 2047;
				unsigned int leaf = ((iova + off) >> 14) & 2047;

				if (apple_dart_dcp_translate(dcp, iova + off) != res.start + off ||
				    !test_bit(root, dcp->foreign) ||
				    !dcp_dart_pte_frontend(READ_ONCE(dcp->table[root][leaf])))
					goto put_node;
			}
		}
		ret = matches == 1 ? 0 : -EINVAL;
put_node:
		of_node_put(node);
		if (ret)
			return ret;
	}
	return seen == wanted && apple_dart_dcp_registers(dcp) ? 0 : -EINVAL;
}

/* Read-only admission; SID0 later maps only free slices, SID16 only via its pair API. */
static int apple_dart_frontend_prepare(struct apple_dart *dart)
{
	struct apple_dart_dcp *borrowed[2];
	struct dcp_adt_tables firmware;
	struct dcp_adt_frontend frontend;
	int ret = apple_dart_dcp_read_adt(dart, &firmware, &frontend);

	if (ret)
		return ret;
	for (unsigned int i = 0; i < 2; i++) {
		struct dcp_adt_tables bounds = firmware;
		struct device_node *node;

		bounds.start = frontend.start[i];
		bounds.end = frontend.end[i];
		ret = apple_dart_dcp_import(dart, i ? 16 : 0, bounds, &borrowed[i]);
		if (ret)
			return ret;
		node = of_find_node_by_path(i ? "disp0_piodma" : "disp0");
		ret = node ? apple_dart_frontend_validate(borrowed[i], node) : -ENODEV;
		of_node_put(node);
		if (ret)
			return dev_err_probe(dart->dev, ret,
				"SID%u inherited frontend mapping validation failed\n", i ? 16 : 0);
	}
	/* Neither stream is published before both imports and all mappings pass. */
	if (!apple_dart_dcp_registers(borrowed[0]) || !apple_dart_dcp_registers(borrowed[1]))
		return -EBUSY;
	dart->frontend[0] = borrowed[0];
	dart->frontend[1] = borrowed[1];
	dev_info(dart->dev, "Inherited SID0/SID16 frontend admitted; SID0 strict DMA outside %u foreign and %u retained slices\n",
		 bitmap_weight(borrowed[0]->foreign, DCP_DART_ENTRIES), DCP_DART_SCANOUT_SLOTS);
	return 0;
}

static ssize_t j713_display_state_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct apple_dart *dart = dev_get_drvdata(dev);
	int status[2];

	if (!dart->frontend[0] || !dart->frontend[1] || !READ_ONCE(dart->iommu.ready))
		return -ENODEV;
	for (unsigned int i = 0; i < 2; i++) {
		struct device_node *node = of_find_node_by_path(i ? "disp0_piodma" : "disp0");

		status[i] = node ? apple_dart_frontend_validate(dart->frontend[i], node) : -ENODEV;
		of_node_put(node);
		if (status[i])
			WRITE_ONCE(dart->frontend[i]->failed, true);
	}
	return sysfs_emit(buf, "sid0=%d sid16=%d host_mapping=%u\n", status[0], status[1],
			 READ_ONCE(dart->frontend[1]->shared_mapping));
}
static DEVICE_ATTR(j713_display_state, 0400, j713_display_state_show, NULL);

static struct attribute *apple_dart_frontend_attrs[] = {
	&dev_attr_j713_display_state.attr,
	NULL,
};

static const struct attribute_group apple_dart_frontend_group = {
	.attrs = apple_dart_frontend_attrs,
};

static int apple_dart_dcp_validate(struct apple_dart_dcp *dcp, struct device *dev)
{
	struct device_node *node;
	const char *uuid;
	unsigned int count = 0;
	unsigned int oslogs = 0;
	u32 seen = 0;
	int nr;
	int ret = 0;

	if (dcp->sid != 23) {
		if (dcp->owner && dcp->owner != dev)
			return -EBUSY;
		ret = apple_dart_frontend_validate(dcp, dev->of_node);
		if (!ret)
			dcp->owner = dev;
		return ret;
	}

	if (!of_device_is_compatible(dev->of_node,
				     dcp->dart->dcp_instance == &dcp_adt_internal ?
				     "apple,t8132-dcp" : "apple,t8132-dcpext") ||
	    of_property_read_string(dev->of_node, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, "81BAF476-679D-3CC0-8C6A-E6B6FA659D5E") ||
	    (dcp->owner && dcp->owner != dev))
		return -ENODEV;
	/* Do not silently skip malformed ranges or allocation failures in a list helper. */
	nr = of_property_count_u32_elems(dev->of_node, "memory-region");
	if (nr <= 0 || nr > 64)
		return -EINVAL;
	for (int i = 0; i < nr; i++) {
		struct resource res;
		const __be32 *maps;
		unsigned int matches = 0;
		int bytes;

		node = of_parse_phandle(dev->of_node, "memory-region", i);
		if (!node)
			return -EINVAL;
		if (!of_device_is_compatible(node, "apple,asc-mem") ||
		    !of_property_read_bool(node, "no-map") ||
		    of_address_to_resource(node, 0, &res)) {
			ret = -EINVAL;
			goto put_node;
		}
		maps = of_get_property(node, "iommu-addresses", &bytes);
		if (of_property_read_bool(node, "apple,dcp-os-log")) {
			struct reserved_mem *rmem = of_reserved_mem_lookup(node);

			if (++oslogs != 1 || maps || resource_size(&res) != 0x24000 ||
			    (res.start & (DCP_DART_PAGE - 1)) || !rmem ||
			    rmem->base != res.start || rmem->size != resource_size(&res))
				ret = -EINVAL;
			/* Explicitly physical and possibly relocated, not a DART map. */
			goto put_node;
		}
		if (!maps || bytes <= 0 || bytes % (5 * sizeof(*maps))) {
			ret = -EINVAL;
			goto put_node;
		}
		for (int j = 0; j < bytes / sizeof(*maps); j += 5) {
			const struct dcp_adt_segment *segment;
			struct device_node *owner = of_find_node_by_phandle(be32_to_cpu(maps[j]));
			bool ours = owner == dev->of_node;
			u64 iova = of_read_number(maps + j + 1, 2);
			u64 size = of_read_number(maps + j + 3, 2);
			bool cells = owner && of_n_addr_cells(owner) == 2 && of_n_size_cells(owner) == 2;

			of_node_put(owner);
			if (!cells) {
				ret = -EINVAL;
				goto put_node;
			}
			if (!ours)
				continue;
			if (++matches != 1 || size != resource_size(&res) ||
			    ((res.start | iova | size) & (DCP_DART_PAGE - 1)) ||
			    !dcp_dart_contains(DCP_DART_ALIAS, DCP_DART_ALIAS + DCP_DART_APERTURE,
					       iova, size)) {
				ret = -EINVAL;
				goto put_node;
			}
			segment = dcp_adt_segment_match(&dcp->firmware, res.start, iova, size);
			if (!segment || (seen & BIT(segment - dcp->firmware.segment))) {
				dev_err(dev, "Inherited DCP mapping lacks matching ADT segment at %llx\n",
					iova);
				ret = -EINVAL;
				goto put_node;
			}
			seen |= BIT(segment - dcp->firmware.segment);
			for (u64 off = 0; off < size; off += DCP_DART_PAGE) {
				unsigned int root = ((iova + off) >> 25) & 2047;
				unsigned int leaf = ((iova + off) >> 14) & 2047;

				if (apple_dart_dcp_translate(dcp, iova + off) != res.start + off ||
				    !test_bit(root, dcp->foreign) ||
				    !dcp_dart_pte_firmware(READ_ONCE(dcp->table[root][leaf]),
							  segment->kind)) {
					dev_err(dev, "Inherited DCP mapping mismatch at %llx\n", iova + off);
					ret = -EINVAL;
					goto put_node;
				}
			}
			count++;
		}
		if (!matches)
			ret = -EINVAL;
put_node:
		of_node_put(node);
		if (ret)
			return ret;
	}
	if (!ret && (!count || oslogs != 1))
		ret = -ENODEV;
	for (u32 i = 0; !ret && i < dcp->firmware.num_segments; i++)
		if (dcp->firmware.segment[i].kind != 10 && !(seen & BIT(i)))
			ret = -EINVAL;
	if (!ret)
		dcp->owner = dev;
	return ret;
}

static void apple_dart_dcp_reserve(struct apple_dart_dcp *dcp, struct list_head *head)
{
	struct iommu_resv_region *region, *next;
	unsigned int start = 0, end;
	LIST_HEAD(slices);

	/* SID0: whole foreign slices and the retained arena; generic DMA gets the rest. */
	while (dcp->sid == 0) {
		while (start < DCP_DART_ENTRIES && !test_bit(start, dcp->foreign) &&
		       !test_bit(start, dcp->arena))
			start++;
		if (start == DCP_DART_ENTRIES) {
			list_splice_tail(&slices, head);
			return;
		}
		for (end = start; end < DCP_DART_ENTRIES && (test_bit(end, dcp->foreign) ||
		     test_bit(end, dcp->arena)); end++)
			;
		region = iommu_alloc_resv_region(DCP_DART_ALIAS + start * DCP_DART_SLICE,
						 (end - start) * DCP_DART_SLICE, 0,
						 IOMMU_RESV_RESERVED, GFP_KERNEL);
		if (!region)
			break;
		list_add_tail(&region->list, &slices);
		start = end;
	}
	/* Never return a partial SID0 list: fall back to the whole aperture, failed. */
	list_for_each_entry_safe(region, next, &slices, list)
		kfree(region);
	if (dcp->sid == 0)
		WRITE_ONCE(dcp->failed, true);
	/* SID23/SID16 domains: only the retained allocators may allocate there. */
	region = iommu_alloc_resv_region(DCP_DART_ALIAS, DCP_DART_APERTURE, 0,
					IOMMU_RESV_RESERVED, GFP_KERNEL);
	if (!region) {
		WRITE_ONCE(dcp->failed, true);
		return;
	}
	list_add_tail(&region->list, head);
}

/* Pin a page an uncertain SID0 translation may still reach; only reboot drops it. */
static unsigned int apple_dart_dcp_retain(u64 pte)
{
	unsigned long pfn = PHYS_PFN(dcp_dart_pte_phys(pte));

	return pte && pfn_valid(pfn) && folio_try_get(page_folio(pfn_to_page(pfn)));
}

/* SID0 only: FLUSH_SID word 0x100 at +0x80 with bounded busy waits; failure is sticky. */
static int apple_dart_dcp_flush(struct apple_dart_dcp *dcp)
{
	struct apple_dart_stream_map map = { .dart = dcp->dart };

	if (dcp->sid != 0 || !apple_dart_dcp_registers(dcp))
		return -EIO;
	bitmap_zero(map.sidmap, DART_MAX_STREAMS);
	set_bit(0, map.sidmap);
	dma_wmb();
	return dcp->dart->hw->invalidate_tlb(&map);
}

static int apple_dart_dcp_map_owned(struct apple_dart_dcp *dcp, u64 iova, phys_addr_t phys,
				    size_t size, size_t count, int prot, gfp_t gfp,
				    size_t *mapped, bool shared)
{
	unsigned int root = (iova >> 25) & 2047;
	unsigned int leaf = (iova >> 14) & 2047;
	u64 flags = 1 | (0xfffULL << 40);
	u64 *fresh = NULL, *expected = NULL;
	unsigned long irqflags;
	bool generic = !shared && dcp->sid == 0;
	int ret = 0;

	/* Generic SID0 DMA keeps the display's no-cache leaf policy. */
	if ((dcp->sid != 23 && !generic && !(shared && (dcp->sid == 16 || dcp->sid == 0) &&
	     test_bit(root, dcp->claimed) && prot == (IOMMU_READ | IOMMU_WRITE))) ||
	    (generic && (prot & ~(IOMMU_READ | IOMMU_WRITE | IOMMU_NOEXEC))) ||
	    (shared && dcp->sid == 0 && !test_bit(root, dcp->arena)) ||
	    !apple_dart_dcp_iova(iova) || size != DCP_DART_PAGE || !count ||
	    ((iova | phys) & (DCP_DART_PAGE - 1)) ||
	    !(prot & (IOMMU_READ | IOMMU_WRITE)) || phys >> 42 ||
	    test_bit(root, dcp->foreign))
		return -EINVAL;
	/* Private slots have one writer: the paired or SID0 scanout allocation API. */
	if (shared != !!test_bit(root, dcp->shared_roots) ||
	    (generic && test_bit(root, dcp->arena)))
		return -EPERM;
	count = min_t(size_t, count, DCP_DART_ENTRIES - leaf);
	/* Failed SID0 unmap must be able to pin every reachable allocation. A
	 * folio reference cannot stop slab objects being reused within that page.
	 * Generic DMA therefore admits only whole normal page-backed allocations.
	 */
	if (generic)
		for (size_t i = 0; i < count; i++) {
			unsigned long pfn = PHYS_PFN(phys + i * size);
			struct page *page;

			if (!pfn_valid(pfn))
				return -EOPNOTSUPP;
			page = pfn_to_page(pfn);
			if (PageReserved(page) || folio_test_slab(page_folio(page)) ||
			    !page_count(page))
				return -EOPNOTSUPP;
		}
	if (count * size > BIT_ULL(42) - phys)
		return -ERANGE;
	if (!(prot & IOMMU_READ))
		flags |= BIT(3);
	if (!(prot & IOMMU_WRITE))
		flags |= BIT(2);
	if (!(prot & IOMMU_CACHE))
		flags |= BIT(1);
	if (!READ_ONCE(dcp->table[root])) {
		fresh = (void *)get_zeroed_page(gfp);
		if (!fresh)
			return -ENOMEM;
		expected = (void *)get_zeroed_page(gfp);
		if (!expected) {
			free_page((unsigned long)fresh);
			return -ENOMEM;
		}
		if ((virt_to_phys(fresh) & (DCP_DART_PAGE - 1)) || virt_to_phys(fresh) >> 42) {
			free_page((unsigned long)fresh);
			free_page((unsigned long)expected);
			return -ERANGE;
		}
	}
	spin_lock_irqsave(&dcp->lock, irqflags);
	/* A paired reservation may have won while this call allocated a leaf table. */
	if (shared != !!test_bit(root, dcp->shared_roots)) {
		ret = -EPERM;
		goto out;
	}
	if (dcp->failed || READ_ONCE(dcp->root[root]) != dcp->entry[root]) {
		dcp->failed = true;
		ret = -EIO;
		goto out;
	}
	if (!dcp->table[root]) {
		u64 entry = (virt_to_phys(fresh) >> 4) | 1;

		/* Publish a zeroed, host-owned table; never replace a foreign entry. */
		dma_wmb();
		if (cmpxchg64(&dcp->root[root], 0, entry)) {
			dcp->failed = true;
			ret = -EBUSY;
			goto out;
		}
		dcp->entry[root] = entry;
		dcp->table[root] = fresh;
		dcp->expected[root] = expected;
		fresh = NULL; /* Tables remain pinned until reboot, including empty ones. */
		expected = NULL;
	}
	for (size_t i = 0; i < count; i++) {
		if (READ_ONCE(dcp->table[root][leaf + i]) != dcp->expected[root][leaf + i]) {
			dcp->failed = true;
			ret = -EIO;
			goto out;
		}
		if (READ_ONCE(dcp->table[root][leaf + i])) {
			ret = -EEXIST;
			goto out;
		}
	}
	for (size_t i = 0; i < count; i++) {
		u64 pte = ((phys + i * size) >> 4) | flags;

		dcp->expected[root][leaf + i] = pte;
		WRITE_ONCE(dcp->table[root][leaf + i], pte);
	}
	dma_wmb();
	if (mapped)
		*mapped += count * size;
out:
	spin_unlock_irqrestore(&dcp->lock, irqflags);
	if (fresh)
		free_page((unsigned long)fresh);
	if (expected)
		free_page((unsigned long)expected);
	return ret;
}

static int apple_dart_dcp_map(struct apple_dart_dcp *dcp, u64 iova, phys_addr_t phys,
			      size_t size, size_t count, int prot, gfp_t gfp, size_t *mapped)
{
	return apple_dart_dcp_map_owned(dcp, iova, phys, size, count, prot, gfp, mapped, false);
}

/*
 * Called with dcp->lock held after a refused SID0 unmap. DMA callers release
 * the whole IOVA range and their backing, so retain every generic leaf before
 * reporting logical retirement. Failed domains reject all subsequent maps;
 * their hardware translations and retained pages are never reused.
 */
static bool apple_dart_dcp_retain_span(struct apple_dart_dcp *dcp, u64 iova,
				      size_t count, unsigned long *retained)
{
	u64 end = DCP_DART_ALIAS + DCP_DART_APERTURE;
	bool complete = true;

	if (count < (end - iova) / DCP_DART_PAGE)
		end = iova + count * DCP_DART_PAGE;
	while (iova < end) {
		unsigned int root = (iova >> 25) & 2047;
		unsigned int leaf = (iova >> 14) & 2047;
		u64 next = min_t(u64, end, (iova | (DCP_DART_SLICE - 1)) + 1);

		/* A generic DMA unmap must not retire a reserved/private slice. */
		if (test_bit(root, dcp->foreign) || test_bit(root, dcp->arena) ||
		    test_bit(root, dcp->shared_roots)) {
			complete = false;
		} else if (dcp->table[root]) {
			for (u64 dva = iova; dva < next; dva += DCP_DART_PAGE, leaf++) {
				u64 pte = dcp->expected[root][leaf];

				if (!pte)
					continue;
				if (apple_dart_dcp_retain(pte))
					(*retained)++;
				else
					complete = false;
			}
		}
		iova = next;
	}
	return complete;
}

static size_t apple_dart_dcp_unmap(struct apple_dart_dcp *dcp, u64 iova,
				 size_t size, size_t count, struct iommu_iotlb_gather *gather)
{
	unsigned int root = (iova >> 25) & 2047;
	unsigned int leaf = (iova >> 14) & 2047;
	unsigned long flags, retained = 0;
	size_t unmapped = 0, cleared = 0, span = count; /* Whole remaining core range. */

	if ((dcp->sid != 23 && dcp->sid != 0) || !apple_dart_dcp_iova(iova) ||
	    size != DCP_DART_PAGE || !count || (iova & (size - 1)) ||
	    count > (DCP_DART_ALIAS + DCP_DART_APERTURE - iova) / size ||
	    test_bit(root, dcp->foreign) || test_bit(root, dcp->shared_roots) ||
	    test_bit(root, dcp->arena))
		return 0;
	spin_lock_irqsave(&dcp->lock, flags);
	if (test_bit(root, dcp->shared_roots))
		goto out;
	count = min_t(size_t, count, DCP_DART_ENTRIES - leaf);
	if (!dcp->table[root]) {
		if (dcp->failed)
			goto refuse;
		goto out;
	}
	if (dcp->failed || READ_ONCE(dcp->root[root]) != dcp->entry[root])
		goto refuse;
	for (size_t i = 0; i < count; i++) {
		if (READ_ONCE(dcp->table[root][leaf + i]) != dcp->expected[root][leaf + i]) {
			dcp->failed = true;
			goto refuse;
		}
	}
	while (cleared < count && dcp->table[root][leaf + cleared])
		WRITE_ONCE(dcp->table[root][leaf + cleared++], 0);
	dma_wmb();
	/* Strict SID0: invalidate before the IOVA or its pages can be reused. */
	if (cleared && dcp->sid == 0 && apple_dart_dcp_flush(dcp)) {
		/* A stale translation may survive: republish the leaves and keep the pages. */
		for (size_t i = 0; i < cleared; i++)
			WRITE_ONCE(dcp->table[root][leaf + i], dcp->expected[root][leaf + i]);
		dma_wmb();
		goto refuse;
	}
	for (size_t i = 0; i < cleared; i++)
		dcp->expected[root][leaf + i] = 0;
	unmapped = cleared * size;
	goto out;
refuse:
	dcp->failed = true;
	/* Claim logical retirement only once every possible DMA backing page
	 * is retained. IOVA reuse cannot publish a new translation: failed is
	 * sticky and every map path checks it under this same lock.
	 */
	if (dcp->sid == 0 && apple_dart_dcp_retain_span(dcp, iova, span, &retained))
		unmapped = span * size;
	dcp->retained += retained;
out:
	spin_unlock_irqrestore(&dcp->lock, flags);
	if (retained)
		dev_err_ratelimited(dcp->dart->dev,
				    "SID0 hardware unmap at %llx quarantined; retaining %lu pages until reboot\n",
				    iova, retained);
	if (unmapped)
		iommu_iotlb_gather_add_range(gather, iova, unmapped);
	return unmapped;
}

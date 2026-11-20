/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Included by apple-dart.c. J713 display coprocessors: the internal DCP's
 * firmware and frontend DARTs, and the external DCPs' firmware and display
 * DARTs. Foreign tables stay live.
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
	/* SID0 arena slices claimed as remappable host windows. */
	DECLARE_BITMAP(window, DCP_DART_ENTRIES);
	unsigned long retained; /* SID0 pages pinned after an uncertain unmap. */
	u32 ttbr;
	unsigned int sid;
	bool failed;
	/* the reason it failed was logged (apple_dart_dcp_fail()) */
	bool failed_logged;
	bool shared_mapping;
	bool shared_flushing;
	/*
	 * J713 external display DART: the slice being filled by
	 * apple_dart_dcp_dispext_map() (-1: none), and how much of it is used.
	 */
	int sub_root;
	u64 sub_used;
};

/*
 * Latch @d failed (sticky), then say why, the first time: every
 * failure of a borrowed DCP stream goes through here, so none is silent.
 * The log arguments are evaluated after the latch, and must not dereference
 * anything the failure may have left invalid.
 */
#define apple_dart_dcp_fail(d, fmt, ...)					\
	do {									\
		struct apple_dart_dcp *__dcp = (d);				\
										\
		WRITE_ONCE(__dcp->failed, true);				\
		if (!READ_ONCE(__dcp->failed_logged)) {				\
			WRITE_ONCE(__dcp->failed_logged, true);			\
			dev_err(__dcp->dart->dev, "DCP SID%u retired: " fmt "\n",	\
				__dcp->sid, ##__VA_ARGS__);			\
		}								\
	} while (0)

/*
 * A power-gated display DART (see struct apple_dart): its power not
 * confirmed since it may have gone off (from sleep, or from a power-down by
 * its DCP, until it is confirmed on again): no register of it may be read or
 * written, and nothing is retired for that.
 */
static inline bool apple_dart_pm_unconfirmed(const struct apple_dart *dart)
{
	return dart->power_gated && READ_ONCE(dart->pm_unverified);
}

/*
 * A use of a power-gated display DART's registers, taken at the start of an
 * operation and dropped at its end, and only with its power confirmed:
 * false (the caller returns -EAGAIN, nothing retired) from the moment it may
 * go off until its next confirmed power-up. Inside it the operation's steps,
 * flushes included, run unchecked: an invalidation waits for it to end. Any
 * other DART: always true, no count.
 */
static inline bool apple_dart_pm_get(struct apple_dart *dart)
{
	unsigned long flags;
	bool ok;

	if (!dart->power_gated)
		return true;
	spin_lock_irqsave(&dart->pm_gate_lock, flags);
	ok = !dart->pm_unverified;
	if (ok)
		dart->pm_users++;
	spin_unlock_irqrestore(&dart->pm_gate_lock, flags);
	return ok;
}

static inline void apple_dart_pm_put(struct apple_dart *dart)
{
	unsigned long flags;

	if (!dart->power_gated)
		return;
	spin_lock_irqsave(&dart->pm_gate_lock, flags);
	if (!--dart->pm_users)
		wake_up_all(&dart->pm_wq);
	spin_unlock_irqrestore(&dart->pm_gate_lock, flags);
}

/*
 * Sleep or a power-down: no power confirmed from now on, a new generation (a
 * check still under way confirms nothing), and every use in flight waited
 * for. @suspend: asleep until the matching resume (no check runs then).
 */
static inline void apple_dart_pm_invalidate(struct apple_dart *dart, bool suspend)
{
	unsigned long flags;

	spin_lock_irqsave(&dart->pm_gate_lock, flags);
	dart->pm_unverified = true;
	dart->pm_sleeping = suspend;
	dart->pm_gen++;
	spin_unlock_irqrestore(&dart->pm_gate_lock, flags);
	wait_event(dart->pm_wq, !READ_ONCE(dart->pm_users));
}

/*
 * The locks of every borrowed stream of @dart, in a fixed order (SID23, then
 * SID0, then SID16). An internal display DART's host_off changes only with
 * all of them held, so under any one of them it is stable: while it is set,
 * the DART's DCP has it powered down (or powering down), and its generic DMA
 * streams (SID0, SID23) update their page tables in memory only, with no
 * register used; its power-up flushes every translation, with these locks
 * held, before it enables the streams again.
 */
static void apple_dart_host_lock(struct apple_dart *dart, unsigned long *flags)
{
	struct apple_dart_dcp *streams[] = { dart->dcp, dart->frontend[0], dart->frontend[1] };
	unsigned int held = 0;

	local_irq_save(*flags);
	for (unsigned int i = 0; i < ARRAY_SIZE(streams); i++)
		if (streams[i])
			spin_lock_nested(&streams[i]->lock, held++);
}

static void apple_dart_host_unlock(struct apple_dart *dart, unsigned long flags)
{
	struct apple_dart_dcp *streams[] = { dart->frontend[1], dart->frontend[0], dart->dcp };

	for (unsigned int i = 0; i < ARRAY_SIZE(streams); i++)
		if (streams[i])
			spin_unlock(&streams[i]->lock);
	local_irq_restore(flags);
}

/* Every borrowed stream of @dart, for one reason. */
#define apple_dart_fail_borrowed(dart, fmt, ...)				\
	do {									\
		struct apple_dart *__dart = (dart);				\
										\
		if (__dart->dcp)						\
			apple_dart_dcp_fail(__dart->dcp, fmt, ##__VA_ARGS__);	\
		for (unsigned int __i = 0; __i < ARRAY_SIZE(__dart->frontend); __i++)	\
			if (__dart->frontend[__i])				\
				apple_dart_dcp_fail(__dart->frontend[__i], fmt,	\
						    ##__VA_ARGS__);		\
	} while (0)

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
/* An external display DART's SID0 arena: the surfaces and host windows. */
#define DCP_DART_DISPEXT_SLOTS	10

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

/*
 * Whether either stream of an external display DART was retired: a hardware
 * failure of one retires the whole DART, so every mapping and publication
 * refuses then.
 */
static inline bool apple_dart_dispext_retired(struct apple_dart *dart)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(dart->frontend); i++)
		if (dart->frontend[i] && READ_ONCE(dart->frontend[i]->failed))
			return true;
	return false;
}

#define DCP_DART_REGS_FMT \
	"registers not as adopted: PROTECT %08x, TTBR0 %08x (adopted %08x), TCR %08x, ENABLE_STREAMS %08x"

static bool apple_dart_dcp_registers(struct apple_dart_dcp *dcp)
{
	struct apple_dart *dart = dcp->dart;
	u32 protect, ttbr, tcr, streams;

	/* An external display DART's callers hold a use of it (apple_dart_pm_get()). */
	if (READ_ONCE(dcp->failed))
		return false;
	protect = readl(dart->regs + DART_T8110_PROTECT);
	ttbr = readl(dart->regs + DART_TTBR(dart, dcp->sid, 0));
	tcr = readl(dart->regs + DART_TCR(dart, dcp->sid));
	streams = readl(dart->regs + DART_T8110_ENABLE_STREAMS);
	if ((protect & DART_T8110_PROTECT_TTBR_TCR) && ttbr == dcp->ttbr &&
	    tcr == DART_T8110_TCR_TRANSLATE_ENABLE && (streams & BIT(dcp->sid)))
		return true;
	/* On an external display DART a hardware failure retires the whole DART. */
	if (dart->dispext)
		apple_dart_fail_borrowed(dart, "SID%u " DCP_DART_REGS_FMT, dcp->sid, protect, ttbr,
					 dcp->ttbr, tcr, streams);
	else
		apple_dart_dcp_fail(dcp, DCP_DART_REGS_FMT, protect, ttbr, dcp->ttbr, tcr, streams);
	return false;
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
	/* An external display DART reads its instance's frontend. */
	if (frontend && dart->dispext)
		ret = dcp_adt_parse_common(data, resource_size(&res), dart->dcp_instance, tables,
					   frontend);
	else
		ret = frontend ? dcp_adt_parse_frontend(data, resource_size(&res),
							dart->dcp_instance, tables,
							frontend) :
			dcp_adt_parse_instance(data, resource_size(&res), dart->dcp_instance,
					       tables);
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

/*
 * Fixed at admission, so every reservation query reports the same SID0 slices.
 * An external display DART takes up to DCP_DART_DISPEXT_SLOTS, for its host
 * windows (direct scanout) besides the retained surfaces; it needs only
 * DCP_DART_SCANOUT_SLOTS.
 */
static int apple_dart_dcp_arena(struct apple_dart_dcp *dcp)
{
	unsigned int want = dcp->dart->dispext ? DCP_DART_DISPEXT_SLOTS :
			    DCP_DART_SCANOUT_SLOTS;
	unsigned int slots = 0;

	for (unsigned int i = 0; i < DCP_DART_ENTRIES && slots < want; i++) {
		if (dcp->entry[i] || test_bit(i, dcp->foreign))
			continue;
		set_bit(i, dcp->arena);
		slots++;
	}
	if (dcp->dart->dispext)
		dev_dbg(dcp->dart->dev, "SID0 arena: %u of %u slices (%u for surfaces)\n",
			slots, want, DCP_DART_SCANOUT_SLOTS);
	return slots >= DCP_DART_SCANOUT_SLOTS ? 0 : -ENOSPC;
}

/* Snapshot only this consumer's real no-map reservations, before table admission. */
static int apple_dart_dcp_leaf_regions(struct apple_dart_dcp *dcp)
{
	struct device_node *consumer, *node;
	/* An external display DART's SID16 serves its dcpext's memory. */
	const char *alias = dcp->sid == 23 || dcp->dart->dispext ?
		dcp->dart->dcp_instance->dcp : dcp->sid == 0 ? "disp0" : "disp0_piodma";
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
	dcp->sub_root = -1;
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
	bool used = false;

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
	/*
	 * Powered down by its DCP (stable under this lock): no register, so
	 * only the host-owned leaves, which expected[] mirrors.
	 */
	if (READ_ONCE(dcp->dart->host_off)) {
		if (!test_bit(root, dcp->foreign) && dcp->expected[root]) {
			pte = dcp->expected[root][leaf];
			if (pte)
				phys = dcp_dart_pte_phys(pte) | (iova & (DCP_DART_PAGE - 1));
		}
		goto out;
	}
	/*
	 * An external display DART's registers only inside a use of them (its
	 * power confirmed, a sleep waits for it): none, no lookup.
	 */
	if (dcp->failed || !apple_dart_pm_get(dcp->dart))
		goto out;
	used = true;
	if (!apple_dart_dcp_registers(dcp))
		goto out;
	if (READ_ONCE(dcp->root[root]) != dcp->entry[root]) {
		apple_dart_dcp_fail(dcp, "translate: root %u changed: %016llx, adopted %016llx",
				    root, READ_ONCE(dcp->root[root]), dcp->entry[root]);
		goto out;
	}
	if (!dcp->table[root])
		goto out;
	pte = READ_ONCE(dcp->table[root][leaf]);
	if (dcp_dart_pte_check(pte, true) ||
	    (test_bit(root, dcp->foreign) && !apple_dart_dcp_leaf_safe(dcp, pte)) ||
	    ((dcp->sid != 23 || !test_bit(root, dcp->foreign)) &&
	     pte != dcp->expected[root][leaf])) {
		/* Inherited SID23 tables have no expected[]: their leaves on their own. */
		if (dcp->expected[root])
			apple_dart_dcp_fail(dcp, "translate: leaf %u/%u %016llx, expected %016llx",
					    root, leaf, pte, dcp->expected[root][leaf]);
		else
			apple_dart_dcp_fail(dcp, "translate: inherited leaf %u/%u %016llx invalid or not safe",
					    root, leaf, pte);
		goto out;
	}
	if (!pte)
		goto out;
	phys = dcp_dart_pte_phys(pte) | (iova & (DCP_DART_PAGE - 1));
out:
	spin_unlock_irqrestore(&dcp->lock, flags);
	if (used)
		apple_dart_pm_put(dcp->dart);
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
	bool main_external = dcp->dart->dcp_instance == &dcp_adt_main_external;
	u32 wanted = dcp->sid == 0 ?
		(main_external ? BIT(4) : BIT(4) | BIT(6)) : BIT(5);
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
			apple_dart_dcp_fail(dcp, "root %u changed: %016llx, adopted %016llx", i,
					    READ_ONCE(dcp->root[i]), dcp->entry[i]);
			ret = -EBUSY;
			break;
		}
		if (dcp->table[i])
			for (unsigned int j = 0; j < DCP_DART_ENTRIES; j++)
				if (READ_ONCE(dcp->table[i][j]) != dcp->expected[i][j]) {
					apple_dart_dcp_fail(dcp, "leaf %u/%u changed: %016llx, expected %016llx",
							    i, j, READ_ONCE(dcp->table[i][j]),
							    dcp->expected[i][j]);
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
		(main_external ||
		 of_property_read_bool(alias, "apple,j713-inherited-mappings")) ?
		0 : -ENODEV;
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
		bool retained_framebuffer = main_external && dcp->sid == 0 &&
			of_property_match_string(consumer, "memory-region-names",
						 "framebuffer") == i;
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
		/*
		 * The retained scanout framebuffer is a physical handoff, not a
		 * frontend-DART segment.  Keep requiring the reserved no-map region,
		 * but only region94 participates in SID0's inherited table contract.
		 */
		if (retained_framebuffer) {
			ret = maps ? -EINVAL : 0;
			goto put_node;
		}
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

/*
 * J713: adopt an external display DART's SID16, the PIODMA stream its
 * dcpext's shared buffers are mapped in, by the internal frontend's rules:
 * the boot loader's root and tables are imported and kept (their region
 * reserved), nothing is written now, and later only free slices are
 * claimed, through the shared-buffer API and apple_dart_dcp_dispext_map();
 * SID0 (scanout) the same way, if it can be. The consumer is the dcpext's
 * piodma child, see apple_dart_dispext_validate().
 */
static int apple_dart_dispext_prepare(struct apple_dart *dart)
{
	struct dcp_adt_tables firmware, bounds;
	struct dcp_adt_frontend frontend;
	struct apple_dart_dcp *borrowed;
	int ret;

	dev_dbg(dart->dev, "external display DART: enabled streams %08x, protect %08x\n",
		readl(dart->regs + DART_T8110_ENABLE_STREAMS),
		readl(dart->regs + DART_T8110_PROTECT));
	if (dart->hw->type != DART_T8110 || dart->num_streams != 32)
		return -ENODEV;
	ret = apple_dart_dcp_read_adt(dart, &firmware, &frontend);
	if (ret)
		return dev_err_probe(dart->dev, ret, "tables not described\n");
	if (!(frontend.seen & 4) || frontend.end[1] <= frontend.start[1])
		return dev_err_probe(dart->dev, -ENODEV, "no SID16 table region\n");
	bounds = firmware;
	bounds.start = frontend.start[1];
	bounds.end = frontend.end[1];
	ret = apple_dart_dcp_import(dart, 16, bounds, &borrowed);
	if (ret)
		return ret;
	if (!apple_dart_dcp_registers(borrowed))
		return -EBUSY;
	dart->frontend[1] = borrowed;
	/* The TTBR/TCR lock as adopted: a later check compares against it. */
	dart->dispext_protect = readl(dart->regs + DART_T8110_PROTECT);
	dart->dispext_lock = readl(dart->regs + DART_T8110_PROTECT_LOCK);
	dev_dbg(dart->dev, "SID16 (PIODMA) adopted: %u foreign 32-MiB slots kept; protect %08x lock %08x\n",
		bitmap_weight(borrowed->foreign, DCP_DART_ENTRIES), dart->dispext_protect,
		dart->dispext_lock);
	/*
	 * SID0 (scanout) the same way, for the firmware's D201 with flag 0;
	 * its free slices are claimed only through
	 * apple_dart_dcp_dispext_map(). Optional: SID16 goes on without.
	 */
	if (!(frontend.seen & 2) || frontend.end[0] <= frontend.start[0]) {
		dev_info(dart->dev, "no SID0 table region: SID0 left alone\n");
		return 0;
	}
	bounds = firmware;
	bounds.start = frontend.start[0];
	bounds.end = frontend.end[0];
	ret = apple_dart_dcp_import(dart, 0, bounds, &borrowed);
	if (ret || !apple_dart_dcp_registers(borrowed)) {
		dev_warn(dart->dev, "SID0 not adopted (%d); SID16 is\n", ret);
		return 0;
	}
	dart->frontend[0] = borrowed;
	dev_dbg(dart->dev, "SID0 (scanout) adopted: %u foreign 32-MiB slots kept\n",
		bitmap_weight(borrowed->foreign, DCP_DART_ENTRIES));
	return 0;
}

/*
 * An adopted external display DART's SID16 consumer: the piodma child
 * of its own dcpext, naming this DART's SID16 only; and the inherited root
 * and tables unchanged since their import.
 */
static int apple_dart_dispext_validate(struct apple_dart_dcp *dcp, struct device_node *consumer)
{
	struct device_node *owner = of_find_node_by_path(dcp->dart->dcp_instance->dcp);
	struct device_node *parent = of_get_parent(consumer);
	struct of_phandle_args spec;
	unsigned long flags;
	int ret = 0;

	if (!owner || parent != owner || !of_node_name_eq(consumer, "piodma") ||
	    !of_device_is_available(consumer) || dcp->sid != 16)
		ret = -ENODEV;
	of_node_put(parent);
	of_node_put(owner);
	if (ret)
		return ret;
	if (!apple_dart_dcp_registers(dcp))
		return -EBUSY;
	/* No fault IRQ is attached for this DART: look for a pending fault. */
	if (apple_dart_fault_pending(dcp->dart)) {
		apple_dart_dcp_fail(dcp, "a DART fault is pending");
		return -EIO;
	}
	spin_lock_irqsave(&dcp->lock, flags);
	for (unsigned int i = 0; i < DCP_DART_ENTRIES && !ret; i++) {
		if (READ_ONCE(dcp->root[i]) != dcp->entry[i])
			ret = -EBUSY;
		else if (dcp->table[i])
			for (unsigned int j = 0; j < DCP_DART_ENTRIES; j++)
				if (READ_ONCE(dcp->table[i][j]) != dcp->expected[i][j]) {
					ret = -EBUSY;
					break;
				}
	}
	if (ret)
		apple_dart_dcp_fail(dcp, "validation failed: %d", ret);
	spin_unlock_irqrestore(&dcp->lock, flags);
	if (ret)
		return ret;
	if (of_count_phandle_with_args(consumer, "iommus", "#iommu-cells") != 1)
		return -EINVAL;
	ret = of_parse_phandle_with_args(consumer, "iommus", "#iommu-cells", 0, &spec);
	if (ret)
		return ret;
	ret = spec.np == dcp->dart->dev->of_node && spec.args_count == 1 &&
	      spec.args[0] == 16 ? 0 : -EINVAL;
	of_node_put(spec.np);
	return ret;
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
			apple_dart_dcp_fail(dart->frontend[i], "frontend validation failed: %d",
					    status[i]);
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

/* The inherited DCP layout is the 26.6.2 firmware's, on every M4 Mac. */
static bool apple_dart_dcp_firmware(const struct device_node *np)
{
	u32 version[3];

	return of_property_count_u32_elems(np, "apple,firmware-version") == 3 &&
	       !of_property_read_u32_array(np, "apple,firmware-version", version, 3) &&
	       version[0] == 26 && version[1] == 6 && version[2] == 2;
}

static int apple_dart_dcp_validate(struct apple_dart_dcp *dcp, struct device *dev)
{
	struct device_node *node;
	unsigned int count = 0;
	unsigned int oslogs = 0;
	u32 seen = 0;
	int nr;
	int ret = 0;

	if (dcp->sid != 23) {
		if (dcp->owner && dcp->owner != dev)
			return -EBUSY;
		ret = dcp->dart->dispext ?
			apple_dart_dispext_validate(dcp, dev->of_node) :
			apple_dart_frontend_validate(dcp, dev->of_node);
		if (!ret)
			dcp->owner = dev;
		return ret;
	}

	if (!of_device_is_compatible(dev->of_node,
				     (dcp->dart->dcp_instance == &dcp_adt_internal ||
				      dcp->dart->dcp_instance == &dcp_adt_main_external) ?
				     "apple,t8132-dcp" : "apple,t8132-dcpext") ||
	    !apple_dart_dcp_firmware(dev->of_node) ||
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
		apple_dart_dcp_fail(dcp, "no SID0 reservation list");
	/* SID23/SID16 domains: only the retained allocators may allocate there. */
	region = iommu_alloc_resv_region(DCP_DART_ALIAS, DCP_DART_APERTURE, 0,
					IOMMU_RESV_RESERVED, GFP_KERNEL);
	if (!region) {
		apple_dart_dcp_fail(dcp, "no memory for its reservation");
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
	/* Raw: inside the caller's use of the DART (or a DART with no such gate). */
	return apple_dart_dcp_invalidate_tlb(&map);
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
		apple_dart_dcp_fail(dcp, "map: root %u changed: %016llx, adopted %016llx", root,
				    READ_ONCE(dcp->root[root]), dcp->entry[root]);
		ret = -EIO;
		goto out;
	}
	if (!dcp->table[root]) {
		u64 entry = (virt_to_phys(fresh) >> 4) | 1;

		/* Publish a zeroed, host-owned table; never replace a foreign entry. */
		dma_wmb();
		if (cmpxchg64(&dcp->root[root], 0, entry)) {
			apple_dart_dcp_fail(dcp, "map: root %u was taken meanwhile", root);
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
			apple_dart_dcp_fail(dcp, "map: leaf %u/%zu changed", root, leaf + i);
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

static size_t __apple_dart_dcp_unmap(struct apple_dart_dcp *dcp, u64 iova,
				     size_t size, size_t count, struct iommu_iotlb_gather *gather);

static size_t apple_dart_dcp_unmap(struct apple_dart_dcp *dcp, u64 iova,
				 size_t size, size_t count, struct iommu_iotlb_gather *gather)
{
	size_t unmapped;

	/*
	 * An internal display DART that its DCP powers down: no use of its
	 * registers is needed. Under the stream's lock __apple_dart_dcp_unmap()
	 * sees whether the DART is powered (apple_dart_host_lock()), and while it
	 * is not, only updates the page tables.
	 */
	if (dcp->dart->ps_prot)
		return __apple_dart_dcp_unmap(dcp, iova, size, count, gather);
	/*
	 * On an external display DART, only inside a use of its registers.
	 * Returning 0 (nothing unmapped) is not safe for general DMA teardown:
	 * the caller would take the IOVA as unmapped and reuse it, or free
	 * pages still mapped. It is unreachable here: this generic path serves
	 * only SID23 and SID0 DMA domains, and an external display DART's SID0
	 * has no generic DMA user (its host mappings are the private window and
	 * scanout APIs, which never come through here).
	 */
	if (!apple_dart_pm_get(dcp->dart)) {
		dev_err_ratelimited(dcp->dart->dev, "SID%u unmap at %llx while unconfirmed since sleep: refused\n",
				    dcp->sid, iova);
		return 0;
	}
	unmapped = __apple_dart_dcp_unmap(dcp, iova, size, count, gather);
	apple_dart_pm_put(dcp->dart);
	return unmapped;
}

static size_t __apple_dart_dcp_unmap(struct apple_dart_dcp *dcp, u64 iova,
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
			apple_dart_dcp_fail(dcp, "unmap: leaf %u/%zu changed", root, leaf + i);
			goto refuse;
		}
	}
	while (cleared < count && dcp->table[root][leaf + cleared])
		WRITE_ONCE(dcp->table[root][leaf + cleared++], 0);
	dma_wmb();
	/*
	 * Strict SID0: invalidate before the IOVA or its pages can be reused.
	 * Not while its DCP has the DART powered down: nothing is cached then,
	 * and the power-up flushes before it enables the streams.
	 */
	if (cleared && dcp->sid == 0 && !READ_ONCE(dcp->dart->host_off) &&
	    apple_dart_dcp_flush(dcp)) {
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
	apple_dart_dcp_fail(dcp, "unmap at %llx refused (state changed or flush failed)", iova);
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

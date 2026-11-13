// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hibernation on Apple Silicon under an aura boot object.
 *
 * A hibernation image restores every saved page to the frame it was taken from, and swsusp saves
 * reserved pages as long as they are mapped. Under aura the memory below Linux's own allocations
 * holds what the boot loader keeps alive across the hand-off: aura itself (resident at EL2), its
 * heap with the firmware buffers it handed over, the device tree, the initramfs, and the ranges
 * the boot firmware placed for the co-processors. Restoring old contents there would overwrite
 * the running boot loader and the live firmware state of the resuming boot.
 *
 * aura marks a boot whose layout is the same on every boot with /chosen/aura,hibernate-layout
 * (start, end of the range it keeps from Linux). On such a boot every /memreserve/ range and every
 * non-reusable /reserved-memory range is registered as nosave, except the kernel image itself.
 * Without the property, hibernation and resume are refused, since the reserved ranges move.
 *
 * The boot firmware still places some firmware segments anew on every boot (the AOP __OS_LOG),
 * inside the range aura keeps from Linux. A kernel restored from an image knows the device tree of the boot
 * it was started on, not of the boot that resumed it. aura therefore hands Linux its device tree
 * at a fixed address inside that range (/chosen/aura,hibernate-dt): the image does not save it,
 * so after a resume the kernel finds this boot's tree at the address it was started with, and
 * aura_hibernate_boot_fw_regions() reads a device's firmware regions from it.
 *
 * aura itself moves with the boot object, and with it its S2R mailbox: a restored kernel reads the
 * mailbox of the boot it runs on from that tree too (aura_hibernate_boot_chosen_u64()).
 */

#define pr_fmt(fmt) "aura-hibernate: " fmt

#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/pfn.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>
#include <linux/soc/apple/aura-hibernate.h>
#include <asm/memory.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/suspend.h>

static bool aura_layout_fixed;
static u64 aura_layout[2];
/* Where aura puts the device tree on every boot; 0 when it does not. */
static phys_addr_t aura_boot_dt;
#define AURA_BOOT_DT_MAX SZ_2M

/*
 * Ranges kept out of the image, in page frames. Filled once at late_initcall,
 * read through the arm64 hibernation platform hooks. A static table avoids
 * register_nosave_region(), which allocates from memblock and is meant for
 * setup_arch() time.
 */
#define AURA_NOSAVE_MAX 128
static struct { unsigned long start, end; } aura_nosave[AURA_NOSAVE_MAX];
static unsigned int aura_nosave_count;

/*
 * Apple silicon Macs run firmware on their coprocessors (storage, secure
 * enclave, display, always-on processor, system controller, GPU and more).
 * Restoring an image means a cold boot through the platform boot chain, which
 * restarts that firmware, but most of the Linux drivers have no
 * freeze/poweroff/restore path that rebuilds their mailbox, ring and DMA state
 * from a cold start. The restored kernel would keep using stale state, so
 * writing and restoring images stays disabled on these machines, except where
 * the boot object gives the OS a fixed memory layout and the drivers rebuild
 * their coprocessor state from the cold boot that restores the image (aura
 * hib=1, see above).
 */
static bool aura_hibernate_blocked(void)
{
	return of_machine_is_compatible("apple,arm-platform") && !aura_layout_fixed;
}

static bool aura_hibernate_pfn_nosave(unsigned long pfn)
{
	unsigned int i;

	for (i = 0; i < aura_nosave_count; i++)
		if (pfn >= aura_nosave[i].start && pfn < aura_nosave[i].end)
			return true;
	return false;
}

/* Whether the ranges kept out of the image cover all of [start, end). */
static bool aura_hibernate_range_nosave(u64 start, u64 end)
{
	unsigned long pfn = PFN_DOWN(start), last = PFN_UP(end);
	unsigned int i;
	bool moved;

	while (pfn < last) {
		moved = false;
		for (i = 0; i < aura_nosave_count; i++) {
			if (pfn >= aura_nosave[i].start && pfn < aura_nosave[i].end) {
				pfn = aura_nosave[i].end;
				moved = true;
			}
		}
		if (!moved)
			return false;
	}
	return true;
}

/*
 * The image restores every page to the frame it came from, so the boot that
 * resumes it must give the kernel what the boot that wrote it did: on Apple
 * machines at EL2 the same CTXR exec window (secondaries only execute inside
 * it) and aura's fixed hibernation layout. They are added to the layout cookie
 * of arm64 hibernation.
 *
 * aura's own image moves with the boot object, and its CPU release addresses
 * and S2R mailbox with it, inside the range the layout keeps from Linux. The
 * restored kernel uses neither address of the boot that wrote the image: its
 * CPUs come back from the boot kernel's pen, a CPU started at its reset vector
 * again finds the release target the boot kernel wrote (both kernels run at the
 * window's fixed address), and the T6040 sleep driver takes this boot's mailbox
 * from aura's tree after the restore. What has to match is the spin table's
 * shape (the same CPUs, at the same offsets from the mailbox) and that this
 * boot keeps all of it out of the image.
 */
static u64 aura_hibernate_layout(u64 h, struct arm64_hibernate_layout *lay)
{
	static const char * const chosen_props[] = {
		"aura,hibernate-layout", "aura,hibernate-dt",
		"aurora,ctxr-exec-window",
	};
	u64 mbox = 0, layout[2], win[2];
	struct device_node *np;
	const void *v;
	u8 kept;
	int i, len;

	np = of_find_node_by_path("/chosen");
	for (i = 0; np && i < ARRAY_SIZE(chosen_props); i++) {
		v = of_get_property(np, chosen_props[i], &len);
		if (!v)
			continue;
		h = arm64_hibernate_layout_mix(h, chosen_props[i], strlen(chosen_props[i]));
		h = arm64_hibernate_layout_mix(h, v, len);
	}
	if (np && !of_property_read_u64(np, "aurora,s2r-mailbox", &mbox)) {
		lay->have_base = true;
		lay->base = mbox;
		kept = aura_hibernate_pfn_nosave(PHYS_PFN(mbox));
		h = arm64_hibernate_layout_mix(h, "aurora,s2r-mailbox",
					       strlen("aurora,s2r-mailbox"));
		h = arm64_hibernate_layout_mix(h, &kept, sizeof(kept));
	}
	/* aura's layout less the OS window, only where all of it is kept out of the image */
	if (np && !of_property_read_u64_array(np, "aura,hibernate-layout", layout, 2) &&
	    !of_property_read_u64_array(np, "aurora,ctxr-exec-window", win, 2) &&
	    layout[0] <= win[0] && win[0] + win[1] <= layout[1]) {
		lay->ex[lay->nex][0] = layout[0];
		lay->ex[lay->nex][1] = win[0];
		if (aura_hibernate_range_nosave(lay->ex[lay->nex][0], lay->ex[lay->nex][1]))
			lay->nex++;
		lay->ex[lay->nex][0] = win[0] + win[1];
		lay->ex[lay->nex][1] = layout[1];
		if (aura_hibernate_range_nosave(lay->ex[lay->nex][0], lay->ex[lay->nex][1]))
			lay->nex++;
	}
	of_node_put(np);
	return h;
}

static const struct arm64_hibernate_platform aura_hibernate_platform = {
	.pfn_nosave = aura_hibernate_pfn_nosave,
	.layout = aura_hibernate_layout,
};

static void __init aura_nosave_add(unsigned long s, unsigned long e)
{
	if (s >= e)
		return;
	if (aura_nosave_count == AURA_NOSAVE_MAX) {
		pr_warn("more than %d reserved ranges; hibernation stays disabled\n", AURA_NOSAVE_MAX);
		aura_nosave_count++;
		return;
	}
	if (aura_nosave_count < AURA_NOSAVE_MAX) {
		aura_nosave[aura_nosave_count].start = s;
		aura_nosave[aura_nosave_count].end = e;
		aura_nosave_count++;
	}
}

static void __init aura_nosave_range(u64 start, u64 end, unsigned long kstart, unsigned long kend,
				     const char *what)
{
	unsigned long s = PFN_DOWN(start), e = PFN_UP(end);

	if (s >= e)
		return;
	/* The kernel image is Linux's own and is saved; split the range around it. */
	if (s < kend && e > kstart) {
		if (s < kstart)
			aura_nosave_add(s, kstart);
		if (e > kend)
			aura_nosave_add(kend, e);
	} else {
		aura_nosave_add(s, e);
	}
	pr_debug("nosave %s [%#llx, %#llx)\n", what, start, end);
}

static bool aura_fw_region_kept(u64 start, u64 size)
{
	u64 end = start + size, a;

	if (end < start)
		return false;
	/*
	 * Inside the range aura keeps from Linux below its device tree slot, where the boot firmware places the
	 * firmware: reserved, and not saved in the image. Not the slot or aura's heap above it.
	 */
	if (start >= aura_layout[0] && end <= aura_boot_dt)
		return true;
	/* Otherwise Linux must neither own nor map any of it (no-map or not RAM). */
	for (a = start & PAGE_MASK; a < end; a += PAGE_SIZE)
		if (memblock_is_map_memory(a))
			return false;
	return true;
}

/*
 * Whether @fdt, mapped from @slot, is a tree aura wrote there: a valid header that fits the slot, and
 * a /chosen naming this slot and the fixed layout @layout.
 */
static bool aura_slot_tree_valid(const void *fdt, phys_addr_t slot, const u64 layout[2])
{
	const fdt64_t *v;
	int node, len;

	if (fdt_check_header(fdt) || fdt_totalsize(fdt) > AURA_BOOT_DT_MAX)
		return false;
	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0)
		return false;
	v = fdt_getprop(fdt, node, "aura,hibernate-dt", &len);
	if (!v || len != sizeof(*v) || fdt64_ld(v) != slot)
		return false;
	v = fdt_getprop(fdt, node, "aura,hibernate-layout", &len);
	return v && len == 2 * sizeof(*v) && fdt64_ld(&v[0]) == layout[0] &&
	       fdt64_ld(&v[1]) == layout[1];
}

/*
 * Reads the firmware memory-regions of @np (its memory-region phandles, each
 * region's reg and the iommu-addresses entry naming @np) from the device tree
 * of the running boot, which after a resume from disk is not the one the
 * kernel was started with. Returns the number of regions, at most @max, or a
 * negative errno: -ENODEV when aura does not hand the tree over at a fixed
 * address. Only flat, untranslated layouts are accepted: @np's parent without
 * dma-ranges, a /reserved-memory without a non-empty ranges.
 */
int aura_hibernate_boot_fw_regions(struct device_node *np, struct aura_fw_region *out,
				   unsigned int max)
{
	const fdt32_t *phandles, *p;
	unsigned int n = 0;
	int node, parent, len, ret, i;
	u32 own;
	char *path;
	void *fdt;

	if (!aura_boot_dt || !np)
		return -ENODEV;
	fdt = memremap(aura_boot_dt, AURA_BOOT_DT_MAX, MEMREMAP_WB);
	if (!fdt)
		return -ENOMEM;
	ret = -EINVAL;
	/* The tree aura handed over on this boot, with the same fixed layout. */
	if (!aura_slot_tree_valid(fdt, aura_boot_dt, aura_layout))
		goto out;

	path = kasprintf(GFP_KERNEL, "%pOF", np);
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}
	node = fdt_path_offset(fdt, path);
	kfree(path);
	ret = -ENOENT;
	if (node < 0)
		goto out;
	ret = -EINVAL;
	own = fdt_get_phandle(fdt, node);
	parent = fdt_parent_offset(fdt, node);
	if (!own || parent < 0 || fdt_getprop(fdt, parent, "dma-ranges", NULL))
		goto out;
	phandles = fdt_getprop(fdt, node, "memory-region", &len);
	if (!phandles || len <= 0 || len % sizeof(*phandles))
		goto out;

	for (i = 0; i < len / (int)sizeof(*phandles); i++) {
		int region, rparent, ac, sc, cells, pos, plen;
		const fdt32_t *reg, *iommu;
		u64 phys, size, iova = 0, length = 0;
		bool found = false;

		region = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(phandles[i]));
		if (region < 0)
			goto out;
		rparent = fdt_parent_offset(fdt, region);
		if (rparent < 0)
			goto out;
		p = fdt_getprop(fdt, rparent, "ranges", &plen);
		if (p && plen)
			goto out;
		ac = fdt_address_cells(fdt, rparent);
		sc = fdt_size_cells(fdt, rparent);
		reg = fdt_getprop(fdt, region, "reg", &plen);
		if (ac < 1 || ac > 2 || sc < 1 || sc > 2 || !reg ||
		    plen < (int)((ac + sc) * sizeof(*reg)))
			goto out;
		phys = fdt32_to_cpu(reg[0]);
		if (ac == 2)
			phys = phys << 32 | fdt32_to_cpu(reg[1]);
		size = fdt32_to_cpu(reg[ac]);
		if (sc == 2)
			size = size << 32 | fdt32_to_cpu(reg[ac + 1]);

		/* Entries: owner phandle, then an address and a size in the owner's bus cells. */
		iommu = fdt_getprop(fdt, region, "iommu-addresses", &plen);
		if (!iommu || plen <= 0 || plen % sizeof(*iommu))
			goto out;
		cells = plen / sizeof(*iommu);
		for (pos = 0; pos < cells;) {
			u32 ph = fdt32_to_cpu(iommu[pos++]);
			int owner = fdt_node_offset_by_phandle(fdt, ph), op, oac, osc;

			if (owner < 0)
				goto out;
			op = fdt_parent_offset(fdt, owner);
			if (op < 0)
				goto out;
			oac = fdt_address_cells(fdt, op);
			osc = fdt_size_cells(fdt, op);
			if (oac < 1 || oac > 2 || osc < 1 || osc > 2 || oac + osc > cells - pos)
				goto out;
			if (ph == own) {
				if (found)
					goto out;
				found = true;
				iova = fdt32_to_cpu(iommu[pos]);
				if (oac == 2)
					iova = iova << 32 | fdt32_to_cpu(iommu[pos + 1]);
				length = fdt32_to_cpu(iommu[pos + oac]);
				if (osc == 2)
					length = length << 32 | fdt32_to_cpu(iommu[pos + oac + 1]);
			}
			pos += oac + osc;
		}
		if (!found || length != size || !size)
			goto out;
		if (!aura_fw_region_kept(phys, size)) {
			pr_warn("%pOF: this boot's firmware region [%#llx, +%#llx) is memory Linux owns\n",
				np, phys, size);
			ret = -EBUSY;
			goto out;
		}
		if (n == max) {
			ret = -E2BIG;
			goto out;
		}
		out[n].phys = phys;
		out[n].iova = iova;
		out[n].size = size;
		n++;
	}
	ret = n;
out:
	memunmap(fdt);
	return ret;
}
EXPORT_SYMBOL_GPL(aura_hibernate_boot_fw_regions);

/*
 * Reads the u64 property @name of /chosen from the device tree of the running boot, which after a
 * resume from disk is not the one the kernel was started with. Returns 0, -ENODEV when aura does
 * not hand the tree over at a fixed address, -EINVAL when the slot holds no tree for this layout,
 * -ENOENT when the tree has no such property.
 */
int aura_hibernate_boot_chosen_u64(const char *name, u64 *val)
{
	const fdt64_t *v;
	int node, len, ret;
	void *fdt;

	if (!aura_boot_dt)
		return -ENODEV;
	fdt = memremap(aura_boot_dt, AURA_BOOT_DT_MAX, MEMREMAP_WB);
	if (!fdt)
		return -ENOMEM;
	ret = -EINVAL;
	if (!aura_slot_tree_valid(fdt, aura_boot_dt, aura_layout))
		goto out;
	ret = -ENOENT;
	node = fdt_path_offset(fdt, "/chosen");
	v = node < 0 ? NULL : fdt_getprop(fdt, node, name, &len);
	if (!v || len != sizeof(*v))
		goto out;
	*val = fdt64_ld(v);
	ret = 0;
out:
	memunmap(fdt);
	return ret;
}
EXPORT_SYMBOL_GPL(aura_hibernate_boot_chosen_u64);

struct aura_hibernate_reprobe {
	struct work_struct work;
	struct device *dev;
};

static void aura_hibernate_reprobe_work(struct work_struct *work)
{
	struct aura_hibernate_reprobe *rp = container_of(work, struct aura_hibernate_reprobe, work);
	int ret;

	ret = device_reprobe(rp->dev);
	/* A failed probe is not reported by device_reprobe() itself. */
	if (ret || !READ_ONCE(rp->dev->driver))
		dev_err(rp->dev, "probe after the resume from disk failed: %d\n", ret);
	put_device(rp->dev);
	kfree(rp);
}

/*
 * Unbinds @dev and probes it again once the tasks are thawed, for a driver whose co-processor
 * the boot that resumed the image left halted: its unbind and its probe know how to drop a
 * session that died under it and start the firmware anew, as at boot, and a restore callback
 * cannot run either. The work runs on the freezable workqueue, which the restore finds frozen,
 * so it starts after every device's restore and its complete step, when probing is allowed
 * again. Built in, so that a driver module going away meanwhile leaves nothing behind.
 */
int aura_hibernate_reprobe_after_thaw(struct device *dev)
{
	struct aura_hibernate_reprobe *rp;

	rp = kzalloc_obj(*rp);
	if (!rp)
		return -ENOMEM;
	INIT_WORK(&rp->work, aura_hibernate_reprobe_work);
	rp->dev = get_device(dev);
	queue_work(system_freezable_wq, &rp->work);
	return 0;
}
EXPORT_SYMBOL_GPL(aura_hibernate_reprobe_after_thaw);

static int __init aura_hibernate_init(void)
{
	unsigned long kstart = PFN_DOWN(__pa_symbol(_text)), kend = PFN_UP(__pa_symbol(_end));
	struct device_node *chosen, *rmem, *child;
	unsigned int count = 0;
	const void *fdt = initial_boot_params;
	u64 layout[2], dt_slot;
	int i, n;

	hibernation_set_platform_check(aura_hibernate_blocked,
				       "coprocessor firmware state is not restored from an image");
	arm64_hibernate_set_platform(&aura_hibernate_platform);

	/* Only a boot loader that keeps the layout fixed publishes it. */
	chosen = of_find_node_by_path("/chosen");
	if (!chosen ||
	    of_property_read_u64_array(chosen, "aura,hibernate-layout", layout, 2)) {
		of_node_put(chosen);
		return 0;
	}
	/*
	 * The device tree slot. Trusted where this kernel's tree is, or, when a later stage (U-Boot and
	 * the EFI stub) handed the kernel its own copy of aura's tree, where the slot still holds aura's
	 * tree for this layout: those stages do not move the firmware segments, so aura's tree and the
	 * kernel's copy agree on them.
	 */
	if (!of_property_read_u64(chosen, "aura,hibernate-dt", &dt_slot)) {
		bool in_layout = dt_slot >= layout[0] && dt_slot + AURA_BOOT_DT_MAX <= layout[1];
		void *slot_fdt;

		if (in_layout && dt_slot == __fdt_pointer) {
			aura_boot_dt = dt_slot;
		} else if (in_layout) {
			slot_fdt = memremap(dt_slot, AURA_BOOT_DT_MAX, MEMREMAP_WB);
			if (slot_fdt && aura_slot_tree_valid(slot_fdt, dt_slot, layout)) {
				aura_boot_dt = dt_slot;
				pr_info("device tree handed over at %pa; using aura's tree at %#llx\n",
					&__fdt_pointer, dt_slot);
			} else {
				pr_warn("device tree at %pa, aura's slot %#llx holds no tree for this layout\n",
					&__fdt_pointer, dt_slot);
			}
			if (slot_fdt)
				memunmap(slot_fdt);
		} else {
			pr_warn("device tree slot %#llx outside the fixed layout\n", dt_slot);
		}
	}
	of_node_put(chosen);

	if (fdt) {
		n = fdt_num_mem_rsv(fdt);
		for (i = 0; i < n; i++) {
			u64 addr, size;

			if (fdt_get_mem_rsv(fdt, i, &addr, &size) || !size)
				continue;
			aura_nosave_range(addr, addr + size, kstart, kend, "memreserve");
			count++;
		}
	}

	rmem = of_find_node_by_path("/reserved-memory");
	for_each_available_child_of_node(rmem, child) {
		struct resource r;

		if (of_property_read_bool(child, "reusable"))
			continue;
		if (of_address_to_resource(child, 0, &r))
			continue;
		aura_nosave_range(r.start, r.end + 1, kstart, kend, "reserved-memory");
		count++;
	}
	of_node_put(rmem);

	if (aura_nosave_count > AURA_NOSAVE_MAX) {
		aura_nosave_count = 0;
		return 0;
	}
	aura_layout[0] = layout[0];
	aura_layout[1] = layout[1];
	aura_layout_fixed = true;
	pr_info("fixed boot layout [%#llx, %#llx), %u reserved ranges kept out of the image%s\n",
		layout[0], layout[1], count,
		aura_boot_dt ? ", device tree at a fixed address" : "");
	return 0;
}
late_initcall(aura_hibernate_init);

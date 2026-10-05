// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T602x/T8112 ANE image staging. Validate size, checksum, load-command
 * bounds and segment layout, then copy into a zeroed 0x500000-byte
 * coherent allocation mapped through the device IOMMU. Populate runtime
 * boot fields from the current device before CPU release.
 *
 * The default uses driver-owned RAM. Reserved-memory mode is optional
 * and requires DT no-map coverage for both windows. A latched RVBAR
 * entry requires an IOMMU alias: resolve every source page through the
 * DMA domain, reject occupied destination pages, map and verify each
 * page. Physical pages need not be contiguous. Record each mapped
 * extent and reject all subsequent DMA allocations overlapping it.
 *
 * Staging and CPU release are separate. Load failures release the
 * image and owned buffer and clear fw_buf; load is nonfatal to probe.
 */
#include <crypto/sha2.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "ane_t6021.h"
#include "ane_t6021_boot.h"
#include "ane_fw_validate.h"

MODULE_FIRMWARE(ANE_FW_SELENE_NAME);
MODULE_FIRMWARE(ANE_FW_BIA_NAME);

static bool fw_load = true;
module_param(fw_load, bool, 0444);
MODULE_PARM_DESC(fw_load,
		 "Validate and map the supported image through DART (default on)");

static unsigned int fw_extra_ram = 0x200000;
module_param(fw_extra_ram, uint, 0444);
MODULE_PARM_DESC(fw_extra_ram,
		 "Page-aligned owned RAM after the 5 MiB image allocation (default 2 MiB, maximum 16 MiB)");

/*
 * Optional reserved-memory mapping at the latched entry. Disabled by
 * default; driver-owned staging needs no reserved RAM. Require DT
 * no-map coverage before using either reserved window.
 */
static bool fw_alias_reserved;
module_param(fw_alias_reserved, bool, 0444);
MODULE_PARM_DESC(fw_alias_reserved,
		 "T6021: map both reserved image windows at the entry IOVA; requires DT no-map coverage (default off, use owned RAM)");

static bool ane_t6021_fw_alias_is_reserved(struct device *dev)
{
	const struct ane_t602x_soc *soc = of_device_get_match_data(dev);

	return fw_alias_reserved && soc->preload_placement;
}

/* Physical windows used by the optional T6021 reserved-memory mapping. */
static const struct { u64 phys, len; } ane_t6021_fw_preload[] = {
	{ 0x10000848000ull, 0xc4000ull },
	{ 0x10001400000ull, 0x438000ull },
};

/*
 * Only DT no-map reserved-memory coverage excludes these pages from kernel
 * allocation.
 */
static bool ane_t6021_fw_preload_reserved(void)
{
	struct device_node *parent = of_find_node_by_path("/reserved-memory");
	unsigned int i, covered = 0;

	for (i = 0; parent && i < ARRAY_SIZE(ane_t6021_fw_preload); i++) {
		u64 phys = ane_t6021_fw_preload[i].phys;
		u64 end = phys + ane_t6021_fw_preload[i].len;
		struct device_node *np;

		for_each_available_child_of_node(parent, np) {
			struct reserved_mem *rmem = of_reserved_mem_lookup(np);

			if (rmem && of_property_read_bool(np, "no-map") &&
			    rmem->base <= phys && end <= rmem->base + rmem->size) {
				covered++;
				of_node_put(np);
				break;
			}
		}
	}
	of_node_put(parent);
	return covered == ARRAY_SIZE(ane_t6021_fw_preload);
}

bool ane_t6021_fwload_placement_ok(struct device *dev)
{
	/* BINDING probe-top predicate, as ane_t6021_fwload_options_ok():
	 * reserved mode never maps RAM the kernel may own.
	 */
	return !fw_load || !ane_t6021_fw_alias_is_reserved(dev) ||
	       ane_t6021_fw_preload_reserved();
}

bool ane_t6021_fwload_requested(void)
{
	return fw_load;
}

/* DART pages are 16 KiB; the firmware allocation size is a page multiple. */
#define ANE_T6021_FW_ALIAS_PAGE	0x4000

bool ane_t6021_fwload_options_ok(void)
{
	/*
	 * Check staging options before allocation, power or CPU release.
	 * The allocation check also enforces 16 KiB alignment and a 16 MiB
	 * limit. Both alias modes map the complete allocation.
	 */
	return fw_extra_ram <= SZ_16M &&
	       IS_ALIGNED(fw_extra_ram, ANE_T6021_FW_ALIAS_PAGE);
}

/*
 * Per-SoC boot parameters and power-state layout. T6020 uses revision
 * 1, T6021/T6022 use 0x11; T8112 reads its revision from two fuse words.
 * The power service requires its PMGR page mapped with IOVA == PA.
 * T8112 checks SET +0x8b8 before power-state and engine reads, and has
 * no supported TD sampling offset.
 */
const struct ane_t602x_soc ane_t6020_soc = {
	.soc = 0x6020, .soc_revision = 0x01,
	.fw = &ane_fw_selene, .tunables = &ane_t6020_asc_tunables,
	.ps_cpu_off = 0x2e0, .pmu_pa = 0x28e084000ull,
	.trace_td_off = 0x1c20458,
};

const struct ane_t602x_soc ane_t6021_soc = {
	.soc = 0x6021, .soc_revision = 0x11, .preload_placement = true,
	.fw = &ane_fw_selene, .tunables = &ane_t602x_asc_tunables,
	.ps_cpu_off = 0x2e0, .pmu_pa = 0x28e084000ull,
	.trace_td_off = 0x1c20458,
};

const struct ane_t602x_soc ane_t6022_soc = {
	.soc = 0x6022, .soc_revision = 0x11,
	.fw = &ane_fw_selene, .tunables = &ane_t602x_asc_tunables,
	.ps_cpu_off = 0x2e0, .pmu_pa = 0x28e084000ull,
	.trace_td_off = 0x1c20458,
};

const struct ane_t602x_soc ane_t8112_soc = {
	.soc = 0x8112, .revision_fuse = true,
	.fw = &ane_fw_bia, .tunables = &ane_t8112_asc_tunables,
	.ps_cpu_off = 0xc008, .pwgate_off = 0x8b8,
	.pmu_pa = 0x23b70c000ull, .ps_off = 0x8,
};

static int ane_t6021_pmu_map(struct ane_t6021 *ane, struct iommu_domain *dom)
{
	const struct ane_t602x_soc *soc = of_device_get_match_data(ane->dev);
	int prot = IOMMU_READ | IOMMU_WRITE;
	int ret;

	if (dev_is_dma_coherent(ane->dev))
		prot |= IOMMU_CACHE;
	ret = iommu_map(dom, soc->pmu_pa, soc->pmu_pa, ANE_T6021_FW_ALIAS_PAGE,
			prot, GFP_KERNEL);
	if (!ret && iommu_iova_to_phys(dom, soc->pmu_pa) != soc->pmu_pa)
		ret = -EIO;
	dev_dbg(ane->dev, "pmu: DART map %#llx (IOVA == PA, %#x bytes): %d\n",
		soc->pmu_pa, ANE_T6021_FW_ALIAS_PAGE, ret);
	return ret;
}

static int ane_t6021_fw_alias_map(struct ane_t6021 *ane, bool reserved)
{
	struct iommu_domain *dom = iommu_get_domain_for_dev(ane->dev);
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	u64 rvbar = readq(eng + ANE_ASC_RVBAR);
	u64 entry = ane_t6021_rvbar_entry_bits(rvbar);
	phys_addr_t pa0 = 0;
	int prot = IOMMU_READ | IOMMU_WRITE;
	u64 off;
	int ret;

	if (!dom)
		return -ENODEV;
	if (!ane_t6021_rvbar_latched(rvbar) || !entry) {
		/* Unlatched branch: the boot path programs RVBAR to the
		 * fw DVA itself (ane_t6021_rvbar_compose), no alias.
		 */
		dev_dbg(ane->dev,
			"fwalias: rvbar %016llx not latched/entry 0 — skip (boot reprograms RVBAR)\n",
			rvbar);
		return 0;
	}
	if (!ane_t6021_rvbar_entry_ok(entry) ||
	    entry & (ANE_T6021_FW_ALIAS_PAGE - 1)) {
		dev_err(ane->dev,
			"fwalias: entry %#llx not %#x-aligned in fold\n",
			entry, ANE_T6021_FW_ALIAS_PAGE);
		return -EINVAL;
	}
	if (entry + ane->fw_size - 1 > dom->geometry.aperture_end) {
		dev_err(ane->dev,
			"fwalias: entry %#llx+%#x outside aperture %#llx\n",
			entry, ane->fw_size,
			(unsigned long long)dom->geometry.aperture_end);
		return -ERANGE;
	}
	if (dev_is_dma_coherent(ane->dev))
		prot |= IOMMU_CACHE;

	if (reserved) {
		/*
		 * Map both reserved windows at the entry IOVAs. Track the
		 * mapped
		 * bytes for each window independently so cleanup never assumes
		 * adjacency or unmaps an untouched range.
		 */
		struct { u64 iova, phys, len; } win[] = {
			{ 0x10000000000ull, ane_t6021_fw_preload[0].phys,
			  ane_t6021_fw_preload[0].len },
			{ 0, ane_t6021_fw_preload[1].phys, ane_t6021_fw_preload[1].len },
			{ entry + 0x4fc000, 0, fw_extra_ram ? ane->fw_size - 0x4fc000 : 0 },
		};
		unsigned int w, windows = fw_extra_ram ? ARRAY_SIZE(win) : 2;

		/* Derive SEG1 base from SEG0 base plus its length. */
		win[1].iova = win[0].iova + win[0].len;
		if (win[0].iova != entry) {
			dev_err(ane->dev,
				"fwalias: remap base %#llx != latched entry %#llx\n",
				win[0].iova, entry);
			return -EINVAL;
		}

		for (w = 0; w < windows; w++) {
			u64 o;

			for (o = 0; o < win[w].len; o += ANE_T6021_FW_ALIAS_PAGE) {
				phys_addr_t pa = w < 2 ? win[w].phys + o :
					iommu_iova_to_phys(dom, ane->fw_iova +
							   win[w].iova + o - entry);
				if (!pa || !IS_ALIGNED(pa, ANE_T6021_FW_ALIAS_PAGE)) {
					ret = -EFAULT;
					goto err_unmap_mapped;
				}
				if (iommu_iova_to_phys(dom, win[w].iova + o)) {
					dev_err(ane->dev,
						"fwalias: reserved entry +%#llx mapped — refusing\n",
						win[w].iova + o - entry);
					ret = -EEXIST;
					goto err_unmap_mapped;
				}
				ret = iommu_map(dom, win[w].iova + o,
						pa,
						ANE_T6021_FW_ALIAS_PAGE, prot,
						GFP_KERNEL);
				if (ret) {
					dev_err(ane->dev,
						"fwalias: reserved map +%#llx: %d\n",
						win[w].iova + o - entry, ret);
					goto err_unmap_mapped;
				}
				/* per-window successfully mapped bytes */
				ane->fw_alias_ext_len[w] = o + ANE_T6021_FW_ALIAS_PAGE;
				ane->fw_alias_ext_iova[w] = win[w].iova;
				if (iommu_iova_to_phys(dom, win[w].iova + o) != pa) {
					ret = -EIO;
					goto err_unmap_mapped;
				}
			}
		}
		ane->fw_alias_extn = windows;
		ane->fw_alias_iova = entry;
		dev_dbg(ane->dev,
			"fwalias: reserved SEG0/SEGi at entry %#llx (%llx+%zx %llx+%zx, preloaded placement)\n",
			entry,
			ane->fw_alias_ext_iova[0], ane->fw_alias_ext_len[0],
			ane->fw_alias_ext_iova[1], ane->fw_alias_ext_len[1]);
		if (fw_extra_ram)
			dev_dbg(ane->dev, "fwalias: owned heap [%#llx,%#llx) roundtrip verified\n",
				win[2].iova, win[2].iova + win[2].len);
		return ane_t6021_pmu_map(ane, dom);

err_unmap_mapped:
		/*
		 * Unmap only each window's successfully mapped bytes, leaving
		 * foreign mappings intact.
		 */
		for (w = 0; w < windows; w++)
			if (ane->fw_alias_ext_len[w])
				iommu_unmap(dom, ane->fw_alias_ext_iova[w],
					    ane->fw_alias_ext_len[w]);
		memset(ane->fw_alias_ext_len, 0, sizeof(ane->fw_alias_ext_len));
		ane->fw_alias_extn = 0;
		return ret;
	}

	for (off = 0; off < ane->fw_size; off += ANE_T6021_FW_ALIAS_PAGE) {
		phys_addr_t pa = iommu_iova_to_phys(dom, ane->fw_iova + off);

		if (!pa || pa & (ANE_T6021_FW_ALIAS_PAGE - 1)) {
			dev_err(ane->dev,
				"fwalias: fw +%#llx untranslated (%pa)\n",
				off, &pa);
			ret = -EFAULT;
			goto err_unmap;
		}
		if (iommu_iova_to_phys(dom, entry + off)) {
			dev_err(ane->dev,
				"fwalias: entry +%#llx already mapped — refusing\n",
				off);
			ret = -EEXIST;
			goto err_unmap;
		}
		ret = iommu_map(dom, entry + off, pa,
				ANE_T6021_FW_ALIAS_PAGE, prot, GFP_KERNEL);
		if (ret) {
			dev_err(ane->dev, "fwalias: iommu_map +%#llx: %d\n",
				off, ret);
			goto err_unmap;
		}
		if (!off)
			pa0 = pa;
	}

	/* full per-page roundtrip: every alias page must resolve to the
	 * same PA as its fw source page (not just page 0)
	 */
	for (off = 0; off < ane->fw_size; off += ANE_T6021_FW_ALIAS_PAGE) {
		if (iommu_iova_to_phys(dom, entry + off) !=
		    iommu_iova_to_phys(dom, ane->fw_iova + off)) {
			dev_err(ane->dev,
				"fwalias: roundtrip mismatch at +%#llx\n", off);
			ret = -EIO;
			/* Mapping finished: unwind every page, not only the
			 * prefix already checked by this verification loop.
			 */
			off = ane->fw_size;
			goto err_unmap;
		}
	}

	ane->fw_alias_iova = entry;
	ane->fw_alias_ext_iova[0] = entry;
	ane->fw_alias_ext_len[0] = ane->fw_size;
	ane->fw_alias_extn = 1;
	dev_dbg(ane->dev,
		"fwalias: entry %#llx <- %u dart pages aliased from fw %pad (first %pa, roundtrip OK)\n",
		entry, ane->fw_size / ANE_T6021_FW_ALIAS_PAGE,
		&ane->fw_iova, &pa0);
	/*
	 * The device power service requires the PMGR page mapped with IOVA ==
	 * PA.
	 */
	return ane_t6021_pmu_map(ane, dom);

err_unmap:
	if (off)
		iommu_unmap(dom, entry, off);
	return ret;
}

/*
 * T8112 revision comes from the DT fuse window using two non-posted
 * 32-bit reads. Decode only those words; no other fuse access is needed.
 */
static int ane_t6021_soc_revision(struct ane_t6021 *ane,
				  const struct ane_t602x_soc *soc, u32 *rev)
{
	struct resource *res;
	void __iomem *fuse;
	u32 w0, w1;

	if (!soc->revision_fuse) {
		*rev = soc->soc_revision;
		return 0;
	}
	res = platform_get_resource_byname(to_platform_device(ane->dev),
					   IORESOURCE_MEM, "fuse");
	if (!res || resource_size(res) != 8) {
		dev_err(ane->dev,
			"fwload: no 8-byte \"fuse\" window: chip revision unknown, refusing\n");
		return -ENODEV;
	}
	fuse = ioremap_np(res->start, 8);
	if (!fuse)
		return -ENOMEM;
	w0 = readl(fuse);
	w1 = readl(fuse + 4);
	iounmap(fuse);
	*rev = ane_t8112_fuse_revision(w0, w1);
	dev_info(ane->dev, "fwload: chip revision %#x (fuse %#llx: %08x %08x)\n",
		 *rev, (u64)res->start, w0, w1);
	return 0;
}

/*
 * Patch the owned image with the entry DVA, DT engine address, SoC
 * revision and a fresh guard value from the running system.
 */
static int ane_t6021_fw_patch(struct ane_t6021 *ane, u8 *img)
{
	const struct ane_t602x_soc *soc = of_device_get_match_data(ane->dev);
	struct resource *res = platform_get_resource(to_platform_device(ane->dev),
						     IORESOURCE_MEM, 0);
	u64 rvbar = readq(ane->base[ANE_T6021_REG_ENGINE] + ANE_ASC_RVBAR);
	u64 entry = ane_t6021_rvbar_latched(rvbar) ?
		    ane_t6021_rvbar_entry_bits(rvbar) : 0;
	u64 guard = get_random_u64();
	struct ane_fw_boot_patch p = {
		.exec_base = entry ?: ane->fw_iova,
		/* The random guard contains one zero byte. */
		.stack_guard = guard & ~(0xffull << (8 * (guard >> 61))),
		.soc = soc->soc,
		.cpu_pa = res->start + ANE_ASC_CPU_BASE,
		.wrapper_pa = res->start + ANE_ASC_WRAPPER_BASE,
	};
	const char *reason = NULL;
	int ret;

	ret = ane_t6021_soc_revision(ane, soc, &p.soc_revision);
	if (ret)
		return ret;
	if (ane_fw_apply_boot_patches(img, soc->fw, soc->tunables, &p, &reason)) {
		dev_err(ane->dev, "fwload: own memory: %s (rev %#x)\n", reason,
			p.soc_revision);
		return -EINVAL;
	}
	dev_dbg(ane->dev,
		"fwload: owned image boot fields (soc %#x rev %#x DATA %#llx cpu %#llx wrapper %#llx)\n",
		p.soc, p.soc_revision,
		p.exec_base + soc->fw->segs[1].vmaddr, p.cpu_pa,
		p.wrapper_pa);
	return 0;
}

int ane_t6021_fwload_probe(struct ane_t6021 *ane)
{
	const struct ane_t602x_soc *soc = of_device_get_match_data(ane->dev);
	const struct ane_fw_image *img = soc->fw;
	const struct firmware *fw = NULL;
	struct ane_fw_seg segs[ANE_FW_NSEGS];
	u64 entry = 0;
	u8 actual_sha[32];
	void *buf;
	dma_addr_t iova;
	unsigned int i;
	const char *reason = NULL;
	u32 alloc_size = ANE_FW_BUF_SIZE + fw_extra_ram;
	int ret;
	bool reserved = ane_t6021_fw_alias_is_reserved(ane->dev);

	if (!fw_load)
		return 0;
	if (fw_extra_ram > SZ_16M || !IS_ALIGNED(fw_extra_ram, ANE_T6021_FW_ALIAS_PAGE))
		return -EINVAL;
	/* Set the coherent DMA mask once at probe, before any ring allocation. */

	ret = request_firmware(&fw, img->name, ane->dev);
	if (ret) {
		dev_err(ane->dev,
			"cannot load firmware %s: %d; install it with omarchy-ane-firmware-fetch (or equivalent for your distribution)\n",
			img->name, ret);
		return ret;
	}

	sha256(fw->data, fw->size, actual_sha);
	ret = ane_fw_validate_blob(fw->data, fw->size, img, actual_sha,
				   segs, &entry, &reason);
	if (ret) {
		dev_err(ane->dev, "fwload: validation failed: %s\n",
			reason ? reason : "?");
		release_firmware(fw);
		return ret;
	}

	buf = dma_alloc_coherent(ane->dev, alloc_size, &iova, GFP_KERNEL);
	if (!buf) {
		dev_err(ane->dev, "fwload: coherent alloc %#x failed\n",
			alloc_size);
		release_firmware(fw);
		return -ENOMEM;
	}

	for (i = 0; i < ANE_FW_NSEGS; i++) {
		if (segs[i].filesize)
			memcpy(buf + segs[i].vmaddr,
			       fw->data + segs[i].fileoff, segs[i].filesize);
	}

	ane->fw_buf = buf;
	ane->fw_iova = iova;
	ane->fw_size = alloc_size;

	dev_dbg(ane->dev,
		"fwload: %s PRELOAD validated + DART-mapped: entry %#llx, iova %pad size %#x\n",
		img->name, entry, &iova, alloc_size);

	ret = reserved ? 0 : ane_t6021_fw_patch(ane, buf);
	if (!ret)
		ret = ane_t6021_fw_alias_map(ane, reserved);
	if (ret) {
		ane_t6021_fwload_remove(ane);
		release_firmware(fw);
		return ret;
	}

	release_firmware(fw);
	return 0;
}

void ane_t6021_fwload_remove(struct ane_t6021 *ane)
{
	if (ane->fw_alias_extn) {
		struct iommu_domain *dom = iommu_get_domain_for_dev(ane->dev);
		int i;

		/*
		 * Unmap only the recorded extents; reserved windows need not
		 * be adjacent.
		 */
		if (dom)
			for (i = 0; i < ane->fw_alias_extn; i++)
				iommu_unmap(dom, ane->fw_alias_ext_iova[i],
					    ane->fw_alias_ext_len[i]);
		ane->fw_alias_extn = 0;
	}
	if (!ane->fw_buf)
		return;
	dma_free_coherent(ane->dev, ane->fw_size, ane->fw_buf, ane->fw_iova);
	ane->fw_buf = NULL;
	ane->fw_size = 0;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: firmware lifecycle
 *
 * The bootloader leaves the coprocessor firmware in two reserved, DART-mapped
 * segments: TEXT and DATA. Running the firmware changes DATA, so the driver
 * keeps a copy of DATA taken before the first boot and puts it back before
 * every later boot. That lets the coprocessor lose power (runtime PM, system
 * sleep) and start again from the same state as the first time.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "ave.h"
#include "ave-regs.h"
#include "ave-dart-pte.h"

/*
 * Legacy TEXT CRCs accepted at T8140 probe. Encoding separately requires
 * a layout from ave_layout_for_crc(); command formats change between builds.
 */
static const u32 ave_known_text_crc[] = {
	0x2ce7359e, /* AppleAVE2FW 9003.78.0 */
	0x69c54454, /* AppleAVE2FW 9003.78.0 (macOS 26.0.x) */
	AVE_FW_TEXT_CRC_T6040_25G83, /* AppleAVE2FW 9003.78.0 / T6040 */
	0x460c7a7c, /* AppleAVE2FW 6070.11.1 / T6000 */
	0x25bfd039, /* AppleAVE2FW 6070.11.1 / T8112 */
};

static int ave_fw_chosen_crc(struct ave_dev *ave, const char *name, u32 *crc)
{
	struct device_node *chosen = of_find_node_by_path("/chosen");
	const char *str;
	int ret;

	if (!chosen)
		return -ENOENT;
	ret = of_property_read_string(chosen, name, &str);
	of_node_put(chosen);
	if (ret)
		return ret;
	return kstrtou32(str, 16, crc);
}

static int ave_fw_segment(struct ave_dev *ave, unsigned int index,
			  struct ave_fw_segment *seg)
{
	struct device_node *np;
	struct resource res;
	const __be32 *prop;
	int len, ret;

	np = of_parse_phandle(ave->dev->of_node, "memory-region", index);
	if (!np)
		return -ENOENT;
	if (!of_node_name_eq(np->parent, "reserved-memory") ||
	    !of_device_is_compatible(np, "apple,asc-mem") ||
	    !of_property_read_bool(np, "no-map")) {
		ret = -EINVAL;
		goto out;
	}
	ret = of_address_to_resource(np, 0, &res);
	if (ret)
		goto out;

	/* iommu-addresses = <&ave iova-hi iova-lo size-hi size-lo> */
	prop = of_get_property(np, "iommu-addresses", &len);
	if (!prop || len != 5 * sizeof(u32) ||
	    be32_to_cpup(prop) != ave->dev->of_node->phandle) {
		ret = -EINVAL;
		goto out;
	}
	seg->phys = res.start;
	seg->size = resource_size(&res);
	seg->iova = of_read_number(prop + 1, 2);
	if (!seg->size || seg->phys > PHYS_ADDR_MAX - seg->size ||
	    seg->iova > U64_MAX - seg->size ||
	    of_read_number(prop + 3, 2) != seg->size ||
	    !IS_ALIGNED(seg->phys, SZ_16K) || !IS_ALIGNED(seg->iova, SZ_16K))
		ret = -EINVAL;
out:
	of_node_put(np);
	return ret;
}

static int ave_fw_check_mapping(struct ave_dev *ave,
				const struct ave_fw_segment *seg)
{
	for (size_t off = 0; off < seg->size; off += SZ_16K)
		if (iommu_iova_to_phys(ave->domain, seg->iova + off) !=
		    seg->phys + off)
			return -EIO;
	return 0;
}

static int ave_fw_crc(const struct ave_fw_segment *seg, u32 *crc, void *copy)
{
	void *va = memremap(seg->phys, seg->size, MEMREMAP_WB);

	if (!va)
		return -ENOMEM;
	*crc = crc32_le(~0U, va, seg->size) ^ ~0U;
	if (copy)
		memcpy(copy, va, seg->size);
	memunmap(va);
	return 0;
}

/*
 * The firmware build is identified by its image CRC. The loader links TEXT
 * to DATA by writing the DATA address into one cell that is zero in the
 * image, so hash TEXT with that cell as the image has it. Any other value
 * in the cell means TEXT is not the build this variant expects.
 */
int ave_fw_text_id_crc(const void *text, size_t size, u32 cell_off,
		       u64 data_iova, u32 *crc)
{
	static const u8 zero[sizeof(u64)];
	u64 cell;

	if (cell_off > size || size - cell_off < sizeof(zero))
		return -EINVAL;
	cell = get_unaligned_le64(text + cell_off);
	if (cell && cell != data_iova)
		return -ENODEV;
	*crc = crc32_le(~0U, text, cell_off);
	*crc = crc32_le(*crc, zero, sizeof(zero));
	*crc = crc32_le(*crc, text + cell_off + sizeof(zero),
			size - cell_off - sizeof(zero));
	*crc ^= ~0U;
	return 0;
}

static int ave_fw_text_id(struct ave_dev *ave, u32 raw, u32 *crc)
{
	void *va;
	int ret;

	*crc = raw;
	if (!ave->variant->text_link_cell)
		return 0;
	va = memremap(ave->text.phys, ave->text.size, MEMREMAP_WB);
	if (!va)
		return -ENOMEM;
	ret = ave_fw_text_id_crc(va, ave->text.size,
				 ave->variant->text_link_cell, ave->data.iova,
				 crc);
	memunmap(va);
	return ret;
}

/*
 * Find the firmware, check that it is a build we speak to and that DATA is
 * still as the bootloader left it, and keep a copy of DATA for restarts.
 */
int ave_fw_probe(struct ave_dev *ave)
{
	struct device *dev = ave->dev;
	u32 raw_crc, text_crc, data_crc, want;
	bool known = !ave->variant->boot_qualified;
	int ret;

	if (of_count_phandle_with_args(dev->of_node, "memory-region", NULL) !=
	    2)
		return dev_err_probe(dev, -ENODEV, "no preloaded firmware\n");

	ret = ave_fw_segment(ave, 0, &ave->text);
	if (!ret)
		ret = ave_fw_segment(ave, 1, &ave->data);
	if (ret)
		return dev_err_probe(dev, ret, "bad firmware reservation\n");

	if (ave->text.size != ave->variant->fw_text_size ||
	    ave->data.size != ave->variant->fw_data_size)
		return dev_err_probe(dev, -EINVAL,
				     "unexpected firmware segment sizes\n");
	if ((ave->text.phys < ave->data.phys + ave->data.size &&
	     ave->data.phys < ave->text.phys + ave->text.size) ||
	    (ave->text.iova < ave->data.iova + ave->data.size &&
	     ave->data.iova < ave->text.iova + ave->text.size))
		return dev_err_probe(dev, -EINVAL,
				     "overlapping firmware segments\n");

	if (ave_fw_check_mapping(ave, &ave->text) ||
	    ave_fw_check_mapping(ave, &ave->data))
		return dev_err_probe(
			dev, -EIO,
			"firmware is not mapped where it was loaded\n");

	ret = ave_fw_crc(&ave->text, &raw_crc, NULL);
	if (ret)
		return ret;
	ret = ave_fw_chosen_crc(ave, "aurora,ave-text-crc32", &want);
	if (ret || raw_crc != want)
		return dev_err_probe(dev, -ENODEV,
				     "firmware text differs from loader CRC\n");
	ret = ave_fw_text_id(ave, raw_crc, &text_crc);
	if (ret)
		return dev_err_probe(dev, ret,
				     "unexpected firmware link cell (text CRC %08x)\n",
				     raw_crc);
	if (text_crc != raw_crc)
		dev_dbg(dev, "loaded TEXT %08x is image %08x\n", raw_crc,
			text_crc);
	for (unsigned int i = 0; i < ARRAY_SIZE(ave_known_text_crc); i++)
		known |= text_crc == ave_known_text_crc[i];
	if (!known ||
	    (ave->variant->text_crc && text_crc != ave->variant->text_crc))
		return dev_err_probe(dev, -ENODEV,
				     "unsupported firmware (text CRC %08x)\n",
				     text_crc);
	ave->text_crc = text_crc;
	ave->layout = ave_layout_for_crc(text_crc);

	ave->data_pristine = vmalloc(ave->data.size);
	if (!ave->data_pristine)
		return -ENOMEM;
	ret = ave_fw_crc(&ave->data, &data_crc, ave->data_pristine);
	if (ret)
		goto free_data;

	/*
	 * The loader records the DATA checksum before anything runs. If it
	 * differs, the firmware already ran and there is no clean copy left.
	 */
	ret = ave_fw_chosen_crc(ave, "aurora,ave-data-crc32", &want);
	if (ret || data_crc != want) {
		ret = dev_err_probe(dev, -ENODEV,
				    "firmware data is not in its boot state\n");
		goto free_data;
	}

	ave->data_crc = data_crc;
	ave->fw_state = AVE_FW_OFF;
	if (!ave->variant->boot_qualified || !ave->layout)
		dev_info(
			dev,
			"probe only: firmware TEXT %08x DATA %08x needs qualification\n",
			text_crc, data_crc);
	else if (!ave->layout->encode_ready)
		dev_info(
			dev,
			"%s: TEXT %08x pristine DATA %08x; encoding disabled\n",
			ave->layout->name, text_crc, data_crc);
	return 0;

free_data:
	vfree(ave->data_pristine);
	ave->data_pristine = NULL;
	return ret;
}

/*
 * Read-only evidence for the 6070 first start: the coprocessor timebase must
 * advance, and the reset path rewrites DATA +0x60358 (0xfeed1b00 at rest)
 * before the firmware enables its MMU.
 */
static void ave_fw_6070_probe_state(struct ave_dev *ave, const char *when)
{
	const struct ave_coproc_regs *r = ave->variant->regs;
	u64 tb0, tb1;
	u32 *va;

	tb0 = readq(ave->coproc + r->timebase);
	udelay(200);
	tb1 = readq(ave->coproc + r->timebase);
	va = memremap(ave->data.phys, ave->data.size, MEMREMAP_WC);
	dev_info(ave->dev,
		 "6070 %s: timebase %#llx -> %#llx, ctrl %#x status %#x, DATA+0x60358 %#x +0x18000 %#x\n",
		 when, tb0, tb1, readl(ave->coproc + r->ctrl),
		 readl(ave->coproc + r->status),
		 va ? READ_ONCE(va[0x60358 / 4]) : 0,
		 va ? READ_ONCE(va[0x18000 / 4]) : 0);
	if (va)
		memunmap(va);
}

/*
 * 6070 (T8112) reset-to-hello probe ladder, read-only (SPEC-6070-hello r5).
 * Offsets are into the firmware TEXT and DATA segments. Nothing here writes
 * a segment, the mailbox or the DART, and nothing gates RUN or recovery: a
 * cacheable firmware store may still be dirty in the AVE cache, so only a
 * visible change means anything. Console lines are bounded; the whole DATA
 * image at the timeout goes to debugfs last_6070_data.
 */
#define AVE_6070_PT_START	0x20000
#define AVE_6070_PT_END		0x60000
#define AVE_6070_DATA_VA	0x000d0000
#define AVE_6070_DATA_VA_END	0x001f8000

static u64 ave_6070_q(const u8 *va, size_t size, size_t off)
{
	return off + 8 <= size ? le64_to_cpu(READ_ONCE(*(const __le64 *)(va + off))) : 0;
}

static u32 ave_6070_l(const u8 *va, size_t size, size_t off)
{
	return off + 4 <= size ? le32_to_cpu(READ_ONCE(*(const __le32 *)(va + off))) : 0;
}

/* Unaligned patchbay values, read through a WC (normal, uncached) mapping */
static u64 ave_6070_uq(const u8 *va, size_t size, size_t off)
{
	return off + 8 <= size ? get_unaligned_le64(va + off) : 0;
}

/* Count, CRC and the first @show nonzero 64-bit words of [lo, hi) */
static void ave_6070_range(struct ave_dev *ave, const char *when, const char *name,
			   const u8 *va, size_t size, size_t lo, size_t hi,
			   unsigned int show)
{
	unsigned int nonzero = 0, shown = 0, len = 0;
	char line[200];
	u32 crc;

	hi = min(hi, size);
	if (lo >= hi)
		return;
	crc = crc32_le(~0U, va + lo, hi - lo) ^ ~0U;
	line[0] = 0;
	for (size_t off = lo; off + 8 <= hi; off += 8) {
		u64 v = ave_6070_q(va, size, off);

		if (!v)
			continue;
		nonzero++;
		if (shown < show && len < sizeof(line) - 40) {
			len += scnprintf(line + len, sizeof(line) - len, " +%#zx=%#llx", off, v);
			shown++;
		}
	}
	dev_info(ave->dev, "6070 %s: %s [%#zx,%#zx) crc %08x nonzero %u%s%s\n",
		 when, name, lo, hi, crc, nonzero, nonzero ? ":" : "", line);
}

/*
 * Diagnostic only (SPEC-6070-hello r6 sections 6-7): walk SID 0 of both AVE
 * DARTs from the live TTBR, read-only, over every 16 KiB page of the
 * firmware TEXT and DATA spans, and compare with the backing ranges. No
 * table, register or TLB is written and no transaction is issued. The DART
 * driver owns the tables; this module decodes them only to report.
 */
#define AVE_6070_DART_PARAMS3	0x08
#define AVE_6070_DART_TTBR0	0x1400
#define AVE_6070_DART_TCR0	0x1000

/* Leaf PTE for @iova, or 0; *@why says where the walk stopped */
static u64 ave_6070_dart_leaf(void __iomem *r, unsigned int levels, u64 iova,
			      const char **why)
{
	u32 ttbr = readl(r + AVE_6070_DART_TTBR0);
	u64 table, pte = 0;

	*why = "ok";
	if (!(ttbr & AVE_DART_TTBR_VALID)) {
		*why = "no ttbr";
		return 0;
	}
	if (!ave_dart_iova_in_range(iova, levels)) {
		*why = "out of range";
		return 0;
	}
	table = ave_dart_ttbr_pa(ttbr);
	for (int level = levels - 1; level >= 0; level--) {
		const __le64 *t = memremap(table, SZ_16K, MEMREMAP_WB | MEMREMAP_WC);

		if (!t) {
			*why = "table not mappable";
			return 0;
		}
		pte = le64_to_cpu(READ_ONCE(t[ave_dart_index(iova, level)]));
		memunmap((void *)t);
		if (!(pte & AVE_DART2_VALID)) {
			*why = level ? "no table" : "no leaf";
			return 0;
		}
		table = ave_dart2_entry_pa(pte);
	}
	return pte;
}

struct ave_6070_span {
	const char *name;
	u64 iova, phys;
	size_t size;
	bool want_w;
};

static void ave_fw_6070_dart_walk(struct ave_dev *ave, const char *when)
{
	const struct ave_6070_span spans[] = {
		{ "TEXT", ave->text.iova, ave->text.phys, ave->text.size, false },
		{ "DATA", ave->data.iova, ave->data.phys, ave->data.size, true },
	};
	static const u64 probe[] = { 0x8000e8000ULL, 0x8000ec000ULL, 0x8000f0000ULL };
	unsigned int agree = 0, compared = 0, lv[2] = { 2, 2 };
	u32 vaw[2] = {};

	for (unsigned int d = 0; d < ave->n_darts; d++) {
		vaw[d] = (readl(ave->dart_regs[d] + AVE_6070_DART_PARAMS3) >> 16) & 0x3f;
		lv[d] = ave_dart_levels(vaw[d] ?: 36);
	}
	for (unsigned int d = 0; d < ave->n_darts; d++) {
		void __iomem *r = ave->dart_regs[d];
		unsigned int levels = lv[d];
		char line[200];
		int len = 0;

		dev_info(ave->dev, "6070 %s: DART%u %pa sid %u tcr %#x ttbr %#x va %u levels %u\n",
			 when, d, &ave->dart_phys[d], ave->dart_sid[d],
			 readl(r + AVE_6070_DART_TCR0), readl(r + AVE_6070_DART_TTBR0),
			 vaw[d], levels);
		for (unsigned int s = 0; s < ARRAY_SIZE(spans); s++) {
			const struct ave_6070_span *sp = &spans[s];
			unsigned int pages = 0, valid = 0, linear = 0, nr = 0, nw = 0, nc = 0;
			bool reported = false;

			for (size_t off = 0; off < sp->size; off += SZ_16K) {
				const char *why;
				u64 pte = ave_6070_dart_leaf(r, levels, sp->iova + off, &why);
				struct ave_dart_leaf l = ave_dart2_leaf(pte);
				bool lin = l.valid && l.pa == sp->phys + off;

				pages++;
				valid += l.valid;
				linear += lin;
				nr += l.valid && l.r;
				nw += l.valid && l.w;
				nc += l.valid && l.cache;
				if (!reported && (!lin || !l.r || (sp->want_w && !l.w))) {
					reported = true;
					dev_info(ave->dev,
						 "6070 %s: DART%u %s first mismatch iova %#llx pa %#llx want %#llx r%u w%u c%u (%s)\n",
						 when, d, sp->name, sp->iova + off, l.pa,
						 (u64)(sp->phys + off), l.r, l.w, l.cache, why);
				}
				if (d == 1 && ave->n_darts == 2) {
					const char *why0;

					compared++;
					agree += ave_6070_dart_leaf(ave->dart_regs[0], lv[0],
								    sp->iova + off, &why0) == pte;
				}
			}
			dev_info(ave->dev,
				 "6070 %s: DART%u %s %#llx+%#zx -> %pa: %u pages, %u valid, %u linear, R %u W %u cache %u%s\n",
				 when, d, sp->name, sp->iova, sp->size, &sp->phys, pages, valid,
				 linear, nr, nw, nc, reported ? "" : ", all as expected");
		}
		for (unsigned int i = 0; i < ARRAY_SIZE(probe); i++) {
			const char *why;
			struct ave_dart_leaf l =
				ave_dart2_leaf(ave_6070_dart_leaf(r, levels, probe[i], &why));

			len += scnprintf(line + len, sizeof(line) - len, " %#llx->%#llx v%u r%u w%u c%u",
					 probe[i], l.pa, l.valid, l.r, l.w, l.cache);
		}
		dev_info(ave->dev, "6070 %s: DART%u pages:%s\n", when, d, line);
	}
	if (compared)
		dev_info(ave->dev, "6070 %s: DART0 and DART1 leaves agree on %u of %u pages\n",
			 when, agree, compared);
}

static void ave_fw_6070_boot_snapshot(struct ave_dev *ave, const char *when,
				      bool keep)
{
	const struct ave_coproc_regs *r = ave->variant->regs;
	size_t ts = ave->text.size, ds = ave->data.size;
	const u8 *t, *d;
	u64 crash;

	t = memremap(ave->text.phys, ts, MEMREMAP_WC);
	d = memremap(ave->data.phys, ds, MEMREMAP_WC);
	if (!t || !d) {
		dev_info(ave->dev, "6070 %s: cannot map TEXT/DATA for the probe\n", when);
		goto out;
	}

	dev_info(ave->dev,
		 "6070 %s: ctrl %#x cpu %#x mbox %#x scratch %#x %#x %#x %#x %#x %#x %#x %#x (s7 assert %u)\n",
		 when, readl(ave->coproc + r->ctrl), readl(ave->coproc + r->status),
		 readl(ave->mbox + AVE_MBOX_STATUS),
		 readl(ave->mbox + AVE_MBOX_SCRATCH(0)), readl(ave->mbox + AVE_MBOX_SCRATCH(1)),
		 readl(ave->mbox + AVE_MBOX_SCRATCH(2)), readl(ave->mbox + AVE_MBOX_SCRATCH(3)),
		 readl(ave->mbox + AVE_MBOX_SCRATCH(4)), readl(ave->mbox + AVE_MBOX_SCRATCH(5)),
		 readl(ave->mbox + AVE_MBOX_SCRATCH(6)), readl(ave->mbox + AVE_MBOX_SCRATCH(7)),
		 !!(readl(ave->mbox + AVE_MBOX_SCRATCH(7)) & BIT(30)));
	/* 4.1 static prerequisites and the loader patchbay */
	dev_info(ave->dev,
		 "6070 %s: TEXT+0x423c %#llx page %u/%#x/%u DATA+0x60000 %#llx +0x60008 %#llx patch crc %08x guard %#llx rt %#llx soc %#x rev %#x asc %#llx %#llx\n",
		 when, (u64)ave_6070_l(t, ts, 0x4240) << 32 | ave_6070_l(t, ts, 0x423c),
		 ave_6070_l(t, ts, 0xcdfd8), ave_6070_l(t, ts, 0xcdfdc),
		 ave_6070_l(t, ts, 0xcdfe0),
		 ave_6070_q(d, ds, 0x60000), ave_6070_q(d, ds, 0x60008),
		 0x37a4 <= ds ? crc32_le(~0U, d + 0x35d8, 0x37a4 - 0x35d8) ^ ~0U : 0,
		 ave_6070_q(d, ds, 0x35d8), ave_6070_uq(d, ds, 0x363d),
		 ave_6070_l(d, ds, 0x3743), ave_6070_l(d, ds, 0x374f),
		 ave_6070_uq(d, ds, 0x375b), ave_6070_uq(d, ds, 0x376b));
	/* 4.2 reset and MMU boundary */
	dev_info(ave->dev,
		 "6070 %s: resume %#llx tmp +0x1fff0 %#llx +0x1fff8 %#llx\n",
		 when, ave_6070_q(d, ds, 0x60358), ave_6070_q(d, ds, 0x1fff0),
		 ave_6070_q(d, ds, 0x1fff8));
	ave_6070_range(ave, when, "temp PTE", d, ds, 0x18000, 0x18080, 16);
	ave_6070_range(ave, when, "temp table", d, ds, 0x18000, 0x20000, 16);
	ave_6070_range(ave, when, "final PT", d, ds, AVE_6070_PT_START, AVE_6070_PT_END, 16);
	ave_6070_range(ave, when, "MMU cfg", d, ds, 0x126fc0, 0x127068, 8);
	/* 4.3 runtime, platform and channel milestones */
	dev_info(ave->dev,
		 "6070 %s: guard live %#llx patch %#llx (%s) env %#llx log %#llx gpr %#llx exc %#llx crashbuf %#llx/%#llx task %#llx gpio %#llx ipi %#llx\n",
		 when, ave_6070_q(d, ds, 0x2338), ave_6070_q(d, ds, 0x35d8),
		 ave_6070_q(d, ds, 0x2338) == ave_6070_q(d, ds, 0x35d8) ? "equal" : "differ",
		 ave_6070_q(d, ds, 0x60be0), ave_6070_q(d, ds, 0x60b00),
		 ave_6070_q(d, ds, 0x126e60), ave_6070_q(d, ds, 0x126f38),
		 ave_6070_q(d, ds, 0x2b58), ave_6070_q(d, ds, 0x2b80),
		 ave_6070_q(d, ds, 0x60bc8), ave_6070_q(d, ds, 0x60b40),
		 ave_6070_q(d, ds, 0x60b68));
	ave_6070_range(ave, when, "IPC logger", d, ds, 0x60c10, 0x60c58, 0);
	/* 4.4 failure state: derived values only, raw records stay private */
	dev_info(ave->dev,
		 "6070 %s: terminal %#llx (3 exception, 4 abort, 2 main returned) asserted %#x\n",
		 when, ave_6070_q(d, ds, 0x126d58), ave_6070_l(d, ds, 0x60af8));
	ave_6070_range(ave, when, "crash state", d, ds, 0x2b30, 0x2bc0, 0);
	ave_6070_range(ave, when, "NMI state", d, ds, 0x1266a0, 0x1266c8, 5);
	dev_info(ave->dev, "6070 %s: DATA+0x1266a0 %#llx +0x1266c0 %#llx\n", when,
		 ave_6070_q(d, ds, 0x1266a0), ave_6070_q(d, ds, 0x1266c0));
	crash = ave_6070_q(d, ds, 0x2b58);
	if (crash >= AVE_6070_DATA_VA && crash < AVE_6070_DATA_VA_END) {
		size_t off = crash - AVE_6070_DATA_VA;

		ave_6070_range(ave, when, "crash buffer", d, ds, off,
			       off + min_t(size_t, 0x400, ds > off ? ds - off : 0), 0);
	}
	/* The DART walk only before RUN and at the timeout (keep marks the latter) */
	if (keep || !strcmp(when, "pre-run"))
		ave_fw_6070_dart_walk(ave, when);
	if (keep && ave->boot_capture && ave->boot_capture_size == ds)
		memcpy(ave->boot_capture, d, ds);
out:
	if (d)
		memunmap((void *)d);
	if (t)
		memunmap((void *)t);
}

static int ave_fw_restore_data(struct ave_dev *ave)
{
	void *va;

	if (!ave->data_dirty && !ave->variant->restore_data_first)
		return 0;
	va = memremap(ave->data.phys, ave->data.size, MEMREMAP_WC);
	if (!va)
		return -ENOMEM;
	memcpy(va, ave->data_pristine, ave->data.size);
	/* The restored image must be in memory before the coprocessor starts. */
	wmb();
	memunmap(va);
	if (ave->layout && ave->layout->abi->transport->scratch_halt) {
		u32 crc;
		int ret = ave_fw_crc(&ave->data, &crc, NULL);

		if (ret || crc != ave->data_crc)
			return ret ?: -EIO;
		dev_info(ave->dev, "restored DATA CRC %08x\n", ave->data_crc);
	}
	ave->data_dirty = false;
	return 0;
}

void ave_free(struct ave_dev *ave, struct ave_buf *b)
{
	if (b->cpu)
		dma_free_attrs(ave->dev, b->alloc, b->cpu, b->dma, b->attrs);
	b->cpu = NULL;
}

/*
 * Firmware-visible memory, zeroed and 16 KiB aligned in the encoder's DART.
 * @size is what the firmware is told; @alloc (at least @size) is mapped.
 */
int ave_alloc_padded(struct ave_dev *ave, struct ave_buf *b, size_t size,
		     size_t alloc, unsigned long attrs)
{
	if (b->cpu)
		return 0;
	b->size = size;
	b->alloc = PAGE_ALIGN(max(size, alloc));
	b->attrs = attrs;
	b->cpu =
		dma_alloc_attrs(ave->dev, b->alloc, &b->dma, GFP_KERNEL, attrs);
	if (!b->cpu)
		return -ENOMEM;
	if (!IS_ALIGNED(b->dma, SZ_16K)) {
		ave_free(ave, b);
		return -EINVAL;
	}
	return 0;
}

int ave_alloc(struct ave_dev *ave, struct ave_buf *b, size_t size,
	      unsigned long attrs)
{
	return ave_alloc_padded(ave, b, size, size, attrs);
}

/*
 * Lifecycle support and streaming qualification are independent: a known
 * transport is not enough to send unqualified Start/Process payloads.
 */
int ave_fw_check_boot(struct ave_dev *ave)
{
	if (ave->variant->boot_qualified && ave->variant->regs && ave->layout &&
	    ave->layout == ave_layout_for_crc(ave->text_crc))
		return 0;
	return -EOPNOTSUPP;
}

int ave_fw_check_build(struct ave_dev *ave)
{
	if (!ave_fw_check_boot(ave) && ave->layout->encode_ready)
		return 0;
	dev_info(
		ave->dev,
		"encoding refused: firmware TEXT %08x DATA %08x has no qualified command payload\n",
		ave->text_crc, ave->data_crc);
	return -EOPNOTSUPP;
}

/* Only after confirmed Halt, or before the firmware was ever started. */
void ave_fw_release_boot(struct ave_dev *ave)
{
	if (ave->extra0_mapped) {
		dma_unmap_resource(ave->dev, ave->extra0_dma,
				   ave->extra0_map_size, DMA_BIDIRECTIONAL, 0);
		ave->extra0_mapped = false;
	}
	ave_free(ave, &ave->heap);
	ave_free(ave, &ave->log);
	ave_free(ave, &ave->boot_cfg);
	ave_free(ave, &ave->ipc);
	if (ave->module_pinned) {
		ave->module_pinned = false;
		module_put(THIS_MODULE);
	}
}

static int ave_fw_map_extra0(struct ave_dev *ave)
{
	struct platform_device *pdev = to_platform_device(ave->dev);
	struct resource *res;

	if (!ave->variant->extra0_size || ave->extra0_mapped)
		return 0;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "extra0");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	if (!res || resource_size(res) < ave->variant->extra0_size ||
	    !IS_ALIGNED(res->start, SZ_16K))
		return dev_err_probe(
			ave->dev, -EINVAL,
			"missing or invalid extra0 register resource\n");
	/* The platform resource supplies the translated physical address. */
	ave->extra0_map_size = ALIGN(ave->variant->extra0_size, SZ_16K);
	ave->extra0_dma = dma_map_resource(ave->dev, res->start,
					   ave->extra0_map_size,
					   DMA_BIDIRECTIONAL, 0);
	if (dma_mapping_error(ave->dev, ave->extra0_dma))
		return -EIO;
	ave->extra0_mapped = true;
	return 0;
}

void ave_fw_remove(struct ave_dev *ave)
{
	ave_fw_release_boot(ave);
	vfree(ave->data_pristine);
	ave->data_pristine = NULL;
}

u64 ave_fw_vcbar(const struct ave_variant *variant, u64 text_iova)
{
	return (text_iova & variant->vcbar_mask) | variant->vcbar_flags;
}

void ave_dpe_apply(void __iomem *codec, const struct ave_dpe_table *t)
{
	for (unsigned int i = 0; i < t->count; i++) {
		const struct ave_dpe_rmw *r = &t->rows[i];

		for (u32 j = 0; j < r->count; j++) {
			void __iomem *reg = codec + r->offset + 4 * j;
			u32 value = r->values ? r->values[j] : r->value;

			writel((readl(reg) & ~r->mask) | value, reg);
		}
	}
	/* The settings must have landed before the coprocessor runs. */
	wmb();
}

/* Count (and log, given a device) registers whose masked field differs. */
unsigned int ave_dpe_check(void __iomem *codec, const struct ave_dpe_table *t,
			   struct device *dev)
{
	unsigned int bad = 0;

	for (unsigned int i = 0; i < t->count; i++) {
		const struct ave_dpe_rmw *r = &t->rows[i];

		for (u32 j = 0; j < r->count; j++) {
			u32 off = r->offset + 4 * j;
			u32 keep = r->retained ? r->retained : r->mask;
			u32 want = (r->values ? r->values[j] : r->value) & keep;
			u32 got = readl(codec + off);

			if ((got & keep) == want)
				continue;
			bad++;
			if (dev)
				dev_warn(dev, "DPE %#x reads %#x, field %#x wants %#x\n",
					 off, got, keep, want);
		}
	}
	return bad;
}

/*
 * Read-only state after a 6070 start that posted no hello: coprocessor
 * control and CPU status, the mailbox set aliases, pending words and
 * scratch0..3, and the DPE words the cold defaults touch.
 */
static void ave_fw_hello_snapshot(struct ave_dev *ave)
{
	const struct ave_coproc_regs *r = ave->variant->regs;
	const struct ave_dpe_table *dpe = ave->variant->dpe;

	dev_err(ave->dev,
		"no hello: ctrl %#x cpu %#x mbox %#x %#x %#x %#x scratch %#x %#x %#x %#x\n",
		readl(ave->coproc + r->ctrl), readl(ave->coproc + r->status),
		readl(ave->mbox + 0x08), readl(ave->mbox + 0x0c),
		readl(ave->mbox + AVE_MBOX_STATUS), readl(ave->mbox + 0x14),
		readl(ave->mbox + AVE_MBOX_SCRATCH(0)),
		readl(ave->mbox + AVE_MBOX_SCRATCH(1)),
		readl(ave->mbox + AVE_MBOX_SCRATCH(2)),
		readl(ave->mbox + AVE_MBOX_SCRATCH(3)));
	if (!dpe)
		return;
	dev_err(ave->dev,
		"no hello: DPE cat %#x %#x cac %#x %#x %#x %#x %#x, %u fields differ\n",
		readl(ave->codec + dpe->cat), readl(ave->codec + dpe->cat + 0x004),
		readl(ave->codec + dpe->cac), readl(ave->codec + dpe->cac + 0x004),
		readl(ave->codec + dpe->cac + 0x0a4),
		readl(ave->codec + dpe->cac + 0x0a8),
		readl(ave->codec + dpe->cac + 0x1ec),
		ave_dpe_check(ave->codec, dpe, NULL));
}

static bool ave_fw_idle(struct ave_dev *ave)
{
	if (readq(ave->coproc + ave->variant->regs->vcbar) !=
	    ave_fw_vcbar(ave->variant, ave->text.iova))
		return false;
	if (readl(ave->coproc + ave->variant->regs->ctrl) ||
	    readl(ave->mbox + AVE_MBOX_STATUS))
		return false;
	for (unsigned int i = 0; i < AVE_MBOX_SCRATCH_COUNT; i++)
		if (readl(ave->mbox + AVE_MBOX_SCRATCH(i)))
			return false;
	return true;
}

/*
 * After a Halt the firmware is restarted from a power cycle, as macOS does
 * for every session: the encoder must have been runtime suspended in
 * between, which puts the coprocessor back in reset. Hardware runs showed
 * that a restart while it kept power gets no answer, so that is refused
 * (-EAGAIN) and the state stays halted until the power really went. The
 * 9003 code base register must still be set: that ABI uses the loader value.
 */
static int ave_fw_prepare_restart(struct ave_dev *ave)
{
	u32 status;

	if (!ave->layout->abi->transport->boot_config) {
		if (ave->fw_state == AVE_FW_HALTED)
			return -EAGAIN; /* A runtime PM suspend/resume must intervene. */
		if (readl(ave->coproc + ave->variant->regs->ctrl))
			return -EBUSY;
		status = readl(ave->mbox + AVE_MBOX_STATUS);
		if (status)
			writel(status, ave->mbox + AVE_MBOX_STATUS);
		for (unsigned int i = 0; i < AVE_MBOX_SCRATCH_COUNT; i++)
			writel(0, ave->mbox + AVE_MBOX_SCRATCH(i));
		return 0;
	}
	if (readq(ave->coproc + ave->variant->regs->vcbar) !=
	    ave_fw_vcbar(ave->variant, ave->text.iova)) {
		dev_err(ave->dev,
			"coprocessor lost its code base (vcbar %#llx)\n",
			readq(ave->coproc + ave->variant->regs->vcbar));
		return -EIO;
	}
	if (ave->fw_state == AVE_FW_RECOVERING) {
		/*
		 * Recovery cleared CTRL itself and re-armed BOOT_ARG as a
		 * reset witness with control off: require it to have lost
		 * power too, before reusing DMA memory or restoring DATA. DARTs
		 * sharing VENC power may suspend later than the consumer.
		 */
		if (readl(ave->coproc + ave->variant->regs->ctrl) ||
		    readl(ave->coproc + ave->variant->regs->boot_arg))
			return -EAGAIN;
		ave_sess_reap(ave);
		ave->fw_state = AVE_FW_HALTED;
		ave->inject_wedge = false;
		dev_info(ave->dev, "firmware recovery power cycle confirmed\n");
	}
	if (ave->fw_state != AVE_FW_HALTED)
		return 0;

	if (readl(ave->coproc + ave->variant->regs->ctrl)) {
		dev_dbg(ave->dev,
			"coprocessor kept power after its halt; retry once it suspends\n");
		return -EAGAIN;
	}
	status = readl(ave->mbox + AVE_MBOX_STATUS);
	if (status)
		writel(status, ave->mbox + AVE_MBOX_STATUS);
	for (unsigned int i = 0; i < AVE_MBOX_SCRATCH_COUNT; i++)
		writel(0, ave->mbox + AVE_MBOX_SCRATCH(i));
	return 0;
}

/* Boot the coprocessor and bring up its command rings. Caller holds hw_lock. */
static int ave_fw_boot(struct ave_dev *ave)
{
	const struct ave_fw_transport *t = ave->layout->abi->transport;
	__le32 *cfg, *log;
	u32 status, hello[4];
	int ret;

	ret = ave_alloc(ave, &ave->ipc, t->ipc_bytes, 0);
	if (!ret && t->boot_config)
		ret = ave_alloc(ave, &ave->boot_cfg, SZ_16K, 0);
	if (!ret && t->boot_config)
		ret = ave_alloc(ave, &ave->log, AVE_LOG_BYTES, 0);
	if (!ret)
		ret = ave_fw_map_extra0(ave);
	if (ret)
		return ret;

	if (ave_fw_check_mapping(ave, &ave->text) ||
	    ave_fw_check_mapping(ave, &ave->data)) {
		dev_err(ave->dev, "firmware segments are no longer mapped\n");
		return -EIO;
	}

	ret = ave_fw_prepare_restart(ave);
	if (ret)
		return ret;

	/*
	 * The bootloader leaves the coprocessor stopped with its code base
	 * set. Do not start it from any other state.
	 */
	if (t->boot_config && !ave_fw_idle(ave)) {
		dev_err(ave->dev,
			"coprocessor is not in its reset state (vcbar %#llx ctrl %#x)\n",
			readq(ave->coproc + ave->variant->regs->vcbar),
			readl(ave->coproc + ave->variant->regs->ctrl));
		return -EIO;
	}

	/*
	 * The coprocessor is in reset here: never started, or halted and then
	 * power cycled (ave_fw_prepare_restart() refuses otherwise), so the
	 * DPE has lost its settings too. Program its defaults before RUN.
	 */
	/* Clear the SVE idle word first: the codec clocks stay gated otherwise. */
	if (ave->variant->mbox_idle) {
		writel(0, ave->mbox + ave->variant->mbox_idle);
		if (readl(ave->mbox + ave->variant->mbox_idle)) {
			dev_err(ave->dev, "SVE idle word did not clear\n");
			return -EIO;
		}
		wmb();
	}
	if (ave->variant->dpe) {
		ave_dpe_apply(ave->codec, ave->variant->dpe);
		if (ave_dpe_check(ave->codec, ave->variant->dpe, ave->dev))
			dev_warn(ave->dev, "DPE defaults did not all read back\n");
		else
			dev_dbg(ave->dev, "DPE defaults applied\n");
	}

	memset(ave->ring, 0, sizeof(ave->ring));
	ave->unacked = 0;
	memset(ave->want, 0, sizeof(ave->want));
	ave->reply_err = 0;
	ave->scalar_next = 0;

	ret = ave_fw_restore_data(ave);
	if (ret)
		return ret;

	if (t->boot_config) {
		cfg = ave->boot_cfg.cpu;
		log = ave->log.cpu;
		memset(cfg, 0, AVE_BOOT_CFG_BYTES);
		memset(log, 0, ave->log.size);
		cfg[AVE_BOOT_CFG_DEVICE] =
			cpu_to_le32(ave->variant->boot_device_id);
		cfg[AVE_BOOT_CFG_UNITS] = cpu_to_le32(1);
		cfg[AVE_BOOT_CFG_GROUPS] = cpu_to_le32(1);
		cfg[AVE_BOOT_CFG_SUBID_MASK] = cpu_to_le32(1);
		cfg[AVE_BOOT_CFG_LOG_LO] =
			cpu_to_le32(lower_32_bits(ave->log.dma));
		cfg[AVE_BOOT_CFG_LOG_HI] =
			cpu_to_le32(upper_32_bits(ave->log.dma));
		cfg[AVE_BOOT_CFG_LOG_BYTES] = cpu_to_le32(AVE_LOG_BYTES);
		cfg[AVE_BOOT_CFG_OPTIONS] =
			cpu_to_le32(AVE_BOOT_CFG_OPTIONS_VALUE);
		log[0] = cpu_to_le32(AVE_LOG_HDR_USED);
		log[1] = cpu_to_le32(AVE_LOG_HDR_BYTES);
		log[2] = cpu_to_le32(AVE_LOG_RING_BYTES);
		dma_wmb();

		writel(AVE_BOOT_MAGIC, ave->mbox + AVE_MBOX_SCRATCH(0));
		writel(lower_32_bits(ave->boot_cfg.dma),
		       ave->mbox + AVE_MBOX_SCRATCH(1));
		writel(upper_32_bits(ave->boot_cfg.dma),
		       ave->mbox + AVE_MBOX_SCRATCH(2));

	} else {
		writel(AVE_BOOT_MAGIC, ave->mbox + AVE_MBOX_SCRATCH(0));
		writel(0, ave->mbox + AVE_MBOX_SCRATCH(1));
		writel(ave->variant->boot_device_id,
		       ave->mbox + AVE_MBOX_SCRATCH(2));
	}
	if (!t->boot_config) {
		/*
		 * Written once in its encoded form. The register reads back
		 * decoded (the code base with bit 0 set), so a readback never
		 * equals the write; the first hello is the confirmation.
		 */
		writeq(ave_fw_vcbar(ave->variant, ave->text.iova),
		       ave->coproc + ave->variant->regs->vcbar);
		dev_dbg(ave->dev, "VCBAR %#llx (reads %#llx) boot argument %#x\n",
			ave_fw_vcbar(ave->variant, ave->text.iova),
			readq(ave->coproc + ave->variant->regs->vcbar),
			ave->variant->boot_arg_value);
	}

	if (!t->boot_config && !ave->module_pinned) {
		if (!try_module_get(THIS_MODULE))
			return -ENODEV;
		ave->module_pinned = true;
	}
	ave->data_dirty = true;
	writel(1, ave->coproc + ave->variant->regs->keep_mappings);
	writel(0, ave->coproc + ave->variant->regs->ctrl);
	writel(ave->variant->boot_arg_value,
	       ave->coproc + ave->variant->regs->boot_arg);
	if (ave->variant->restore_data_first) {
		ave_fw_6070_probe_state(ave, "before RUN");
		ave_fw_6070_boot_snapshot(ave, "pre-run", false);
	}
	writel(AVE_COPROC_CTRL_RUN, ave->coproc + ave->variant->regs->ctrl);
	if (ave->variant->restore_data_first) {
		/* Read-only ladder; none of it decides anything. */
		udelay(50);
		ave_fw_6070_boot_snapshot(ave, "50us", false);
		ave_fw_6070_probe_state(ave, "after RUN");
		usleep_range(900, 1100);
		ave_fw_6070_boot_snapshot(ave, "1ms", false);
		ret = readl_poll_timeout(ave->mbox + AVE_MBOX_STATUS, status,
					 status & AVE_MBOX_STATUS_PENDING, 50,
					 100 * USEC_PER_MSEC);
		if (ret)
			ave_fw_6070_boot_snapshot(ave, "100ms", false);
	}

	ret = readl_poll_timeout(ave->mbox + AVE_MBOX_STATUS, status,
				 status & AVE_MBOX_STATUS_PENDING, 50,
				 AVE_MBOX_TIMEOUT_US);
	if (ret) {
		dev_err(ave->dev, "firmware did not answer (status %#x)\n",
			readl(ave->coproc + ave->variant->regs->status));
		if (!t->boot_config)
			ave_fw_hello_snapshot(ave);
		if (ave->variant->restore_data_first)
			ave_fw_6070_boot_snapshot(ave, "timeout", true);
		return ret;
	}
	for (unsigned int i = 0; i < 4; i++)
		hello[i] = readl(ave->mbox + AVE_MBOX_SCRATCH(i));
	dma_rmb();

	ret = ave_ipc_negotiate(ave, hello);
	if (ret) {
		dev_err(ave->dev, "firmware start-up failed: %d\n", ret);
		return ret;
	}

	ave->fw_state = AVE_FW_RUNNING;
	ret = ave_cmd_config(ave);
	if (ret)
		return ret;

	dev_dbg(ave->dev, "firmware running, %u/%u ring slots\n",
		ave->ring[0].slots, ave->ring[1].slots);
	return 0;
}

/* Caller holds hw_lock. */
int ave_fw_ensure_running(struct ave_dev *ave)
{
	enum ave_fw_state prev;
	int ret;

	lockdep_assert_held(&ave->hw_lock);
	ret = ave_fw_check_boot(ave);
	if (ret)
		return ret;
	switch (ave->fw_state) {
	case AVE_FW_RUNNING:
		return 0;
	case AVE_FW_BROKEN:
		dev_err_ratelimited(ave->dev, "firmware recovery is waiting for idle runtime suspend\n");
		return -EIO;
	case AVE_FW_OFF:
	case AVE_FW_HALTED:
	case AVE_FW_RECOVERING:
		break;
	}

	/* A boot that reached RUN must use the same recovery as a session fault. */
	prev = ave->fw_state;
	ret = ave_fw_boot(ave);
	if (ret && ret != -EAGAIN) {
		if (ave->layout->abi->transport->scratch_halt &&
		    (ave->module_pinned ||
		     readl(ave->coproc + ave->variant->regs->ctrl))) {
			if (!ave->module_pinned && try_module_get(THIS_MODULE))
				ave->module_pinned = true;
			ave_fw_mark_broken(ave, ret, "boot");
			return ret;
		}
		if (readl(ave->coproc + ave->variant->regs->ctrl))
			ave_fw_mark_broken(ave, ret, "boot");
		else
			ave->fw_state = prev;
	}
	return ret;
}

/*
 * Tell the firmware to stop, once every session is closed. Halt gets no
 * ring reply. 6070 confirms quiescence through scratch0 before the host
 * stops the IOP; 9003 retains its existing settle interval.
 * Caller holds hw_lock.
 */
void ave_fw_halt(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_RUNNING)
		return;
	if (ave->layout->abi->transport->scratch_halt) {
		for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
			if (ave->sessions[i] &&
			    ave->sessions[i]->sess->fw_open) {
				ave->fw_state = AVE_FW_BROKEN;
				return;
			}
		writel(0, ave->mbox + AVE_MBOX_SCRATCH(0));
	}
	if (ave_cmd_halt(ave)) {
		dev_err(ave->dev, "cannot send Halt; firmware treated as stuck\n");
		ave_fw_mark_broken(ave, -EIO, "Halt");
		return;
	}
	if (ave->layout->abi->transport->scratch_halt) {
		u32 status;
		int ret;

		ret = readl_poll_timeout(ave->mbox + AVE_MBOX_SCRATCH(0),
					 status, status == AVE_BOOT_MAGIC, 100,
					 AVE_MBOX_TIMEOUT_US);
		if (ret) {
			dev_err(ave->dev,
				"Halt not confirmed; retaining firmware mappings and power\n");
			ave->fw_state = AVE_FW_BROKEN;
			return;
		}
		dev_dbg(ave->dev, "Halt confirmed: scratch0 %#x\n", status);
		/* Scratch confirmation establishes quiescence; stop and read back the IOP.
		 */
		writel(0, ave->coproc + ave->variant->regs->ctrl);
		ret = readl_poll_timeout(ave->coproc + ave->variant->regs->ctrl,
					 status, !status, 100,
					 AVE_MBOX_TIMEOUT_US);
		if (ret) {
			ave->fw_state = AVE_FW_BROKEN;
			return;
		}
	} else {
		msleep(AVE_HALT_SETTLE_MS);
	}
	ave->fw_state = AVE_FW_HALTED;
	memset(ave->ring, 0, sizeof(ave->ring));
	ave->ipc_fw_base = 0;
}

/*
 * Stop the firmware before its power goes away. Caller holds hw_lock.
 *
 * Sessions keep the device runtime active, so the firmware has normally
 * been halted already. A firmware that stopped answering keeps its power:
 * system sleep must not silently force-stop a potentially live DMA engine.
 * Runtime recovery is separate and only runs after all owners have gone.
 */
int ave_fw_shutdown(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	switch (ave->fw_state) {
	case AVE_FW_RUNNING:
		ave_fw_halt(ave);
		return ave->fw_state == AVE_FW_HALTED ? 0 : -EBUSY;
	case AVE_FW_BROKEN:
	case AVE_FW_RECOVERING:
		return -EBUSY;
	default:
		return 0;
	}
}

/* Caller holds hw_lock. Wake even idle sessions so none pins a broken boot. */
void ave_fw_mark_broken(struct ave_dev *ave, int err, const char *what)
{
	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_BROKEN)
		dev_err(ave->dev, "firmware failed (%s: %d); scheduling recovery\n", what, err);
	ave->fw_state = AVE_FW_BROKEN;
	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i])
			mod_delayed_work(ave->wq, &ave->sessions[i]->poll_work, 0);
}

/*
 * Only runtime PM may force-stop a broken firmware, with no session owners
 * and no runtime references. genpd and the IOMMU's PM device links then drop
 * the VENC domains and DARTs; AVE must not reset or reprogram the DART itself.
 * System sleep continues to use the strict ave_fw_shutdown() above.
 */
int ave_fw_runtime_suspend(struct ave_dev *ave)
{
	int ret;

	lockdep_assert_held(&ave->hw_lock);
	/* 6070 requires scratch-confirmed Halt; never force its power gate. */
	if (ave->layout && ave->layout->abi->transport->scratch_halt)
		return ave_fw_shutdown(ave);
	if (ave->fw_state != AVE_FW_BROKEN && ave->fw_state != AVE_FW_RECOVERING)
		return ave_fw_shutdown(ave);
	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i])
			return -EBUSY;
	if (ave->fw_state == AVE_FW_RECOVERING)
		return 0;
	if (!ave_recovery_allow(&ave->recovery, jiffies)) {
		dev_err_ratelimited(ave->dev, "firmware recovery limit: 3 attempts per 10 minutes\n");
		return -EBUSY;
	}
	ave->recovery_count++;
	dev_warn(ave->dev, "firmware recovery %u: stopping coprocessor and cycling VENC power\n",
		 ave->recovery_count);

	/* A ring may not exist after a failed boot. Never wait for Halt's reply. */
	ret = ave->inject_wedge ? -ETIMEDOUT : -EIO;
	if (ave->ipc_fw_base && ave->ring[0].slots && !ave->inject_wedge) {
		ave->fw_state = AVE_FW_RUNNING;
		ret = ave_cmd_halt(ave);
	}
	if (ret)
		dev_warn(ave->dev, "recovery Halt failed: %d; forcing control off\n", ret);
	else
		msleep(AVE_HALT_SETTLE_MS);
	writel(0, ave->coproc + ave->variant->regs->ctrl);
	if (readl(ave->coproc + ave->variant->regs->ctrl)) {
		ave->fw_state = AVE_FW_BROKEN;
		return -EIO;
	}
	/* CTRL=0 must not masquerade as power loss on the next attach. */
	writel(AVE_COPROC_BOOT_ARG_VALUE, ave->coproc + ave->variant->regs->boot_arg);
	if (readl(ave->coproc + ave->variant->regs->boot_arg) != AVE_COPROC_BOOT_ARG_VALUE) {
		ave->fw_state = AVE_FW_BROKEN;
		return -EIO;
	}
	ave->fw_state = AVE_FW_RECOVERING;
	return 0;
}

/* The coprocessor lost power behind the firmware's back. Caller holds hw_lock. */
void ave_fw_lost(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	ave->fw_state = AVE_FW_OFF;
	memset(ave->ring, 0, sizeof(ave->ring));
	ave->ipc_fw_base = 0;
}

/* Explicit lifecycle-only validation, without allocating encoder surfaces. */
int ave_fw_lifecycle_test(struct ave_dev *ave, enum ave_codec codec)
{
	struct ave_ctx *ctx;
	struct ave_session *sess;
	int ret;

	if (!ave->layout || ave->layout->abi != &ave_abi_6070)
		return -EOPNOTSUPP;
	ctx = kzalloc_obj(*ctx);
	sess = kzalloc_obj(*sess);
	if (!ctx || !sess) {
		ret = -ENOMEM;
		goto free;
	}
	ctx->ave = ave;
	ctx->sess = sess;
	sess->p.codec = codec;
	ret = pm_runtime_resume_and_get(ave->dev);
	if (ret < 0)
		goto free;
	mutex_lock(&ave->hw_lock);
	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i]) {
			ret = -EBUSY;
			goto unlock;
		}
	ret = ave_fw_ensure_running(ave);
	if (!ret)
		ret = ave_cmd_open(ctx);
	if (!ret)
		ret = ave_cmd_close(ctx);
	if (!ret) {
		ave_fw_halt(ave);
		if (ave->fw_state != AVE_FW_HALTED)
			ret = -EIO;
	} else if (ave->fw_state == AVE_FW_RUNNING) {
		/* Unconfirmed client lifetime: do not Halt or free anything it may own. */
		ave->fw_state = AVE_FW_BROKEN;
	}
unlock:
	mutex_unlock(&ave->hw_lock);
	/* Runtime suspend refuses broken firmware, retaining its DMA and power. */
	if (ave->fw_state != AVE_FW_BROKEN) {
		int err = pm_runtime_put_sync_suspend(ave->dev);

		if (!ret && err < 0)
			ret = err;
	} else {
		dev_err(ave->dev,
			"lifecycle failed (%d); retaining runtime power reference\n",
			ret);
	}
	dev_info(ave->dev, "6070 lifecycle codec %u: %d\n", codec, ret);
free:
	kfree(sess);
	kfree(ctx);
	return ret;
}

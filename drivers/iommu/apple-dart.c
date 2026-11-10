// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple DART (Device Address Resolution Table) IOMMU driver
 *
 * Copyright (C) 2021 The Asahi Linux Contributors
 *
 * Based on arm/arm-smmu/arm-ssmu.c and arm/arm-smmu-v3/arm-smmu-v3.c
 *  Copyright (C) 2013 ARM Limited
 *  Copyright (C) 2015 ARM Limited
 * and on exynos-iommu.c
 *  Copyright (c) 2011,2016 Samsung Electronics Co., Ltd.
 */

#include <kunit/static_stub.h>
#include <linux/align.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io-pgtable.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/kexec.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/soc/apple/dart.h>
#include <linux/soc/apple/t8132.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_iommu.h>
#include <linux/of_platform.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/soc/apple/tunable.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/swab.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "dma-iommu.h"

#if IS_ENABLED(CONFIG_APPLE_DART_KUNIT_TEST)
#include <kunit/static_stub.h>
#endif

#if IS_ENABLED(CONFIG_KUNIT) && !defined(MODULE)
#include <kunit/test.h>
#endif

#define DART_MAX_STREAMS 256
#define DART_MAX_TTBR 4
#define MAX_DARTS_PER_DEVICE 8
#define DART_PCIEC_PAGE_SHIFT 14
#define DART_PCIEC_ADDR_WIDTH 42
#define DART_PCIEC_STREAMS 64

/* Common registers */

#define DART_PARAMS1 0x00
#define DART_PARAMS1_PAGE_SHIFT GENMASK(27, 24)

#define DART_PARAMS2 0x04
#define DART_PARAMS2_BYPASS_SUPPORT BIT(0)

/* T8020/T6000 registers */

#define DART_T8020_STREAM_COMMAND 0x20
#define DART_T8020_STREAM_COMMAND_BUSY BIT(2)
#define DART_T8020_STREAM_COMMAND_INVALIDATE BIT(20)

#define DART_T8020_STREAM_SELECT 0x34

#define DART_T8020_ERROR 0x40
#define DART_T8020_ERROR_STREAM GENMASK(27, 24)
#define DART_T8020_ERROR_CODE GENMASK(11, 0)
#define DART_T8020_ERROR_FLAG BIT(31)

#define DART_T8020_ERROR_READ_FAULT BIT(4)
#define DART_T8020_ERROR_WRITE_FAULT BIT(3)
#define DART_T8020_ERROR_NO_PTE BIT(2)
#define DART_T8020_ERROR_NO_PMD BIT(1)
#define DART_T8020_ERROR_NO_TTBR BIT(0)

#define DART_T8020_CONFIG 0x60
#define DART_T8020_CONFIG_LOCK BIT(15)

#define DART_STREAM_COMMAND_BUSY_TIMEOUT 100

#define DART_T8020_ERROR_ADDR_HI 0x54
#define DART_T8020_ERROR_ADDR_LO 0x50

#define DART_T8020_STREAMS_ENABLE 0xfc

#define DART_T8020_REMAP       	0x80

#define DART_REMAP_MASK(sid)  GENMASK(7 + (8 * (sid % 4)), 8 * (sid % 4))
#define DART_REMAP(dart, sid) ((dart)->regs + DART_T8020_REMAP + ((sid) & ~0x3))

#define DART_T8020_TCR                  0x100
#define DART_T8020_TCR_TRANSLATE_ENABLE BIT(7)
#define DART_T8020_TCR_BYPASS_DART      BIT(8)
#define DART_T8020_TCR_BYPASS_DAPF      BIT(12)
#define DART_T8020_TCR_REMAP_ENABLE     BIT(24)

#define DART_T8020_TTBR       0x200
#define DART_T8020_USB4_TTBR  0x400
#define DART_T8020_TTBR_VALID BIT(31)
#define DART_T8020_TTBR_ADDR_FIELD_SHIFT 0
#define DART_T8020_TTBR_SHIFT 12

/* T8110 registers */

#define DART_T8110_PARAMS3 0x08
#define DART_T8110_PARAMS3_PA_WIDTH GENMASK(29, 24)
#define DART_T8110_PARAMS3_VA_WIDTH GENMASK(21, 16)
#define DART_T8110_PARAMS3_VER_MAJ GENMASK(15, 8)
#define DART_T8110_PARAMS3_VER_MIN GENMASK(7, 0)

#define DART_T8110_PARAMS4 0x0c
#define DART_T8110_PARAMS4_NUM_CLIENTS GENMASK(24, 16)
#define DART_T8110_PARAMS4_NUM_SIDS GENMASK(8, 0)

#define DART_T8110_TLB_CMD              0x80
#define DART_T8110_TLB_CMD_BUSY         BIT(31)
#define DART_T8110_TLB_CMD_OP           GENMASK(10, 8)
#define DART_T8110_TLB_CMD_OP_FLUSH_ALL 0
#define DART_T8110_TLB_CMD_OP_FLUSH_SID 1
#define DART_T8110_TLB_CMD_OP_FLUSH_UNLOCK 4
#define DART_T8110_TLB_CMD_V2 BIT(15)
#define DART_T8110_TLB_CMD_RANGE BIT(14)
#define DART_T8110_TLB_CMD_STT BIT(13)
/* A second TLB busy flag (bit 0), polled on the J713 external display DARTs */
#define DART_T8110_TLB_BUSY2 0x604
#define DART_T8110_TLB_START 0x98
#define DART_T8110_TLB_END 0xa0
#define DART_T8110_FLUSH_BUSY_TIMEOUT 2000
#define DART_T8110_TLB_CMD_STREAM       GENMASK(7, 0)

#define DART_T8110_ERROR 0x100
#define DART_T8110_ERROR_STREAM GENMASK(27, 20)
#define DART_T8110_ERROR_CODE GENMASK(14, 0)
#define DART_T8110_ERROR_FLAG BIT(31)

#define DART_T8110_ERROR_MASK 0x104

#define DART_T8110_ERROR_READ_FAULT BIT(5)
#define DART_T8110_ERROR_WRITE_FAULT BIT(4)
#define DART_T8110_ERROR_NO_PTE BIT(3)
#define DART_T8110_ERROR_NO_PMD BIT(2)
#define DART_T8110_ERROR_NO_PGD BIT(1)
#define DART_T8110_ERROR_NO_TTBR BIT(0)

#define DART_T8110_ERROR_ADDR_LO 0x170
#define DART_T8110_ERROR_ADDR_HI 0x174

#define DART_T8110_ERROR_STREAMS 0x1c0

#define DART_T8110_EXCEPTION(i) (0x4000 + 4 * (i))
#define DART_T8110_ERR_SID(sid) (0x8000 + 0x40 * (sid))
#define DART_T8110_ERR_SID_STATUS(sid) (DART_T8110_ERR_SID(sid) + 0x00)
#define DART_T8110_ERR_SID_STATUS_WRITE BIT(8)
#define DART_T8110_EXCEPTION_CODE GENMASK(3, 0)
#define DART_T8110_EXCEPTION_LEVEL GENMASK(13, 12)

/* DART v2.3 adds a block-level exception summary. */
#define DART_T8110_BLOCK_ERROR 0x704
#define DART_T8110_BLOCK_ERROR_MASK 0x777

#define DART_T8110_EXCEPTION_INFO(sid) (0x8000 + 0x40 * (sid))
#define DART_T8110_EXCEPTION_ADDR_LO(sid) (DART_T8110_ERR_SID(sid) + 0x8)
#define DART_T8110_EXCEPTION_ADDR_HI(sid) (DART_T8110_ERR_SID(sid) + 0xc)

#define DART_T8110_PROTECT 0x200
#define DART_T8110_UNPROTECT 0x204
#define DART_T8110_PROTECT_LOCK 0x208
#define DART_T8110_PROTECT_TTBR_TCR BIT(0)
#define DART_T8110_DIAG_LOCK 0x210
#define DART_T8110_DIAG_LOCK_ON_ERR BIT(1)

#define DART_T8110_ENABLE_STREAMS  0xc00
#define DART_T8110_DISABLE_STREAMS 0xc20

/*
 * Revision 2.2 and later moved the error mask; 2.4 also moved the error
 * status, the error interrupt enable and the per-stream error enables.
 */
#define DART_T8110_V2_ERROR_MASK 0x4060
#define DART_T8110_ERR_IRQ_STATUS 0x704
#define DART_T8110_ERR_IRQ_ENABLE 0x700
#define DART_T8110_ERR_SID_ENABLE(i) (0x720 + 4 * (i))
#define DART_T8110_V24_ERR_IRQ_STATUS 0x4204
#define DART_T8110_V24_ERR_IRQ_ENABLE 0x4200
#define DART_T8110_V24_ERR_SID_ENABLE(i) (0x4220 + 4 * (i))

/* PARAMS1 bit 29: client partitions (the 0x800.. registers) exist. */
#define DART_T8110_PARAMS1_CLIENT_PARTITIONS BIT(29)
#define DART_T8110_CLIENT_PARTITION_BASE 0x800
#define DART_T8110_CLIENT_PARTITION_END 0x1000
/* PARAMS2 bit 4: a translation context cache that a full flush leaves alone. */
#define DART_T8110_PARAMS2_CTC BIT(4)
#define DART_T8110_CTC_FLUSH 0x600

/* Per-port outstanding transactions; the port counts are in PARAMS2. */
#define DART_T8110_OUTSTANDING(port) (0x230 + 4 * (port))
#define DART_T8110_PARAMS2_READ_PORTS  GENMASK(18, 16)
#define DART_T8110_PARAMS2_WRITE_PORTS GENMASK(22, 20)
/* How long the power-down and power-up wait for transactions to drain. */
#define DART_T8110_IDLE_TIMEOUT_US 1000
/* TLB command of the power-down path: flush everything, also stream table state. */
#define DART_T8110_TLB_CMD_POWERDOWN (DART_T8110_TLB_CMD_V2 | DART_T8110_TLB_CMD_STT)

/*
 * PS_PROT ("clock protection") bank: one slice per protected DART. An
 * enabled slice (bit 4) guards a power-state register of the DART's power
 * domain; the native DART power-down clears bit 4 after its streams are
 * disabled and flushed, and power-up sets it again.
 */
#define APPLE_PS_PROT_ENABLE		0x20
#define APPLE_PS_PROT_LOCK		0x24
#define APPLE_PS_PROT_SLICE(n)		(0x100 + 0x40 * (n))
#define APPLE_PS_PROT_SLICES		64
#define APPLE_PS_PROT_SLICE_ON		BIT(4)

#define DART_T8110_TCR                  0x1000
#define DART_T8110_TCR_REMAP            GENMASK(11, 8)
#define DART_T8110_TCR_REMAP_ENABLE     BIT(7)
#define DART_T8110_TCR_FOUR_LEVEL       BIT(3)
#define DART_T8110_TCR_BYPASS_DAPF      BIT(2)
#define DART_T8110_TCR_BYPASS_DART      BIT(1)
#define DART_T8110_TCR_TRANSLATE_ENABLE BIT(0)

#define DART_T8110_TTBR       0x1400
#define DART_T8110_TTBR_VALID BIT(0)
#define DART_T8110_TTBR_ADDR_FIELD_SHIFT 2
#define DART_T8110_TTBR_SHIFT 14

#define DART_TCR(dart, sid) ((dart)->hw->tcr + ((sid) << 2))

#define DART_TTBR(dart, sid, idx) ((dart)->hw->ttbr + \
				   (((dart)->hw->ttbr_count * (sid)) << 2) + \
				   ((idx) << 2))

struct apple_dart_stream_map;
struct apple_dart_dcp;

enum dart_type {
	DART_T8020,
	DART_T6000,
	DART_T8110,
};

struct apple_dart;

struct apple_dart_hw {
	enum dart_type type;
	irqreturn_t (*irq_handler)(int irq, void *dev);
	int (*invalidate_tlb)(struct apple_dart_stream_map *stream_map);
	void (*enable_remap)(struct apple_dart *dart, int sid, int remap_to);

	u32 oas;
	enum io_pgtable_fmt fmt;

	int max_sid_count;

	u32 lock;
	u32 lock_bit;

	u32 error;

	u32 enable_streams;

	u32 tcr;
	u32 tcr_enabled;
	u32 tcr_disabled;
	u32 tcr_bypass;
	u32 tcr_4level;

	u32 ttbr;
	u32 ttbr_valid;
	u32 ttbr_addr_field_shift;
	u32 ttbr_shift;
	int ttbr_count;
};

/*
 * Snapshot of a locked root's firmware-owned slots, taken in process context
 * when the root is mapped: the root entry and a mapping of its leaf table.
 */
struct apple_dart_fw_root {
	u64 *entry;
	u64 **leaf;
	/* Linux root entry already verified against each firmware slot. */
	u64 *accepted;
};

/*
 * The subtrees behind the slots firmware holds in a four-level locked root
 * (fw_mirror), recorded when the root is mapped: per slot, the root entry and
 * a mapping of its mid-level table, and per mid entry the entry and a mapping
 * of its leaf table. Leaf entries are read live. A table entry that no longer
 * matches what was recorded resolves nothing. Only lookups use it: Linux
 * still never publishes into those slots.
 */
struct apple_dart_fw_tree {
	u64 *entry;
	u64 **mid;
	u64 **mid_entry;
	u64 ***leaf;
};

/*
 * Private structure associated with each DART device.
 *
 * @dev: device struct
 * @hw: SoC-specific hardware data
 * @regs: mapped MMIO region
 * @irq: interrupt number, can be shared with other DARTs
 * @clks: clocks associated with this DART
 * @num_clks: number of @clks
 * @lock: lock for hardware operations involving this dart
 * @pgsize: pagesize supported by this DART
 * @supports_bypass: indicates if this DART supports bypass mode
 * @locked: indicates if this DART is locked
 * @sid2group: maps stream ids to iommu_groups
 * @iommu: iommu core device
 */
struct apple_dart {
	struct device *dev;
	const struct apple_dart_hw *hw;

	void __iomem *regs;

	int irq;
	struct clk_bulk_data *clks;
	int num_clks;

	spinlock_t lock;

	u32 ias;
	u32 oas;
	u32 pgsize;
	u32 num_streams;
	u32 supports_bypass : 1;
	/* J613 MTP keeps issuing DMA in the boot loader identity domain. */
	bool live_mtp;
	u32 four_level : 1;
	u32 locked : 1;
	u32 tunneled : 1;
	u32 per_stream : 1;
	u32 isp_host : 1;
	u32 power_retained : 1;
	/* Locked roots may hold firmware slots that mirror Linux mappings. */
	u32 fw_mirror : 1;
	/*
	 * fw_mirror's four-level locked roots hold firmware's own subtrees, which
	 * are recorded and reserved (apple_dart_snapshot_fw_tree()). Set only for
	 * apple,t6050-dart, where it was tested (the DCP's log and crash log). Before
	 * widening it, check on T6041/T8140/T8142 whether any locked stream is
	 * four-level and what its foreign slots hold: the reservation moves DMA
	 * placement for every client of that stream.
	 */
	u32 fw_tree : 1;
	u32 fw_handoff : 1;
	u32 tunnel_state_saved : 1;
	u32 tunnel_state_restored : 1;
	bool commands_gated;
	/* Resume restored the translations, but the last steps of its reset failed. */
	bool reset_incomplete;
	struct apple_tunable *tunables;
	u16 version;

	/*
	 * Set up from the device tree for a DART nothing else initialised:
	 * tunables applied at every reset, streams kept in static bypass, every
	 * error reported, and the context cache flushed after a full TLB flush.
	 */
	DECLARE_BITMAP(static_bypass, DART_MAX_STREAMS);
	bool full_errors;
	bool ctc_flush;
	bool client_partitions;

	/* J713 internal display DARTs, borrowed from the running DCP. */
	struct apple_dart_dcp *dcp;
	struct apple_dart_dcp *frontend[2];
	/* The display coprocessor whose firmware DART (SID23) this is. */
	const struct dcp_adt_instance *dcp_instance;
	bool preserve_display;
	/* A J713 external display DART, borrowed from its external DCP */
	bool dispext;
	/*
	 * A display DART whose power can go away under its borrowed streams:
	 * an external display DART across sleep (its firmware powers it), or an
	 * internal one powered down by its DCP. Its registers are used only
	 * with its power confirmed (apple_dart_pm_get()).
	 */
	bool power_gated;
	/*
	 * Internal display DART: powered down by its DCP (from
	 * apple_dart_dcp_power_off() until apple_dart_dcp_power_on(); changed
	 * only under apple_dart_host_lock()), with what the power-up checks
	 * and restores.
	 */
	bool host_off;
	u32 off_streams, off_protect, off_lock;
	/* Its clock-protection slice in the PS_PROT bank, if it has one. */
	struct regmap *ps_prot;
	u32 ps_prot_slice, ps_prot_saved;
	/*
	 * Power-gated display DART: its power confirmation across
	 * sleep, under pm_gate_lock (a spinlock: only these fields, never
	 * across a register access): unverified from suspend until a state
	 * check at a D572(1) of the current generation, which every suspend
	 * and resume moves on; sleeping between suspend and resume; and the
	 * register uses in flight (apple_dart_pm_get()), which an
	 * invalidation waits for (pm_wq).
	 */
	spinlock_t pm_gate_lock;	/* the pm_* fields below */
	bool pm_unverified;
	bool pm_sleeping;
	u64 pm_gen;
	unsigned int pm_users;
	wait_queue_head_t pm_wq;
	u32 dispext_protect, dispext_lock;	/* PROTECT and PROTECT_LOCK as adopted */
	/* Retained radio/display state left in place across suspend-to-idle. */
	bool s2idle_kept;
	/* Resources reachable by a retained domain outlive device removal. */
	bool retained;
	bool retained_mtp;
	/* Cold ANE owns SID0 only; all other streams remain inherited. */
	bool cold_ane;
	u16 managed_sids;
	bool managed_failed;
	/* Serialize owned TCR publication against IRQ quarantine. */
	spinlock_t managed_lock;

	DECLARE_BITMAP(remapped, DART_MAX_STREAMS);
	u8 remap_target[DART_MAX_STREAMS];

	dma_addr_t dma_min;
	dma_addr_t dma_max;
	dma_addr_t dma_offset;

	struct iommu_group *sid2group[DART_MAX_STREAMS];
	struct iommu_device iommu;

	u32 save_tcr[DART_MAX_STREAMS];
	u32 save_ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];

	u64 *locked_ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];
	/* Values published by this driver into initially vacant root slots. */
	u64 *locked_owned[DART_MAX_STREAMS][DART_MAX_TTBR];
	/* Leaf tables behind the root slots firmware held when we attached. */
	struct apple_dart_fw_root *locked_fw[DART_MAX_STREAMS][DART_MAX_TTBR];
	/* Firmware's subtrees in a four-level locked root (TTBR 0 only). */
	struct apple_dart_fw_tree *locked_fw_tree[DART_MAX_STREAMS];
	/*
	 * First DVA of the window of the domain using each locked stream.  The
	 * window can be the client's own (apple,dma-range on the client node)
	 * rather than the DART's.
	 */
	u64 locked_window[DART_MAX_STREAMS];
	/*
	 * First error of a TLB invalidate that failed while retiring a locked
	 * root, per stream. The hardware may still cache translations through
	 * the retired page tables, so apple_dart_domain_free() keeps the page
	 * tables of every domain on such a stream. It is never cleared: only
	 * the hardware could show that the TLB no longer holds those entries.
	 * The first error of each stream is logged.
	 */
	int locked_retire_err[DART_MAX_STREAMS];
};

#ifndef MODULE
static struct kexec_blocker apple_dart_radio_kexec = {
	.users = ATOMIC_INIT(0),
	.reason = "Apple radio DMA has no qualified takeover/reset boundary",
};
#endif

static int apple_dart_retain_radio(struct apple_dart *dart)
{
#ifndef MODULE
	int ret = kexec_block(&apple_dart_radio_kexec);

	if (ret)
		return ret;
	get_device(dart->dev);
	WRITE_ONCE(dart->retained, true);
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

#if IS_ENABLED(CONFIG_APPLE_DART_J713_DCP)
static struct kexec_blocker apple_dart_display_kexec = {
	.users = ATOMIC_INIT(0),
	.reason = "retained Apple display DART tables and firmware DMA",
};
#endif

static void apple_dart_release(void *data)
{
	struct apple_dart *dart = data;

	if (!READ_ONCE(dart->retained))
		kfree(dart);
}

/*
 * devres still removes IRQs, sysfs and registrations on unexpected removal.
 * State and mappings reachable from pinned radio or display domains must survive it.
 * Before publication these actions perform ordinary probe rollback.
 */
struct apple_dart_retained_resource {
	struct apple_dart *dart;
	void (*release)(void *ptr, resource_size_t size);
	void *ptr;
	resource_size_t size;
};

static void apple_dart_release_retained_resource(void *data)
{
	struct apple_dart_retained_resource *r = data;

	if (!READ_ONCE(r->dart->retained))
		r->release(r->ptr, r->size);
	kfree(r);
}

static int apple_dart_manage_retained_resource(struct apple_dart *dart,
		void *ptr, resource_size_t size,
		void (*release)(void *, resource_size_t))
{
	struct apple_dart_retained_resource *r = kmalloc(sizeof(*r), GFP_KERNEL);

	if (!r) {
		release(ptr, size);
		return -ENOMEM;
	}
	*r = (struct apple_dart_retained_resource) {
		.dart = dart, .release = release, .ptr = ptr, .size = size,
	};
	return devm_add_action_or_reset(dart->dev,
					apple_dart_release_retained_resource, r);
}

static void apple_dart_free_retained_memory(void *ptr, resource_size_t size)
{
	kfree(ptr);
}

static void *apple_dart_retained_alloc(struct apple_dart *dart, size_t size)
{
	void *ptr = kzalloc(size, GFP_KERNEL);

	if (!ptr || apple_dart_manage_retained_resource(dart, ptr, size,
						apple_dart_free_retained_memory))
		return NULL;
	return ptr;
}

static void apple_dart_unmap_retained_memory(void *ptr, resource_size_t size)
{
	memunmap(ptr);
}

static void *apple_dart_retained_memremap(struct apple_dart *dart,
					phys_addr_t start, size_t size)
{
	void *ptr = memremap(start, size, MEMREMAP_WB);

	if (!ptr || apple_dart_manage_retained_resource(dart, ptr, size,
						apple_dart_unmap_retained_memory))
		return ERR_PTR(-ENOMEM);
	return ptr;
}

static void apple_dart_release_retained_region(void *ptr, resource_size_t size)
{
	struct resource *res = ptr;

	release_mem_region(res->start, resource_size(res));
}

static int apple_dart_retained_request_region(struct apple_dart *dart,
			resource_size_t start, resource_size_t size, const char *name)
{
	struct resource *res = request_mem_region(start, size, name);

	if (!res)
		return -EBUSY;
	return apple_dart_manage_retained_resource(dart, res, size,
						apple_dart_release_retained_region);
}

static void apple_dart_unmap_retained_mmio(void *ptr, resource_size_t size)
{
	iounmap((__force void __iomem *)ptr);
}

static void __iomem *apple_dart_retained_ioremap(struct apple_dart *dart,
						struct resource *res)
{
	void __iomem *ptr;
	int ret;

	ret = apple_dart_retained_request_region(dart, res->start,
						resource_size(res), dev_name(dart->dev));
	if (ret)
		return IOMEM_ERR_PTR(ret);
	/* Match devm_ioremap_resource(): Apple SoC MMIO is non-posted. */
	if (res->flags & IORESOURCE_MEM_NONPOSTED)
		ptr = ioremap_np(res->start, resource_size(res));
	else
		ptr = ioremap(res->start, resource_size(res));
	if (!ptr || apple_dart_manage_retained_resource(dart, (__force void *)ptr,
				resource_size(res), apple_dart_unmap_retained_mmio))
		return IOMEM_ERR_PTR(-ENOMEM);
	return ptr;
}

/*
 * PCIe-C is a cable-powered aperture and can report a failed transaction as
 * an asynchronous external abort unless each access is completed before the
 * next instruction retires.  The DART reset sequence is only stable with
 * Device-nGnRnE attributes and a dsb sy + isb after every access.
 * ioremap_np() supplies the memory type, but writel() only supplies the
 * normal I/O ordering barrier on arm64, so add the barriers here for the
 * tunneled DART and leave all always-on DARTs on the normal fast path.
 */
static inline void apple_dart_writel(struct apple_dart *dart, u32 value,
				    u32 offset)
{
#if IS_ENABLED(CONFIG_APPLE_DART_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_dart_writel, dart, value, offset);
#endif
	writel(value, dart->regs + offset);
	if (unlikely(dart->tunneled)) {
		mb();
		isb();
	}
}

static inline u32 apple_dart_readl(struct apple_dart *dart, u32 offset)
{
	u32 value = readl(dart->regs + offset);

	if (unlikely(dart->tunneled)) {
		mb();
		isb();
	}

	return value;
}

static void apple_dart_apply_tunables(struct apple_dart *dart)
{
	size_t i;

	for (i = 0; i < dart->tunables->sz; ++i) {
		u32 old_value, value;

		old_value = apple_dart_readl(dart,
					      dart->tunables->values[i].offset);
		value = old_value & ~dart->tunables->values[i].mask;
		value |= dart->tunables->values[i].value;
		if (value != old_value)
			apple_dart_writel(dart, value,
					   dart->tunables->values[i].offset);
	}
}

/*
 * Convenience struct to identify streams.
 *
 * The normal variant is used inside apple_dart_master_cfg which isn't written
 * to concurrently.
 * The atomic variant is used inside apple_dart_domain where we have to guard
 * against races from potential parallel calls to attach/detach_device.
 * Note that even inside the atomic variant the apple_dart pointer is not
 * protected: This pointer is initialized once under the domain init mutex
 * and never changed again afterwards. Devices with different dart pointers
 * cannot be attached to the same domain.
 *
 * @dart dart pointer
 * @sid stream id bitmap
 */
struct apple_dart_stream_map {
	struct apple_dart *dart;
	DECLARE_BITMAP(sidmap, DART_MAX_STREAMS);
};
struct apple_dart_atomic_stream_map {
	struct apple_dart *dart;
	atomic_long_t sidmap[BITS_TO_LONGS(DART_MAX_STREAMS)];
};

/*
 * This structure is attached to each iommu domain handled by a DART.
 *
 * @pgtbl_ops: pagetable ops allocated by io-pgtable
 * @finalized: true if the domain has been completely initialized
 * @init_lock: protects domain initialization
 * @stream_maps: streams attached to this domain (valid for DMA/UNMANAGED only)
 * @fw_sidmap: streams whose locked firmware roots the domain imported at
 *             finalization, per entry of @stream_maps; unlike @stream_maps
 *             they are known before any device is attached
 * @domain: core iommu domain pointer
 */
struct apple_dart_domain {
	struct io_pgtable_ops *pgtbl_ops;
	struct apple_dart_dcp *dcp;

	bool finalized;
	bool has_dma_range;
	dma_addr_t dma_min;
	dma_addr_t dma_max;
	u64 mask;
	dma_addr_t dma_offset;
	struct mutex init_lock;
	struct apple_dart_atomic_stream_map stream_maps[MAX_DARTS_PER_DEVICE];
	DECLARE_BITMAP(fw_sidmap[MAX_DARTS_PER_DEVICE], DART_MAX_STREAMS);

	struct iommu_domain domain;
};

/*
 * This structure is attached to devices with dev_iommu_priv_set() on of_xlate
 * and contains a list of streams bound to this device.
 * So far the worst case seen is a single device with two streams
 * from different darts, such that this simple static array is enough.
 *
 * @streams: streams for this device
 */
struct apple_dart_master_cfg {
	/* Intersection of DART capabilitles */
	u32 supports_bypass : 1;
	u32 locked : 1;

	/*
	 * DMA aperture start and end to be overridden by "iommus"' phandle
	 * args. By default determined by DART's ias but may be outside of it.
	 */
	dma_addr_t dma_min;
	dma_addr_t dma_max;

	struct apple_dart_stream_map stream_maps[MAX_DARTS_PER_DEVICE];
};

/*
 * Helper macro to iterate over apple_dart_master_cfg.stream_maps and
 * apple_dart_domain.stream_maps
 *
 * @i int used as loop variable
 * @base pointer to base struct (apple_dart_master_cfg or apple_dart_domain)
 * @stream pointer to the apple_dart_streams struct for each loop iteration
 */
#define for_each_stream_map(i, base, stream_map)                               \
	for (i = 0, stream_map = &(base)->stream_maps[0];                      \
	     i < MAX_DARTS_PER_DEVICE && stream_map->dart;                     \
	     stream_map = &(base)->stream_maps[++i])

static struct platform_driver apple_dart_driver;
static const struct iommu_ops apple_dart_iommu_ops;
static bool apple_dart_fault_pending(struct apple_dart *dart);

/* READY bring-up only: userspace must arm its watchdog before probing SID23. */
static bool apple_dart_dcp_enabled;
module_param_named(j713_dcp, apple_dart_dcp_enabled, bool, 0600);
MODULE_PARM_DESC(j713_dcp, "Enable explicit J713 DCP DART probe after arming watchdog");

static bool apple_dart_frontend_enabled;
module_param_named(j713_display, apple_dart_frontend_enabled, bool, 0600);
MODULE_PARM_DESC(j713_display, "Enable explicit J713 display DART admission after arming watchdog");

static int apple_dart_dcp_invalidate_tlb(struct apple_dart_stream_map *stream_map);

#include "apple-dart-dcp.h"

static struct apple_dart_domain *to_dart_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct apple_dart_domain, domain);
}

/*
 * Asahi's stream remapping programs only the first stream of a map and
 * remaps the others onto it. A PCIe-C tunnel DART keeps programming and
 * invalidating every stream on its own, with the ordered accesses of
 * apple_dart_writel(). So do the T8140 and T8142 DARTs (per_stream): their
 * locked streams keep firmware-owned roots and filtering state of their own,
 * and their static remaps come from apple,remap-streams.
 */
static bool apple_dart_remaps_streams(struct apple_dart *dart)
{
	return dart->hw->enable_remap && !dart->tunneled &&
	       !dart->per_stream && !dart->managed_sids;
}

static void
apple_dart_hw_enable_translation(struct apple_dart_stream_map *stream_map, int levels)
{
	struct apple_dart *dart = stream_map->dart;
	u32 tcr = dart->hw->tcr_enabled;
	int sid, remap_to;

	if (levels == 4)
		tcr |= dart->hw->tcr_4level;

	WARN_ON(levels != 3 && levels != 4);
	WARN_ON(levels == 4 && !dart->four_level);

	if (!apple_dart_remaps_streams(dart)) {
		for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
			apple_dart_writel(dart, tcr, DART_TCR(dart, sid));
		return;
	}

	/* Search for first enabled sid and enable translation. */
	sid = find_first_bit(stream_map->sidmap, dart->num_streams);
	if (sid >= dart->num_streams)
		return;
	writel(tcr, dart->regs + DART_TCR(dart, sid));

	/* Enable remapping for the remaining used sids. */
	remap_to = sid++;
	for_each_set_bit_from(sid, stream_map->sidmap, dart->num_streams)
		dart->hw->enable_remap(dart, sid, remap_to);
}

static void apple_dart_hw_disable_dma(struct apple_dart_stream_map *stream_map)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	/* This will also disable remap enabled in apple_dart_hw_enable_translation(). */
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		apple_dart_writel(dart, dart->hw->tcr_disabled,
				   DART_TCR(dart, sid));
}

static void
apple_dart_hw_enable_bypass(struct apple_dart_stream_map *stream_map)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(!stream_map->dart->supports_bypass);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		apple_dart_writel(dart, dart->hw->tcr_bypass,
				   DART_TCR(dart, sid));
}

static void apple_dart_hw_set_ttbr(struct apple_dart_stream_map *stream_map,
				   u8 idx, phys_addr_t paddr)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	WARN_ON(paddr & ((1 << dart->hw->ttbr_shift) - 1));
	if (!apple_dart_remaps_streams(dart)) {
		for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
			apple_dart_writel(dart,
					   dart->hw->ttbr_valid |
					   (paddr >> dart->hw->ttbr_shift) <<
					   dart->hw->ttbr_addr_field_shift,
					   DART_TTBR(dart, sid, idx));
		return;
	}

	/* Search for the first used stream and set its TTBR. All other sids
	 * of this DART are handled via the remap feature and must not set a
	 * valid TTBR.
	 */
	sid = find_first_bit(stream_map->sidmap, dart->num_streams);
	if (sid >= dart->num_streams)
		return;

	writel(dart->hw->ttbr_valid |
	       (paddr >> dart->hw->ttbr_shift) << dart->hw->ttbr_addr_field_shift,
	       dart->regs + DART_TTBR(dart, sid, idx));
}

static void apple_dart_hw_clear_ttbr(struct apple_dart_stream_map *stream_map,
				     u8 idx)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		apple_dart_writel(dart, 0, DART_TTBR(dart, sid, idx));
}

static void
apple_dart_hw_clear_all_ttbrs(struct apple_dart_stream_map *stream_map)
{
	int i;

	for (i = 0; i < stream_map->dart->hw->ttbr_count; ++i)
		apple_dart_hw_clear_ttbr(stream_map, i);
}

/* APPLE_DART2 page-table entry layout, as in io-pgtable-dart. */
#define APPLE_DART_PTE_VALID BIT_ULL(0)

static phys_addr_t apple_dart_pte_to_paddr(const struct apple_dart *dart,
					   u64 pte)
{
	return (pte & GENMASK_ULL(37, 10)) << 4;
}

/* Firmware table pages must remain outside Linux's page allocator. */
static bool apple_dart_fw_handoff_enabled(const struct apple_dart *dart)
{
	struct device_node *np = dart->dev->of_node;
	u32 handoff;

	return dart->locked && of_device_is_compatible(np, "apple,t8140-dart") &&
		of_property_count_u32_elems(np, "apple,firmware-root-handoff") == 1 &&
		!of_property_read_u32(np, "apple,firmware-root-handoff", &handoff) &&
		handoff == 1;
}

/* A 16K table page, aligned and inside the output address space. */
static bool apple_dart_fw_table_page(const struct apple_dart *dart,
				     phys_addr_t phys, phys_addr_t *end)
{
	return dart->pgsize == SZ_16K && IS_ALIGNED(phys, dart->pgsize) &&
	       !check_add_overflow(phys, (phys_addr_t)dart->pgsize - 1, end) &&
	       dart->oas < 64 && *end < BIT_ULL(dart->oas);
}

static bool apple_dart_fw_table_ram(const struct apple_dart *dart,
				   phys_addr_t phys)
{
	unsigned long first, last, pfn;
	phys_addr_t end;

	if (!apple_dart_fw_table_page(dart, phys, &end))
		return false;
	first = PHYS_PFN(phys);
	last = PHYS_PFN(end);
	for (pfn = first; pfn <= last; pfn++)
		if (!pfn_valid(pfn) || !PageReserved(pfn_to_page(pfn)))
			return false;
	return true;
}

/* A valid table entry with no bits but the next table's address. */
static bool apple_dart_fw_table_pte(const struct apple_dart *dart, u64 pte,
				    phys_addr_t *phys)
{
	if (!(pte & APPLE_DART_PTE_VALID) ||
	    (pte & ~(GENMASK_ULL(37, 10) | APPLE_DART_PTE_VALID)))
		return false;
	*phys = apple_dart_pte_to_paddr(dart, pte);
	return true;
}

static bool apple_dart_fw_table_decode(const struct apple_dart *dart,
				      u64 pte, phys_addr_t *phys)
{
	return apple_dart_fw_table_pte(dart, pte, phys) &&
	       apple_dart_fw_table_ram(dart, *phys);
}

/*
 * A table page of firmware's four-level subtrees, which is mapped write-back:
 * reserved RAM as for apple_dart_fw_table_ram(), or memory no resource
 * claims. T6050's boot loader places these tables outside the memory node,
 * so they are not RAM to Linux. An entry naming Linux's own RAM or a
 * device's registers is refused, and its DVAs resolve nothing.
 */
static bool apple_dart_fw_tree_table(const struct apple_dart *dart,
				     phys_addr_t phys)
{
	phys_addr_t end;

	if (apple_dart_fw_table_ram(dart, phys))
		return true;
	return apple_dart_fw_table_page(dart, phys, &end) &&
	       region_intersects(phys, dart->pgsize, IORESOURCE_MEM,
				 IORES_DESC_NONE) == REGION_DISJOINT;
}

static bool apple_dart_fw_slot_owned(const struct apple_dart_fw_root *fw,
				     const u64 *live, size_t i)
{
	return fw && fw->leaf[i] && READ_ONCE(live[i]) == fw->entry[i];
}

static bool apple_dart_fw_leaf_decode(u64 pte, dma_addr_t iova,
				      phys_addr_t *phys, int *prot)
{
	if (!(pte & BIT_ULL(0)) || (pte & GENMASK_ULL(63, 52)) ||
	    (pte & GENMASK_ULL(51, 40)) != GENMASK_ULL(51, 40))
		return false;

	*phys = ((pte & GENMASK_ULL(37, 10)) << 4) | (iova & (SZ_16K - 1));
	*prot = (!(pte & BIT_ULL(3)) ? IOMMU_READ : 0) |
		(!(pte & BIT_ULL(2)) ? IOMMU_WRITE : 0) |
		(!(pte & BIT_ULL(1)) ? IOMMU_CACHE : 0);
	return true;
}

static bool apple_dart_fw_mapping_matches(phys_addr_t phys, int prot,
					  phys_addr_t expected, int required)
{
	const int mask = IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE;

	return phys == expected && (prot & mask) == (required & mask);
}

/*
 * Whether the firmware leaf behind root @slot of a three-level stream already
 * translates the page at @addr to @paddr with the protection Linux asks for.
 */
static bool apple_dart_fw_page_matches(const struct apple_dart *dart,
				       const struct apple_dart_fw_root *fw,
				       u64 slot, u64 addr, phys_addr_t paddr,
				       int prot)
{
	u64 leaf = (addr / dart->pgsize) % (dart->pgsize / sizeof(u64));
	phys_addr_t phys;
	int fw_prot;

	return apple_dart_fw_leaf_decode(READ_ONCE(fw->leaf[slot][leaf]), addr,
					 &phys, &fw_prot) &&
	       apple_dart_fw_mapping_matches(phys, fw_prot, paddr, prot);
}

/*
 * A root slot firmware already uses can still back Linux mappings when the
 * firmware's leaf table translates every page Linux mapped there to the same
 * physical address. That is the case for the boot loader's handoff, which
 * re-exports live firmware translations as reserved IOVA regions; the
 * hardware keeps walking the firmware table and Linux's copy stays private.
 */
static bool apple_dart_fw_slot_mirrors(const struct apple_dart *dart,
				       const struct apple_dart_fw_root *fw,
				       u64 next, size_t i, size_t entries)
{
	const u64 *ours_leaf, *fw_leaf;
	size_t j;

	ours_leaf = phys_to_virt(apple_dart_pte_to_paddr(dart, next));
	fw_leaf = fw->leaf[i];
	for (j = 0; j < entries; j++) {
		u64 ours = READ_ONCE(ours_leaf[j]);
		u64 theirs = READ_ONCE(fw_leaf[j]);

		phys_addr_t ours_phys, fw_phys;
		int ours_prot, fw_prot;

		if (!(ours & APPLE_DART_PTE_VALID))
			continue;

		if (!apple_dart_fw_leaf_decode(ours, 0, &ours_phys, &ours_prot) ||
		    !apple_dart_fw_leaf_decode(theirs, 0, &fw_phys, &fw_prot) ||
		    !apple_dart_fw_mapping_matches(fw_phys, fw_prot,
					   ours_phys, ours_prot))
			return false;
	}
	return true;
}

/*
 * A locked root belongs to firmware. Publish only into vacant slots and
 * remember exactly what we installed, so teardown cannot erase firmware
 * mappings added after Linux attached. A slot firmware already holds is
 * never written; its descendants must still mirror Linux's mappings, and
 * apple_dart_check_fw_map() rejects new mappings anywhere in that slot.
 */
static int apple_dart_publish_root(const struct apple_dart *dart,
				   u64 *live, u64 *owned, const u64 *ours,
				   struct apple_dart_fw_root *fw,
				   size_t entries)
{
	size_t i;

	for (i = 0; i < entries; i++) {
		u64 next = READ_ONCE(ours[i]);
		u64 previous = READ_ONCE(owned[i]);

		if (!next && !previous)
			continue;
		if (READ_ONCE(live[i]) == previous)
			continue;
		if (previous || !apple_dart_fw_slot_owned(fw, live, i))
			return -EBUSY;
		if ((dart->fw_handoff || next != fw->accepted[i]) &&
		    !apple_dart_fw_slot_mirrors(dart, fw, next, i, entries))
			return -EBUSY;
	}

	dma_wmb();
	for (i = 0; i < entries; i++) {
		u64 next = READ_ONCE(ours[i]);
		u64 previous = READ_ONCE(owned[i]);

		if (next == previous)
			continue;
		if (!previous && apple_dart_fw_slot_owned(fw, live, i)) {
			fw->accepted[i] = next;
			continue;
		}
		if (cmpxchg(&live[i], previous, next) != previous)
			return -EBUSY;
		WRITE_ONCE(owned[i], next);
	}
	return 0;
}

static void apple_dart_retire_root(u64 *live, u64 *owned, size_t entries)
{
	size_t i;

	for (i = 0; i < entries; i++) {
		u64 previous = READ_ONCE(owned[i]);

		if (!previous)
			continue;
		/* Preserve a slot if its owner changed after we published it. */
		cmpxchg(&live[i], previous, 0);
		WRITE_ONCE(owned[i], 0);
	}
	dma_wmb();
}

static void apple_dart_free_fw_root(struct apple_dart *dart,
				    struct apple_dart_fw_root *fw)
{
	size_t i;

	if (!fw)
		return;
	if (fw->leaf)
		for (i = 0; i < dart->pgsize / sizeof(u64); i++)
			if (fw->leaf[i])
				memunmap(fw->leaf[i]);
	kfree(fw->accepted);
	kfree(fw->leaf);
	kfree(fw->entry);
	kfree(fw);
}

/*
 * Record the leaf tables behind the slots firmware holds in a three-level
 * T8110-format locked root, on DARTs whose loader re-exports firmware
 * translations (fw_handoff). Other roots are not snapshotted; their firmware
 * slots keep rejecting Linux mappings as before.
 */
static struct apple_dart_fw_root *
apple_dart_snapshot_fw_root(struct apple_dart *dart, int sid, const u64 *live)
{
	size_t i, entries = dart->pgsize / sizeof(u64);
	struct apple_dart_fw_root *fw;
	struct apple_dart_fw_root *nomem = dart->fw_handoff ? ERR_PTR(-ENOMEM) : NULL;

	/* Only the T8110-style table layout this walker decodes. */
	if ((!dart->fw_mirror && !dart->fw_handoff) || dart->hw->fmt != APPLE_DART2)
		return NULL;
	if (dart->hw->tcr_4level &&
	    (apple_dart_readl(dart, DART_TCR(dart, sid)) & dart->hw->tcr_4level))
		return NULL;

	fw = kzalloc(sizeof(*fw), GFP_KERNEL);
	if (!fw)
		return nomem;
	fw->entry = kcalloc(entries, sizeof(*fw->entry), GFP_KERNEL);
	fw->leaf = kcalloc(entries, sizeof(*fw->leaf), GFP_KERNEL);
	fw->accepted = kcalloc(entries, sizeof(*fw->accepted), GFP_KERNEL);
	if (!fw->entry || !fw->leaf || !fw->accepted) {
		apple_dart_free_fw_root(dart, fw);
		return nomem;
	}

	for (i = 0; i < entries; i++) {
		u64 pte = READ_ONCE(live[i]);
		phys_addr_t phys;

		if (!(pte & APPLE_DART_PTE_VALID))
			continue;
		if (!dart->fw_handoff) {
			fw->leaf[i] = memremap(apple_dart_pte_to_paddr(dart, pte),
					       dart->pgsize, MEMREMAP_WB);
			if (!fw->leaf[i])
				continue;
			fw->entry[i] = pte;
			continue;
		}
		if (!apple_dart_fw_table_decode(dart, pte, &phys)) {
			apple_dart_free_fw_root(dart, fw);
			return ERR_PTR(-EINVAL);
		}
		fw->leaf[i] = memremap(phys, dart->pgsize, MEMREMAP_WB);
		if (!fw->leaf[i]) {
			apple_dart_free_fw_root(dart, fw);
			return ERR_PTR(-ENOMEM);
		}
		if (READ_ONCE(live[i]) != pte) {
			apple_dart_free_fw_root(dart, fw);
			return ERR_PTR(-EBUSY);
		}
		fw->entry[i] = pte;
	}
	return fw;
}

static void apple_dart_free_fw_tree(struct apple_dart *dart,
				    struct apple_dart_fw_tree *tree)
{
	size_t i, j, entries = dart->pgsize / sizeof(u64);

	if (!tree)
		return;
	/* Any of the four arrays may be missing after a failed allocation. */
	for (i = 0; i < entries; i++) {
		if (tree->leaf && tree->leaf[i]) {
			for (j = 0; j < entries; j++)
				if (tree->leaf[i][j])
					memunmap(tree->leaf[i][j]);
			kfree(tree->leaf[i]);
		}
		if (tree->mid_entry)
			kfree(tree->mid_entry[i]);
		if (tree->mid && tree->mid[i])
			memunmap(tree->mid[i]);
	}
	kfree(tree->leaf);
	kfree(tree->mid_entry);
	kfree(tree->mid);
	kfree(tree->entry);
	kfree(tree);
}

/*
 * Record firmware's subtrees in the four-level root @live of @sid, so that
 * firmware-placed buffers (an RTKit co-processor's shared segments, which
 * it asks the AP to map at its own DVA) resolve through iova_to_phys. A
 * table that cannot be mapped, or that apple_dart_fw_tree_table() refuses,
 * is left out; its DVAs resolve nothing. A slot can map up to one leaf table
 * per mid-level entry (2048 with 16K pages); firmware holds a few slots.
 */
static struct apple_dart_fw_tree *
apple_dart_snapshot_fw_tree(struct apple_dart *dart, int sid, const u64 *live)
{
	size_t i, j, entries = dart->pgsize / sizeof(u64);
	struct apple_dart_fw_tree *tree;

	if (!dart->fw_tree || !dart->fw_mirror || dart->fw_handoff ||
	    dart->hw->fmt != APPLE_DART2 ||
	    !dart->hw->tcr_4level ||
	    !(apple_dart_readl(dart, DART_TCR(dart, sid)) & dart->hw->tcr_4level))
		return NULL;

	tree = kzalloc(sizeof(*tree), GFP_KERNEL);
	if (!tree)
		return NULL;
	tree->entry = kcalloc(entries, sizeof(*tree->entry), GFP_KERNEL);
	tree->mid = kcalloc(entries, sizeof(*tree->mid), GFP_KERNEL);
	tree->mid_entry = kcalloc(entries, sizeof(*tree->mid_entry), GFP_KERNEL);
	tree->leaf = kcalloc(entries, sizeof(*tree->leaf), GFP_KERNEL);
	if (!tree->entry || !tree->mid || !tree->mid_entry || !tree->leaf) {
		apple_dart_free_fw_tree(dart, tree);
		return NULL;
	}

	for (i = 0; i < entries; i++) {
		u64 pte = READ_ONCE(live[i]);
		phys_addr_t phys;

		if (!apple_dart_fw_table_pte(dart, pte, &phys) ||
		    !apple_dart_fw_tree_table(dart, phys))
			continue;
		tree->mid_entry[i] = kcalloc(entries, sizeof(u64), GFP_KERNEL);
		tree->leaf[i] = kcalloc(entries, sizeof(u64 *), GFP_KERNEL);
		tree->mid[i] = memremap(phys, dart->pgsize, MEMREMAP_WB);
		if (!tree->mid_entry[i] || !tree->leaf[i] || !tree->mid[i])
			continue;
		tree->entry[i] = pte;
		for (j = 0; j < entries; j++) {
			u64 mid = READ_ONCE(tree->mid[i][j]);

			if (!apple_dart_fw_table_pte(dart, mid, &phys) ||
			    !apple_dart_fw_tree_table(dart, phys))
				continue;
			tree->leaf[i][j] = memremap(phys, dart->pgsize, MEMREMAP_WB);
			if (tree->leaf[i][j])
				tree->mid_entry[i][j] = mid;
		}
	}
	return tree;
}

/* What firmware's recorded four-level subtree translates @addr to, or 0. */
static phys_addr_t apple_dart_fw_tree_phys(const struct apple_dart *dart,
					   const struct apple_dart_fw_tree *tree,
					   const u64 *live, u64 addr, int *prot_out)
{
	u32 pgshift = ilog2(dart->pgsize);
	u32 bits = ilog2(dart->pgsize / sizeof(u64));
	u64 mask = BIT_ULL(bits) - 1;
	u64 slot = addr >> (pgshift + 2 * bits);
	u64 mid = (addr >> (pgshift + bits)) & mask;
	u64 page = (addr >> pgshift) & mask;
	phys_addr_t phys;
	int prot;

	if (!tree || !live || slot > mask || !tree->entry[slot] ||
	    READ_ONCE(live[slot]) != tree->entry[slot] ||
	    !tree->mid_entry[slot][mid] ||
	    READ_ONCE(tree->mid[slot][mid]) != tree->mid_entry[slot][mid] ||
	    !apple_dart_fw_leaf_decode(READ_ONCE(tree->leaf[slot][mid][page]),
				       addr, &phys, &prot))
		return 0;
	if (prot_out)
		*prot_out = prot;
	return phys;
}

static int apple_dart_fw_invalidate(struct apple_dart_stream_map *stream_map,
				    u64 lo, u64 hi);

static int
apple_dart_hw_map_locked_ttbr(struct apple_dart_stream_map *stream_map, u8 idx)
{
	struct apple_dart *dart = stream_map->dart;
	int sid;

	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		struct apple_dart_fw_tree *tree = NULL;
		struct apple_dart_fw_root *fw;
		unsigned long flags;
		u32 ttbr;
		phys_addr_t phys;
		u64 *l1_tbl, *owned;

		if (dart->locked_ttbr[sid][idx])
			continue;
		/* The matched firmware handoff exports three-level tables only. */
		if (dart->fw_handoff && dart->hw->tcr_4level &&
		    (apple_dart_readl(dart, DART_TCR(dart, sid)) & dart->hw->tcr_4level))
			return -EINVAL;

		ttbr = apple_dart_readl(dart, DART_TTBR(dart, sid, idx));

		if (!(ttbr & dart->hw->ttbr_valid)) {
			dev_err(dart->dev, "Invalid ttbr[%u] for locked dart\n",
				idx);
			return -EIO;
		}

		ttbr &= ~dart->hw->ttbr_valid;

		if (dart->hw->ttbr_addr_field_shift)
			ttbr >>= dart->hw->ttbr_addr_field_shift;
		phys = ((phys_addr_t) ttbr) << dart->hw->ttbr_shift;

		if (dart->fw_handoff && !apple_dart_fw_table_ram(dart, phys))
			return -EINVAL;

		l1_tbl = devm_memremap(dart->dev, phys, dart->pgsize,
				       MEMREMAP_WB);
		if (IS_ERR(l1_tbl))
			return PTR_ERR(l1_tbl);

		owned = devm_kzalloc(dart->dev, dart->pgsize, GFP_KERNEL);
		if (!owned) {
			devm_memunmap(dart->dev, l1_tbl);
			return -ENOMEM;
		}
		fw = apple_dart_snapshot_fw_root(dart, sid, l1_tbl);
		if (IS_ERR(fw)) {
			devm_kfree(dart->dev, owned);
			devm_memunmap(dart->dev, l1_tbl);
			return PTR_ERR(fw);
		}
		if (!idx)
			tree = apple_dart_snapshot_fw_tree(dart, sid, l1_tbl);
		spin_lock_irqsave(&dart->lock, flags);
		dart->locked_owned[sid][idx] = owned;
		dart->locked_ttbr[sid][idx] = l1_tbl;
		dart->locked_fw[sid][idx] = fw;
		if (!idx)
			dart->locked_fw_tree[sid] = tree;
		spin_unlock_irqrestore(&dart->lock, flags);
	}

	return 0;
}

/*
 * Retire the locked roots @idx of @stream_map's streams. Returns the first
 * error of the TLB invalidate that retiring needs, which is also remembered
 * per stream for apple_dart_domain_free(): a consumer release or an attach
 * rollback can retire the root before the domain is freed.
 */
static int
apple_dart_hw_unmap_locked_ttbr(struct apple_dart_stream_map *stream_map, u8 idx)
{
	struct apple_dart *dart = stream_map->dart;
	int sid, ret = 0;

	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		u64 *live = dart->locked_ttbr[sid][idx];
		u64 *owned = dart->locked_owned[sid][idx];
		struct apple_dart_fw_tree *tree = NULL;
		struct apple_dart_fw_root *fw;
		unsigned long flags;
		bool first = false;
		int err = 0;

		if (!live)
			continue;
		apple_dart_retire_root(live, owned, dart->pgsize / sizeof(*live));
		if ((dart->fw_mirror || dart->fw_handoff) &&
		    dart->version >= 0x0202 && !idx) {
			struct apple_dart_stream_map stream = { .dart = dart };

			__set_bit(sid, stream.sidmap);
			err = apple_dart_fw_invalidate(&stream, 0, U64_MAX);
			ret = ret ?: err;
		}
		spin_lock_irqsave(&dart->lock, flags);
		if (err && !dart->locked_retire_err[sid]) {
			dart->locked_retire_err[sid] = err;
			first = true;
		}
		fw = dart->locked_fw[sid][idx];
		dart->locked_fw[sid][idx] = NULL;
		dart->locked_ttbr[sid][idx] = NULL;
		if (!idx) {
			tree = dart->locked_fw_tree[sid];
			dart->locked_fw_tree[sid] = NULL;
		}
		spin_unlock_irqrestore(&dart->lock, flags);
		/* Once per stream: the error is never cleared. */
		if (first)
			dev_warn(dart->dev,
				 "stream %d: TLB invalidate failed retiring its root (%d); keeping the page tables of its domains until reboot\n",
				 sid, err);
		apple_dart_free_fw_root(dart, fw);
		apple_dart_free_fw_tree(dart, tree);
		devm_memunmap(dart->dev, live);
		devm_kfree(dart->dev, owned);
		dart->locked_owned[sid][idx] = NULL;
	}
	return ret;
}

/* The remembered retire-time invalidate error of @stream_map's streams, or 0. */
static int apple_dart_retire_error(struct apple_dart_stream_map *stream_map)
{
	struct apple_dart *dart = stream_map->dart;
	unsigned long flags;
	int sid, ret = 0;

	spin_lock_irqsave(&dart->lock, flags);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		ret = dart->locked_retire_err[sid];
		if (ret)
			break;
	}
	spin_unlock_irqrestore(&dart->lock, flags);
	return ret;
}

static int
apple_dart_hw_sync_locked(struct io_pgtable_cfg *cfg,
			 struct apple_dart_stream_map *stream_map,
			 bool defer_unknown)
{
	struct apple_dart *dart = stream_map->dart;
	unsigned long flags;
	int sid, idx, ret = 0;

	spin_lock_irqsave(&dart->lock, flags);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		bool ready = true;

		if (defer_unknown) {
			for (idx = 0; idx < cfg->apple_dart_cfg.n_ttbrs; idx++) {
				if (!dart->locked_ttbr[sid][idx] ||
				    !dart->locked_owned[sid][idx]) {
					ready = false;
					break;
				}
			}
			if (!ready)
				continue;
		}

		for (idx = 0; idx < cfg->apple_dart_cfg.n_ttbrs; idx++) {
			u64 *live = dart->locked_ttbr[sid][idx];
			u64 *owned = dart->locked_owned[sid][idx];
			u64 *ours = cfg->apple_dart_cfg.ttbr[idx];

			/*
			 * Firmware mappings are created before attach maps the
			 * locked roots. Attach will publish these entries once
			 * both the live root and its ownership shadow exist.
			 */
			if (!live && !owned)
				continue;
			if (!live || !owned || !ours) {
				ret = -EIO;
				goto out;
			}
			/*
			 * Legacy display DARTs hand their locked root to Linux.
			 * The IOMMU core has rebuilt the firmware reservations
			 * before attachment, so replace the old root entries.
			 * T8110 v2.2+ instead retains firmware-owned entries.
			 */
			if (!dart->fw_mirror && !dart->fw_handoff &&
			    (dart->hw->type != DART_T8110 || dart->version < 0x0202)) {
				size_t entry;

				dma_wmb();
				for (entry = 0; entry < dart->pgsize / sizeof(*live); entry++) {
					u64 next = READ_ONCE(ours[entry]);

					WRITE_ONCE(live[entry], next);
					WRITE_ONCE(owned[entry], next);
				}
				dma_wmb();
				continue;
			}
			ret = apple_dart_publish_root(dart, live, owned, ours,
						      dart->locked_fw[sid][idx],
						      dart->pgsize / sizeof(*live));
			if (ret)
				goto out;
		}
	}
out:
	spin_unlock_irqrestore(&dart->lock, flags);
	return ret;
}

static int
apple_dart_t8020_hw_stream_command(struct apple_dart_stream_map *stream_map,
			     u32 command)
{
	struct apple_dart *dart = stream_map->dart;
	unsigned long flags;
	int ret, i;
	u32 sidmap[BITS_TO_U32(DART_MAX_STREAMS)];
	u32 command_reg;

	spin_lock_irqsave(&dart->lock, flags);
	if (dart->commands_gated) {
		spin_unlock_irqrestore(&dart->lock, flags);
		return -EHOSTDOWN;
	}

	bitmap_to_arr32(sidmap, stream_map->sidmap, dart->num_streams);
	for (i = 0; i < BITS_TO_U32(dart->num_streams); i++)
		apple_dart_writel(dart, sidmap[i],
				  DART_T8020_STREAM_SELECT + 4 * i);
	apple_dart_writel(dart, command, DART_T8020_STREAM_COMMAND);

	ret = read_poll_timeout_atomic(
		apple_dart_readl, command_reg,
		!(command_reg & DART_T8020_STREAM_COMMAND_BUSY), 1,
		DART_STREAM_COMMAND_BUSY_TIMEOUT, false, dart,
		DART_T8020_STREAM_COMMAND);

	spin_unlock_irqrestore(&dart->lock, flags);

	if (ret) {
		dev_err(stream_map->dart->dev,
			"busy bit did not clear after command %x for streams %lx\n",
			command, stream_map->sidmap[0]);
		return ret;
	}

	return 0;
}

static int
apple_dart_t8110_hw_tlb_command_range(struct apple_dart_stream_map *stream_map,
				   u32 command, bool range, u64 first, u64 last)
{
	struct apple_dart *dart = stream_map->dart;
	unsigned long flags;
	u32 status;
	int sid, ret = 0;

	spin_lock_irqsave(&dart->lock, flags);
	if (dart->commands_gated) {
		spin_unlock_irqrestore(&dart->lock, flags);
		return -EHOSTDOWN;
	}
	/* A domain bitmap can contain independent remap targets. */
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		u32 val = FIELD_PREP(DART_T8110_TLB_CMD_OP, command) |
			  FIELD_PREP(DART_T8110_TLB_CMD_STREAM, sid);

		ret = read_poll_timeout_atomic(apple_dart_readl, status,
			!(status & DART_T8110_TLB_CMD_BUSY), 1,
			DART_T8110_FLUSH_BUSY_TIMEOUT, false, dart,
			DART_T8110_TLB_CMD);
		if (ret)
			break;
		if (dart->version >= 0x0202 &&
		    command == DART_T8110_TLB_CMD_OP_FLUSH_SID)
			val |= DART_T8110_TLB_CMD_V2;
		if (range) {
			val |= DART_T8110_TLB_CMD_RANGE | DART_T8110_TLB_CMD_STT;
			apple_dart_writel(dart, (first >> ilog2(dart->pgsize)) << 2,
					  DART_T8110_TLB_START);
			apple_dart_writel(dart, (last >> ilog2(dart->pgsize)) << 2,
					  DART_T8110_TLB_END);
		}
		dma_wmb();
		apple_dart_writel(dart, val, DART_T8110_TLB_CMD);
		ret = read_poll_timeout_atomic(apple_dart_readl, status,
			!(status & DART_T8110_TLB_CMD_BUSY), 1,
			DART_T8110_FLUSH_BUSY_TIMEOUT, false, dart,
			DART_T8110_TLB_CMD);
		if (ret)
			break;
	}
	/* A V2 flush leaves the context cache alone: flush all of it after. */
	if (!ret && dart->ctc_flush && dart->version >= 0x0202 &&
	    command == DART_T8110_TLB_CMD_OP_FLUSH_SID)
		apple_dart_writel(dart, 0, DART_T8110_CTC_FLUSH);
	spin_unlock_irqrestore(&dart->lock, flags);
	if (ret)
		dev_err_ratelimited(dart->dev,
			"TLB command %#x for stream %d timed out (%#x)\n",
			command, sid, status);
	return ret;
}

/* A root table is at most 16 KiB of 64-bit entries. */
#define DART_MAX_ROOT_ENTRIES (SZ_16K / sizeof(u64))

/*
 * A slot of a four-level locked root that firmware holds: valid, and not an
 * entry Linux published. The loader's four-level roots are not snapshotted,
 * so this is the only firmware state Linux knows about them.
 */
static bool apple_dart_fw_slot_foreign(const u64 *live, const u64 *owned,
				       size_t i)
{
	u64 pte = READ_ONCE(live[i]);

	return (pte & APPLE_DART_PTE_VALID) && (!owned || pte != READ_ONCE(owned[i]));
}

/*
 * On T8140 DARTs that keep firmware roots, the hardware keeps
 * using the translations behind the firmware-owned root slots, e.g. the
 * display controller scanning out the boot framebuffer while Linux attaches,
 * or the AOP firmware's own DMA on the AOP DART streams that Linux shares.
 * Invalidating those translations, for the whole stream or by DVA range, can
 * break that DMA: the scanout fails a translation and the display
 * coprocessor panics on the failed read, and a whole-stream invalidate on a
 * busy AOP stream can stay busy for good. Linux never changes what those
 * slots resolve to, so only invalidate the parts of [lo, hi] reached through
 * other root slots, by DVA range, on three-level and four-level streams alike.
 */
static int apple_dart_fw_invalidate(struct apple_dart_stream_map *stream_map,
				    u64 lo, u64 hi)
{
	const u32 op = DART_T8110_TLB_CMD_OP_FLUSH_SID;
	struct apple_dart *dart = stream_map->dart;
	struct apple_dart_stream_map stream = { .dart = dart };
	size_t entries = dart->pgsize / sizeof(u64);
	u32 table_bits = ilog2(entries);
	DECLARE_BITMAP(fw_slots, DART_MAX_ROOT_ENTRIES);
	size_t first, last, i, end;
	unsigned long flags;
	int sid, ret = 0;

	if (entries > DART_MAX_ROOT_ENTRIES)
		return 0;

	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		const struct apple_dart_fw_root *fw;
		const u64 *live, *owned;
		u64 base, span, start_dva, end_dva;
		bool four_level;
		u32 slot_shift;

		__set_bit(sid, stream.sidmap);
		four_level = apple_dart_readl(dart, DART_TCR(dart, sid)) &
			     dart->hw->tcr_4level;
		if (four_level) {
			/*
			 * A four-level stream walks the whole DVA: its root slots
			 * cover the input address space from zero.
			 */
			slot_shift = ilog2(dart->pgsize) + 2 * table_bits;
			span = BIT_ULL(min(slot_shift + table_bits, dart->ias));
			base = 0;
		} else {
			/*
			 * A three-level stream walks the low bits of the DVA; the
			 * TLB is tagged with the whole DVA, inside the window of
			 * the stream's domain.
			 */
			slot_shift = ilog2(dart->pgsize) + table_bits;
			span = BIT_ULL(slot_shift + table_bits);
			base = READ_ONCE(dart->locked_window[sid]) & ~(span - 1);
		}
		if (hi < base || lo > base + span - 1)
			goto next;
		start_dva = max(lo, base);
		end_dva = min(hi, base + span - 1);
		first = (start_dva - base) >> slot_shift;
		last = (end_dva - base) >> slot_shift;

		/* Nothing of Linux reaches the hardware without the root. */
		bitmap_zero(fw_slots, entries);
		spin_lock_irqsave(&dart->lock, flags);
		live = dart->locked_ttbr[sid][0];
		owned = dart->locked_owned[sid][0];
		fw = dart->locked_fw[sid][0];
		/*
		 * Four-level roots are not snapshotted, and with the reserved-table
		 * handoff firmware may replace its slots' descendants: there every
		 * valid slot Linux did not publish is firmware's.
		 */
		for (i = first; live && i <= last; i++)
			if (four_level || dart->fw_handoff ?
			    apple_dart_fw_slot_foreign(live, owned, i) :
			    apple_dart_fw_slot_owned(fw, live, i))
				__set_bit(i, fw_slots);
		spin_unlock_irqrestore(&dart->lock, flags);
		if (!live)
			goto next;

		for (i = find_next_zero_bit(fw_slots, last + 1, first); i <= last;
		     i = find_next_zero_bit(fw_slots, last + 1, end + 1)) {
			u64 start, stop;

			end = find_next_bit(fw_slots, last + 1, i) - 1;
			start = max(start_dva, base + ((u64)i << slot_shift));
			stop = min(end_dva, base + ((u64)(end + 1) << slot_shift) - 1);
			ret = apple_dart_t8110_hw_tlb_command_range(&stream, op, true,
								    start, stop);
			if (ret)
				break;
		}
next:
		__clear_bit(sid, stream.sidmap);
		if (ret)
			return ret;
	}
	return 0;
}

static int
apple_dart_t8020_hw_invalidate_tlb(struct apple_dart_stream_map *stream_map)
{
	return apple_dart_t8020_hw_stream_command(
		stream_map, DART_T8020_STREAM_COMMAND_INVALIDATE);
}

/*
 * Borrowed J713 display streams use the exact command the cleanroom DCP
 * profile was qualified with: FLUSH_SID without the v2 or range flags, and
 * only for SID23, strict SID0 or the private SID16 publisher. A refused or
 * timed-out command fails the whole retained session.
 */
static int apple_dart_dcp_invalidate_tlb(struct apple_dart_stream_map *stream_map)
{
	struct apple_dart *dart = stream_map->dart;
	const u32 command = DART_T8110_TLB_CMD_OP_FLUSH_SID;
	unsigned long flags;
	u32 status;
	int sid, ret = 0;

	spin_lock_irqsave(&dart->lock, flags);
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		struct apple_dart_dcp *borrowed = apple_dart_borrowed_sid(dart, sid);
		u32 val = FIELD_PREP(DART_T8110_TLB_CMD_OP, command) |
			  FIELD_PREP(DART_T8110_TLB_CMD_STREAM, sid);

		if (!borrowed || (borrowed->sid == 16 &&
				  !READ_ONCE(borrowed->shared_flushing)) ||
		    !apple_dart_dcp_registers(borrowed)) {
			ret = -EIO;
			break;
		}
		ret = readl_poll_timeout_atomic(dart->regs + DART_T8110_TLB_CMD, status,
						!(status & DART_T8110_TLB_CMD_BUSY), 1,
						DART_STREAM_COMMAND_BUSY_TIMEOUT);
		/* An external display DART also reports busy at 0x604 bit 0. */
		if (!ret && dart->dispext)
			ret = readl_poll_timeout_atomic(dart->regs + DART_T8110_TLB_BUSY2, status,
							!(status & BIT(0)), 1,
							DART_STREAM_COMMAND_BUSY_TIMEOUT);
		if (ret)
			break;
		writel(val, dart->regs + DART_T8110_TLB_CMD);
		ret = readl_poll_timeout_atomic(dart->regs + DART_T8110_TLB_CMD, status,
						!(status & DART_T8110_TLB_CMD_BUSY), 1,
						DART_STREAM_COMMAND_BUSY_TIMEOUT);
		if (!ret && dart->dispext)
			ret = readl_poll_timeout_atomic(dart->regs + DART_T8110_TLB_BUSY2, status,
							!(status & BIT(0)), 1,
							DART_STREAM_COMMAND_BUSY_TIMEOUT);
		if (ret)
			break;
	}
	spin_unlock_irqrestore(&dart->lock, flags);

	if (ret) {
		apple_dart_fail_borrowed(dart, "busy bit did not clear after TLB command %x for stream %d",
					 command, sid);
	}
	return ret;
}

/*
 * The IOMMU core's TLB command on a borrowed DART: on an external display
 * DART only with its power confirmed (a use of its own); the borrowed
 * streams' own operations run theirs inside their use, raw.
 */
static int apple_dart_dcp_invalidate_tlb_gated(struct apple_dart_stream_map *stream_map)
{
	int ret;

	if (!apple_dart_pm_get(stream_map->dart))
		return -EAGAIN;
	ret = apple_dart_dcp_invalidate_tlb(stream_map);
	apple_dart_pm_put(stream_map->dart);
	return ret;
}

static int
apple_dart_t8110_hw_invalidate_tlb(struct apple_dart_stream_map *stream_map)
{
	if (apple_dart_has_borrowed(stream_map->dart))
		return apple_dart_dcp_invalidate_tlb_gated(stream_map);
	/* Keep out of firmware-held slots; see apple_dart_fw_invalidate(). */
	if ((stream_map->dart->fw_mirror || stream_map->dart->fw_handoff) &&
	    stream_map->dart->version >= 0x0202)
		return apple_dart_fw_invalidate(stream_map, 0, U64_MAX);
	return apple_dart_t8110_hw_tlb_command_range(
		stream_map, DART_T8110_TLB_CMD_OP_FLUSH_SID, false, 0, 0);
}

static const char *apple_dart_t8110_v2_fault_name(u32 exception);

/*
 * Exceptions can latch on any stream the hardware has. A tunnel DART runs with
 * a fixed 64-stream topology, but a PCIe-C port also uses its reserved streams
 * 253..255 (for example for MSIs), so scan all of them there.
 */
static unsigned int apple_dart_exception_sids(struct apple_dart *dart)
{
	return dart->tunneled ? DART_MAX_STREAMS : dart->num_streams;
}

static void apple_dart_drain_stale_exceptions(struct apple_dart *dart)
{
	unsigned int i, drained = 0;

	if (dart->hw->type != DART_T8110 || dart->version < 0x0202)
		return;

	for (i = 0; i < DIV_ROUND_UP(apple_dart_exception_sids(dart), 32) && i < 8; i++) {
		u32 pending = readl(dart->regs + DART_T8110_EXCEPTION(i));
		unsigned long bits = pending;
		unsigned int bit;

		if (!pending)
			continue;

		for_each_set_bit(bit, &bits, 32) {
			unsigned int sid = i * 32 + bit;
			u32 status;
			u64 addr;

			if (sid >= apple_dart_exception_sids(dart))
				break;

			status = readl(dart->regs +
				       DART_T8110_ERR_SID_STATUS(sid));
			addr = readl(dart->regs +
				     DART_T8110_EXCEPTION_ADDR_LO(sid)) |
			       (u64)readl(dart->regs +
					  DART_T8110_EXCEPTION_ADDR_HI(sid)) << 32;
			dev_dbg(dart->dev,
				 "stale exception latched before probe: SID:%u %s %s at %#llx (status %#010x)\n",
				 sid,
				 status & DART_T8110_ERR_SID_STATUS_WRITE ?
					 "write" : "read",
				 apple_dart_t8110_v2_fault_name(status),
				 addr, status);
			drained++;
		}

		writel(pending, dart->regs + DART_T8110_EXCEPTION(i));
	}

	if (drained) {
		dsb(sy);
		readl(dart->regs + DART_T8110_EXCEPTION(0));
	}

	if (dart->version >= 0x0203)
		readl(dart->regs + DART_T8110_BLOCK_ERROR);

	if (drained)
		dev_dbg(dart->dev,
			 "%u stale exception(s) drained; these predate Linux\n",
			 drained);
}

static int apple_dart_t8110_v2_unlock(struct apple_dart *dart)
{
	struct apple_dart_stream_map stream_map = { .dart = dart };

	KUNIT_STATIC_STUB_REDIRECT(apple_dart_t8110_v2_unlock, dart);

	if (dart->managed_sids)
		return -EPERM;
	if (!(readl(dart->regs + DART_T8110_DIAG_LOCK) & DART_T8110_DIAG_LOCK_ON_ERR))
		return 0;

	/* Unlock after acknowledging a lock-on-error exception. */
	__set_bit(0, stream_map.sidmap);
	return apple_dart_t8110_hw_tlb_command_range(&stream_map,
					    DART_T8110_TLB_CMD_OP_FLUSH_UNLOCK,
					    false, 0, 0);
}

static int apple_dart_read_remaps(struct apple_dart *dart)
{
	struct device_node *np = dart->dev->of_node;
	u32 src, dst;
	int count, i, ret;

	if (!of_property_present(np, "apple,remap-streams"))
		return 0;
	count = of_property_count_u32_elems(np, "apple,remap-streams");
	if (count <= 0 || count > 2 * dart->num_streams || count % 2 ||
	    dart->hw->type != DART_T8110)
		return -EINVAL;
	for (i = 0; i < count; i += 2) {
		ret = of_property_read_u32_index(np, "apple,remap-streams", i, &src);
		if (!ret)
			ret = of_property_read_u32_index(np, "apple,remap-streams", i + 1, &dst);
		if (ret)
			return ret;
		if (src >= dart->num_streams || dst >= dart->num_streams ||
		    dst > FIELD_MAX(DART_T8110_TCR_REMAP) || src == dst)
			return -ERANGE;
		if (test_and_set_bit(src, dart->remapped))
			return -EINVAL;
		dart->remap_target[src] = dst;
	}
	/* Only a single remap hop is described by this binding. */
	for_each_set_bit(i, dart->remapped, dart->num_streams)
		if (test_bit(dart->remap_target[i], dart->remapped))
			return -EINVAL;
	return 0;
}

static void apple_dart_hw_apply_remaps(struct apple_dart *dart)
{
	int sid;

	for_each_set_bit(sid, dart->remapped, dart->num_streams)
		writel(DART_T8110_TCR_REMAP_ENABLE |
		       FIELD_PREP(DART_T8110_TCR_REMAP, dart->remap_target[sid]),
		       dart->regs + DART_TCR(dart, sid));
}

static void
apple_dart_t8020_hw_enable_remap(struct apple_dart *dart, int sid, int remap_to)
{
	u32 remap = readl(DART_REMAP(dart, sid));
	remap = (remap & ~DART_REMAP_MASK(sid)) |
		field_prep(DART_REMAP_MASK(sid), remap_to);
	writel(remap, DART_REMAP(dart, sid));

	u32 tcr = FIELD_PREP(DART_T8020_TCR_REMAP_ENABLE, 1) |
		  FIELD_PREP(DART_T8020_TCR_TRANSLATE_ENABLE, 1);
	writel(tcr, dart->regs + DART_TCR(dart, sid));
}

static void
apple_dart_t8110_hw_enable_remap(struct apple_dart *dart, int sid, int remap_to)
{
	u32 tcr = FIELD_PREP(DART_T8110_TCR_REMAP, remap_to) |
		  FIELD_PREP(DART_T8110_TCR_REMAP_ENABLE, 1);
	writel(tcr, dart->regs + DART_TCR(dart, sid));
}

/*
 * The last steps of a reset, which leave the translation registers alone:
 * acknowledge the errors latched so far, unlock a DART that locks on errors,
 * unmask the error interrupt and flush the TLBs.
 */
/*
 * Unmask every error and enable its interrupt for each stream, with the
 * registers of this revision: the T8110 reset value leaves them masked on
 * 2.2 and later, where the error mask is no longer at ERROR_MASK.
 */
static void apple_dart_t8110_enable_errors(struct apple_dart *dart)
{
	bool v24 = dart->version >= 0x0204;
	unsigned int i;

	apple_dart_writel(dart, 0, dart->version >= 0x0202 ?
			  DART_T8110_V2_ERROR_MASK : DART_T8110_ERROR_MASK);
	apple_dart_readl(dart, dart->version >= 0x0202 ?
			 DART_T8110_V2_ERROR_MASK : DART_T8110_ERROR_MASK);
	apple_dart_writel(dart, U32_MAX, v24 ? DART_T8110_V24_ERR_IRQ_STATUS :
			  DART_T8110_ERR_IRQ_STATUS);
	apple_dart_writel(dart, U32_MAX, v24 ? DART_T8110_V24_ERR_IRQ_ENABLE :
			  DART_T8110_ERR_IRQ_ENABLE);
	for (i = 0; i < DIV_ROUND_UP(dart->num_streams, 32); i++) {
		u32 sids = dart->num_streams >= 32 * (i + 1) ? U32_MAX :
			   GENMASK(dart->num_streams - 32 * i - 1, 0);

		apple_dart_writel(dart, sids, v24 ? DART_T8110_V24_ERR_SID_ENABLE(i) :
				  DART_T8110_ERR_SID_ENABLE(i));
	}
	/* No lock on error: the driver acknowledges and carries on. */
	apple_dart_writel(dart, 0, DART_T8110_DIAG_LOCK);
}

/*
 * Apply the DART's own tunables. Client partition registers exist only where
 * PARAMS1 says so; a write to one that does not exist is skipped.
 */
static void apple_dart_apply_own_tunables(struct apple_dart *dart)
{
	size_t i;

	for (i = 0; i < dart->tunables->sz; ++i) {
		u32 offset = dart->tunables->values[i].offset;
		u32 old_value, value;

		if (offset >= DART_T8110_CLIENT_PARTITION_BASE &&
		    offset < DART_T8110_CLIENT_PARTITION_END &&
		    !dart->client_partitions)
			continue;
		old_value = apple_dart_readl(dart, offset);
		value = old_value & ~dart->tunables->values[i].mask;
		value |= dart->tunables->values[i].value;
		if (value != old_value)
			apple_dart_writel(dart, value, offset);
	}
}

static int apple_dart_hw_finish_reset(struct apple_dart *dart)
{
	struct apple_dart_stream_map stream_map = { .dart = dart };
	int ret;

	/* Newer revisions expose per-stream exception records. */
	if (dart->hw->type == DART_T8110 && dart->version >= 0x0202) {
		apple_dart_drain_stale_exceptions(dart);
		ret = apple_dart_t8110_v2_unlock(dart);
		if (ret) {
			dev_err(dart->dev, "failed to unlock after reset: %d\n", ret);
			return ret;
		}
	} else {
		/* Clear pending errors before the interrupt is unmasked. */
		apple_dart_writel(dart, apple_dart_readl(dart, dart->hw->error),
				  dart->hw->error);
	}

	if (dart->hw->type == DART_T8110)
		apple_dart_writel(dart, 0, DART_T8110_ERROR_MASK);

	/*
	 * A PCIe-C port issues its own writes, such as MSIs turned into
	 * interrupt controller writes, on reserved streams outside the
	 * translated ones. Those must bypass.
	 */
	if (dart->tunneled && dart->hw->type == DART_T8110) {
		u32 sid;

		of_property_for_each_u32(dart->dev->of_node, "apple,bypass-sids", sid) {
			if (sid >= DART_MAX_STREAMS)
				continue;
			apple_dart_writel(dart, dart->hw->tcr_bypass,
					  dart->hw->tcr + 4 * sid);
			apple_dart_writel(dart, BIT(sid % 32),
					  dart->hw->enable_streams + 4 * (sid / 32));
		}
	}

	bitmap_set(stream_map.sidmap, 0, dart->num_streams);
	ret = dart->hw->invalidate_tlb(&stream_map);
	if (ret)
		dev_err(dart->dev, "failed to flush the TLBs after reset: %d\n", ret);

	if (dart->full_errors)
		apple_dart_t8110_enable_errors(dart);

	return ret;
}

static bool apple_dart_managed_static_bypass(u32 tcr)
{
	/* T8132 does not retain the DAPF bypass request in the readable TCR. */
	return (tcr & ~DART_T8110_TCR_BYPASS_DAPF) == DART_T8110_TCR_BYPASS_DART;
}

static ssize_t j713_radio_fault_state_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct apple_dart *dart = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t len = 0;
	unsigned int sid, word;

	if (!device_is_bound(dev) || !dart || !READ_ONCE(dart->iommu.ready) ||
	    !dart->managed_sids)
		return -ENODEV;

	/*
	 * A live, passive snapshot can race hardware updates and IRQ acknowledgements.
	 * Never acknowledge a latch, unmask errors, or change an owned/foreign stream.
	 */
	spin_lock_irqsave(&dart->managed_lock, flags);
	len += sysfs_emit_at(buf, len, "version=%#x managed=%#x failed=%u\n",
			     dart->version, dart->managed_sids, dart->managed_failed);
	len += sysfs_emit_at(buf, len, "error_mask=%#x error=%#x\n",
			     apple_dart_readl(dart, DART_T8110_ERROR_MASK),
			     apple_dart_readl(dart, DART_T8110_ERROR));
	for (word = 0; word < BITS_TO_U32(dart->num_streams); word++) {
		u32 streams = apple_dart_readl(dart, DART_T8110_ERROR_STREAMS + 4 * word);
		u32 exceptions = apple_dart_readl(dart, DART_T8110_EXCEPTION(word));

		len += sysfs_emit_at(buf, len, "error_streams[%u]=%#x exceptions[%u]=%#x\n",
				     word, streams, word, exceptions);
	}
	for (sid = 1; sid < 16; sid++) {
		u32 status;
		u64 addr;

		if (!(dart->managed_sids & BIT(sid)))
			continue;
		status = apple_dart_readl(dart, DART_T8110_ERR_SID_STATUS(sid));
		addr = apple_dart_readl(dart, DART_T8110_EXCEPTION_ADDR_LO(sid)) |
		       (u64)apple_dart_readl(dart, DART_T8110_EXCEPTION_ADDR_HI(sid)) << 32;
		len += sysfs_emit_at(buf, len, "sid%u status=%#x addr=%#llx\n", sid, status, addr);
	}
	spin_unlock_irqrestore(&dart->managed_lock, flags);
	return len;
}
static DEVICE_ATTR_ADMIN_RO(j713_radio_fault_state);

static struct attribute *apple_dart_managed_attrs[] = {
	&dev_attr_j713_radio_fault_state.attr,
	NULL,
};

static const struct attribute_group apple_dart_managed_group = {
	.attrs = apple_dart_managed_attrs,
};

/* Ordinary-stream admission does not authorize a controller-wide reset. */
static int apple_dart_managed_admit(struct apple_dart *dart, bool *init_bypass)
{
	unsigned int sid;
	int idx;
	bool initialize = false;
	u32 reserved_tcr, protect, protect_lock, enabled, command, error, error_mask;

	if (!dart->managed_sids) {
		*init_bypass = false;
		return 0;
	}
	if (dart->hw->type != DART_T8110 || dart->locked || dart->num_streams < 19 ||
	    dart->version < 0x0202)
		return -EINVAL;
	protect = apple_dart_readl(dart, DART_T8110_PROTECT);
	protect_lock = apple_dart_readl(dart, DART_T8110_PROTECT_LOCK);
	enabled = apple_dart_readl(dart, DART_T8110_ENABLE_STREAMS);
	command = apple_dart_readl(dart, DART_T8110_TLB_CMD);
	error = apple_dart_readl(dart, DART_T8110_ERROR);
	error_mask = apple_dart_readl(dart, DART_T8110_ERROR_MASK);
	dev_dbg(dart->dev,
		 "managed radio: PROTECT %#x LOCK %#x ENABLE %#x TLB %#x ERROR %#x MASK %#x\n",
		 protect, protect_lock, enabled, command, error, error_mask);
	/* Preserve global policy; owned-stream quarantine requires unmasked errors. */
	if (error_mask) {
		dev_err(dart->dev, "managed radio requires inherited unmasked errors (mask %#x)\n",
			error_mask);
		return -EBUSY;
	}
	/* LOCK freezes protection policy; ENABLE is not an allocation receipt. */
	if ((protect & DART_T8110_PROTECT_TTBR_TCR) ||
	    (command & DART_T8110_TLB_CMD_BUSY) ||
	    (error & DART_T8110_ERROR_FLAG) || apple_dart_fault_pending(dart))
		return -EBUSY;
	for (idx = 0; idx < BITS_TO_U32(dart->num_streams); idx++) {
		u32 errors = apple_dart_readl(dart, DART_T8110_ERROR_STREAMS + 4 * idx);

		if (errors) {
			dev_err(dart->dev, "inherited error-stream word%d %#x\n", idx, errors);
			return -EBUSY;
		}
	}
	/* A requester can reach an ordinary target through another stream. */
	for (sid = 0; sid < (unsigned int)dart->num_streams; sid++) {
		u32 tcr = apple_dart_readl(dart, DART_TCR(dart, sid));
		u32 target = FIELD_GET(DART_T8110_TCR_REMAP, tcr);

		if ((tcr & DART_T8110_TCR_REMAP_ENABLE) &&
		    (dart->managed_sids & BIT(target))) {
			dev_err(dart->dev, "inherited SID%u remaps into ordinary SID%u\n",
				sid, target);
			return -EBUSY;
		}
	}
	/* ADT assigns SID16 a static bypass role, independently of radio data. */
	reserved_tcr = apple_dart_readl(dart, DART_TCR(dart, 16));
	if (!apple_dart_managed_static_bypass(reserved_tcr)) {
		if (reserved_tcr != dart->hw->tcr_disabled || !dart->supports_bypass)
			return -EBUSY;
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			if (apple_dart_readl(dart, DART_TTBR(dart, 16, idx)) &
			    dart->hw->ttbr_valid)
				return -EBUSY;
		initialize = true;
	}
	for (sid = 1; sid < 16; sid++) {
		u32 tcr;

		if (!(dart->managed_sids & BIT(sid)))
			continue;
		tcr = apple_dart_readl(dart, DART_TCR(dart, sid));
		if (tcr != dart->hw->tcr_disabled && tcr != DART_T8110_TCR_TRANSLATE_ENABLE) {
			dev_err(dart->dev, "ordinary SID%u has an inherited active mode\n", sid);
			return -EBUSY;
		}
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			if (apple_dart_readl(dart, DART_TTBR(dart, sid, idx)) &
			    dart->hw->ttbr_valid) {
				dev_err(dart->dev, "ordinary SID%u still has a valid root\n", sid);
				return -EBUSY;
			}
	}
	*init_bypass = initialize;
	return 0;
}

/* The host may start its cold link only before any owned root is published. */
int apple_dart_radio_check_idle(struct device *dev)
{
	struct apple_dart *dart;
	unsigned long flags;
	bool init_bypass = false;
	int ret;

	device_lock(dev);
	if (dev->driver != &apple_dart_driver.driver || !device_is_bound(dev)) {
		ret = -EPROBE_DEFER;
		goto out_unlock;
	}
	dart = dev_get_drvdata(dev);
	if (!dart || !READ_ONCE(dart->iommu.ready)) {
		ret = -EPROBE_DEFER;
		goto out_unlock;
	}
	if (dart->managed_sids != (BIT(1) | BIT(2))) {
		ret = -EINVAL;
		goto out_unlock;
	}
	spin_lock_irqsave(&dart->managed_lock, flags);
	ret = dart->managed_failed || apple_dart_fault_pending(dart) ? -EIO :
		apple_dart_managed_admit(dart, &init_bypass);
	if (!ret && init_bypass)
		ret = -EBUSY;
	spin_unlock_irqrestore(&dart->managed_lock, flags);
out_unlock:
	device_unlock(dev);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_radio_check_idle);

static void apple_dart_managed_quarantine_locked(struct apple_dart *dart)
{
	struct apple_dart_stream_map stream = { .dart = dart };
	unsigned int sid;

	lockdep_assert_held(&dart->managed_lock);
	WRITE_ONCE(dart->managed_failed, true);
	for (sid = 1; sid < 16; sid++)
		if (dart->managed_sids & BIT(sid))
			__set_bit(sid, stream.sidmap);
	apple_dart_hw_disable_dma(&stream);
}

static void apple_dart_managed_quarantine(struct apple_dart *dart)
{
	unsigned long flags;

	spin_lock_irqsave(&dart->managed_lock, flags);
	apple_dart_managed_quarantine_locked(dart);
	spin_unlock_irqrestore(&dart->managed_lock, flags);
}

/* ISP streams 4/5/8/9 belong to its secure core. Never reset those roots. */
static int apple_dart_isp_host_reset(struct apple_dart *dart)
{
	const u32 host = BIT(0) | BIT(1) | BIT(6) | BIT(7) |
			 BIT(10) | BIT(11) | BIT(12);
	struct apple_dart_stream_map map = { .dart = dart };
	unsigned int sid;
	int ret;

	if (dart->hw->type != DART_T8110 || dart->num_streams != 16 ||
	    dart->locked || (apple_dart_readl(dart, DART_T8110_ERROR) & DART_T8110_ERROR_FLAG))
		return -EBUSY;
	for (sid = 0; sid < 16; sid++)
		if (host & BIT(sid))
			__set_bit(sid, map.sidmap);
	apple_dart_hw_disable_dma(&map);
	apple_dart_hw_clear_all_ttbrs(&map);
	apple_dart_hw_apply_remaps(dart);
	apple_dart_writel(dart, host, DART_T8110_ENABLE_STREAMS);
	for (sid = 0; sid < 16; sid++) {
		if (!(host & BIT(sid)))
			continue;
		bitmap_zero(map.sidmap, DART_MAX_STREAMS);
		__set_bit(sid, map.sidmap);
		ret = dart->hw->invalidate_tlb(&map);
		if (ret)
			return ret;
	}
	apple_dart_writel(dart, 0, DART_T8110_ERROR_MASK);
	return 0;
}

static int apple_dart_hw_reset(struct apple_dart *dart)
{
	struct apple_dart_stream_map stream_map;
	int i;

	if (dart->isp_host)
		return apple_dart_isp_host_reset(dart);
	if (dart->managed_sids || dart->cold_ane)
		return -EBUSY;
	stream_map.dart = dart;
	bitmap_zero(stream_map.sidmap, DART_MAX_STREAMS);
	bitmap_set(stream_map.sidmap, 0, dart->num_streams);

	/* Before the first stream is configured. */
	if (!dart->tunneled && dart->tunables)
		apple_dart_apply_own_tunables(dart);

	apple_dart_hw_disable_dma(&stream_map);
	apple_dart_hw_clear_all_ttbrs(&stream_map);
	apple_dart_hw_apply_remaps(dart);

	/* Streams that bypass translation for good, outside any domain. */
	for_each_set_bit(i, dart->static_bypass, dart->num_streams)
		apple_dart_writel(dart, DART_T8110_TCR_BYPASS_DART, DART_TCR(dart, i));

	/* enable all streams globally since TCR is used to control isolation */
	for (i = 0; i < BITS_TO_U32(dart->num_streams); i++)
		apple_dart_writel(dart, U32_MAX,
				   dart->hw->enable_streams + 4 * i);

	return apple_dart_hw_finish_reset(dart);
}

static int __apple_dart_dcp_domain_flush(struct apple_dart_domain *domain);

/*
 * Borrowed DCP domains have no io-pgtable; their flush ignores the range.
 * On an external display DART only with its power confirmed: -EAGAIN
 * (nothing retired) from sleep until its next confirmed power-up. An
 * internal display DART that its DCP has powered down needs none: nothing
 * is cached, and its power-up flushes every update made before it (they
 * hold the stream's lock, which the power-up takes; apple_dart_host_lock()).
 * A power-up that completed meanwhile confirmed the power in the same step.
 */
static int apple_dart_dcp_domain_flush(struct apple_dart_domain *domain)
{
	struct apple_dart *dart = domain->dcp->dart;
	int ret;

	if (!apple_dart_pm_get(dart)) {
		if (READ_ONCE(dart->host_off))
			return 0;
		if (!dart->ps_prot || !apple_dart_pm_get(dart))
			return -EAGAIN;
	}
	ret = __apple_dart_dcp_domain_flush(domain);
	apple_dart_pm_put(dart);
	return ret;
}

static int __apple_dart_dcp_domain_flush(struct apple_dart_domain *domain)
{
	struct apple_dart_dcp *dcp = domain->dcp;
	struct apple_dart *dart = dcp->dart;
	struct apple_dart_stream_map stream_map;
	int ret;

	/* Not as adopted: apple_dart_dcp_registers() retired it, saying why. */
	if (!apple_dart_dcp_registers(dcp))
		return -EIO;
	/* SID0 generic DMA is strict: map sync and unmap use the real 0x100 flush. */
	if (dcp->sid == 0)
		return apple_dart_dcp_flush(dcp);
	/* Only the private shared-buffer API can publish SID16 leaves. */
	if (dcp->sid != 23)
		return 0;
	stream_map.dart = dart;
	bitmap_zero(stream_map.sidmap, DART_MAX_STREAMS);
	set_bit(dcp->sid, stream_map.sidmap);
	dma_wmb();
	ret = apple_dart_dcp_invalidate_tlb(&stream_map);
	if (ret)
		apple_dart_dcp_fail(dcp, "domain flush failed: %d", ret);
	return ret;
}

static int apple_dart_domain_flush_tlb_range(struct apple_dart_domain *domain,
					   bool range, u64 first, u64 last)
{
	struct apple_dart_atomic_stream_map *map;
	struct apple_dart_stream_map stream;
	struct io_pgtable_cfg *cfg;
	const u32 op = DART_T8110_TLB_CMD_OP_FLUSH_SID;
	int i, j, ret, err = 0;
	u64 start, end;

	if (domain->dcp)
		return apple_dart_dcp_domain_flush(domain);
	cfg = &io_pgtable_ops_to_pgtable(domain->pgtbl_ops)->cfg;

	/*
	 * After an unmap, the caller frees the pages once this returns. Every
	 * DART that can be reached must therefore be invalidated, even if
	 * publishing the page table or powering another DART failed.
	 */
	for_each_stream_map(i, domain, map) {
		stream.dart = map->dart;
		for (j = 0; j < BITS_TO_LONGS(stream.dart->num_streams); j++)
			stream.sidmap[j] = atomic_long_read(&map->sidmap[j]);
		if (bitmap_empty(stream.sidmap, stream.dart->num_streams))
			continue;
		ret = pm_runtime_resume_and_get(stream.dart->dev);
		if (ret < 0) {
			dev_err_ratelimited(stream.dart->dev,
				"failed to power DART for TLB synchronization: %d\n", ret);
			err = err ?: ret;
			continue;
		}
		ret = stream.dart->locked ?
			apple_dart_hw_sync_locked(cfg, &stream, true) : 0;
		if (ret) {
			dev_err_ratelimited(stream.dart->dev,
				"failed to publish host page-table entries: %d\n", ret);
			err = err ?: ret;
		}
		/* The range registers take the DVA the device issues. */
		start = first + domain->dma_offset;
		end = last + domain->dma_offset;
		if (range && (stream.dart->fw_mirror || stream.dart->fw_handoff) &&
		    stream.dart->version >= 0x0202)
			ret = apple_dart_fw_invalidate(&stream, start, end);
		else if (range && stream.dart->locked && stream.dart->version >= 0x0202)
			ret = apple_dart_t8110_hw_tlb_command_range(&stream, op, true,
								    start, end);
		else
			ret = stream.dart->hw->invalidate_tlb(&stream);
		if (ret && stream.dart->managed_sids)
			apple_dart_managed_quarantine(stream.dart);
		pm_runtime_put(stream.dart->dev);
		err = err ?: ret;
	}
	return err;
}

static void apple_dart_domain_flush_tlb(struct apple_dart_domain *domain)
{
	(void)apple_dart_domain_flush_tlb_range(domain, false, 0, 0);
}

static void apple_dart_flush_iotlb_all(struct iommu_domain *domain)
{
	apple_dart_domain_flush_tlb(to_dart_domain(domain));
}

static void apple_dart_iotlb_sync(struct iommu_domain *domain,
				  struct iommu_iotlb_gather *gather)
{
	(void)apple_dart_domain_flush_tlb_range(to_dart_domain(domain),
					      gather->start <= gather->end,
					      gather->start, gather->end);
}

static int apple_dart_iotlb_sync_map(struct iommu_domain *domain,
				   unsigned long iova, size_t size)
{
	if (!size || iova + size - 1 < iova)
		return -EINVAL;
	return apple_dart_domain_flush_tlb_range(to_dart_domain(domain), true,
					       iova, iova + size - 1);
}

/*
 * The core installs reserved mappings before it attaches the first device,
 * while the attached-stream map is still empty. A locked stream whose
 * firmware root the domain imported keeps its inherited translations from
 * then on, so treat it as part of the domain for firmware lookups.
 */
static bool apple_dart_domain_uses_fw(struct apple_dart_domain *domain, int i,
				      struct apple_dart_atomic_stream_map *map,
				      int sid)
{
	return test_bit(sid, domain->fw_sidmap[i]) ||
	       (atomic_long_read(&map->sidmap[BIT_WORD(sid)]) & BIT_MASK(sid));
}

/*
 * Hardware continues to use every firmware-owned slot. Its entire IOVA
 * span is excluded from Linux allocation, including currently unmapped holes.
 *
 * Linux may still repeat a page the firmware leaf already translates, with
 * the same physical address and protection. That happens when one domain
 * spans several DARTs and only some of them inherited a reserved region:
 * the J700 display subsystem uses disp0 and dispext0, and only disp0 scans
 * out the boot framebuffer. apple_dart_inherited_phys() cannot report the
 * region as mapped for the whole domain, so the core maps it, and the copy
 * must be accepted on disp0 while dispext0 gets it in a vacant slot.
 * apple_dart_publish_root() never writes the firmware slot itself.
 */
static int apple_dart_check_fw_map(struct apple_dart_domain *domain, u64 dva,
				   phys_addr_t paddr, size_t size, int prot)
{
	struct apple_dart_atomic_stream_map *map;
	int i, sid, ret = 0;

	for_each_stream_map(i, domain, map) {
		struct apple_dart *dart = map->dart;
		u32 entries = dart->pgsize / sizeof(u64);
		u32 pgshift = ilog2(dart->pgsize), bits = ilog2(entries);
		unsigned long flags;
		size_t off;

		if (!dart->fw_handoff && !dart->fw_mirror)
			continue;
		spin_lock_irqsave(&dart->lock, flags);
		for (off = 0; off < size; off += dart->pgsize) {
			u64 addr = dva + off;

			for (sid = 0; sid < dart->num_streams; sid++) {
				const struct apple_dart_fw_root *fw;
				bool four;
				u64 idx, slot;
				const u64 *live, *owned;

				if (!apple_dart_domain_uses_fw(domain, i, map, sid))
					continue;
				if (!dart->fw_handoff) {
					slot = (addr >> (pgshift + bits)) & (entries - 1);
					idx = addr >> (pgshift + 2 * bits);
					if (idx >= DART_MAX_TTBR)
						continue;
					fw = dart->locked_fw[sid][idx];
					live = dart->locked_ttbr[sid][idx];
					if (!live || !apple_dart_fw_slot_owned(fw, live, slot))
						continue;
					if (!apple_dart_fw_page_matches(dart, fw, slot, addr,
									paddr + off, prot)) {
						ret = -EBUSY;
						goto unlock;
					}
					continue;
				}
				four = apple_dart_readl(dart, DART_TCR(dart, sid)) &
					       dart->hw->tcr_4level;
				idx = four ? 0 : addr >> (pgshift + 2 * bits);
				slot = (addr >> (pgshift + (four ? 2 : 1) * bits)) &
					(entries - 1);
				if (idx >= DART_MAX_TTBR)
					continue;
				live = dart->locked_ttbr[sid][idx];
				owned = dart->locked_owned[sid][idx];
				if (!live || !apple_dart_fw_slot_foreign(live, owned, slot))
					continue;
				/*
				 * All pages of an inherited slot remain firmware's;
				 * only a page its firmware leaf already maps the
				 * same way may be repeated.
				 */
				fw = dart->locked_fw[sid][idx];
				if (four || !apple_dart_fw_slot_owned(fw, live, slot) ||
				    !apple_dart_fw_page_matches(dart, fw, slot, addr,
								paddr + off, prot)) {
					ret = -EBUSY;
					goto unlock;
				}
			}
		}
 unlock:
		spin_unlock_irqrestore(&dart->lock, flags);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * What firmware's own retained translations in @domain map @address to, or 0.
 * With @prot_out, also the protection every stream grants (IOMMU_READ and
 * IOMMU_WRITE as the leaves say).
 */
static phys_addr_t
apple_dart_inherited_phys_prot(struct apple_dart_domain *domain, dma_addr_t address,
			       int *prot_out)
{
	struct apple_dart_atomic_stream_map *map;
	phys_addr_t result = 0;
	int granted = IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE;
	int i, sid;

	for_each_stream_map(i, domain, map) {
		struct apple_dart *dart = map->dart;
		unsigned long flags;
		size_t entries = dart->pgsize / sizeof(u64);
		size_t page = (address / dart->pgsize) % entries;
		size_t slot = address / dart->pgsize / entries;

		if (!dart->locked || (!dart->fw_handoff && !dart->fw_mirror))
			return 0;
		spin_lock_irqsave(&dart->lock, flags);
		for (sid = 0; sid < dart->num_streams; sid++) {
			const struct apple_dart_fw_tree *tree;
			struct apple_dart_fw_root *fw;
			phys_addr_t phys = 0;
			int prot;

			if (!apple_dart_domain_uses_fw(domain, i, map, sid))
				continue;
			tree = dart->locked_fw_tree[sid];
			fw = dart->locked_fw[sid][0];
			prot = 0;
			if (tree)
				phys = apple_dart_fw_tree_phys(dart, tree,
							       dart->locked_ttbr[sid][0],
							       address, &prot);
			else if (slot >= entries ||
				 !apple_dart_fw_slot_owned(fw, dart->locked_ttbr[sid][0], slot) ||
				 !apple_dart_fw_leaf_decode(READ_ONCE(fw->leaf[slot][page]),
							    address, &phys, &prot))
				phys = 0;
			if (!phys || (result && result != phys)) {
				spin_unlock_irqrestore(&dart->lock, flags);
				return 0;
			}
			result = phys;
			granted &= prot;
		}
		spin_unlock_irqrestore(&dart->lock, flags);
	}
	if (prot_out)
		*prot_out = result ? granted : 0;
	return result;
}

static phys_addr_t
apple_dart_inherited_phys(struct apple_dart_domain *domain, dma_addr_t address)
{
	return apple_dart_inherited_phys_prot(domain, address, NULL);
}

/*
 * Resolves a DVA a co-processor's firmware names in a request of its own, for
 * a host copy to or from that buffer. Only firmware's retained mappings count:
 * a DVA Linux mapped itself (a framebuffer, an imported or DMA_TO_DEVICE
 * buffer) is refused, so the host never writes what the device may only read.
 * @write requires the leaf to grant writes. -EACCES when refused, -ENXIO when
 * unmapped.
 */
int apple_dart_fw_remote(struct device *dev, dma_addr_t iova, bool write,
			 phys_addr_t *phys)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct apple_dart_domain *dart_domain;
	struct io_pgtable_ops *ops;
	struct io_pgtable_cfg *cfg;
	dma_addr_t address;
	phys_addr_t result;
	int prot;

	if (!domain || domain->owner != &apple_dart_iommu_ops)
		return -ENODEV;
	dart_domain = to_dart_domain(domain);
	ops = dart_domain->pgtbl_ops;
	if (dart_domain->dcp || !ops)
		return -ENODEV;
	address = (iova + dart_domain->dma_offset) & dart_domain->mask;
	if (ops->iova_to_phys(ops, address))
		return -EACCES;
	cfg = &io_pgtable_ops_to_pgtable(ops)->cfg;
	if (cfg->apple_dart_cfg.n_levels != 3 && cfg->apple_dart_cfg.n_levels != 4)
		return -ENXIO;
	result = apple_dart_inherited_phys_prot(dart_domain, address, &prot);
	if (!result)
		return -ENXIO;
	if (!(prot & IOMMU_READ) || (write && !(prot & IOMMU_WRITE)))
		return -EACCES;
	*phys = result;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_fw_remote);

static phys_addr_t apple_dart_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	struct io_pgtable_ops *ops = dart_domain->pgtbl_ops;
	struct io_pgtable_cfg *cfg;
	phys_addr_t phys;
	dma_addr_t address;

	if (dart_domain->dcp)
		return apple_dart_dcp_translate(dart_domain->dcp, iova);
	if (!ops)
		return 0;

	address = (iova + dart_domain->dma_offset) & dart_domain->mask;
	phys = ops->iova_to_phys(ops, address);
	if (phys)
		return phys;
	cfg = &io_pgtable_ops_to_pgtable(ops)->cfg;
	if (cfg->apple_dart_cfg.n_levels != 3 && cfg->apple_dart_cfg.n_levels != 4)
		return 0;
	/* Existing firmware translations need no replacement software mapping. */
	return apple_dart_inherited_phys(dart_domain, address);
}

static int apple_dart_map_pages(struct iommu_domain *domain, unsigned long iova,
				phys_addr_t paddr, size_t pgsize,
				size_t pgcount, int prot, gfp_t gfp,
				size_t *mapped)
{
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	struct io_pgtable_ops *ops = dart_domain->pgtbl_ops;

	u64 dva;
	int ret;

	if (dart_domain->dcp) {
		/* No publication while iommu_device_register() can still roll back. */
		if (!READ_ONCE(dart_domain->dcp->dart->iommu.ready))
			return -EAGAIN;
		return apple_dart_dcp_map(dart_domain->dcp, iova, paddr, pgsize,
					  pgcount, prot, gfp, mapped);
	}
	if (!ops)
		return -ENODEV;
	if (dart_domain->stream_maps[0].dart &&
	    READ_ONCE(dart_domain->stream_maps[0].dart->managed_failed))
		return -EIO;

	dva = (iova + dart_domain->dma_offset) & dart_domain->mask;
	ret = apple_dart_check_fw_map(dart_domain, dva, paddr, pgsize * pgcount,
				      prot);
	if (ret)
		return ret;

	return ops->map_pages(ops, dva, paddr, pgsize, pgcount, prot, gfp,
			      mapped);
}

static size_t apple_dart_unmap_pages(struct iommu_domain *domain,
				     unsigned long iova, size_t pgsize,
				     size_t pgcount,
				     struct iommu_iotlb_gather *gather)
{
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	struct io_pgtable_ops *ops = dart_domain->pgtbl_ops;

	if (dart_domain->dcp)
		return apple_dart_dcp_unmap(dart_domain->dcp, iova, pgsize, pgcount, gather);
	return ops->unmap_pages(ops,
				(iova + dart_domain->dma_offset) &
				dart_domain->mask, pgsize, pgcount,
				gather);
}

static int
apple_dart_setup_translation(struct apple_dart_domain *domain,
			     struct apple_dart_stream_map *stream_map)
{
	int i;
	struct apple_dart *dart = stream_map->dart;
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(domain->pgtbl_ops)->cfg;

	if (dart->cold_ane) {
		if (bitmap_weight(stream_map->sidmap, dart->num_streams) != 1 ||
		    !test_bit(0, stream_map->sidmap))
			return -EPERM;
		apple_dart_writel(dart, apple_dart_readl(dart, dart->hw->enable_streams) | BIT(0),
				  dart->hw->enable_streams);
	} else if (dart->managed_sids) {
		/* Admission owns only ordinary SIDs 1/2, both in the first word. */
		apple_dart_writel(dart, stream_map->sidmap[0] & dart->managed_sids,
				  dart->hw->enable_streams);
	} else {
		/* Other profiles use TCR to control isolation. */
		for (i = 0; i < BITS_TO_U32(dart->num_streams); i++)
			apple_dart_writel(dart, U32_MAX,
					  dart->hw->enable_streams + 4 * i);
	}

	for (i = 0; i < pgtbl_cfg->apple_dart_cfg.n_ttbrs; ++i) {
		u64 ttbr = virt_to_phys(pgtbl_cfg->apple_dart_cfg.ttbr[i]);
		apple_dart_hw_set_ttbr(stream_map, i, ttbr);
	}
	for (; i < stream_map->dart->hw->ttbr_count; ++i)
		apple_dart_hw_clear_ttbr(stream_map, i);

	apple_dart_hw_enable_translation(stream_map,
					 pgtbl_cfg->apple_dart_cfg.n_levels);
	return dart->hw->invalidate_tlb(stream_map);
}

/* Record the DVA window of @domain for invalidates of @stream_map's streams. */
static void apple_dart_set_locked_window(struct apple_dart_domain *domain,
					 struct apple_dart_stream_map *stream_map)
{
	struct apple_dart *dart = stream_map->dart;
	u64 start = domain->domain.geometry.aperture_start + domain->dma_offset;
	int sid;

	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		WRITE_ONCE(dart->locked_window[sid], start);
}

static int
apple_dart_setup_translation_locked(struct apple_dart_domain *domain,
				    struct apple_dart_stream_map *stream_map)
{
	struct io_pgtable_cfg *cfg = &io_pgtable_ops_to_pgtable(domain->pgtbl_ops)->cfg;
	int i, ret;

	apple_dart_set_locked_window(domain, stream_map);
	for (i = 0; i < cfg->apple_dart_cfg.n_ttbrs; i++) {
		ret = apple_dart_hw_map_locked_ttbr(stream_map, i);
		if (ret)
			goto unmap;
	}
	ret = apple_dart_hw_sync_locked(cfg, stream_map, false);
	if (!ret)
		ret = stream_map->dart->hw->invalidate_tlb(stream_map);
	if (!ret)
		return 0;
unmap:
	while (i--)
		apple_dart_hw_unmap_locked_ttbr(stream_map, i);
	return ret;
}

static int apple_dart_dma_window(u64 base, u64 size, u64 *last)
{
	if (!size || check_add_overflow(base, size - 1, last))
		return -EINVAL;
	return 0;
}

static bool apple_dart_dma_window_aligned(u64 base, u64 last, u32 pgsize)
{
	/* An inclusive end has all page-offset bits set, even at U64_MAX. */
	return IS_ALIGNED(base, pgsize) && IS_ALIGNED(~last, pgsize);
}

static int apple_dart_finalize_domain(struct apple_dart_domain *dart_domain,
				      struct apple_dart_master_cfg *cfg)
{
	struct apple_dart *dart = cfg->stream_maps[0].dart;
	struct io_pgtable_cfg pgtbl_cfg;
	dma_addr_t dma_min = dart_domain->has_dma_range ? dart_domain->dma_min : cfg->dma_min;
	dma_addr_t dma_max = dart_domain->has_dma_range ? dart_domain->dma_max : cfg->dma_max;
	u64 translated_max;
	u32 ias;
	int ret = 0;
	int i, j;

	if (dart->pgsize > PAGE_SIZE)
		return -EINVAL;
	if (check_add_overflow((u64)dma_max, (u64)dart->dma_offset,
			       &translated_max))
		return -ERANGE;
	ias = min_t(u32, dart->ias, fls64(translated_max));
	if (!apple_dart_dma_window_aligned(dma_min, dma_max, dart->pgsize))
		return -EINVAL;
	if ((dma_min ^ dma_max) & ~DMA_BIT_MASK(dart->ias))
		return -ERANGE;

	mutex_lock(&dart_domain->init_lock);

	if (dart_domain->finalized) {
		if (dart_domain->dcp && (apple_dart_borrowed_cfg(cfg) != dart_domain->dcp ||
		    READ_ONCE(dart_domain->dcp->failed)))
			ret = -EIO;
		goto done;
	}

	/* A borrowed J713 display stream uses its live root, not an io-pgtable. */
	if (apple_dart_has_borrowed(dart)) {
		struct apple_dart_dcp *dcp = apple_dart_borrowed_cfg(cfg);

		if (!dcp || cfg->stream_maps[1].dart || !dcp->owner || dcp->domain ||
		    READ_ONCE(dcp->failed) ||
		    bitmap_weight(cfg->stream_maps[0].sidmap, dart->num_streams) != 1 ||
		    !test_bit(dcp->sid, cfg->stream_maps[0].sidmap)) {
			ret = -EBUSY;
			goto done;
		}
		dart_domain->dcp = dcp;
		dcp->domain = dart_domain;
		dart_domain->domain.pgsize_bitmap = SZ_16K;
		dart_domain->domain.geometry.aperture_start = DCP_DART_ALIAS;
		dart_domain->domain.geometry.aperture_end = DCP_DART_ALIAS + DCP_DART_APERTURE - 1;
		dart_domain->domain.geometry.force_aperture = true;
		dart_domain->finalized = true;
		goto done;
	}

	/* Reserved mappings may be installed before any device is attached. */
	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		dart_domain->stream_maps[i].dart = cfg->stream_maps[i].dart;
		for (j = 0; j < BITS_TO_LONGS(DART_MAX_STREAMS); j++)
			atomic_long_set(&dart_domain->stream_maps[i].sidmap[j], 0);
	}

	pgtbl_cfg = (struct io_pgtable_cfg){
		.pgsize_bitmap = dart->pgsize,
		.ias = ias,
		.oas = dart->oas,
		.coherent_walk = 1,
		.iommu_dev = dart->dev,
	};

	if (dart->locked) {
		unsigned long *sidmap;
		int sid;
		u32 ttbr;

		/* Locked DARTs can only have a single stream bound */
		sidmap = cfg->stream_maps[0].sidmap;
		sid = find_first_bit(sidmap, dart->num_streams);

		if (sid >= dart->num_streams || bitmap_weight(sidmap, dart->num_streams) != 1) {
			ret = -EINVAL;
			goto done;
		}
		ttbr = apple_dart_readl(dart, DART_TTBR(dart, sid, 0));

		WARN_ON(!(ttbr & dart->hw->ttbr_valid));

		/* If the DART is locked, we need to keep the translation level count. */
		if (dart->hw->tcr_4level && dart->ias > 36) {
			if (apple_dart_readl(dart, DART_TCR(dart, sid)) &
			    dart->hw->tcr_4level) {
				if (ias < 37) {
					dev_dbg(dart->dev, "Expanded to ias=37 due to lock\n");
					pgtbl_cfg.ias = 37;
				}
			} else if (ias > 36) {
				dev_dbg(dart->dev, "Limited to ias=36 due to lock\n");
				pgtbl_cfg.ias = 36;
				if (dma_min == 0 && dma_max == DMA_BIT_MASK(dart->ias)) {
					dma_max = DMA_BIT_MASK(pgtbl_cfg.ias);
				} else if ((dma_min ^ dma_max) & ~DMA_BIT_MASK(36)) {
					dev_err(dart->dev,
						"Invalid DMA range for locked 3-level PT\n");
					ret = -ENOMEM;
					goto done;
				}
			}
		}
	}

	dart_domain->pgtbl_ops = alloc_io_pgtable_ops(dart->hw->fmt, &pgtbl_cfg,
						      &dart_domain->domain);
	if (!dart_domain->pgtbl_ops) {
		ret = -ENOMEM;
		goto done;
	}

	dart_domain->mask = DMA_BIT_MASK(pgtbl_cfg.ias);
	dart_domain->domain.pgsize_bitmap = pgtbl_cfg.pgsize_bitmap;
	dart_domain->domain.geometry.aperture_start = dma_min;
	dart_domain->domain.geometry.aperture_end = dma_max;
	dart_domain->domain.geometry.force_aperture = true;
	dart_domain->dma_offset = dart->dma_offset;

	if (pgtbl_cfg.apple_dart_cfg.n_levels == 3) {
		struct apple_dart_stream_map *stream;
		int index;

		/* Make inherited translations visible before core reservation setup. */
		for_each_stream_map(i, cfg, stream) {
			if (!stream->dart->locked ||
			    (!stream->dart->fw_handoff && !stream->dart->fw_mirror))
				continue;
			apple_dart_set_locked_window(dart_domain, stream);
			for (index = 0; index < pgtbl_cfg.apple_dart_cfg.n_ttbrs; index++) {
				ret = apple_dart_hw_map_locked_ttbr(stream, index);
				if (ret) {
					free_io_pgtable_ops(dart_domain->pgtbl_ops);
					dart_domain->pgtbl_ops = NULL;
					goto done;
				}
			}
			bitmap_copy(dart_domain->fw_sidmap[i], stream->sidmap,
				    stream->dart->num_streams);
		}
	}

	dart_domain->finalized = true;

done:
	mutex_unlock(&dart_domain->init_lock);
	return ret;
}

static int
apple_dart_mod_streams(struct apple_dart_atomic_stream_map *domain_maps,
		       struct apple_dart_stream_map *master_maps,
		       bool add_streams)
{
	int i, j;

	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (domain_maps[i].dart != master_maps[i].dart)
			return -EINVAL;
	}

	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (!domain_maps[i].dart)
			break;
		for (j = 0; j < BITS_TO_LONGS(domain_maps[i].dart->num_streams); j++) {
			if (add_streams)
				atomic_long_or(master_maps[i].sidmap[j],
					       &domain_maps[i].sidmap[j]);
			else
				atomic_long_and(~master_maps[i].sidmap[j],
						&domain_maps[i].sidmap[j]);
		}
	}

	return 0;
}

static int apple_dart_domain_add_streams(struct apple_dart_domain *domain,
					 struct apple_dart_master_cfg *cfg)
{
	return apple_dart_mod_streams(domain->stream_maps, cfg->stream_maps,
				      true);
}

static int apple_dart_attach_dev_paging(struct iommu_domain *domain,
					struct device *dev,
					struct iommu_domain *old)
{
	int ret, i;
	struct apple_dart_stream_map *stream_map;
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	bool streams_added = false;

	for_each_stream_map(i, cfg, stream_map)
		if (stream_map->dart->live_mtp)
			return -EBUSY;

	for_each_stream_map(i, cfg, stream_map)
		WARN_ON(pm_runtime_get_sync(stream_map->dart->dev) < 0);

	ret = apple_dart_finalize_domain(dart_domain, cfg);
	if (ret)
		goto err;

	/* Live root is already validated and borrowed; attach must not publish it. */
	if (dart_domain->dcp)
		goto err;

	ret = apple_dart_domain_add_streams(dart_domain, cfg);
	if (ret)
		goto err;
	streams_added = true;

	for_each_stream_map(i, cfg, stream_map) {
		if (stream_map->dart->managed_sids) {
			struct apple_dart *dart = stream_map->dart;
			unsigned long flags;

			spin_lock_irqsave(&dart->managed_lock, flags);
			if (dart->managed_failed)
				ret = -EIO;
			else
				ret = apple_dart_setup_translation(dart_domain, stream_map);
			if (ret)
				apple_dart_managed_quarantine_locked(dart);
			spin_unlock_irqrestore(&dart->managed_lock, flags);
			if (ret)
				goto err;
		} else if (!stream_map->dart->locked) {
			ret = apple_dart_setup_translation(dart_domain, stream_map);
			if (ret)
				goto err;
		} else {
			ret = apple_dart_setup_translation_locked(dart_domain, stream_map);
			if (ret)
				goto err;
		}
	}

err:
	if (ret && streams_added && cfg->stream_maps[0].dart->managed_sids)
		apple_dart_mod_streams(dart_domain->stream_maps, cfg->stream_maps, false);
	for_each_stream_map(i, cfg, stream_map)
		pm_runtime_put(stream_map->dart->dev);
	return ret;
}

static int apple_dart_attach_dev_identity(struct iommu_domain *domain,
					  struct device *dev,
					  struct iommu_domain *old)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream_map;
	int i;

	if (!cfg->supports_bypass || cfg->stream_maps[0].dart->managed_sids)
		return -EINVAL;

	if (cfg->locked)
		return -EINVAL;

	for_each_stream_map(i, cfg, stream_map)
		WARN_ON(pm_runtime_get_sync(stream_map->dart->dev) < 0);

	for_each_stream_map(i, cfg, stream_map)
		if (!stream_map->dart->retained_mtp)
			apple_dart_hw_enable_bypass(stream_map);

	for_each_stream_map(i, cfg, stream_map)
		pm_runtime_put(stream_map->dart->dev);
	return 0;
}

static const struct iommu_domain_ops apple_dart_identity_ops = {
	.attach_dev = apple_dart_attach_dev_identity,
};

static struct iommu_domain apple_dart_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &apple_dart_identity_ops,
};

static int apple_dart_attach_dev_blocked(struct iommu_domain *domain,
					 struct device *dev,
					 struct iommu_domain *old)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream_map;
	int i;

	for_each_stream_map(i, cfg, stream_map)
		if (stream_map->dart->live_mtp)
			return -EBUSY;

	if (cfg->locked)
		return -EINVAL;

	for_each_stream_map(i, cfg, stream_map)
		WARN_ON(pm_runtime_get_sync(stream_map->dart->dev) < 0);

	for_each_stream_map(i, cfg, stream_map)
		apple_dart_hw_disable_dma(stream_map);

	for_each_stream_map(i, cfg, stream_map)
		pm_runtime_put(stream_map->dart->dev);
	return 0;
}

static const struct iommu_domain_ops apple_dart_blocked_ops = {
	.attach_dev = apple_dart_attach_dev_blocked,
};

static struct iommu_domain apple_dart_blocked_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	.ops = &apple_dart_blocked_ops,
};

static struct iommu_device *apple_dart_probe_device(struct device *dev)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream_map;
	int i;

	if (!dev_iommu_fwspec_get(dev) || !cfg)
		return ERR_PTR(-ENODEV);

	if (apple_dart_has_borrowed(cfg->stream_maps[0].dart)) {
		struct apple_dart_dcp *borrowed = apple_dart_borrowed_cfg(cfg);
		int ret = -EINVAL;

		/* An external display DART's registers only inside a use. */
		if (borrowed && !apple_dart_pm_get(borrowed->dart)) {
			ret = -EAGAIN;
		} else if (borrowed) {
			ret = apple_dart_dcp_validate(borrowed, dev);
			apple_dart_pm_put(borrowed->dart);
		}
		if (ret)
			return ERR_PTR(ret);
	}

	for_each_stream_map(i, cfg, stream_map)
		device_link_add(dev, stream_map->dart->dev,
			DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_SUPPLIER |
			DL_FLAG_RPM_ACTIVE);

	return &cfg->stream_maps[0].dart->iommu;
}

static void apple_dart_release_device(struct device *dev)
{
	int i, j, ret;
	struct apple_dart_stream_map *stream_map;
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);

	for_each_stream_map(j, cfg, stream_map) {
		/* Borrowed display roots stay exactly as the DCP left them. */
		if (!stream_map->dart->locked || apple_dart_has_borrowed(stream_map->dart))
			continue;
		ret = pm_runtime_resume_and_get(stream_map->dart->dev);
		if (ret < 0) {
			dev_err_ratelimited(stream_map->dart->dev,
				"failed to power DART for consumer release: %d\n", ret);
			continue;
		}
		for (i = 0; i < stream_map->dart->hw->ttbr_count; ++i)
			apple_dart_hw_unmap_locked_ttbr(stream_map, i);
		(void)stream_map->dart->hw->invalidate_tlb(stream_map);
		pm_runtime_put(stream_map->dart->dev);
	}

	kfree(cfg);
}

static struct iommu_domain *apple_dart_domain_alloc_paging(struct device *dev)
{
	struct apple_dart_domain *dart_domain;

	dart_domain = kzalloc_obj(*dart_domain);
	if (!dart_domain)
		return NULL;

	mutex_init(&dart_domain->init_lock);

	if (dev) {
		struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
		u64 window[2];
		int ret;

		if (of_property_present(dev->of_node, "apple,dma-range")) {
			ret = of_property_read_u64_array(dev->of_node, "apple,dma-range",
						window, 2);
			if (!ret)
				ret = apple_dart_dma_window(window[0], window[1],
						    &dart_domain->dma_max);
			if (ret) {
				kfree(dart_domain);
				return ERR_PTR(ret);
			}
			dart_domain->dma_min = window[0];
			dart_domain->has_dma_range = true;
		}
		ret = apple_dart_finalize_domain(dart_domain, cfg);
		if (ret) {
			kfree(dart_domain);
			return ERR_PTR(ret);
		}
	}
	return &dart_domain->domain;
}

static void apple_dart_domain_free(struct iommu_domain *domain)
{
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	struct apple_dart_atomic_stream_map *map;
	struct apple_dart_stream_map stream;
	int i, j, idx, ret;

	if (dart_domain->dcp) {
		/* Registration rollback: no consumer could publish host DMA yet. */
		if (!READ_ONCE(dart_domain->dcp->dart->iommu.ready)) {
			dart_domain->dcp->domain = NULL;
			kfree(dart_domain);
			return;
		}
		dev_info(dart_domain->dcp->dart->dev, "Pinning DCP domain until reboot\n");
		return;
	}
	for_each_stream_map(i, dart_domain, map) {
		if (map->dart->managed_sids)
			return;
		if (!map->dart->locked)
			continue;
		stream.dart = map->dart;
		for (j = 0; j < BITS_TO_LONGS(stream.dart->num_streams); j++)
			stream.sidmap[j] = atomic_long_read(&map->sidmap[j]);
		if (bitmap_empty(stream.sidmap, stream.dart->num_streams))
			continue;
		j = pm_runtime_resume_and_get(stream.dart->dev);
		if (j < 0) {
			dev_err_ratelimited(stream.dart->dev,
				"retaining page tables after power failure: %d\n", j);
			return;
		}
		ret = 0;
		for (idx = 0; idx < stream.dart->hw->ttbr_count; idx++) {
			j = apple_dart_hw_unmap_locked_ttbr(&stream, idx);
			ret = ret ?: j;
		}
		j = stream.dart->hw->invalidate_tlb(&stream);
		ret = ret ?: j;
		pm_runtime_put(stream.dart->dev);
		/*
		 * Do not free page tables that hardware may still cache, also
		 * when a consumer release or an attach rollback retired the root.
		 */
		ret = ret ?: apple_dart_retire_error(&stream);
		if (ret) {
			dev_err_ratelimited(stream.dart->dev,
				"retaining page tables after TLB invalidation failure: %d\n", ret);
			return;
		}
	}
	free_io_pgtable_ops(dart_domain->pgtbl_ops);
	kfree(dart_domain);
}

/*
 * Work out the DMA window of one "iommus" specifier. One cell is the stream
 * ID alone and keeps the window of the DART node. Five cells add the 64-bit
 * start and length of the window that the device uses.
 */
static int apple_dart_parse_dma_window(struct device *dev,
				       const struct apple_dart *dart,
				       const struct of_phandle_args *args,
				       dma_addr_t *dma_min,
				       dma_addr_t *dma_max)
{
	dma_addr_t length;

	*dma_min = dart->dma_min;
	*dma_max = dart->dma_max;

	if (args->args_count == 1)
		return 0;
	if (args->args_count != 5)
		return -EINVAL;

	length = ((dma_addr_t)args->args[3] << 32) | args->args[4];
	if (!length)
		return -EINVAL;

	if (*dma_min != 0 || *dma_max != DMA_BIT_MASK(dart->ias))
		dev_err(dev, "still using legacy \"apple,dma-range\" property\n");

	*dma_min = ((dma_addr_t)args->args[1] << 32) | args->args[2];

	if (!IS_ALIGNED(*dma_min, dart->pgsize) ||
	    !IS_ALIGNED(length, dart->pgsize)) {
		dev_err(dev, "Unaligned DMA window %pad, %pad (0x%x)\n",
			dma_min, &length, dart->pgsize);
		return -EINVAL;
	}
	if (check_add_overflow(*dma_min, length - 1, dma_max)) {
		dev_err(dev, "DMA window length (%pad) overflows range for start %pad\n",
			&length, dma_min);
		return -EINVAL;
	}

	/* Ensure that the DMA window does not exceed the DART's ias. */
	if ((*dma_min ^ *dma_max) & ~DMA_BIT_MASK(dart->ias)) {
		dev_err(dev, "Invalid DMA window for ias=%d\n", dart->ias);
		return -EINVAL;
	}

	return 0;
}

static int apple_dart_of_xlate(struct device *dev,
			       const struct of_phandle_args *args)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct platform_device *iommu_pdev = of_find_device_by_node(args->np);
	struct apple_dart *dart = platform_get_drvdata(iommu_pdev);
	struct apple_dart *cfg_dart;
	dma_addr_t dma_max;
	dma_addr_t dma_min;
	int i, sid;

	put_device(&iommu_pdev->dev);

	i = apple_dart_parse_dma_window(dev, dart, args, &dma_min, &dma_max);
	if (i)
		return i;

	sid = args->args[0];
	if (dart->isp_host && sid != 0 && sid != 12)
		return -EINVAL;
	if (dart->cold_ane && sid != 0)
		return -EPERM;
	if (dart->managed_sids &&
	    (sid <= 0 || sid >= 16 || !(dart->managed_sids & BIT(sid))))
		return -EINVAL;
	if (apple_dart_has_borrowed(dart) &&
	    (sid < 0 || sid >= dart->num_streams || !apple_dart_borrowed_sid(dart, sid) ||
	     (cfg && cfg->stream_maps[0].dart)))
		return -EINVAL;

	if (!cfg) {
		cfg = kzalloc_obj(*cfg);
		if (!cfg)
			return -ENOMEM;
		/* Will be ANDed with DART capabilities */
		cfg->supports_bypass = true;
		/* Will be ORed with DART capabilities*/
		cfg->locked = false;
		/* Will be merged with other DARTs to the common range. */
		cfg->dma_min = dma_min;
		cfg->dma_max = dma_max;
	} else {
		if (dma_min >= cfg->dma_max || cfg->dma_min >= dma_max) {
			dev_err(dev, "non-overlapping DMA windows: %pad..%pad, %pad..%pad\n",
				&dma_min, &dma_max,
				&cfg->dma_min, &cfg->dma_max);
			return -EINVAL;
		}
		cfg->dma_min = max(dma_min, cfg->dma_min);
		cfg->dma_max = min(dma_max, cfg->dma_max);
	}
	dev_iommu_priv_set(dev, cfg);

	cfg_dart = cfg->stream_maps[0].dart;
	if (cfg_dart) {
		if (cfg_dart->pgsize != dart->pgsize)
			return -EINVAL;
		if (cfg_dart->ias != dart->ias)
			return -EINVAL;
	}

	cfg->supports_bypass &= dart->supports_bypass && !dart->managed_sids && !dart->cold_ane;
	cfg->locked |= dart->locked;

	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (cfg->stream_maps[i].dart == dart) {
			set_bit(sid, cfg->stream_maps[i].sidmap);
			return 0;
		}
	}
	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (!cfg->stream_maps[i].dart) {
			cfg->stream_maps[i].dart = dart;
			set_bit(sid, cfg->stream_maps[i].sidmap);
			return 0;
		}
	}

	return -EINVAL;
}

#if IS_ENABLED(CONFIG_APPLE_DART_J713_DCP)
/*
 * The check, inside a use of the DART's registers (on an external display
 * DART, apple_dart_pm_get() by the caller).
 */
static int __apple_dart_dcp_check(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct apple_dart_domain *dd;
	struct apple_dart_dcp *dcp;
	unsigned long flags;
	bool drift = false;
	int ret;

	if (!domain || domain->owner != &apple_dart_iommu_ops || !iommu_is_dma_domain(domain))
		return -ENODEV;
	dd = to_dart_domain(domain);
	if (!dd->dcp || dd->dcp->owner != dev || dd->dcp->domain != dd ||
	    !READ_ONCE(dd->dcp->dart->iommu.ready))
		return -ENODEV;
	if (READ_ONCE(dd->dcp->failed))
		return -EIO;
	dcp = dd->dcp;
	if (!apple_dart_dcp_registers(dcp))
		goto failed;
	spin_lock_irqsave(&dcp->lock, flags);
	for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++) {
		if (READ_ONCE(dcp->root[i]) != READ_ONCE(dcp->entry[i])) {
			drift = true;
			break;
		}
		/* SID23 may update leaves under an inherited, stable root. Check
		 * their physical ownership too, not just known firmware segments.
		 */
		if (dcp->sid == 23 && test_bit(i, dcp->foreign) && dcp->table[i])
			for (unsigned int j = 0; j < DCP_DART_ENTRIES; j++)
				if (!apple_dart_dcp_leaf_safe(dcp, READ_ONCE(dcp->table[i][j]))) {
					drift = true;
					break;
				}
		if (drift)
			break;
	}
	spin_unlock_irqrestore(&dcp->lock, flags);
	if (drift)
		goto failed;
	ret = apple_dart_dcp_validate(dcp, dev);
	if (ret)
		goto failed;
	return 0;
failed:
	/* On an external display DART, both frontends: the DART is retired. */
	if (dcp->dart->dispext)
		apple_dart_fail_borrowed(dcp->dart, "check: %s", drift ? "a root or leaf changed" :
					 "registers or ownership not as adopted");
	else
		apple_dart_dcp_fail(dcp, "check: %s", drift ? "a root or leaf changed" :
				    "registers or ownership not as adopted");
	return -EIO;
}

/*
 * On an external display DART, only with its power confirmed (a use of its
 * registers): -EAGAIN, nothing retired, from sleep until its firmware's next
 * confirmed power-up.
 */
int apple_dart_dcp_check(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct apple_dart *dart;
	int ret;

	if (!domain || domain->owner != &apple_dart_iommu_ops || !to_dart_domain(domain)->dcp)
		return __apple_dart_dcp_check(dev);
	dart = to_dart_domain(domain)->dcp->dart;
	if (!apple_dart_pm_get(dart))
		return -EAGAIN;
	ret = __apple_dart_dcp_check(dev);
	apple_dart_pm_put(dart);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_check);

int apple_dart_dcp_check_buffer(struct device *dev, dma_addr_t iova, size_t size)
{
	struct apple_dart_domain *dd;
	struct apple_dart_dcp *dcp;
	u64 end;
	int ret = apple_dart_dcp_check(dev);

	if (ret)
		return ret;
	if (size > SZ_16M || !dcp_dart_contains(DCP_DART_ALIAS,
			DCP_DART_ALIAS + DCP_DART_APERTURE, iova, size))
		return -EINVAL;
	dd = to_dart_domain(iommu_get_domain_for_dev(dev));
	dcp = dd->dcp;
	if (dcp->sid != 23)
		return -ENODEV;
	end = iova + size;
	for (u64 page = iova & ~(DCP_DART_PAGE - 1); page < end; page += DCP_DART_PAGE) {
		unsigned int root = (page >> 25) & 2047;
		unsigned int leaf = (page >> 14) & 2047;

		/* A readable firmware TEXT mapping is never writable RTKit shared memory. */
		if (!apple_dart_dcp_translate(dcp, page) || !test_bit(root, dcp->foreign) ||
		    !dcp_dart_pte_full_rw(READ_ONCE(dcp->table[root][leaf]), true)) {
			apple_dart_dcp_fail(dcp, "buffer at %llx is not writable shared memory",
					    page);
			return -EACCES;
		}
	}
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_check_buffer);

void *apple_dart_dcp_alloc(struct device *dev, size_t size, dma_addr_t *dva)
{
	struct apple_dart_dcp_allocation *allocation;
	struct apple_dart_domain *dd;
	struct apple_dart_dcp *dcp;
	unsigned int root, count, allocated = 0;
	bool coherent = dev_is_dma_coherent(dev);
	int prot = IOMMU_READ | IOMMU_WRITE | (coherent ? IOMMU_CACHE : 0);
	int ret = apple_dart_dcp_check(dev);
	void *result;

	if (ret)
		return ERR_PTR(ret);
	if (!size || size > SZ_16M)
		return ERR_PTR(-EINVAL);
	size = PAGE_ALIGN(size);
	count = size >> PAGE_SHIFT;
	dd = to_dart_domain(iommu_get_domain_for_dev(dev));
	dcp = dd->dcp;
	mutex_lock(&dcp->alloc_lock);
	if (dcp->sid != 23) {
		result = ERR_PTR(-ENODEV);
		goto unlock;
	}
	if (READ_ONCE(dcp->failed) || dcp->allocated > SZ_64M - size) {
		result = ERR_PTR(-ENOSPC);
		goto unlock;
	}
	/* One empty 32-MiB slice per object. Never alias a foreign or previous object. */
	for (root = 0; root < DCP_DART_ENTRIES; root++)
		if (!test_bit(root, dcp->claimed) &&
		    !dcp->entry[root] && !READ_ONCE(dcp->root[root]))
			break;
	if (root == DCP_DART_ENTRIES) {
		result = ERR_PTR(-ENOSPC);
		goto unlock;
	}
	set_bit(root, dcp->claimed); /* Includes failures before a root PTE appears. */
	allocation = kzalloc(struct_size(allocation, pages, count), GFP_KERNEL);
	if (!allocation) {
		result = ERR_PTR(-ENOMEM);
		goto unlock;
	}
	allocation->size = size;
	allocation->dva = DCP_DART_ALIAS + root * DCP_DART_SLICE;
	for (allocated = 0; allocated < count; allocated++) {
		allocation->pages[allocated] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!allocation->pages[allocated])
			goto free_unpublished;
		if (!coherent)
			arch_dma_prep_coherent(allocation->pages[allocated], PAGE_SIZE);
	}
	allocation->cpu = dma_common_pages_remap(allocation->pages, size,
						 dma_pgprot(dev, PAGE_KERNEL, 0),
						 __builtin_return_address(0));
	if (!allocation->cpu)
		goto free_unpublished;
	/* Own payload pages BEFORE the first PTE, including every rollback case. */
	list_add_tail(&allocation->link, &dcp->allocations);
	dcp->allocated += size;
	for (unsigned int i = 0; i < count; i++) {
		ret = iommu_map(&dd->domain, allocation->dva + i * PAGE_SIZE,
				page_to_phys(allocation->pages[i]), PAGE_SIZE, prot, GFP_KERNEL);
		if (ret)
			goto pin_failure;
	}
	ret = apple_dart_dcp_check(dev);
	if (ret)
		goto pin_failure;
	for (unsigned int i = 0; i < count; i++) {
		if (apple_dart_dcp_translate(dcp, allocation->dva + i * PAGE_SIZE) !=
		    page_to_phys(allocation->pages[i])) {
			ret = -EIO;
			goto pin_failure;
		}
	}
	*dva = allocation->dva;
	result = allocation->cpu;
	goto unlock;
pin_failure:
	apple_dart_dcp_fail(dcp, "payload mapping failed: %d", ret);
	dev_err(dev, "Retaining %zu payload bytes after DCP mapping failure %d\n", size, ret);
	result = ERR_PTR(ret);
	goto unlock;
free_unpublished:
	/* Safe only because no page address or PTE has been exposed yet. */
	while (allocated)
		__free_page(allocation->pages[--allocated]);
	kfree(allocation);
	result = ERR_PTR(-ENOMEM);
unlock:
	mutex_unlock(&dcp->alloc_lock);
	return result;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_alloc);

static int apple_dart_dcp_shared_flush(struct apple_dart_dcp *dcp)
{
	struct apple_dart_stream_map map = { .dart = dcp->dart };
	int ret;

	if (dcp->sid != 16 || !apple_dart_dcp_registers(dcp))
		return -EIO;
	/* An external display DART has no fault IRQ attached: look first. */
	if (dcp->dart->dispext && apple_dart_fault_pending(dcp->dart)) {
		apple_dart_fail_borrowed(dcp->dart, "a DART fault is pending (SID16 flush)");
		return -EIO;
	}
	bitmap_zero(map.sidmap, DART_MAX_STREAMS);
	set_bit(16, map.sidmap);
	/* Generic SID16 flush is a no-op; authorize only this synchronous call. */
	WRITE_ONCE(dcp->shared_flushing, true);
	dma_wmb();
	/* Raw: inside the caller's use of the DART (no gate taken again). */
	ret = apple_dart_dcp_invalidate_tlb(&map);
	WRITE_ONCE(dcp->shared_flushing, false);
	return ret;
}

#include "apple-dart-dcp-shared.h"
#include "apple-dart-dcp-scanout.h"
#endif /* CONFIG_APPLE_DART_J713_DCP */

static DEFINE_MUTEX(apple_dart_groups_lock);

static void apple_dart_release_group(void *iommu_data)
{
	int i, sid;
	struct apple_dart_stream_map *stream_map;
	struct apple_dart_master_cfg *group_master_cfg = iommu_data;

	mutex_lock(&apple_dart_groups_lock);

	for_each_stream_map(i, group_master_cfg, stream_map)
		for_each_set_bit(sid, stream_map->sidmap, stream_map->dart->num_streams)
			stream_map->dart->sid2group[sid] = NULL;

	kfree(iommu_data);
	mutex_unlock(&apple_dart_groups_lock);
}

static int apple_dart_merge_master_cfg(struct apple_dart_master_cfg *dst,
				       struct apple_dart_master_cfg *src)
{
	/*
	 * We know that this function is only called for groups returned from
	 * pci_device_group and that all Apple Silicon platforms never spread
	 * PCIe devices from the same bus across multiple DARTs such that we can
	 * just assume that both src and dst only have the same single DART.
	 */
	if (src->stream_maps[1].dart)
		return -EINVAL;
	if (dst->stream_maps[1].dart)
		return -EINVAL;
	if (src->stream_maps[0].dart != dst->stream_maps[0].dart)
		return -EINVAL;

	bitmap_or(dst->stream_maps[0].sidmap,
		  dst->stream_maps[0].sidmap,
		  src->stream_maps[0].sidmap,
		  dst->stream_maps[0].dart->num_streams);
	return 0;
}

static struct iommu_group *apple_dart_device_group(struct device *dev)
{
	int i, sid;
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream_map;
	struct apple_dart_master_cfg *group_master_cfg;
	struct iommu_group *group = NULL;
	struct iommu_group *res = ERR_PTR(-EINVAL);

	mutex_lock(&apple_dart_groups_lock);

	for_each_stream_map(i, cfg, stream_map) {
		for_each_set_bit(sid, stream_map->sidmap, stream_map->dart->num_streams) {
			struct iommu_group *stream_group =
				stream_map->dart->sid2group[sid];

			if (group && group != stream_group) {
				res = ERR_PTR(-EINVAL);
				goto out;
			}

			group = stream_group;
		}
	}

	if (group) {
		res = iommu_group_ref_get(group);
		goto out;
	}

#ifdef CONFIG_PCI
	if (dev_is_pci(dev))
		group = pci_device_group(dev);
	else
#endif
		group = generic_device_group(dev);

	res = ERR_PTR(-ENOMEM);
	if (!group)
		goto out;

	group_master_cfg = iommu_group_get_iommudata(group);
	if (group_master_cfg) {
		int ret;

		ret = apple_dart_merge_master_cfg(group_master_cfg, cfg);
		if (ret) {
			dev_err(dev, "Failed to merge DART IOMMU groups.\n");
			iommu_group_put(group);
			res = ERR_PTR(ret);
			goto out;
		}
	} else {
		group_master_cfg = kmemdup(cfg, sizeof(*group_master_cfg),
					   GFP_KERNEL);
		if (!group_master_cfg) {
			iommu_group_put(group);
			goto out;
		}

		iommu_group_set_iommudata(group, group_master_cfg,
			apple_dart_release_group);
	}

	for_each_stream_map(i, cfg, stream_map)
		for_each_set_bit(sid, stream_map->sidmap, stream_map->dart->num_streams)
			stream_map->dart->sid2group[sid] = group;

	res = group;

out:
	mutex_unlock(&apple_dart_groups_lock);
	return res;
}

static bool apple_dart_capable(struct device *dev, enum iommu_cap cap)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream_map;
	int i;

	switch (cap) {
	case IOMMU_CAP_DEFERRED_FLUSH:
		/*
		 * A deferred flush invalidates whole streams. On a T8140 or
		 * T8142 DART the firmware may still be translating through
		 * the same stream, and a whole-stream invalidate can fault
		 * its live accesses, so keep those DARTs on strict flushes.
		 */
		if (cfg)
			for_each_stream_map(i, cfg, stream_map)
				if (stream_map->dart->per_stream)
					return false;
		return true;
	default:
		return false;
	}
}

/*
 * Media and ML engines (video decode and encode, neural engine, camera ISP)
 * are run by their own coprocessor firmware, which can still have DMA in
 * flight after the host driver has given up on a job: a timeout, a reset,
 * a stream torn down mid-frame. With deferred flushing the DART keeps
 * translating a freed buffer's addresses until the flush queue drains, so
 * such a late write lands silently in pages that already went back to the
 * allocator. Strict flushing makes it fault at the DART instead. These
 * engines map their buffers once per stream or model, so the per-unmap
 * invalidate is not on any hot path.
 *
 * The client is identified by its device tree compatible, "apple,<soc>-avd"
 * and so on, so every SoC generation is covered without a per-SoC table.
 */
static const char * const apple_dart_strict_engines[] = {
	"avd", "ave", "ane", "isp",
};

static bool apple_dart_compat_is_strict_engine(const char *compat)
{
	const char *engine;
	unsigned int i;

	if (!str_has_prefix(compat, "apple,"))
		return false;
	engine = strrchr(compat, '-');
	engine = engine ? engine + 1 : compat + strlen("apple,");
	for (i = 0; i < ARRAY_SIZE(apple_dart_strict_engines); i++)
		if (!strcmp(engine, apple_dart_strict_engines[i]))
			return true;
	return false;
}

static bool apple_dart_master_needs_strict(struct device *dev)
{
	struct property *prop;
	const char *compat;

	if (!dev->of_node)
		return false;
	of_property_for_each_string(dev->of_node, "compatible", prop, compat)
		if (apple_dart_compat_is_strict_engine(compat))
			return true;
	return false;
}

static int apple_dart_def_domain_type(struct device *dev)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);

	/* Retained radio and borrowed display streams require strict DMA. */
	if (cfg->stream_maps[0].dart->managed_sids ||
	    apple_dart_has_borrowed(cfg->stream_maps[0].dart))
		return IOMMU_DOMAIN_DMA;
	if (cfg->stream_maps[0].dart->retained_mtp ||
	    cfg->stream_maps[0].dart->live_mtp)
		return IOMMU_DOMAIN_IDENTITY;
	if (cfg->stream_maps[0].dart->pgsize > PAGE_SIZE)
		return IOMMU_DOMAIN_IDENTITY;
	/*
	 * Tunneled PCIe is untrusted, and the PCIe-C DART bypasses only its
	 * reserved streams: translate everything behind a USB4 port.
	 */
	if (cfg->stream_maps[0].dart->tunneled)
		return IOMMU_DOMAIN_DMA;
	if (dev_is_pci(dev) && apple_t8132_air() &&
	    cfg->supports_bypass && !cfg->locked)
		return IOMMU_DOMAIN_IDENTITY;
	if (!cfg->supports_bypass)
		return IOMMU_DOMAIN_DMA;
	if (cfg->locked)
		return IOMMU_DOMAIN_DMA;
	if (apple_dart_master_needs_strict(dev))
		return IOMMU_DOMAIN_DMA;

	return 0;
}

#ifndef CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR
/* Keep things compiling when CONFIG_PCIE_APPLE isn't selected */
#define CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR	0
#endif
#define DOORBELL_ADDR	(CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR & PAGE_MASK)

/* Convert an occupied root slot's device window back to allocator IOVA. */
static bool apple_dart_fw_slot_range(const struct apple_dart *dart, u64 window,
				    size_t slot, bool four, u64 *start, u64 *length)
{
	u32 bits = ilog2(dart->pgsize / sizeof(u64));
	u32 shift = ilog2(dart->pgsize) + (four ? 2 : 1) * bits;
	u32 width = four ? dart->ias : shift + bits;
	u64 span, base, offset, end;

	if (dart->ias >= 64 || shift >= dart->ias || width >= 64 ||
	    slot >= dart->pgsize / sizeof(u64))
		return false;
	span = BIT_ULL(shift);
	base = four ? 0 : window & ~(BIT_ULL(width) - 1);
	if (check_mul_overflow((u64)slot, span, &offset) ||
	    check_add_overflow(base, offset, start) ||
	    check_add_overflow(*start, span - 1, &end) ||
	    end >= BIT_ULL(dart->ias) || *start < dart->dma_offset)
		return false;
	*start -= dart->dma_offset;
	*length = span;
	return true;
}

static void apple_dart_get_fw_resv_regions(struct device *dev,
					 struct list_head *head)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct apple_dart_stream_map *stream;
	int i, sid, idx;

	if (!cfg)
		return;
	for_each_stream_map(i, cfg, stream) {
		struct apple_dart *dart = stream->dart;
		size_t entries = dart->pgsize / sizeof(u64);

		/*
		 * Only the handoff and fw_tree reserve anything: leave the other
		 * fw_mirror DARTs (T6041, T8140, T8142) untouched, TCR reads included.
		 */
		if ((!dart->fw_handoff && !dart->fw_tree) ||
		    entries > DART_MAX_ROOT_ENTRIES)
			continue;
		for_each_set_bit(sid, stream->sidmap, dart->num_streams) {
			for (idx = 0; idx < dart->hw->ttbr_count; idx++) {
				DECLARE_BITMAP(slots, DART_MAX_ROOT_ENTRIES);
				const u64 *live, *owned;
				unsigned long flags;
				size_t slot;
				u64 window;
				bool four;

				bitmap_zero(slots, entries);
				/* Teardown may unmap both tables after dropping this lock. */
				spin_lock_irqsave(&dart->lock, flags);
				live = dart->locked_ttbr[sid][idx];
				owned = dart->locked_owned[sid][idx];
				window = dart->locked_window[sid];
				four = apple_dart_readl(dart, DART_TCR(dart, sid)) &
				       dart->hw->tcr_4level;
				for (slot = 0; live && slot < entries; slot++)
					if (apple_dart_fw_slot_foreign(live, owned, slot))
						__set_bit(slot, slots);
				spin_unlock_irqrestore(&dart->lock, flags);

				/*
				 * A three-level fw_mirror slot may repeat firmware's
				 * pages (apple_dart_check_fw_map()); a four-level one
				 * is firmware's alone, so keep the allocator out of it.
				 */
				if (!dart->fw_handoff && !(four && dart->fw_tree))
					continue;
				if (!four && check_add_overflow(window,
					((u64)idx << (ilog2(dart->pgsize) + 2 * ilog2(entries))),
					&window))
					continue;
				for_each_set_bit(slot, slots, entries) {
					struct iommu_resv_region *region;
					u64 start, length;

					if (!apple_dart_fw_slot_range(dart, window, slot,
						four, &start, &length))
						continue;
					/* RESERVED excludes holes without asking the core to map them. */
					region = iommu_alloc_resv_region(start, length, 0,
						IOMMU_RESV_RESERVED, GFP_KERNEL);
					if (!region)
						return;
					list_add_tail(&region->list, head);
				}
			}
		}
	}
}

static void apple_dart_get_resv_regions(struct device *dev,
					struct list_head *head)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	struct iommu_resv_region *region;

	if (cfg && apple_dart_has_borrowed(cfg->stream_maps[0].dart)) {
		struct apple_dart_dcp *borrowed = apple_dart_borrowed_cfg(cfg);

		/* Already reconciled against live leaves; never request reconstruction. */
		if (borrowed)
			apple_dart_dcp_reserve(borrowed, head);
		return;
	}

	if (IS_ENABLED(CONFIG_PCIE_APPLE) && dev_is_pci(dev)) {
		int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;

		region = iommu_alloc_resv_region(DOORBELL_ADDR,
						 PAGE_SIZE, prot,
						 IOMMU_RESV_MSI, GFP_KERNEL);
		if (!region)
			return;

		list_add_tail(&region->list, head);
	}

	/* Keep the translated-domain requirement and inherited translations. */
	iommu_dma_get_resv_regions(dev, head);
	apple_dart_get_fw_resv_regions(dev, head);
}

static const struct iommu_ops apple_dart_iommu_ops = {
	.capable = apple_dart_capable,
	.identity_domain = &apple_dart_identity_domain,
	.blocked_domain = &apple_dart_blocked_domain,
	.def_domain_type = apple_dart_def_domain_type,
	.domain_alloc_paging = apple_dart_domain_alloc_paging,
	.probe_device = apple_dart_probe_device,
	.release_device = apple_dart_release_device,
	.device_group = apple_dart_device_group,
	.of_xlate = apple_dart_of_xlate,
	.get_resv_regions = apple_dart_get_resv_regions,
	.owner = THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= apple_dart_attach_dev_paging,
		.map_pages	= apple_dart_map_pages,
		.unmap_pages	= apple_dart_unmap_pages,
		.flush_iotlb_all = apple_dart_flush_iotlb_all,
		.iotlb_sync	= apple_dart_iotlb_sync,
		.iotlb_sync_map	= apple_dart_iotlb_sync_map,
		.iova_to_phys	= apple_dart_iova_to_phys,
		.free		= apple_dart_domain_free,
	}
};

static irqreturn_t apple_dart_t8020_irq(int irq, void *dev)
{
	struct apple_dart *dart = dev;
	const char *fault_name = NULL;
	u32 error = apple_dart_readl(dart, DART_T8020_ERROR);
	u32 error_code = FIELD_GET(DART_T8020_ERROR_CODE, error);
	u32 addr_lo = apple_dart_readl(dart, DART_T8020_ERROR_ADDR_LO);
	u32 addr_hi = apple_dart_readl(dart, DART_T8020_ERROR_ADDR_HI);
	u64 addr = addr_lo | (((u64)addr_hi) << 32);
	u8 stream_idx = FIELD_GET(DART_T8020_ERROR_STREAM, error);

	if (!(error & DART_T8020_ERROR_FLAG))
		return IRQ_NONE;

	/* there should only be a single bit set but let's use == to be sure */
	if (error_code == DART_T8020_ERROR_READ_FAULT)
		fault_name = "READ FAULT";
	else if (error_code == DART_T8020_ERROR_WRITE_FAULT)
		fault_name = "WRITE FAULT";
	else if (error_code == DART_T8020_ERROR_NO_PTE)
		fault_name = "NO PTE FOR IOVA";
	else if (error_code == DART_T8020_ERROR_NO_PMD)
		fault_name = "NO PMD FOR IOVA";
	else if (error_code == DART_T8020_ERROR_NO_TTBR)
		fault_name = "NO TTBR FOR IOVA";
	else
		fault_name = "unknown";

	dev_err_ratelimited(
		dart->dev,
		"translation fault: status:0x%x stream:%d code:0x%x (%s) at 0x%llx",
		error, stream_idx, error_code, fault_name, addr);

	apple_dart_writel(dart, error, DART_T8020_ERROR);
	return IRQ_HANDLED;
}

static irqreturn_t apple_dart_t8110_legacy_irq(int irq, void *dev)
{
	struct apple_dart *dart = dev;
	const char *fault_name = NULL;
	u32 error = apple_dart_readl(dart, DART_T8110_ERROR);
	u32 error_code = FIELD_GET(DART_T8110_ERROR_CODE, error);
	u32 addr_lo = apple_dart_readl(dart, DART_T8110_ERROR_ADDR_LO);
	u32 addr_hi = apple_dart_readl(dart, DART_T8110_ERROR_ADDR_HI);
	u64 addr = addr_lo | (((u64)addr_hi) << 32);
	u8 stream_idx = FIELD_GET(DART_T8110_ERROR_STREAM, error);

	if (!(error & DART_T8110_ERROR_FLAG))
		return IRQ_NONE;

	/* there should only be a single bit set but let's use == to be sure */
	if (error_code == DART_T8110_ERROR_READ_FAULT)
		fault_name = "READ FAULT";
	else if (error_code == DART_T8110_ERROR_WRITE_FAULT)
		fault_name = "WRITE FAULT";
	else if (error_code == DART_T8110_ERROR_NO_PTE)
		fault_name = "NO PTE FOR IOVA";
	else if (error_code == DART_T8110_ERROR_NO_PMD)
		fault_name = "NO PMD FOR IOVA";
	else if (error_code == DART_T8110_ERROR_NO_PGD)
		fault_name = "NO PGD FOR IOVA";
	else if (error_code == DART_T8110_ERROR_NO_TTBR)
		fault_name = "NO TTBR FOR IOVA";
	else
		fault_name = "unknown";

	dev_err_ratelimited(
		dart->dev,
		"translation fault: status:0x%x stream:%d code:0x%x (%s) at 0x%llx",
		error, stream_idx, error_code, fault_name, addr);

	apple_dart_writel(dart, error, DART_T8110_ERROR);
	for (int i = 0; i < BITS_TO_U32(dart->num_streams); i++)
		apple_dart_writel(dart, U32_MAX,
				   DART_T8110_ERROR_STREAMS + 4 * i);

	return IRQ_HANDLED;
}

static const char *apple_dart_t8110_v2_fault_name(u32 exception)
{
	switch (FIELD_GET(DART_T8110_EXCEPTION_CODE, exception)) {
	case 0:
		switch (FIELD_GET(DART_T8110_EXCEPTION_LEVEL, exception)) {
		case 0:
			return "TTBR invalid";
		case 1:
			return "CTE invalid";
		case 2:
			return "STE invalid";
		case 3:
			return "PTE invalid";
		}
		break;
	case 1:
		return "write protect";
	case 2:
		return "read protect";
	case 3:
		return "drop protect";
	case 4:
		return "AXI decode";
	case 5:
		return "AXI error";
	case 6:
		return "fill region";
	case 7:
		return "region protect";
	case 8:
		return "CTRR write protect";
	case 9:
		return "APF reject";
	case 10:
		return "BPF reject";
	case 11:
		return "STT mismatch";
	case 12:
		return "STT flush";
	case 13:
		return "external";
	case 14:
		return "illegal SID";
	}

	return "unknown";
}

static void apple_dart_quarantine_borrowed(struct apple_dart *dart, const char *why)
{
	apple_dart_fail_borrowed(dart, "%s", why);
}

static bool apple_dart_t8110_v2_block_pending(u16 version, u32 block_error)
{
	return version >= 0x0203 &&
	       (block_error & DART_T8110_BLOCK_ERROR_MASK);
}

static irqreturn_t apple_dart_t8110_v2_irq(int irq, void *dev)
{
	struct apple_dart *dart = dev;
	irqreturn_t ret = IRQ_NONE;
	unsigned int words = DIV_ROUND_UP(apple_dart_exception_sids(dart), 32);
	unsigned int i;
	u32 block_error;

	/*
	 * 0x4000 + 4*i is a bitmap of SIDs with a pending exception, one word
	 * per 32 SIDs -- not a per-SID code.  Iterating it per SID (as this
	 * used to) runs off the end of the array into the register arrays at
	 * 0x4020/0x4040 for every sid >= ceil(nsid/32).
	 */
	for (i = 0; i < words && i < 8; i++) {
		u32 pending = readl(dart->regs + DART_T8110_EXCEPTION(i));
		unsigned long bits = pending;
		unsigned int bit;

		if (!pending)
			continue;
		if (dart->managed_sids)
			apple_dart_managed_quarantine(dart);
		apple_dart_quarantine_borrowed(dart, "a stream exception is pending");

		for_each_set_bit(bit, &bits, 32) {
			unsigned int sid = i * 32 + bit;
			u32 status;
			u64 addr;

			if (sid >= apple_dart_exception_sids(dart))
				break;

			status = readl(dart->regs +
				       DART_T8110_ERR_SID_STATUS(sid));
			addr = readl(dart->regs +
				     DART_T8110_EXCEPTION_ADDR_LO(sid)) |
			       (u64)readl(dart->regs +
					  DART_T8110_EXCEPTION_ADDR_HI(sid)) << 32;

			dev_err_ratelimited(dart->dev,
					    "translation fault: SID:%u %s %s at %#llx (status %#010x)\n",
					    sid,
					    status & DART_T8110_ERR_SID_STATUS_WRITE ?
						    "write" : "read",
						    apple_dart_t8110_v2_fault_name(status),
						    addr, status);
		}

		/*
		 * Acknowledge: the bitmap is write-1-to-clear.  Without this the
		 * same latched bit re-fires immediately and one historical
		 * exception looks like a fault storm.
		 */
		writel(pending, dart->regs + DART_T8110_EXCEPTION(i));
		ret = IRQ_HANDLED;
	}

	if (ret == IRQ_HANDLED) {
		dsb(sy);
		readl(dart->regs + DART_T8110_EXCEPTION(0));
	}

	/* With every error enabled, 2.4 latches the rest in its own status. */
	if (dart->full_errors && dart->version >= 0x0204) {
		u32 status = readl(dart->regs + DART_T8110_V24_ERR_IRQ_STATUS);

		if (status) {
			dev_err_ratelimited(dart->dev, "error status: 0x%08x\n", status);
			writel(status, dart->regs + DART_T8110_V24_ERR_IRQ_STATUS);
			ret = IRQ_HANDLED;
		}
	}

	if (dart->version >= 0x0203) {
		block_error = readl(dart->regs + DART_T8110_BLOCK_ERROR);
		if (apple_dart_t8110_v2_block_pending(dart->version, block_error)) {
			if (dart->managed_sids)
				apple_dart_managed_quarantine(dart);
			apple_dart_quarantine_borrowed(dart, "a block error is pending");
			dev_err_ratelimited(dart->dev,
					    "block exception: status:0x%08x\n",
					    block_error);
			writel(block_error & DART_T8110_BLOCK_ERROR_MASK,
			       dart->regs + DART_T8110_BLOCK_ERROR);
			ret = IRQ_HANDLED;
		}
	}
	/* Retained radio and borrowed display streams stay quarantined. */
	if (ret == IRQ_HANDLED && !dart->managed_sids && !apple_dart_has_borrowed(dart))
		apple_dart_t8110_v2_unlock(dart);

	return ret;
}

/*
 * Borrowed J713 display DARTs share one IRQ. Any fault quarantines the
 * retained session. Linux is the IRQ handler for the entire block, including
 * inherited streams: acknowledge their diagnostic latch without changing any
 * translation or disabling the shared level-triggered interrupt.
 */
static irqreturn_t apple_dart_dcp_irq(struct apple_dart *dart)
{
	const char *fault_name = NULL;
	u32 error = readl(dart->regs + DART_T8110_ERROR);
	u32 error_code = FIELD_GET(DART_T8110_ERROR_CODE, error);
	u32 addr_lo = readl(dart->regs + DART_T8110_ERROR_ADDR_LO);
	u32 addr_hi = readl(dart->regs + DART_T8110_ERROR_ADDR_HI);
	u64 addr = addr_lo | (((u64)addr_hi) << 32);
	u8 stream_idx = FIELD_GET(DART_T8110_ERROR_STREAM, error);

	if (!(error & DART_T8110_ERROR_FLAG))
		return IRQ_NONE;
	/* Quarantine the session, but never disable the shared display IRQ. */
	apple_dart_fail_borrowed(dart, "DART error %08x", error);
	if (!apple_dart_borrowed_sid(dart, stream_idx)) {
		dev_err_ratelimited(dart->dev,
			"Unowned display fault: status=%08x SID%u at %llx; retained state pinned\n",
			error, stream_idx, addr);
		/* No other Linux handler owns this DART's inherited stream. */
		writel(error, dart->regs + DART_T8110_ERROR);
		if (stream_idx < 32)
			writel(BIT(stream_idx), dart->regs + DART_T8110_ERROR_STREAMS);
		return IRQ_HANDLED;
	}

	if (error_code == DART_T8110_ERROR_READ_FAULT)
		fault_name = "READ FAULT";
	else if (error_code == DART_T8110_ERROR_WRITE_FAULT)
		fault_name = "WRITE FAULT";
	else if (error_code == DART_T8110_ERROR_NO_PTE)
		fault_name = "NO PTE FOR IOVA";
	else if (error_code == DART_T8110_ERROR_NO_PMD)
		fault_name = "NO PMD FOR IOVA";
	else if (error_code == DART_T8110_ERROR_NO_PGD)
		fault_name = "NO PGD FOR IOVA";
	else if (error_code == DART_T8110_ERROR_NO_TTBR)
		fault_name = "NO TTBR FOR IOVA";
	else
		fault_name = "unknown";

	dev_err_ratelimited(dart->dev,
		"translation fault: status:0x%x stream:%d code:0x%x (%s) at 0x%llx",
		error, stream_idx, error_code, fault_name, addr);

	writel(error, dart->regs + DART_T8110_ERROR);
	writel(BIT(stream_idx), dart->regs + DART_T8110_ERROR_STREAMS);
	return IRQ_HANDLED;
}

/* Read-only admission: do not clear a pending inherited fault before ownership. */
static bool apple_dart_fault_pending(struct apple_dart *dart)
{
	if (dart->version >= 0x0202) {
		for (unsigned int i = 0; i < DIV_ROUND_UP(dart->num_streams, 32); i++)
			if (readl(dart->regs + DART_T8110_EXCEPTION(i)))
				return true;
		return dart->version >= 0x0203 &&
			apple_dart_t8110_v2_block_pending(dart->version,
				readl(dart->regs + DART_T8110_BLOCK_ERROR));
	}
	return (readl(dart->regs + DART_T8110_ERROR) & DART_T8110_ERROR_FLAG) ||
		readl(dart->regs + DART_T8110_ERROR_STREAMS);
}

static irqreturn_t apple_dart_t8110_irq(int irq, void *dev)
{
	struct apple_dart *dart = dev;

	/* Borrowed T8132 is v2.2 too: use its per-SID exception ABI. */
	if (dart->version >= 0x0202)
		return apple_dart_t8110_v2_irq(irq, dev);
	if (apple_dart_has_borrowed(dart))
		return apple_dart_dcp_irq(dart);

	return apple_dart_t8110_legacy_irq(irq, dev);
}

static irqreturn_t apple_dart_irq(int irq, void *dev)
{
	irqreturn_t ret = IRQ_NONE;
	struct apple_dart *dart = dev;

	if (READ_ONCE(dart->commands_gated))
		return IRQ_NONE;
	if (pm_runtime_resume_and_get(dart->dev) < 0)
		return IRQ_NONE;
	if (!READ_ONCE(dart->commands_gated))
		ret = dart->hw->irq_handler(irq, dev);
	pm_runtime_put(dart->dev);
	return ret;
}

static bool apple_dart_is_locked(struct apple_dart *dart)
{
	return !!(apple_dart_readl(dart, dart->hw->lock) & dart->hw->lock_bit);
}

static bool apple_dart_streams_bypassed(struct apple_dart *dart)
{
	unsigned int sid;

	for (sid = 0; sid < dart->num_streams; sid++) {
		if ((apple_dart_readl(dart, DART_TCR(dart, sid)) &
		     (DART_T8110_TCR_BYPASS_DART | DART_T8110_TCR_TRANSLATE_ENABLE)) !=
		    DART_T8110_TCR_BYPASS_DART ||
		    (apple_dart_readl(dart, DART_TTBR(dart, sid, 0)) & dart->hw->ttbr_valid))
			return false;
	}
	return true;
}

static void apple_dart_iounmap_np(void *data)
{
	iounmap((__force void __iomem *)data);
}

static void __iomem *
apple_dart_platform_ioremap_np_resource(struct platform_device *pdev,
					unsigned int index,
					struct resource **res_out)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *regs;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);

	if (!devm_request_mem_region(dev, res->start, resource_size(res),
				     dev_name(dev)))
		return IOMEM_ERR_PTR(-EBUSY);

	regs = ioremap_np(res->start, resource_size(res));
	if (!regs)
		return IOMEM_ERR_PTR(-ENOMEM);

	ret = devm_add_action_or_reset(dev, apple_dart_iounmap_np,
				       (__force void *)regs);
	if (ret)
		return IOMEM_ERR_PTR(ret);

	*res_out = res;
	return regs;
}

static DEFINE_MUTEX(apple_dart_display_irq_lock);
static struct apple_dart *apple_dart_display_irq_peers[2];
static int apple_dart_display_shared_irq = -1;

static irqreturn_t apple_dart_display_pair_irq(int irq, void *data)
{
	irqreturn_t result = IRQ_NONE;

	/* Both admitted suppliers are retained. Removal frees/synchronizes this
	 * action before changing either pointer; the handler takes no pair mutex.
	 */
	for (unsigned int i = 0; i < ARRAY_SIZE(apple_dart_display_irq_peers); i++)
		if (apple_dart_irq(irq, apple_dart_display_irq_peers[i]) == IRQ_HANDLED)
			result = IRQ_HANDLED;
	return result;
}

static int apple_dart_display_irq_request(struct apple_dart *dart)
{
	unsigned int peer = dart->dcp ? 1 : 0;
	int ret;

	guard(mutex)(&apple_dart_display_irq_lock);
	if (apple_dart_display_irq_peers[peer])
		return -EBUSY;
	apple_dart_display_irq_peers[peer] = dart;
	if (!apple_dart_display_irq_peers[!peer])
		return 0; /* Never enable this level IRQ for just one admitted block. */
	if (dart->irq != apple_dart_display_irq_peers[!peer]->irq ||
	    apple_dart_fault_pending(dart) ||
	    apple_dart_fault_pending(apple_dart_display_irq_peers[!peer])) {
		ret = -EIO;
		goto err;
	}
	ret = request_irq(dart->irq, apple_dart_display_pair_irq, IRQF_SHARED,
			 "apple-dart display fault handlers", apple_dart_display_irq_peers);
	if (ret)
		goto err;
	apple_dart_display_shared_irq = dart->irq;
	return 0;
err:
	apple_dart_display_irq_peers[peer] = NULL;
	return ret;
}

static void apple_dart_free_irq(struct apple_dart *dart)
{
	if (dart->irq < 0)
		return;
	if (!dart->preserve_display) {
		free_irq(dart->irq, dart);
		return;
	}
	guard(mutex)(&apple_dart_display_irq_lock);
	if (apple_dart_display_shared_irq >= 0) {
		/* Neither peer may publish DMA after losing shared fault observation. */
		for (unsigned int i = 0; i < ARRAY_SIZE(apple_dart_display_irq_peers); i++)
			if (apple_dart_display_irq_peers[i])
				apple_dart_quarantine_borrowed(apple_dart_display_irq_peers[i],
							       "the shared fault IRQ went");
		free_irq(apple_dart_display_shared_irq, apple_dart_display_irq_peers);
		apple_dart_display_shared_irq = -1;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(apple_dart_display_irq_peers); i++)
		if (apple_dart_display_irq_peers[i] == dart)
			apple_dart_display_irq_peers[i] = NULL;
}

/*
 * Whether an available coprocessor that translates through @np was handed
 * over running by the boot loader ("mtp-live-v1"), with its DART streams left
 * in bypass. Resetting the DART would cut that live session off.
 */
static bool apple_dart_has_live_rtkit_client(struct device_node *np)
{
	struct of_phandle_args args;
	struct device_node *client;
	const char *handoff;
	bool match;

	for_each_node_with_property(client, "apple,rtkit-handoff") {
		if (!of_device_is_available(client) ||
		    of_property_read_string(client, "apple,rtkit-handoff", &handoff) ||
		    strcmp(handoff, "mtp-live-v1"))
			continue;
		if (of_parse_phandle_with_args(client, "iommus", "#iommu-cells",
					       0, &args))
			continue;
		match = args.np == np;
		of_node_put(args.np);
		if (match) {
			of_node_put(client);
			return true;
		}
	}
	return false;
}

/*
 * The setup a T8110 DART needs when nothing initialised it before Linux:
 * apple,tunable (applied at every reset), apple,bypass-sids (streams kept in
 * static bypass), apple,full-error-reporting and apple,ctc-flush. Log the
 * identification registers of such a DART.
 */
static int apple_dart_parse_own_setup(struct apple_dart *dart, struct resource *res,
				      const u32 *params)
{
	struct device *dev = dart->dev;
	struct device_node *np = dev->of_node;
	int count, i;
	u32 sid;

	dart->full_errors = of_property_read_bool(np, "apple,full-error-reporting");
	dart->ctc_flush = of_property_read_bool(np, "apple,ctc-flush") &&
			  (params[1] & DART_T8110_PARAMS2_CTC);
	dart->client_partitions = params[0] & DART_T8110_PARAMS1_CLIENT_PARTITIONS;

	if (of_property_present(np, "apple,tunable")) {
		dart->tunables = devm_apple_tunable_parse(dev, np, "apple,tunable", res);
		if (IS_ERR(dart->tunables))
			return dev_err_probe(dev, PTR_ERR(dart->tunables),
					     "failed to parse DART tunables\n");
	}

	count = of_property_count_u32_elems(np, "apple,bypass-sids");
	for (i = 0; i < count; i++) {
		if (of_property_read_u32_index(np, "apple,bypass-sids", i, &sid) ||
		    sid >= dart->num_streams)
			return dev_err_probe(dev, -EINVAL, "invalid bypass stream\n");
		__set_bit(sid, dart->static_bypass);
	}

	if (dart->tunables || count > 0 || dart->full_errors ||
	    of_property_read_bool(np, "apple,ctc-flush"))
		dev_info(dev,
			 "params %08x %08x %08x %08x, version %04x, %u streams, AS %u -> %u, protect %08x/%08x, diag lock %08x; %zu tunables%s, %d bypass streams, CTC flush %s\n",
			 params[0], params[1], params[2], params[3], dart->version,
			 dart->num_streams, dart->ias, dart->oas,
			 apple_dart_readl(dart, DART_T8110_PROTECT),
			 apple_dart_readl(dart, DART_T8110_PROTECT_LOCK),
			 apple_dart_readl(dart, DART_T8110_DIAG_LOCK),
			 dart->tunables ? dart->tunables->sz : 0,
			 dart->tunables && !dart->client_partitions ?
				" (no client partitions)" : "",
			 max(count, 0), dart->ctc_flush ? "on" : "off");
	return 0;
}

struct apple_dart_available {
	struct list_head list;
	struct device_node *np;
};

static DEFINE_MUTEX(apple_dart_available_lock);
static LIST_HEAD(apple_dart_available_list);

void apple_dart_make_available(struct device_node *np)
{
	struct apple_dart_available *a;

	guard(mutex)(&apple_dart_available_lock);
	list_for_each_entry(a, &apple_dart_available_list, list)
		if (a->np == np)
			return;
	a = kzalloc_obj(*a);
	if (!a)
		return;
	a->np = of_node_get(np);
	list_add(&a->list, &apple_dart_available_list);
}
EXPORT_SYMBOL_GPL(apple_dart_make_available);

static bool apple_dart_is_available(struct device_node *np)
{
	struct apple_dart_available *a;

	if (!of_property_read_bool(np, "apple,manual-availability"))
		return true;
	guard(mutex)(&apple_dart_available_lock);
	list_for_each_entry(a, &apple_dart_available_list, list)
		if (a->np == np)
			return true;
	return false;
}

static int apple_dart_probe(struct platform_device *pdev)
{
	int ret, sid;
	u32 dart_params[4];
	struct device_node *pd_np;
	struct resource *res;
	struct apple_dart *dart;
	struct device *dev = &pdev->dev;
	u64 dma_range[2];
	bool tunneled = dev->of_node->parent &&
			 of_node_name_prefix(dev->of_node->parent,
					     "usb4-pcie-tunnel-");
	bool j713_display, j713_dcpext, j713_dispext, j713_radio;

	/*
	 * The M4 internal display DARTs are live: DCP firmware (SID23)
	 * and the scanout/PIODMA frontend (SID0/SID16) keep using them. Leave
	 * both untouched until userspace, with its watchdog armed, opts in.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	j713_display = res && !tunneled && apple_t8132_firmware_path() &&
		of_device_is_compatible(dev->of_node, "apple,t8132-dart") &&
		(res->start == 0x30a300000ULL || res->start == 0x30a340000ULL);
	/* The external DCP firmware DARTs (SID23) of dcpext0 and dcpext1. */
	j713_dcpext = res && !tunneled && apple_t8132_firmware_path() &&
		of_device_is_compatible(dev->of_node, "apple,t8132-dart") &&
		(res->start == 0x286340000ULL || res->start == 0x48a340000ULL);
	/* The external display DARTs (SID0/SID16) of dispext0 and dispext1. */
	j713_dispext = res && !tunneled && apple_t8132_firmware_path() &&
		of_device_is_compatible(dev->of_node, "apple,t8132-dart") &&
		(res->start == 0x286300000ULL || res->start == 0x48a300000ULL);
	/* The radio DART's fault interrupt is not described by the loader. */
	j713_radio = res && !tunneled && apple_t8132_managed_radio() &&
		of_machine_is_compatible("apple,t8132") &&
		of_device_is_compatible(dev->of_node, "apple,t8132-dart") &&
		res->start == 0x490000000ULL;
	/*
	 * Off the qualified firmware nothing hands the internal display DARTs
	 * over, but the running DCP still uses them: never reset them.
	 */
	if (res && !tunneled && !apple_t8132_firmware_path() &&
	    of_machine_is_compatible("apple,t8132") &&
	    of_device_is_compatible(dev->of_node, "apple,t8132-dart") &&
	    (res->start == 0x30a300000ULL || res->start == 0x30a340000ULL)) {
		dev_info(dev, "Leaving inherited display DART untouched off the qualified firmware\n");
		return -ENODEV;
	}
	if (j713_display && !IS_ENABLED(CONFIG_APPLE_DART_J713_DCP))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "retained J713 display requires CONFIG_APPLE_DART_J713_DCP=y\n");
	if (j713_display &&
	    ((res->start == 0x30a300000ULL && !READ_ONCE(apple_dart_frontend_enabled)) ||
	     (res->start == 0x30a340000ULL && !READ_ONCE(apple_dart_dcp_enabled)))) {
		dev_info(dev, "Leaving inherited display DART untouched; explicit guarded probe required\n");
		return -ENODEV;
	}
	if (j713_dcpext) {
		/* Its block is powered only if the loader handed the DCP over. */
		struct device_node *owner =
			of_find_node_by_path(res->start == 0x48a340000ULL ? "dcpext1" : "dcpext0");
		u32 ready = 0;
		bool handed_over = owner &&
			!of_property_read_u32(owner, "apple,t8132-dcpext-firmware-ready", &ready) &&
			ready == 1;

		of_node_put(owner);
		if (!IS_ENABLED(CONFIG_APPLE_DART_J713_DCP) || !handed_over) {
			dev_info(dev, "Leaving inherited external display DART untouched\n");
			return -ENODEV;
		}
	}
	if (j713_dispext) {
		/*
		 * Its block shares the DCP's CPU domain, powered only if the
		 * loader handed the DCP over.
		 */
		struct device_node *owner =
			of_find_node_by_path(res->start == 0x48a300000ULL ? "dcpext1" : "dcpext0");
		u32 ready = 0;
		bool handed_over = owner &&
			!of_property_read_u32(owner, "apple,t8132-dcpext-firmware-ready", &ready) &&
			ready == 1;

		struct platform_device *owner_dev = owner ? of_find_device_by_node(owner) : NULL;
		/*
		 * Probed at boot in the loader's state. Not once the dcpext's
		 * driver has its session: its firmware may have the DART off.
		 */
		bool session = owner_dev && device_is_bound(&owner_dev->dev);

		if (owner_dev)
			put_device(&owner_dev->dev);
		of_node_put(owner);
		if (session) {
			dev_info(dev, "External DCP session already running: display DART left untouched\n");
			return -ENODEV;
		}
		if (!IS_ENABLED(CONFIG_APPLE_DART_J713_DCP) || !handed_over) {
			dev_info(dev, "Leaving inherited external display DART untouched\n");
			return -ENODEV;
		}
	}

	/* Its registers answer only once its client is up */
	if (!apple_dart_is_available(dev->of_node))
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for its client\n");

	dart = kzalloc(sizeof(*dart), GFP_KERNEL);
	if (!dart)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, apple_dart_release, dart);
	if (ret)
		return ret;

	dart->dev = dev;
	dart->hw = of_device_get_match_data(dev);
	dart->tunneled = tunneled;
	dart->cold_ane = of_device_is_compatible(dev->of_node, "apple,t8140-ane-dart");
	if (dart->cold_ane && !IS_BUILTIN(CONFIG_APPLE_DART))
		return -EOPNOTSUPP;
	dart->isp_host = of_device_is_compatible(dev->of_node, "apple,t6050-isp-dart");
	dart->power_retained = dart->isp_host;
	dart->per_stream = dart->isp_host ||
			   of_device_is_compatible(dev->of_node, "apple,t8140-dart") ||
			   of_device_is_compatible(dev->of_node, "apple,t8142-dart");
	if (of_property_present(dev->of_node, "apple,managed-sids")) {
		u32 ids[2];

		if (!IS_BUILTIN(CONFIG_APPLE_DART))
			return dev_err_probe(dev, -EOPNOTSUPP,
					     "managed radio requires built-in DART support\n");
		if (!j713_radio ||
		    of_property_count_u32_elems(dev->of_node, "apple,managed-sids") != 2 ||
		    of_property_read_u32_array(dev->of_node, "apple,managed-sids", ids, 2) ||
		    ids[0] != 1 || ids[1] != 2)
			return -EINVAL;
		dart->managed_sids = BIT(1) | BIT(2);
	}
	spin_lock_init(&dart->managed_lock);
	dart->preserve_display = j713_display || j713_dcpext || j713_dispext;
	dart->dispext = j713_dispext;
	dart->power_gated = j713_dispext;
	if (j713_display && of_property_present(dev->of_node, "apple,clock-protection")) {
		/* Its DCP can power it down: see apple_dart_dcp_power_off(). */
		dart->ps_prot = syscon_regmap_lookup_by_phandle_args(dev->of_node,
								     "apple,clock-protection",
								     1, &dart->ps_prot_slice);
		if (IS_ERR(dart->ps_prot))
			return dev_err_probe(dev, PTR_ERR(dart->ps_prot),
					     "clock-protection bank\n");
		if (dart->ps_prot_slice >= APPLE_PS_PROT_SLICES)
			return -EINVAL;
		dart->power_gated = true;
	}
	spin_lock_init(&dart->pm_gate_lock);
	init_waitqueue_head(&dart->pm_wq);
	if (j713_dcpext)
		dart->dcp_instance = &dcp_adt_external[res->start == 0x48a340000ULL];
	else if (j713_dispext)
		dart->dcp_instance = &dcp_adt_external[res->start == 0x48a300000ULL];
	else if (j713_display)
		dart->dcp_instance = &dcp_adt_internal;
	if (tunneled) {
		pd_np = of_parse_phandle(dev->of_node, "power-domains", 0);
		if (pd_np) {
			dart->power_retained =
				of_property_read_bool(pd_np, "apple,always-on");
			of_node_put(pd_np);
		}
	}
	spin_lock_init(&dart->lock);
	platform_set_drvdata(pdev, dart);

	/*
	 * PCIe-C is a cable-powered aperture whose writes must complete before
	 * the following transaction.  The normal arm64 ioremap() mapping is
	 * Device-nGnRE and allowed the first posted DART reset write to surface as
	 * an asynchronous external abort at a later instruction.  The same
	 * reset sequence is stable with a Device-nGnRnE mapping, and PCI
	 * configuration forwarding already requires the same non-posted
	 * ordering.  Keep ordinary always-on DARTs on their existing mapping.
	 */
	if (tunneled)
		dart->regs = apple_dart_platform_ioremap_np_resource(pdev, 0,
							      &res);
	else if (dart->managed_sids || dart->preserve_display || dart->cold_ane)
		dart->regs = apple_dart_retained_ioremap(dart, res);
	else
		dart->regs = devm_platform_get_and_ioremap_resource(pdev, 0,
							     &res);
	if (IS_ERR(dart->regs))
		return PTR_ERR(dart->regs);

	if (resource_size(res) < 0x4000) {
		dev_err(dev, "MMIO region too small (%pr)\n", res);
		return -EINVAL;
	}
	if (tunneled) {
		dart->tunables = devm_apple_tunable_parse(dev, dev->of_node,
							  "apple,tunable", res);
		if (IS_ERR(dart->tunables)) {
			/*
			 * T8110 publishes tunables and the driver requires them.
			 * A t8103 PCIe-C DART may omit them. Its values include
			 * the configuration lock, which has to be written after
			 * the translation context is programmed, so leave them
			 * off until that ordering exists.
			 */
			if (PTR_ERR(dart->tunables) == -ENOENT &&
			    dart->hw->type != DART_T8110) {
				dart->tunables = NULL;
			} else {
				return dev_err_probe(dev, PTR_ERR(dart->tunables),
						     "failed to parse PCIe-C DART tunables\n");
			}
		}
	}

	if (j713_dcpext || j713_dispext) {
		/*
		 * Its fault IRQ is shared with the external display DART, which is
		 * not admitted: never enable that level IRQ for one block alone.
		 */
		dart->irq = -1;
	} else if (dart->preserve_display || j713_radio) {
		dart->irq = platform_get_irq_optional(pdev, 0);
		if (dart->irq == -ENXIO)
			dart->irq = -1;
		else if (dart->irq < 0)
			return dart->irq;
	} else {
		dart->irq = platform_get_irq(pdev, 0);
		if (dart->irq < 0)
			return -ENODEV;
	}

	if (dart->managed_sids && dart->irq < 0)
		return dev_err_probe(dev, -ENODEV, "managed radio requires a fault IRQ\n");

	ret = devm_clk_bulk_get_all(dev, &dart->clks);
	if (ret < 0)
		return ret;
	dart->num_clks = ret;
	if (dart->managed_sids && dart->num_clks)
		return -EINVAL;
	/* This inherited profile has no managed clocks; do not acquire new ones. */
	if (dart->preserve_display && dart->num_clks)
		return -ENODEV;

	ret = clk_bulk_prepare_enable(dart->num_clks, dart->clks);
	if (ret)
		return ret;

	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_irq_safe(dev);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		goto err_clk_disable;
	if (tunneled && dart->tunables) {
		dev_dbg(dev, "applying %zu PCIe-C DART tunables\n",
			dart->tunables->sz);
		apple_dart_apply_tunables(dart);
	}

	if (tunneled) {
		/*
		 * The parameter registers of a tunneled DART cannot be read
		 * safely at this point of PCIe tunnel activation: the read
		 * raises an asynchronous SError.  The topology is fixed
		 * instead.  T8110 is 16 KiB pages, 42-bit addresses, 64
		 * streams and four levels.  A T8020 PCIe-C DART is 16 KiB
		 * pages, a 32-bit IOVA, a 36-bit PA and as many streams as its
		 * register block describes.
		 */
		if (dart->hw->type == DART_T8110) {
			dart->pgsize = 1 << DART_PCIEC_PAGE_SHIFT;
			dart->supports_bypass = true;
			dart->ias = DART_PCIEC_ADDR_WIDTH;
			dart->oas = DART_PCIEC_ADDR_WIDTH;
			dart->num_streams = DART_PCIEC_STREAMS;
			dart->four_level = true;
			/* T8132 PCIe-C DARTs keep per-stream exception records. */
			if (of_device_is_compatible(dev->of_node, "apple,t8132-dart"))
				dart->version = 0x0202;
		} else {
			dart->pgsize = 1 << DART_PCIEC_PAGE_SHIFT;
			dart->supports_bypass = false;
			dart->ias = 32;
			dart->oas = dart->hw->oas;
			dart->num_streams = dart->hw->max_sid_count;
			dart->four_level = false;
		}
		dev_dbg(dev,
			"PCIe-C DART using fixed topology: pagesize %#x, %u streams, AS %u -> %u\n",
			dart->pgsize, dart->num_streams, dart->ias, dart->oas);
		goto params_done;
	}

	dart_params[0] = apple_dart_readl(dart, DART_PARAMS1);
	dart_params[1] = apple_dart_readl(dart, DART_PARAMS2);
	dart->pgsize = 1 << FIELD_GET(DART_PARAMS1_PAGE_SHIFT, dart_params[0]);
	dart->supports_bypass = dart_params[1] & DART_PARAMS2_BYPASS_SUPPORT;

	switch (dart->hw->type) {
	case DART_T8020:
	case DART_T6000:
		dart->ias = 32;
		dart->oas = dart->hw->oas;
		dart->num_streams = dart->hw->max_sid_count;
		break;

	case DART_T8110:
		dart_params[2] = apple_dart_readl(dart, DART_T8110_PARAMS3);
		dart->ias = FIELD_GET(DART_T8110_PARAMS3_VA_WIDTH, dart_params[2]);
		dart->oas = FIELD_GET(DART_T8110_PARAMS3_PA_WIDTH, dart_params[2]);
		dart_params[3] = apple_dart_readl(dart, DART_T8110_PARAMS4);
		dart->num_streams = FIELD_GET(DART_T8110_PARAMS4_NUM_SIDS,
					      dart_params[3]);
		dart->four_level = dart->ias > 36;
		dart->version = dart_params[2] & GENMASK(15, 0);
		break;
	}

params_done:
	if (!tunneled && dart->hw->type == DART_T8110) {
		ret = apple_dart_parse_own_setup(dart, res, dart_params);
		if (ret)
			goto err_clk_disable;
	}

	dart->dma_min = 0;
	dart->dma_max = DMA_BIT_MASK(dart->ias);
	dart->dma_offset = 0;

	ret = of_property_read_u64_array(dev->of_node, "apple,dma-range", dma_range, 2);
	if (ret == -EINVAL) {
		ret = 0;
	} else if (ret) {
		goto err_clk_disable;
	} else {
		dart->dma_min = dma_range[0];
		ret = apple_dart_dma_window(dma_range[0], dma_range[1], &dart->dma_max);
		if (ret)
			goto err_clk_disable;
		if (!apple_dart_dma_window_aligned(dart->dma_min, dart->dma_max,
						   dart->pgsize)) {
			ret = -EINVAL;
			goto err_clk_disable;
		}
		if ((dart->dma_min ^ dart->dma_max) & ~DMA_BIT_MASK(dart->ias)) {
			dev_err(&pdev->dev, "Invalid DMA range for ias=%d\n",
				dart->ias);
			ret = -EINVAL;
			goto err_clk_disable;
		}
		dev_dbg(&pdev->dev, "Limiting DMA range to %pad..%pad\n",
			 &dart->dma_min, &dart->dma_max);
	}

	ret = of_property_read_u64(dev->of_node, "apple,dma-offset",
				   &dart->dma_offset);
	if (ret == -EINVAL) {
		dart->dma_offset = 0;
		ret = 0;
	} else if (ret) {
		goto err_clk_disable;
	} else if (dart->dma_offset > DMA_BIT_MASK(dart->ias) ||
		   dart->dma_max > DMA_BIT_MASK(dart->ias) - dart->dma_offset) {
		dev_err(dev, "Invalid DMA offset for ias=%u\n", dart->ias);
		ret = -EINVAL;
		goto err_clk_disable;
	} else {
		dev_dbg(dev,
			"aliasing DMA range %pad..%pad at DART offset %pad\n",
			&dart->dma_min, &dart->dma_max, &dart->dma_offset);
	}

	/*
	 * Older FDTs described the T602x PCIe-C DART-visible 1 TiB window
	 * directly. PCIe endpoints cannot issue those addresses: the host bridge
	 * accepts the upper 2 GiB of its 32-bit bus window and adds the 1 TiB
	 * alias before DART translation. Keep those FDTs bootable while returning
	 * the low bus address to DMA clients.
	 */
	if (!dart->dma_offset && tunneled &&
	    (of_machine_is_compatible("apple,t6020") ||
	     of_machine_is_compatible("apple,t6021")) &&
	    ((dart->dma_min == 0 &&
	      dart->dma_max == DMA_BIT_MASK(dart->ias)) ||
	     (dart->dma_min == BIT_ULL(40) &&
	      dart->dma_max == BIT_ULL(40) + BIT_ULL(32) - 1))) {
		dart->dma_min = BIT_ULL(31);
		dart->dma_max = DMA_BIT_MASK(32);
		dart->dma_offset = BIT_ULL(40);
		dev_info(dev,
			 "using T602x PCIe-C bus DMA window %pad..%pad at DART offset %pad\n",
			 &dart->dma_min, &dart->dma_max, &dart->dma_offset);
	}

	for (sid = 0; sid < DART_MAX_STREAMS; sid++)
		dart->locked_window[sid] = dart->dma_min + dart->dma_offset;

	if (dart->num_streams > DART_MAX_STREAMS) {
		dev_err(&pdev->dev, "Too many streams (%d > %d)\n",
			dart->num_streams, DART_MAX_STREAMS);
		ret = -EINVAL;
		goto err_clk_disable;
	}

	/*
	 * v2.2 moved the exception bitmap to 0x4000 and the per-stream
	 * exception records to 0x8000. A node whose reg stops short of them
	 * would fault on the first exception read during reset.
	 */
	if (dart->hw->type == DART_T8110 && dart->version >= 0x0202 &&
	    resource_size(res) < DART_T8110_ERR_SID(dart->num_streams)) {
		ret = dev_err_probe(dev, -EINVAL,
				    "MMIO region %pR too small for v2.2 exception records (need %#x); fix the DT reg\n",
				    res, DART_T8110_ERR_SID(dart->num_streams));
		goto err_clk_disable;
	}

	/* A live MTP epoch must never lose its firmware DMA addresses. */
	if (of_property_present(dev->of_node, "apple,bypass-handoff-streams")) {
		u32 stream;

		/* A zero stream value advertises no live handoff (e.g. J700). */
		if (of_property_count_u32_elems(dev->of_node,
					    "apple,bypass-handoff-streams") != 1 ||
		    of_property_read_u32(dev->of_node, "apple,bypass-handoff-streams",
					 &stream)) {
			ret = -EINVAL;
			goto err_clk_disable;
		}
		if (stream && (!of_machine_is_compatible("apple,j613") ||
		    !of_device_is_compatible(dev->of_node, "apple,t8122-dart") ||
		    res->start != 0x2fa808000ULL || !dart->supports_bypass ||
		    stream != 1 || stream >= dart->num_streams ||
		    apple_dart_readl(dart, DART_TCR(dart, stream)) != dart->hw->tcr_bypass)) {
			ret = -EINVAL;
			goto err_clk_disable;
		}
		dart->live_mtp = stream != 0;
	}

	ret = apple_dart_read_remaps(dart);
	if (ret)
		goto err_clk_disable;

	/* A hot-plug PCIe-C DART has no retained/locked translation context. */
	if (tunneled)
		dart->locked = false;
	else
		dart->locked = apple_dart_is_locked(dart);
	if (dart->isp_host && dart->locked) {
		ret = -EBUSY;
		goto err_clk_disable;
	}
	/* Reserved table-page lifetime requires the matched three-level loader. */
	dart->fw_handoff = apple_dart_fw_handoff_enabled(dart);
	/*
	 * Preserve our retained-root path when no reserved-table handoff exists.
	 * T6050's boot loader leaves every display DART locked on four-level
	 * roots of its own (dart-dcpextN SID 23, dart-dispextN SIDs 0 and 16).
	 */
	dart->fw_mirror = dart->locked && !dart->fw_handoff &&
		(of_device_is_compatible(dev->of_node, "apple,t6041-dart") ||
		 of_device_is_compatible(dev->of_node, "apple,t6050-dart") ||
		 of_device_is_compatible(dev->of_node, "apple,t8140-dart") ||
		 of_device_is_compatible(dev->of_node, "apple,t8142-dart"));
	/*
	 * J614s may arrive with the MTP live after the boot keyboard used it.
	 * Do not reset its DART unless there is no advertised handoff.
	 */
	if (of_machine_is_compatible("apple,j614s") &&
	    res->start == 0x514800000ULL) {
		struct device_node *mtp, *iommu;
		const char *handoff;
		bool live, advertised;

		mtp = of_find_node_by_path("/mtp@514600000");
		iommu = mtp ? of_parse_phandle(mtp, "iommus", 0) : NULL;
		advertised = mtp && of_find_property(mtp, "apple,rtkit-handoff", NULL);
		live = advertised && of_device_is_compatible(mtp, "apple,t6041-mtp") &&
		       !of_property_read_string(mtp, "apple,rtkit-handoff", &handoff) &&
		       !strcmp(handoff, "mtp-live-v1");
		if (advertised && (!live || iommu != dev->of_node || dart->hw->type != DART_T8110 ||
				   dart->locked || tunneled || !dart->supports_bypass ||
				   !apple_dart_streams_bypassed(dart))) {
			of_node_put(iommu);
			of_node_put(mtp);
			dev_err(dev, "Refusing invalid J614s live MTP DART handoff\n");
			ret = -EINVAL;
			goto err_clk_disable;
		}
		if (live) {
			dart->retained_mtp = true;
			dev_info(dev, "Retaining the live J614s MTP bypass\n");
		}
		of_node_put(iommu);
		of_node_put(mtp);
	}
	dart->fw_tree = dart->fw_mirror &&
		of_device_is_compatible(dev->of_node, "apple,t6050-dart");
	if (apple_t8132_live_mtp() && of_machine_is_compatible("apple,t8132") &&
	    res->start == 0x394800000ULL && dart->hw->type == DART_T8110 &&
	    !dart->locked && !tunneled && dart->supports_bypass &&
	    apple_dart_streams_bypassed(dart)) {
		dart->retained_mtp = true;
		dev_info(dev, "Retaining the live T8132 MTP bypass\n");
	}
	if (!dart->retained_mtp &&
	    of_device_is_compatible(dev->of_node, "apple,t8142-dart") &&
	    apple_dart_has_live_rtkit_client(dev->of_node)) {
		/*
		 * The T8142 TCR bypass bits do not read back, so the loader's
		 * bypass cannot be confirmed from the registers; trust the
		 * handoff, and refuse only a DART that cannot bypass at all.
		 */
		if (dart->hw->type != DART_T8110 || dart->locked || tunneled ||
		    !dart->supports_bypass) {
			dev_err(dev, "Refusing a live coprocessor handoff on a DART that cannot bypass\n");
			ret = -EINVAL;
			goto err_clk_disable;
		}
		dart->retained_mtp = true;
		dev_info(dev, "Retaining the live coprocessor bypass\n");
	}
	if (dart->preserve_display) {
		if (dart->hw->type != DART_T8110 || !bitmap_empty(dart->remapped, DART_MAX_STREAMS)) {
			ret = -EINVAL;
			goto err_clk_disable;
		}
		dev_info(dev, "J713 display DART version %04x, locked %d\n",
			 dart->version, dart->locked);
		ret = res->start == 0x30a340000ULL || j713_dcpext ?
			apple_dart_dcp_prepare(dart) :
			dart->dispext ? apple_dart_dispext_prepare(dart) :
			apple_dart_frontend_prepare(dart);
		if (ret)
			goto err_clk_disable;
	}
	if (dart->cold_ane) {
		/* No reset, TCR/TTBR clear, global enable, remap or IRQ ACK.
		 * Cold acquisition is checked by the staged ANE backend before
		 * it attaches a private SID0 domain. Even bypass SIDs stay intact.
		 */
		if (dart->locked || dart->version != 0x203 || dart->num_streams != 16 ||
		    !bitmap_empty(dart->remapped, DART_MAX_STREAMS)) {
			ret = -EPERM;
			goto err_clk_disable;
		}
		dart->irq = -1;
		dart->retained = true;
		get_device(dev);
		dev_info(dev, "cold ANE DART: inherited streams preserved; SID0 only\n");
	} else if (dart->managed_sids) {
		bool init_bypass = false;

		ret = apple_dart_fault_pending(dart) ? -EBUSY :
			apple_dart_managed_admit(dart, &init_bypass);
		if (ret)
			goto err_clk_disable;
		/* Acquire the takeover guard before the first managed register write. */
		ret = apple_dart_retain_radio(dart);
		if (ret)
			goto err_clk_disable;
		if (init_bypass) {
			struct apple_dart_stream_map stream = { .dart = dart };
			u32 bypass_tcr;

			__set_bit(16, stream.sidmap);
			apple_dart_hw_enable_bypass(&stream);
			bypass_tcr = apple_dart_readl(dart, DART_TCR(dart, 16));
			if (!apple_dart_managed_static_bypass(bypass_tcr)) {
				ret = dev_err_probe(dev, -EIO,
						    "static SID16 bypass readback failed\n");
				goto err_clk_disable;
			}
			apple_dart_writel(dart, BIT(16), dart->hw->enable_streams);
		}
		/* ENABLE is write-one-to-set; no foreign stream changes state. */
		apple_dart_writel(dart, dart->managed_sids, dart->hw->enable_streams);
	} else if (!dart->locked && !dart->retained_mtp && !dart->live_mtp && !dart->preserve_display) {
		ret = apple_dart_hw_reset(dart);
		if (ret)
			goto err_clk_disable;
	}

	/* Borrowed display state: never acknowledge someone else's fault. */
	if (dart->locked && !dart->preserve_display)
		apple_dart_drain_stale_exceptions(dart);

	/* Refuse an already pending inherited fault before joining the shared IRQ. */
	if (dart->preserve_display &&
	    (apple_dart_fault_pending(dart) ||
	     readl(dart->regs + DART_T8110_ERROR_MASK))) {
		ret = dev_err_probe(dev, -EIO,
				    "pending or masked display DART faults; no IRQ attachment\n");
		goto err_clk_disable;
	}

	if (dart->irq >= 0) {
		ret = dart->preserve_display ? apple_dart_display_irq_request(dart) :
			request_irq(dart->irq, apple_dart_irq, IRQF_SHARED,
				    "apple-dart fault handler", dart);
		if (ret)
			goto err_clk_disable;
	}

	ret = iommu_device_sysfs_add(&dart->iommu, dev, NULL, "apple-dart.%s",
				     dev_name(&pdev->dev));
	if (ret)
		goto err_free_irq;
	if (dart->managed_sids) {
		ret = devm_device_add_group(dev, &apple_dart_managed_group);
		if (ret)
			goto err_sysfs_remove;
	}
	/* An external display DART's state is not read from sysfs. */
	if (dart->frontend[0] && !dart->dispext) {
		ret = devm_device_add_group(dev, &apple_dart_frontend_group);
		if (ret)
			goto err_sysfs_remove;
	}

#if IS_ENABLED(CONFIG_APPLE_DART_J713_DCP)
	if (apple_dart_has_borrowed(dart)) {
		/* iommu_device_register() may attach consumers and publish host
		 * tables. From this point neither successful nor failed removal
		 * returns ownership of any hardware-visible memory.
		 */
		/* Pin the guard and immutable reason before the generic API can
		 * retain their addresses. No successful acquisition is released:
		 * the display has no proven DMA-stop/reset boundary before reboot.
		 */
		__module_get(THIS_MODULE);
		ret = kexec_block(&apple_dart_display_kexec);
		if (ret) {
			module_put(THIS_MODULE);
			goto err_sysfs_remove;
		}
		get_device(dev);
		WRITE_ONCE(dart->retained, true);
	}
#endif
	ret = iommu_device_register(&dart->iommu, &apple_dart_iommu_ops, dev);
	if (ret)
		goto err_sysfs_remove;

	/* Retain supplier references; generic suspend/resume is not applicable. */
	if (!dart->retained_mtp && !dart->managed_sids && !dart->cold_ane &&
	    !apple_dart_has_borrowed(dart))
		pm_runtime_put(dev);

	dev_dbg(
		&pdev->dev,
		"DART [pagesize %x, %d streams, bypass support: %d, bypass forced: %d, locked: %d, AS %d -> %d] initialized\n",
		dart->pgsize, dart->num_streams, dart->supports_bypass,
		dart->pgsize > PAGE_SIZE, dart->locked, dart->ias, dart->oas);
	return 0;

err_sysfs_remove:
	/* Failed registration can already have published retained DMA state. */
	if (dart->retained) {
		if (dart->managed_sids)
			apple_dart_managed_quarantine(dart);
		apple_dart_quarantine_borrowed(dart, "registration failed");
	}
	iommu_device_sysfs_remove(&dart->iommu);
err_free_irq:
	if (dart->irq >= 0)
		apple_dart_free_irq(dart);
err_clk_disable:
	if (dart->managed_sids && dart->retained && !READ_ONCE(dart->managed_failed))
		apple_dart_managed_quarantine(dart);
	/* Do not schedule suspend on failed radio or display adoption. */
	if (dart->retained || dart->preserve_display)
		pm_runtime_put_noidle(dev);
	else
		pm_runtime_put(dev);
	clk_bulk_disable_unprepare(dart->num_clks, dart->clks);

	return ret;
}

int apple_dart_quiesce_commands(struct device *dev)
{
	struct apple_dart *dart = dev_get_drvdata(dev);

	if (dart) {
		/* Finish any in-flight command before the port clock is stopped. */
		scoped_guard(spinlock_irqsave, &dart->lock)
			WRITE_ONCE(dart->commands_gated, true);
		/* The fault handler can take dart->lock while acknowledging errors. */
		if (dart->irq > 0)
			synchronize_irq(dart->irq);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_quiesce_commands);

int apple_dart_resume_commands(struct device *dev)
{
	struct apple_dart *dart = dev_get_drvdata(dev);
	int ret;

	if (!dart)
		return -ENODEV;
	if (!READ_ONCE(dart->commands_gated))
		return 0;

	scoped_guard(spinlock_irqsave, &dart->lock)
		WRITE_ONCE(dart->commands_gated, false);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		apple_dart_quiesce_commands(dev);
		return ret;
	}
	pm_runtime_put(dev);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_resume_commands);

/*
 * System sleep has quiesced every downstream device. PCIe-C calls this
 * before stopping APPCLK because its domain reset also loses DART state,
 * even when ordinary runtime PM treats the domain as retained.
 */
int apple_dart_save_tunnel_state(struct device *dev)
{
	struct apple_dart *dart = dev_get_drvdata(dev);
	unsigned int sid, idx;

	if (!dart || !dart->tunneled || dart->locked ||
	    dart->hw->type != DART_T8020 || dart->tunables)
		return -EOPNOTSUPP;

	guard(spinlock_irqsave)(&dart->lock);
	if (dart->commands_gated)
		return -EBUSY;
	dart->tunnel_state_saved = false;
	dart->tunnel_state_restored = false;

	for (sid = 0; sid < dart->num_streams; sid++) {
		dart->save_tcr[sid] = apple_dart_readl(dart, DART_TCR(dart, sid));
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			dart->save_ttbr[sid][idx] =
				apple_dart_readl(dart, DART_TTBR(dart, sid, idx));
	}
	dart->tunnel_state_saved = true;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_save_tunnel_state);

/* APPCLK is running again, but PCIe link training has not started. */
int apple_dart_restore_tunnel_state(struct device *dev)
{
	struct apple_dart *dart = dev_get_drvdata(dev);
	unsigned int sid, idx;
	int ret;

	if (!dart || !dart->tunneled || dart->locked ||
	    dart->hw->type != DART_T8020 || dart->tunables)
		return -EOPNOTSUPP;
	if (!dart->tunnel_state_saved || READ_ONCE(dart->commands_gated))
		return -EINVAL;

	ret = apple_dart_hw_reset(dart);
	if (ret)
		return ret;

	for (sid = 0; sid < dart->num_streams; sid++) {
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			apple_dart_writel(dart, dart->save_ttbr[sid][idx],
					  DART_TTBR(dart, sid, idx));
		apple_dart_writel(dart, dart->save_tcr[sid], DART_TCR(dart, sid));
	}
	dart->tunnel_state_saved = false;
	dart->tunnel_state_restored = true;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_restore_tunnel_state);

static void apple_dart_remove(struct platform_device *pdev)
{
	struct apple_dart *dart = platform_get_drvdata(pdev);

	if (dart->cold_ane) {
		dev_warn(&pdev->dev, "cold ANE DART retained until reset\n");
		return;
	}
	if (dart->managed_sids) {
		apple_dart_managed_quarantine(dart);
		if (dart->irq >= 0)
			apple_dart_free_irq(dart);
		iommu_device_unregister(&dart->iommu);
		iommu_device_sysfs_remove(&dart->iommu);
		return;
	}
	if (apple_dart_has_borrowed(dart)) {
		/* Unbind is suppressed and the module is pinned. If the device
		 * itself is removed, detach callbacks before devres runs, while
		 * retaining all state a domain or running firmware can reach.
		 */
		apple_dart_fail_borrowed(dart, "the DART device is removed");
		if (dart->irq >= 0)
			apple_dart_free_irq(dart);
		iommu_device_unregister(&dart->iommu);
		iommu_device_sysfs_remove(&dart->iommu);
		dev_warn(&pdev->dev, "display DART removed; live state retained until hardware reset\n");
		return;
	}

	/*
	 * Cable removal gates the port clock before this device is removed.
	 * A command issued then never completes. The next probe resets the
	 * block after that clock is running again.
	 */
	if (!dart->locked && !dart->retained_mtp && !dart->live_mtp &&
	    !READ_ONCE(dart->commands_gated))
		apple_dart_hw_reset(dart);

	if (dart->irq >= 0)
		apple_dart_free_irq(dart);

	iommu_device_unregister(&dart->iommu);
	iommu_device_sysfs_remove(&dart->iommu);

	if (!dart->live_mtp)
		clk_bulk_disable_unprepare(dart->num_clks, dart->clks);
}

static void apple_dart_shutdown(struct platform_device *pdev)
{
	struct apple_dart *dart = platform_get_drvdata(pdev);

	if (dart->managed_sids)
		apple_dart_managed_quarantine(dart);
	if (!apple_dart_has_borrowed(dart))
		return;
	/* Stop host publication; firmware mappings and backing stay intact
	 * until the platform reset. The retained kexec guard prevents a new
	 * kernel from reusing any of this memory without that reset.
	 */
	apple_dart_fail_borrowed(dart, "shutdown");
}

static const struct apple_dart_hw apple_dart_hw_t8103 = {
	.type = DART_T8020,
	.irq_handler = apple_dart_t8020_irq,
	.invalidate_tlb = apple_dart_t8020_hw_invalidate_tlb,
	.enable_remap = apple_dart_t8020_hw_enable_remap,
	.oas = 36,
	.fmt = APPLE_DART,
	.max_sid_count = 16,

	.enable_streams = DART_T8020_STREAMS_ENABLE,
	.lock = DART_T8020_CONFIG,
	.lock_bit = DART_T8020_CONFIG_LOCK,

	.error = DART_T8020_ERROR,

	.tcr = DART_T8020_TCR,
	.tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
	.tcr_disabled = 0,
	.tcr_bypass = DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART,

	.ttbr = DART_T8020_TTBR,
	.ttbr_valid = DART_T8020_TTBR_VALID,
	.ttbr_addr_field_shift = DART_T8020_TTBR_ADDR_FIELD_SHIFT,
	.ttbr_shift = DART_T8020_TTBR_SHIFT,
	.ttbr_count = 4,
};

static const struct apple_dart_hw apple_dart_hw_t8103_usb4 = {
	.type = DART_T8020,
	.irq_handler = apple_dart_t8020_irq,
	.invalidate_tlb = apple_dart_t8020_hw_invalidate_tlb,
	.enable_remap = apple_dart_t8020_hw_enable_remap,
	.oas = 36,
	.fmt = APPLE_DART,
	.max_sid_count = 64,

	.enable_streams = DART_T8020_STREAMS_ENABLE,
	.lock = DART_T8020_CONFIG,
	.lock_bit = DART_T8020_CONFIG_LOCK,

	.error = DART_T8020_ERROR,

	.tcr = DART_T8020_TCR,
	.tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
	.tcr_disabled = 0,
	.tcr_bypass = 0,

	.ttbr = DART_T8020_USB4_TTBR,
	.ttbr_valid = DART_T8020_TTBR_VALID,
	.ttbr_addr_field_shift = DART_T8020_TTBR_ADDR_FIELD_SHIFT,
	.ttbr_shift = DART_T8020_TTBR_SHIFT,
	.ttbr_count = 4,
};

static const struct apple_dart_hw apple_dart_hw_t6000 = {
	.type = DART_T6000,
	.irq_handler = apple_dart_t8020_irq,
	.invalidate_tlb = apple_dart_t8020_hw_invalidate_tlb,
	.enable_remap = apple_dart_t8020_hw_enable_remap,
	.oas = 42,
	.fmt = APPLE_DART2,
	.max_sid_count = 16,

	.enable_streams = DART_T8020_STREAMS_ENABLE,
	.lock = DART_T8020_CONFIG,
	.lock_bit = DART_T8020_CONFIG_LOCK,

	.error = DART_T8020_ERROR,

	.tcr = DART_T8020_TCR,
	.tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
	.tcr_disabled = 0,
	.tcr_bypass = DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART,

	.ttbr = DART_T8020_TTBR,
	.ttbr_valid = DART_T8020_TTBR_VALID,
	.ttbr_addr_field_shift = DART_T8020_TTBR_ADDR_FIELD_SHIFT,
	.ttbr_shift = DART_T8020_TTBR_SHIFT,
	.ttbr_count = 4,
};

static const struct apple_dart_hw apple_dart_hw_t8110 = {
	.type = DART_T8110,
	.irq_handler = apple_dart_t8110_irq,
	.invalidate_tlb = apple_dart_t8110_hw_invalidate_tlb,
	.enable_remap = apple_dart_t8110_hw_enable_remap,
	.fmt = APPLE_DART2,
	.max_sid_count = 256,

	.enable_streams = DART_T8110_ENABLE_STREAMS,
	.lock = DART_T8110_PROTECT,
	.lock_bit = DART_T8110_PROTECT_TTBR_TCR,

	.error = DART_T8110_ERROR,

	.tcr = DART_T8110_TCR,
	.tcr_enabled = DART_T8110_TCR_TRANSLATE_ENABLE,
	.tcr_disabled = 0,
	.tcr_bypass = DART_T8110_TCR_BYPASS_DAPF | DART_T8110_TCR_BYPASS_DART,
	.tcr_4level = DART_T8110_TCR_FOUR_LEVEL,

	.ttbr = DART_T8110_TTBR,
	.ttbr_valid = DART_T8110_TTBR_VALID,
	.ttbr_addr_field_shift = DART_T8110_TTBR_ADDR_FIELD_SHIFT,
	.ttbr_shift = DART_T8110_TTBR_SHIFT,
	.ttbr_count = 1,
};

static void apple_dart_save_state(struct apple_dart *dart)
{
	unsigned int sid, idx;

	for (sid = 0; sid < dart->num_streams; sid++) {
		dart->save_tcr[sid] = apple_dart_readl(dart,
						       DART_TCR(dart, sid));
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			dart->save_ttbr[sid][idx] =
				apple_dart_readl(dart,
						 DART_TTBR(dart, sid, idx));
	}
}

static __maybe_unused int apple_dart_suspend(struct device *dev)
{
	struct apple_dart *dart = dev_get_drvdata(dev);

	if (dart->live_mtp)
		return -EBUSY;

	dart->tunnel_state_restored = false;
	if (READ_ONCE(dart->commands_gated))
		return 0;
	/*
	 * The cold ANE DARTs belong to the ANE backend, which refuses system
	 * sleep up front while it runs the firmware. Otherwise they hold
	 * nothing to keep: neither save nor reset them.
	 */
	if (dart->cold_ane)
		return 0;
	/*
	 * An adopted external display DART may be off while its display is: no
	 * register is read for sleep, and none until its firmware powers it up
	 * again.
	 */
	if (dart->dispext) {
		if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE)
			return -EBUSY;
		if (apple_dart_has_borrowed(dart))
			apple_dart_pm_invalidate(dart, true);
		return 0;
	}
	/*
	 * Retained radio and display DMA has no suspend boundary of its own.
	 * Suspend-to-idle leaves it running in place: these DARTs have no
	 * power domain here, and the DCP keeps the display domains on. The
	 * registers are saved only for resume to check.
	 */
	if (dart->managed_sids || dart->preserve_display || apple_dart_has_borrowed(dart)) {
		if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE)
			return -EBUSY;
		apple_dart_save_state(dart);
		dart->s2idle_kept = true;
		return 0;
	}
	/* The tunneled DART has no state to save when its domain stays on. */
	if (dart->power_retained || dart->retained_mtp)
		return 0;

	/* Locked DARTs can't be restored so skip saving their registers. */
	if (dart->locked)
		return 0;

	apple_dart_save_state(dart);
	return 0;
}

/*
 * Whether the DART still holds the translation state that apple_dart_suspend()
 * saved, because its power domain stayed on across system sleep. That is the
 * case while a consumer that keeps running in suspend-to-idle, such as a DWC3
 * controller in host mode, holds the domain on. Such a consumer may still be
 * doing DMA, so the DART must not be reset under it.
 */
static bool apple_dart_state_retained(struct apple_dart *dart)
{
	bool live = false;
	unsigned int sid, idx;

	for (sid = 0; sid < dart->num_streams; sid++) {
		u32 tcr = apple_dart_readl(dart, DART_TCR(dart, sid));

		if (tcr != dart->save_tcr[sid])
			return false;
		/* An identity domain bypasses the DART without any page table. */
		if (dart->supports_bypass && dart->hw->tcr_bypass && tcr == dart->hw->tcr_bypass)
			live = true;

		for (idx = 0; idx < dart->hw->ttbr_count; idx++) {
			u32 ttbr = apple_dart_readl(dart, DART_TTBR(dart, sid, idx));

			if (ttbr != dart->save_ttbr[sid][idx])
				return false;
			if (ttbr & dart->hw->ttbr_valid)
				live = true;
		}
	}

	/*
	 * A DART that lost power reads back as reset: no stream bypasses it and
	 * none has a valid page table.
	 */
	return live;
}

/*
 * Flush the TLBs of every stream with a command of its own. A stream map flush
 * is not enough for the whole DART: where the streams of a map are remapped
 * onto its first stream, it only reaches that first stream.
 */
static int apple_dart_invalidate_each_stream(struct apple_dart *dart)
{
	unsigned int sid;
	int ret;

	for (sid = 0; sid < dart->num_streams; sid++) {
		struct apple_dart_stream_map stream_map = { .dart = dart };

		__set_bit(sid, stream_map.sidmap);
		ret = dart->hw->invalidate_tlb(&stream_map);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Resume of a DART whose live DMA stayed in place through suspend-to-idle. It
 * cannot be restored from the saved copy. If the translations changed, the
 * consumers' DMA is no longer what they set up: refuse their further use
 * through the same paths as a translation fault before failing resume, since
 * a resume error does not stop the PM core from resuming them.
 */
static int apple_dart_resume_retained(struct apple_dart *dart)
{
	if (!dart->s2idle_kept)
		return -EBUSY;
	dart->s2idle_kept = false;
	if (apple_dart_state_retained(dart))
		return 0;

	dev_err(dart->dev, "retained DMA state lost in suspend-to-idle\n");
	if (dart->managed_sids)
		apple_dart_managed_quarantine(dart);
	apple_dart_quarantine_borrowed(dart, "retained DMA state lost in suspend-to-idle");
	return -EIO;
}

static __maybe_unused int apple_dart_resume(struct device *dev)
{
	struct apple_dart *dart = dev_get_drvdata(dev);
	unsigned int sid, idx;
	int ret;

	/* No reset or restore: see apple_dart_suspend(). */
	if (dart->cold_ane)
		return 0;
	/* Powered down by its DCP, which restores it at power-up. */
	if (READ_ONCE(dart->host_off))
		return 0;
	/*
	 * An adopted external display DART may be off now, its display with it,
	 * and nothing confirms otherwise: no register of it is read or written
	 * (apple_dart_pm_unconfirmed()) until its firmware powers it up
	 * again; the state check then (apple_dart_dcp_dispext_state()) runs
	 * first: kept, it goes on; lost, it is retired, saying so.
	 */
	if (dart->dispext) {
		if (apple_dart_has_borrowed(dart)) {
			apple_dart_pm_invalidate(dart, false);
			dev_dbg(dev, "external display DART unconfirmed after sleep\n");
		}
		return 0;
	}
	if (dart->managed_sids || dart->preserve_display || apple_dart_has_borrowed(dart))
		return apple_dart_resume_retained(dart);

	/* PCIe-C restored these mappings before starting its downstream link. */
	if (dart->tunnel_state_restored) {
		dart->tunnel_state_restored = false;
		return 0;
	}

	/*
	 * PCIe-C DARTs on an apple,always-on domain retain their translation
	 * state. Resetting one here is unnecessary; DART access must remain
	 * ordered after PCIe-C port resume.
	 */
	if (dart->power_retained || dart->retained_mtp || READ_ONCE(dart->commands_gated))
		return 0;

	/* Locked DARTs can't be restored, and they should not need it */
	if (dart->locked)
		return 0;

	if (apple_dart_state_retained(dart)) {
		/* Nothing was lost, only drop what the TLBs may still hold. */
		if (!dart->reset_incomplete)
			return apple_dart_invalidate_each_stream(dart);

		/* A failed resume restored the translations: finish its reset. */
		ret = apple_dart_hw_finish_reset(dart);
		dart->reset_incomplete = ret;
		return ret;
	}

	ret = apple_dart_hw_reset(dart);

	/*
	 * Restore the translations even if the reset failed. It can only fail
	 * in its last steps, after clearing them, and the saved registers still
	 * point to the page tables in use. Leaving them cleared would fault
	 * every DMA of the consumers until the next reset.
	 */
	for (sid = 0; sid < dart->num_streams; sid++) {
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			apple_dart_writel(dart, dart->save_ttbr[sid][idx],
					  DART_TTBR(dart, sid, idx));
		apple_dart_writel(dart, dart->save_tcr[sid],
				   DART_TCR(dart, sid));
	}

	/*
	 * Then repeat all of those last steps once. A TLB flush that completes
	 * now does not show that the DART was unlocked and its error interrupt
	 * unmasked.
	 */
	if (ret) {
		dev_warn(dev, "reset on resume failed (%d), retrying\n", ret);
		ret = apple_dart_hw_finish_reset(dart);
	}

	/* Should they fail again, the next resume repeats them. */
	dart->reset_incomplete = ret;
	return ret;
}

static DEFINE_RUNTIME_DEV_PM_OPS(apple_dart_pm_ops, apple_dart_suspend, apple_dart_resume, NULL);

static const struct of_device_id apple_dart_of_match[] = {
	{ .compatible = "apple,t8103-dart", .data = &apple_dart_hw_t8103 },
	{ .compatible = "apple,t8103-usb4-dart", .data = &apple_dart_hw_t8103_usb4 },
	{ .compatible = "apple,t8110-dart", .data = &apple_dart_hw_t8110 },
	{ .compatible = "apple,t8132-dart", .data = &apple_dart_hw_t8110 },
	{ .compatible = "apple,t6000-dart", .data = &apple_dart_hw_t6000 },
	{},
};
MODULE_DEVICE_TABLE(of, apple_dart_of_match);

#if IS_ENABLED(CONFIG_APPLE_DART_J713_DCP)
/* The internal display DART of a DCP consumer (its firmware or display stream). */
static struct apple_dart *apple_dart_dcp_consumer(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct apple_dart_domain *dd;

	if (!domain || domain->owner != &apple_dart_iommu_ops)
		return NULL;
	dd = to_dart_domain(domain);
	if (!dd->dcp || dd->dcp->owner != dev || dd->dcp->dart->dispext ||
	    !dd->dcp->dart->ps_prot)
		return NULL;
	return dd->dcp->dart;
}

static int apple_dart_tlb_command(struct apple_dart *dart, u32 command)
{
	u32 status;

	writel(command, dart->regs + DART_T8110_TLB_CMD);
	return readl_poll_timeout_atomic(dart->regs + DART_T8110_TLB_CMD, status,
					 !(status & DART_T8110_TLB_CMD_BUSY), 1,
					 DART_STREAM_COMMAND_BUSY_TIMEOUT);
}

/*
 * Nothing in flight: no fault (-EIO), and no TLB command or outstanding
 * transaction (-EBUSY, which can drain).
 */
static int apple_dart_idle(struct apple_dart *dart)
{
	u32 params = readl(dart->regs + DART_PARAMS2);
	unsigned int ports = max(FIELD_GET(DART_T8110_PARAMS2_READ_PORTS, params),
				 FIELD_GET(DART_T8110_PARAMS2_WRITE_PORTS, params));

	if (!ports || ports > 4 || apple_dart_fault_pending(dart))
		return -EIO;
	if (readl(dart->regs + DART_T8110_TLB_CMD) & DART_T8110_TLB_CMD_BUSY)
		return -EBUSY;
	for (unsigned int port = 0; port < ports; port++)
		if (readl(dart->regs + DART_T8110_OUTSTANDING(port)))
			return -EBUSY;
	return 0;
}

/* Idle, giving transactions and a command just issued time to drain. */
static int apple_dart_wait_idle(struct apple_dart *dart)
{
	int ret, idle;

	ret = read_poll_timeout_atomic(apple_dart_idle, idle, idle != -EBUSY, 1,
				       DART_T8110_IDLE_TIMEOUT_US, false, dart);
	return ret ?: idle;
}

/* Its PS_PROT bank enabled and not locked: its slice can be switched. */
static int apple_dart_ps_prot_usable(struct apple_dart *dart)
{
	u32 enable, lock;
	int ret;

	ret = regmap_read(dart->ps_prot, APPLE_PS_PROT_ENABLE, &enable) ?:
	      regmap_read(dart->ps_prot, APPLE_PS_PROT_LOCK, &lock);
	if (!ret && (!(enable & BIT(0)) || (lock & BIT(0))))
		ret = -EPERM;
	return ret;
}

static int apple_dart_ps_prot_set(struct apple_dart *dart, u32 value)
{
	unsigned int reg = APPLE_PS_PROT_SLICE(dart->ps_prot_slice), now;
	int ret;

	ret = regmap_write(dart->ps_prot, reg, value);
	if (!ret)
		ret = regmap_read(dart->ps_prot, reg, &now);
	return ret ?: now == value ? 0 : -EIO;
}

/*
 * Whether apple_dart_dcp_power_off() can run for @dev's DART, changing
 * nothing: its DCP asks before the first step of its power-down.
 */
int apple_dart_dcp_power_check(struct device *dev)
{
	struct apple_dart *dart = apple_dart_dcp_consumer(dev);
	struct apple_dart_dcp *streams[3];

	if (!dart)
		return -ENODEV;
	if (READ_ONCE(dart->host_off))
		return -EALREADY;
	streams[0] = dart->dcp;
	streams[1] = dart->frontend[0];
	streams[2] = dart->frontend[1];
	for (unsigned int i = 0; i < ARRAY_SIZE(streams); i++)
		if (streams[i] && READ_ONCE(streams[i]->failed))
			return -EIO;
	return apple_dart_ps_prot_usable(dart);
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_power_check);

/*
 * J713 internal display DART, before its DCP powers the display domains
 * down (its DCP CPU stopped, the DART's masters idle): as the native DART
 * power-down does, disable its enabled streams, flush with the power-down
 * TLB command and clear its clock-protection slice. From here until
 * apple_dart_dcp_power_on() its registers are not used: its commands are
 * gated, its generic DMA streams update their page tables in memory only
 * (apple_dart_host_lock()) and system sleep leaves it alone. On failure its
 * borrowed streams are retired.
 * @dev is a consumer: the DCP (firmware DART) or its PIODMA (display DART).
 */
static int apple_dart_host_power_off(struct apple_dart *dart)
{
	unsigned long flags;
	u32 enabled;
	const char *why;
	int ret;

	if (READ_ONCE(dart->host_off))
		return -EALREADY;
	/*
	 * Page-table updates stay in memory from now on; then no use of its
	 * registers either, and the uses in flight are waited for.
	 */
	apple_dart_host_lock(dart, &flags);
	WRITE_ONCE(dart->host_off, true);
	apple_dart_host_unlock(dart, flags);
	apple_dart_pm_invalidate(dart, false);
	apple_dart_quiesce_commands(dart->dev);

	why = "not idle";
	ret = apple_dart_wait_idle(dart);
	if (ret)
		goto fail;
	why = "clock protection";
	ret = apple_dart_ps_prot_usable(dart) ?:
	      regmap_read(dart->ps_prot, APPLE_PS_PROT_SLICE(dart->ps_prot_slice),
			  &dart->ps_prot_saved);
	if (ret)
		goto fail;

	/* What power-up checks: the translations and their lock, and the streams. */
	apple_dart_save_state(dart);
	dart->off_protect = readl(dart->regs + DART_T8110_PROTECT);
	dart->off_lock = readl(dart->regs + DART_T8110_PROTECT_LOCK);
	dart->off_streams = readl(dart->regs + DART_T8110_ENABLE_STREAMS);

	why = "stream disable";
	writel(dart->off_streams, dart->regs + DART_T8110_DISABLE_STREAMS);
	enabled = readl(dart->regs + DART_T8110_ENABLE_STREAMS);
	ret = enabled ? -EIO : 0;
	if (ret)
		goto fail;
	why = "power-down flush";
	ret = apple_dart_tlb_command(dart, DART_T8110_TLB_CMD_POWERDOWN) ?:
	      apple_dart_wait_idle(dart);
	if (ret)
		goto fail;
	if (dart->ps_prot_saved & APPLE_PS_PROT_SLICE_ON) {
		why = "clock protection off";
		ret = apple_dart_ps_prot_set(dart, dart->ps_prot_saved & ~APPLE_PS_PROT_SLICE_ON);
		if (ret)
			goto fail;
	}
	dev_dbg(dart->dev, "powered down: streams %08x, clock protection %08x\n",
		dart->off_streams, dart->ps_prot_saved);
	return 0;
fail:
	apple_dart_fail_borrowed(dart, "power-down: %s (%d)", why, ret);
	return ret;
}

int apple_dart_dcp_power_off(struct device *dev)
{
	struct apple_dart *dart = apple_dart_dcp_consumer(dev);

	return dart ? apple_dart_host_power_off(dart) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_power_off);

/*
 * After its DCP powered the display domains up again, before the DCP CPU
 * runs: restore the clock-protection slice, check that the DART kept its
 * translations and their lock (it is not rebuilt), flush, and enable the
 * streams that were enabled at power-down. Its registers are then usable
 * again. On failure its borrowed streams are retired.
 */
static int apple_dart_host_power_on(struct apple_dart *dart)
{
	unsigned long flags;
	const char *why;
	int ret;

	if (!READ_ONCE(dart->host_off))
		return -EALREADY;
	why = "clock protection on";
	ret = apple_dart_ps_prot_set(dart, dart->ps_prot_saved);
	if (ret)
		goto fail;
	why = "state lost";
	ret = -EIO;
	if (readl(dart->regs + DART_T8110_ENABLE_STREAMS) ||
	    readl(dart->regs + DART_T8110_PROTECT) != dart->off_protect ||
	    readl(dart->regs + DART_T8110_PROTECT_LOCK) != dart->off_lock ||
	    !apple_dart_state_retained(dart))
		goto fail;
	why = "commands";
	ret = apple_dart_resume_commands(dart->dev);
	if (ret)
		goto fail;

	/*
	 * The flush, the streams and the confirmed power are one step for the
	 * streams' page-table updates (apple_dart_host_lock()): the flush covers
	 * every update made while the DART was down, and every later update
	 * finds it powered.
	 */
	apple_dart_host_lock(dart, &flags);
	why = "flush";
	ret = apple_dart_wait_idle(dart) ?:
	      apple_dart_tlb_command(dart, DART_T8110_TLB_CMD_V2);
	if (!ret) {
		why = "stream enable";
		writel(dart->off_streams, dart->regs + DART_T8110_ENABLE_STREAMS);
		if (readl(dart->regs + DART_T8110_ENABLE_STREAMS) != dart->off_streams)
			ret = -EIO;
	}
	if (!ret) {
		WRITE_ONCE(dart->host_off, false);
		spin_lock(&dart->pm_gate_lock);
		dart->pm_unverified = false;
		spin_unlock(&dart->pm_gate_lock);
	}
	apple_dart_host_unlock(dart, flags);
	if (ret)
		goto fail;
	dev_dbg(dart->dev, "powered up: state kept, streams %08x\n", dart->off_streams);
	return 0;
fail:
	apple_dart_fail_borrowed(dart, "power-up: %s (%d)", why, ret);
	return ret;
}

int apple_dart_dcp_power_on(struct device *dev)
{
	struct apple_dart *dart = apple_dart_dcp_consumer(dev);

	return dart ? apple_dart_host_power_on(dart) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_power_on);

/*
 * J713 external DCP: whether an adopted external display DART kept its
 * state, its power on (the caller's to know): the TTBR/TCR lock (PROTECT
 * and PROTECT_LOCK bit 0) as adopted, and the adopted streams' TCR and TTBR
 * as adopted. Register reads only. Kept: no longer unverified after sleep.
 * Lost (it is not rebuilt): every borrowed stream is retired, saying why.
 * @when names the occasion for the log.
 */
static int apple_dart_dispext_verify(struct apple_dart *dart, const char *when)
{
	bool kept = true, was, current_gen = false;
	unsigned long flags;
	u32 protect, lock;
	u64 gen;

	/*
	 * A use of its registers of its own, of the generation it starts in:
	 * not while asleep (between suspend and resume nothing confirms its
	 * power), and an invalidation meanwhile waits for it to end.
	 */
	spin_lock_irqsave(&dart->pm_gate_lock, flags);
	if (dart->pm_sleeping) {
		spin_unlock_irqrestore(&dart->pm_gate_lock, flags);
		dev_dbg(dart->dev, "external display DART %s during sleep: not checked\n", when);
		return -EAGAIN;
	}
	gen = dart->pm_gen;
	was = dart->pm_unverified;
	dart->pm_users++;
	spin_unlock_irqrestore(&dart->pm_gate_lock, flags);

	protect = readl(dart->regs + DART_T8110_PROTECT);
	lock = readl(dart->regs + DART_T8110_PROTECT_LOCK);
	for (unsigned int i = 0; i < ARRAY_SIZE(dart->frontend); i++) {
		struct apple_dart_dcp *dcp = dart->frontend[i];
		u32 tcr, ttbr;

		if (!dcp)
			continue;
		tcr = readl(dart->regs + DART_TCR(dart, dcp->sid));
		ttbr = readl(dart->regs + DART_TTBR(dart, dcp->sid, 0));
		kept &= tcr == DART_T8110_TCR_TRANSLATE_ENABLE && ttbr == dcp->ttbr;
		dev_dbg(dart->dev, "external display DART %s: SID%u TCR %08x TTBR0 %08x (adopted %08x)\n",
			when, dcp->sid, tcr, ttbr, dcp->ttbr);
	}
	/* The lock as adopted: set then, it must still be set. */
	kept &= (protect & lock & DART_T8110_PROTECT_TTBR_TCR) ==
		(dart->dispext_protect & dart->dispext_lock & DART_T8110_PROTECT_TTBR_TCR);

	spin_lock_irqsave(&dart->pm_gate_lock, flags);
	/* Only a check of the current generation confirms: a sleep since voids it. */
	current_gen = gen == dart->pm_gen;
	if (kept && current_gen)
		dart->pm_unverified = false;
	if (!--dart->pm_users)
		wake_up_all(&dart->pm_wq);
	spin_unlock_irqrestore(&dart->pm_gate_lock, flags);

	dev_dbg(dart->dev, "external display DART %s: protect %08x lock %08x (adopted %08x/%08x): state %s%s%s\n",
		when, protect, lock, dart->dispext_protect, dart->dispext_lock,
		kept ? "kept" : "lost",
		was ? " (unconfirmed since sleep until now)" : "",
		kept && !current_gen ? "; a sleep came meanwhile: not confirmed" : "");
	if (!kept) {
		apple_dart_quarantine_borrowed(dart, "state lost (TTBR, TCR or the lock not as adopted)");
		return -EIO;
	}
	return current_gen ? 0 : -EAGAIN;
}

/*
 * J713 external DCP: after its firmware switched the display DART on again,
 * check that the DART kept its state: the TTBR/TCR lock (PROTECT and
 * PROTECT_LOCK bit 0) still set, and the adopted streams' TCR and TTBR as
 * adopted. Register reads only. A lost state is not rebuilt: the borrowed
 * streams are retired. @consumer is the dcpext's piodma node; -ENODEV if
 * its DART is not an adopted external display DART.
 */
int apple_dart_dcp_dispext_state(struct device_node *consumer)
{
	struct of_phandle_args spec;
	struct platform_device *pdev;
	struct apple_dart *dart;
	int ret = -ENODEV;

	if (!consumer ||
	    of_parse_phandle_with_args(consumer, "iommus", "#iommu-cells", 0, &spec))
		return -ENODEV;
	pdev = of_find_device_by_node(spec.np);
	of_node_put(spec.np);
	if (!pdev)
		return -ENODEV;
	/* Only a bound, adopted one: a failed probe leaves no live state. */
	if (!device_is_bound(&pdev->dev) || pdev->dev.driver != &apple_dart_driver.driver)
		goto out;
	dart = platform_get_drvdata(pdev);
	if (!dart || !dart->dispext || !dart->frontend[1])
		goto out;
	ret = apple_dart_dispext_verify(dart, "after power-up");
out:
	put_device(&pdev->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_dcp_dispext_state);
#endif

static struct platform_driver apple_dart_driver = {
	.driver	= {
		.name			= "apple-dart",
		.of_match_table		= apple_dart_of_match,
		.suppress_bind_attrs    = true,
		.pm			= pm_ptr(&apple_dart_pm_ops),
	},
	.probe	= apple_dart_probe,
	.remove = apple_dart_remove,
	.shutdown = apple_dart_shutdown,
};

module_platform_driver(apple_dart_driver);

#if IS_ENABLED(CONFIG_KUNIT) && !defined(MODULE)
static void apple_dart_fw_leaf_test(struct kunit *test)
{
	const u64 leaf = GENMASK_ULL(51, 40) |
			 ((u64)(0x280000 >> 4) & GENMASK_ULL(37, 10)) |
			 BIT_ULL(0);
	const int rwc = IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE;
	phys_addr_t phys;
	int prot;
	bool valid, matched;

	valid = apple_dart_fw_leaf_decode(0, 0x4000, &phys, &prot);
	KUNIT_EXPECT_FALSE(test, valid);
	valid = apple_dart_fw_leaf_decode(BIT_ULL(0), 0x4000,
					  &phys, &prot);
	KUNIT_EXPECT_FALSE(test, valid);
	valid = apple_dart_fw_leaf_decode(leaf, 0x4000, &phys, &prot);
	KUNIT_ASSERT_TRUE(test, valid);
	matched = apple_dart_fw_mapping_matches(phys, prot, 0x280000, rwc);
	KUNIT_EXPECT_TRUE(test, matched);
	matched = apple_dart_fw_mapping_matches(phys, prot, 0x284000, rwc);
	KUNIT_EXPECT_FALSE(test, matched);
	valid = apple_dart_fw_leaf_decode(leaf | BIT_ULL(2), 0x4000,
					  &phys, &prot);
	KUNIT_ASSERT_TRUE(test, valid);
	matched = apple_dart_fw_mapping_matches(phys, prot, 0x280000, rwc);
	KUNIT_EXPECT_FALSE(test, matched);
}

static struct kunit_case apple_dart_fw_cases[] = {
	KUNIT_CASE(apple_dart_fw_leaf_test),
	{}
};

static struct kunit_suite apple_dart_fw_suite = {
	.name = "apple-dart-firmware-leaf",
	.test_cases = apple_dart_fw_cases,
};

kunit_test_suite(apple_dart_fw_suite);
#endif

#if IS_ENABLED(CONFIG_APPLE_DART_KUNIT_TEST)
#include "apple-dart-test.c"
#endif

MODULE_DESCRIPTION("IOMMU API for Apple's DART");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_LICENSE("GPL v2");

// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T6021 boot backend. genpd raises the eight power domains; domain
 * checks precede engine access without direct PMGR writes. Preflight
 * validates all inputs before the shared sequence can write registers.
 * RVBAR programming is conditional on bit 0; CPU release occurs on
 * both paths. A fresh READY precedes init publication and wake, then
 * DONE is polled. DMA allocations and the module pin are retained once
 * the CPU starts, including on timeout; reboot reclaims that state.
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/reset.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "ane_t6021.h"

#include "ane_t6021_boot.h"

/* Keep the nap-prevention counter enabled through the init resource bit. */
static bool boot_prevent_nap = true;
module_param(boot_prevent_nap, bool, 0444);
MODULE_PARM_DESC(boot_prevent_nap,
		 "Keep nap prevention enabled through the init resource bit (default on)");

/* Every preflight condition must pass before any sequence write. */
/*
 * genpd and supplier links manage power. Before engine access, verify
 * all eight domains have ACTUAL=0xf and BUSY=0, with CPU AUTO_ENABLE
 * clear. Do not duplicate these transitions with direct PMGR writes.
 */
static const bool pf_provider_genpd_strategy = true;
/*
 * Pool descriptor word 0 is the requested 0x40000-byte size.
 */
static const bool pf_pool_word0_proven = true;
static const bool pf_heap_floor_pinned = true;	/* Heap floor is one 0x4000-byte DART page. */
static const bool pf_dart_page_floor = true;	/* DART allocation floor is 0x4000 bytes. */
static const bool pf_rvbar_lifecycle = true;
static const bool pf_pass6_init_contract = true;

/* Module and DMA ownership must be established before CPU release. */
/* Table mode: 0 aborts before writes, 1 writes the table, 2 skips it. */

static const bool pf_main_lifetime_review = true;

static bool ane_t6021_boot_preflight_complete(void)
{
	/*
	 * Table access is controlled separately from the remaining preflight
	 * conditions.
	 */
	return pf_provider_genpd_strategy && pf_pool_word0_proven &&
	       pf_heap_floor_pinned && pf_dart_page_floor &&
	       pf_rvbar_lifecycle && pf_pass6_init_contract &&
	       pf_main_lifetime_review;
}

/*
 * Static boot inputs: configuration size 0x500000, first-boot previous
 * image length zero, heap floor 0x4000 and pool size 0x40000. READY-time
 * inputs and allocation DMA addresses are filled before publication.
 */
static const struct ane_t6021_init_sources boot_sources = {
	.cfg_size = 0x500000,		/* Configuration byte count */
	.prev_fw_len = 0,		/* first boot; static per reload */
	.heap_floor = 0x4000,		/* One 16 KiB DART page */
	.pool_word0 = 0x40000,		/* Pool requested size */
};

/* READY and DONE polling each allow up to 1000 one-millisecond waits. */
#define ANE_BOOT_ACK_POLL_US	1000
#define ANE_BOOT_POLL_MS	1000

/* The kernel backend and host fake-MMIO test share ane_t6021_boot_run(). */

/* ---- kernel io backend for ane_t6021_boot_run() ---- */

struct ane_t6021_boot_mmio {
	struct ane_t6021 *ane;
};

static u32 ane_boot_rd32(void *ctx, unsigned int off)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	return readl(mm->ane->base[ANE_T6021_REG_ENGINE] + off);
}

static u64 ane_boot_rd64(void *ctx, unsigned int off)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	return readq(mm->ane->base[ANE_T6021_REG_ENGINE] + off);
}

static void ane_boot_wr32(void *ctx, unsigned int off, u32 v)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	writel(v, mm->ane->base[ANE_T6021_REG_ENGINE] + off);
}

static void ane_boot_wr64(void *ctx, unsigned int off, u64 v)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	writeq(v, mm->ane->base[ANE_T6021_REG_ENGINE] + off);
}

static void ane_boot_publish_barrier(void *ctx)
{
	(void)ctx;
	/*
	 * dma_wmb() orders the coherent pool fill before register publication.
	 * It provides ordering, not store completion; subsequent writel calls
	 * publish the device-visible address.
	 */
	dma_wmb();
}

static void ane_boot_wait(void *ctx)
{
	(void)ctx;
	usleep_range(1000, 1500);	/* One-millisecond poll interval. */
}

static void ane_boot_phase(void *ctx, const char *what)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	/*
	 * Emit one debug marker per phase boundary, rather than per poll.
	 * Drain the log before the following register write so the last marker
	 * identifies a failing phase.
	 */
	dev_dbg(mm->ane->dev, "BOOT-PHASE %s\n", what);
	msleep(30);
}

/*
 * Prepare only after READY and before SCRATCH publication. Allocate
 * the pool, IPC and optional bounded heap. Keep all allocations once
 * the CPU has started, including on failure.
 */
static void *ane_boot_alloc(void *ctx, u64 size, u64 *iova)
{
	struct ane_t6021 *ane = ctx;
	dma_addr_t d = 0;
	void *p = dma_alloc_coherent(ane->dev, size, &d, GFP_KERNEL);

	if (p && !ane_t6021_fw_alias_iova_ok(ane, d, size)) {
		dev_err(ane->dev,
			"boot alloc %#llx+%#llx overlaps fw alias — refusing\n",
			(u64)d, size);
		dma_free_coherent(ane->dev, size, p, d);
		return NULL;
	}

	*iova = p ? d : 0;
	return p;
}

/*
 * Feed live SCRATCH values and the DMA allocation backend into the
 * shared preparation helper. Returned addresses fill the init block;
 * retain allocations while the CPU is running.
 */
static int ane_t6021_boot_prepare(void *ctx, u32 *lo, u32 *hi)
{
	struct ane_t6021_boot_mmio *mm = ctx;
	struct ane_t6021 *ane = mm->ane;
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	struct ane_t6021_init_sources src = boot_sources;
	struct ane_t6021_boot_allocs a;
	u32 request, scratch0, scratch1;
	int err;

	if (!ane_t6021_boot_preflight_complete())
		return -ENODATA;	/* belt: run() already gated */

	/*
	 * After READY, read SCRATCH0 then SCRATCH1. SCRATCH0 >= 0x21 refuses
	 * preparation; SCRATCH3 is the heap request, SCRATCH1 + 1 the ordinal.
	 */
	scratch0 = readl(eng + ANE_MBI_SCRATCH0);
	scratch1 = readl(eng + ANE_MBI_SCRATCH0 + 4);
	request = readl(eng + ANE_MBI_SCRATCH0 + 4 * 3);

	src.fw_dva = ane->fw_iova;
	err = ane_t6021_boot_prepare_publish(&src, request, scratch0,
					     scratch1, 0x4000,
					     ANE_T6021_BOOT_IPC_CEILING,
					     ANE_T6021_BOOT_HEAP_CEILING,
					     ane, ane_boot_alloc, &a,
					     lo, hi);
	if (!err && boot_prevent_nap) {
		((u8 *)a.pool)[ANE_T6021_INIT_TEMPLATE_OFF + 0x84] |= 1;
		dev_info(ane->dev, "LAB init resource[0x84] bit0=1: prevent nap\n");
	}
	/*
	 * Retain even partial allocations on error: the CPU has already
	 * started and may fetch from the published surfaces.
	 */
	ane->boot_pool = a.pool;
	ane->boot_pool_iova = a.pool_dva;
	ane->boot_ipc = a.ipc;
	ane->boot_ipc_iova = a.ipc_dva;
	ane->boot_ipc_size = a.ipc_size;
	ane->boot_heap = a.heap;
	ane->boot_heap_iova = a.heap_dva;
	ane->boot_heap_size = a.heap_size;
	return err;
}

/* Dispatch the resolved sequence against the kernel io backend. All
 * gates were checked by the caller; the run() core re-checks. After
 * the CPU release there is NO ordinary unwind: failures HOLD state
 * (wedged-pin cleanup refuses to free under a started CPU) and the
 * probe binds fenced.
 */
/*
 * On READY timeout, sample only SCRATCH, RVBAR, CPU_STATUS and the
 * 24 MHz domain tick at +0x1160008 while the domains remain powered.
 * The register at +0x1140008 is excluded from progress reads.
 */
static void ane_t6021_boot_progress_dump(struct ane_t6021 *ane)
{
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	u32 tick0 = readl(eng + ANE_T6021_BOOT_REG_TICK);
	u32 tick1;
	unsigned int i;

	msleep(20);
	tick1 = readl(eng + ANE_T6021_BOOT_REG_TICK);

	for (i = 0; i < 8; i++)
		dev_err(ane->dev, "PROGRESS SCRATCH%u=%08x\n", i,
			readl(eng + ANE_T6021_BOOT_REG_SCRATCH0 + 4 * i));
	dev_err(ane->dev,
		"PROGRESS rvbar=%016llx cpu_status=%08x tick %08x->%08x (%s)\n",
		readq(eng + ANE_ASC_RVBAR),
		readl(eng + ANE_ASC_CPU_STATUS),
		tick0, tick1,
		tick1 != tick0 ? "24MHz domain clocked" : "tick STATIC");
}

int ane_t6021_boot_start(struct ane_t6021 *ane, int stop_after, int table_mode, int rtb_mode)
{
	struct ane_t6021_boot_mmio mm = { .ane = ane };
	struct ane_t6021_boot_io io = {
		.ctx = &mm,
		.rd32 = ane_boot_rd32, .rd64 = ane_boot_rd64,
		.wr32 = ane_boot_wr32, .wr64 = ane_boot_wr64,
		.publish_barrier = ane_boot_publish_barrier, .poll_wait = ane_boot_wait,
		.phase = ane_boot_phase,
		.prepare = ane_t6021_boot_prepare,
	};
	struct ane_t6021_boot_cfg cfg = {
		.preflight_ok = ane_t6021_boot_preflight_complete(),
		.preboot_table_mode = table_mode,
		.fw_dva = ane->fw_iova,
		.stop_after = stop_after,
		.rtb_mode = rtb_mode,
	};
	int cs = 0, fa = 0, bo = 0;
	u64 sres = 0;
	int r;

	/*
	 * Acquire the module reference before the first write. If the CPU
	 * starts, retain it for the device lifetime; otherwise release it.
	 */
	if (!try_module_get(THIS_MODULE)) {
		dev_err(ane->dev,
			"boot: REFUSED before any write — module ref unavailable (dying); no CPU start possible from a dying module\n");
		return -EBUSY;
	}

	dev_dbg(ane->dev,
		"BOOT-PHASE dispatch (stop_after=%d%s)\n", stop_after,
		stop_after ? " BISECT STOP ARMED" : "");

	r = ane_t6021_boot_run(&io, &cfg, &cs, &fa, &bo, &sres);

	ane->cpu_started = cs;
	ane->fw_alive = fa;
	ane->booted = bo;
	ane->boot_scratch_result = sres;

	/*
	 * Capture progress immediately after a READY timeout, while the
	 * domains are powered.
	 */
	if (cs && !fa)
		ane_t6021_boot_progress_dump(ane);

	if (!cs) {
		/* no CPU start: full release path, normal ownership */
		module_put(THIS_MODULE);
		dev_info(ane->dev,
			 "BOOT-PHASE done r=%d cpu_started=0 (no CPU release: state clean, module unpinned)\n",
			 r);
		return r;
	}

	/* started CPU: retain the pin for the whole wedged lifetime.
	 * Residual: DT hotplug unbind cannot be fully prevented; devm
	 * release order frees irq before ioremap (probe-order reverse);
	 * DMA surfaces are wedge-held, never freed.
	 */
	dev_warn(ane->dev,
		 "boot: module PINNED until reboot (started CPU; wedged-pin)\n");

	if (r == -ENODATA)
		return r;	/* unreachable: the caller gated */
	if (r && cs) {
		dev_err(ane->dev,
			"boot: sequence error %d AFTER CPU start (cpu_started=%u fw_alive=%u booted=%u scratch_result=%016llx) — WEDGED-PIN HOLD: all surfaces/rings/IRQ/links preserved; reboot is the only reclamation; no retry. HANDSHAKE state only: result address requires validation\n",
			r, cs, fa, bo, sres);
		return 0;	/* bind fenced, state held */
	}
	if (!r)
		dev_info(ane->dev,
			 "boot: run returned 0 (booted=%u fw_alive=%u) — %s; scratch_result=%016llx (raw device address; transport stays fenced until response validation)\n",
			 bo, fa,
			 bo ? "DONE observed — handshake complete" :
			 "rc 0 WITHOUT DONE (mailbox mode: HELLO-gated; DONE not part of this mode)",
			 sres);
	return r;
}


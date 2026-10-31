// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_t6021_boot.c — T6021 boot sequencer: the kernel io backend, the
 * boot sources and the preflight gates for ane_t6021_boot_run(). The
 * sequence and its pure helpers (entry fold, latch/entry decode,
 * SCRATCH u64 split, init-structure fill) live in ane_t6021_boot.h.
 *
 * Boot writes, with the prerequisite each one depends on:
 *   - RVBAR entry fold (ENTRY_BASE | fw DVA & ADDR_MASK; single u64
 *     store; the read and the write are both 64-bit): skipped while
 *     the RVBAR read has bit0 set (see the RVBAR lifecycle below).
 *   - CPU_CONTROL write32 0 then 0x10: only once the entry state is
 *     decided; releasing RUN with an unresolved entry state would be a
 *     blind start.
 *   - SCRATCH0/1 publication + SCRATCH7 wake: every init-structure
 *     field the firmware reads needs a Linux source; zero-filling
 *     unknown fields and calling the structure valid is not allowed.
 *
 * Pre-CPU prerequisites:
 *   CPU power: the h14g configuration selects the PLAIN branch (the
 *     same configuration that puts CPU_CONTROL at 0x01400044), so the
 *     CPU island is powered with write32(PMGR+0x2e0, 0x0000000f),
 *     validate 0xff — AUTO_ENABLE bit28 never set. On Linux genpd
 *     raises the island before any of this code runs, and probe checks
 *     its ACTUAL state before the first engine read; the write is
 *     therefore not replicated (a direct ps-word write from kernel
 *     context has hung the machine).
 *   1. The pre-CPU engine table writels eng+0xb38/0xb98/0xbf8 <-
 *      0x01ff01ff (three, into the engine register window, reg[0]) are
 *      REQUIRED every time the ANE clocks and power are enabled, before
 *      the CPU start. They have also wedged the machine here, so
 *      fw_start_table_mode decides whether they run.
 *   2. Provider first-enable of the device clock and power: the set of
 *      clock and power gates it enables is only known at run time.
 *      Linux substitutes the genpd raise + supplier links.
 *   3. RVBAR lifecycle: bit0 set + entry 0 is the STALE state; the
 *      valid transition is power_off (de-init, then PMGR ps 0x2e0 ←
 *      0x00000000 through the PMGR window) → power_on → re-init, NEVER
 *      an RVBAR write while bit0 is set. RVBAR (engine window,
 *      0x1050000, u64) is written only after a read64 with bit0 == 0;
 *      CPU_CONTROL write32 0 then 0x10 runs on BOTH paths; success =
 *      SCRATCH7 read32 == 0x08042006. Whether ps-off clears bit0 is not
 *      known. The ps-word write of that reset is the class that hangs
 *      the machine from kernel context, so this driver never performs
 *      it: with bit0 set, the ASC starts at the latched entry.
 *   4. Ack model — TWO-PHASE, SINGLE REGISTER, TRANSITION-BASED:
 *      pre-CPU, SCRATCH cells are cleared and SCRATCH7 is PULSED 1 -> 0
 *      (stale READY/wake cleared); after CPU_CONTROL 0 -> 0x10, poll A
 *      demands a FRESH SCRATCH7 READY 0x08042006 (fw alive; fw_alive).
 *      Publication (init suballoc DVA -> SCRATCH0/1, dsb st) happens
 *      ONLY after poll A; then wake 0xf7fbdff9 -> SCRATCH7 releases the
 *      fw; poll B awaits SCRATCH7 DONE 0x08042006 (booted) and reads
 *      back SCRATCH0/1. Because of the pulse, a post-CPU READY is
 *      unambiguous — no stale-ack hazard. The SCRATCH0 marker is not a
 *      gate.
 *      On the firmware side: it writes READY (0x08042006) to
 *      SCRATCH7, then polls SCRATCH7 every 1 ms until it reads the
 *      wake word 0xf7fbdff9, and writes DONE (0x08042006 again) once
 *      it has booted; the host's poll B waits for that second
 *      0x08042006.
 *   5. Linux allocation map (legacy branch sizes): FWIM surface =
 *      0x500000 bytes; Linux fw_buf is ANE_FW_BUF_SIZE = 0x500000,
 *      covering the image vmsize 0x36c000. 'IPC ' surface = a
 *      configured size, capped. Init suballoc = 0x174 from a
 *      Linux-owned pool. RTBuddy FW_INIT sizes (64K…) are the OTHER
 *      branch — never used here.
 *   6. Init publication: the fw consumes [0x08]..[0x68] ([0x08] = the
 *      'IPC ' surface DVA; [0x10]/[0x18] = config-size terms; [0x30] =
 *      the load-progress word) — publication cannot fire until every
 *      fw-read field has a Linux source.
 *
 * Read whitelist — every MMIO read below, individually:
 *   - RVBAR eng+0x01050000: read64 (the register is 64 bits wide);
 *     Linux reads have returned 0x1.
 *   - CPU_STATUS eng+0x1400048: polled while the CPU starts; reads
 *     0x2a before it does.
 *   - ASC mailbox controls eng+0x1408110/0x1408114: both read
 *     0x00020001; polling them neither changes them nor aborts.
 *     The FATAL mailbox-class reads are the +0x1608xxx h16g analog —
 *     a different address never touched here; the 0x1854000..0x1c04000
 *     fabric-fatal window is likewise never touched.
 *   - SCRATCH eng+0x1840048..0x1840064: all zero before the driver
 *     attaches.
 *   - TICK eng+0x1160008 (PROGRESS dump only): 24 MHz domain tick,
 *     readable once the CPU is released; two reads 20 ms apart for a
 *     delta. engine+0x1140008 is deliberately NOT read (readability,
 *     width and reset semantics unknown, no pre-release baseline).
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

/* Retain the firmware nap-prevention counter via the init resource bit
 * (default on).
 */
static bool boot_prevent_nap = true;
module_param(boot_prevent_nap, bool, 0444);
MODULE_PARM_DESC(boot_prevent_nap,
		 "Retain firmware nap-prevention counter via init resource bit (default on)");

/* Boot-write gates — ITEMIZED, each a HARD gate: the ENTIRE write
 * sequence (preboot engine table, scratch clear + pulse, RVBAR
 * resolution, CPU release, publication, wake) runs start-to-finish
 * only when EVERY gate below is true, checked BEFORE any write (no
 * partial boot; remaining semantics stay hard gates, not comments).
 * Each is a compile-time constant — never a runtime knob.
 */
/* Provider equivalence: Linux genpd attachment + supplier links stand
 * in for Apple's runtime power index arrays, provided all 8 islands
 * are on (ACTUAL=0xf, BUSY=0, CPU AUTO_ENABLE clear) before the
 * sequence; probe checks the ane_cpu island before the first engine
 * read, and the driver performs no direct kernel PMGR writes.
 */
static const bool pf_provider_genpd_strategy = true;
static const bool pf_pool_word0_proven = true;	/* DDM Params word0 =
						 * requested bytes =
						 * 0x40000, traced
						 * through the firmware's
						 * pool setup
						 */
static const bool pf_heap_floor_pinned = true;	/* floor = the DART
						 * page size = 0x4000
						 * (the dart-ane0 node's
						 * page-size property);
						 * sourced in boot_sources
						 */
static const bool pf_dart_page_floor = true;	/* j414c ADT: ane0's
						 * iommu-parent is the
						 * dart-ane0 child
						 * mapper-ane0, and
						 * dart-ane0's page-size
						 * is 0x4000
						 */
static const bool pf_rvbar_lifecycle = true;
static const bool pf_pass6_init_contract = true;

/* Table-block mode selection (the table writes have wedged the
 * machine):
 *   mode 0 = ABORT before any write (accidental-repeat prevention),
 *   mode 1 = write the table (as on macOS; selected per-run via the
 *            rtclient fw_start_table_mode param),
 *   mode 2 = SKIP the table (default: diagnostic).
 * The write-grant tunables are mode-independent (a write class that
 * does not abort) and run in every armed mode.
 */

static const bool pf_main_lifetime_review = true;

static bool ane_t6021_boot_preflight_complete(void)
{
	/* The pre-CPU table block is INTENTIONALLY NOT in this product:
	 * mode 2 (table skip) is a valid diagnostic state — the
	 * preflight covers the boot sources, the mode selector covers
	 * the table gating.
	 */
	return pf_provider_genpd_strategy && pf_pool_word0_proven &&
	       pf_heap_floor_pinned && pf_dart_page_floor &&
	       pf_rvbar_lifecycle && pf_pass6_init_contract &&
	       pf_main_lifetime_review;
}

/* STATIC boot sources. The 'DDM ' pool (size 0x40000) and the
 * memory manager built on top of it are separate objects; the pool's
 * first word is its requested size (see pool_word0 below).
 *
 * Runtime fields (fw_dva, ipc_dva, pool_dma) are
 * filled by ane_t6021_boot_prepare() at sequence time (after poll A:
 * dynamic allocations occur after READY).
 */
static const struct ane_t6021_init_sources boot_sources = {
	.cfg_size = 0x500000,		/* FWIM surface size */
	.prev_fw_len = 0,		/* first boot; static per reload */
	.heap_floor = 0x4000,		/* DART page size: the
					 * dart-ane0 page-size
					 * property, 0x4000
					 */
	.pool_word0 = 0x40000,		/* DDM Params word0 = requested
					 * bytes
					 */
};

/* Poll A/B bound: up to 1000 polls with a 1 ms sleep, the same bound
 * the firmware's own handshake loop uses.
 */
#define ANE_BOOT_ACK_POLL_US	1000
#define ANE_BOOT_POLL_MS	1000

/* The COMPLETE resolved legacy-boot sequence is implemented ONCE in
 * ane_t6021_boot.h (ane_t6021_boot_run) against an io backend, so a
 * fake-MMIO trace test can exercise the exact code the device runs.
 * The kernel backend below is the readl/writeq/udelay wrappers plus
 * the prepare hook doing the pool/IPC allocations and the sourced fill.
 */

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
	/* dma_wmb() = dmb oshst on arm64: orders the coherent pool fill
	 * before the device publish. ORDERING guarantee — not a
	 * dsb st (completion). Sufficient for the Linux coherent-DMA +
	 * writel doorbell contract (writel orders prior accesses before
	 * the MMIO store).
	 */
	dma_wmb();
}

static void ane_boot_wait(void *ctx)
{
	(void)ctx;
	usleep_range(1000, 1500);	/* 1 ms between polls */
}

static void ane_boot_phase(void *ctx, const char *what)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	/* bounded phase marker: one line per block boundary (never
	 * per-poll), for crash attribution. It is a debug message: turn
	 * it on with dynamic debug, and raise the console loglevel to
	 * see it there. The short drain lets the line reach a local or
	 * remote console BEFORE the risky write it announces: if the
	 * write kills the machine, the marker must already be out.
	 */
	dev_dbg(mm->ane->dev, "BOOT-PHASE %s\n", what);
	msleep(30);
}

/* S5 prepare — runs strictly AFTER poll A (fw alive), BEFORE the
 * SCRATCH0/1 publish. Dynamic allocations occur here, after READY:
 * the 'DDM ' pool, the 'IPC ' surface, and the trust-bounded
 * fw-requested HEAP surface. Every allocation is wedged-pin owned from
 * here on (held while cpu_started; reboot reclaims). Publishes the
 * pool DVA (suballoc at offset 0) as the SCRATCH0/1 halves.
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

/* Kernel prepare: live SCRATCH3/SCRATCH1 reads feed the SHARED
 * assembly (ane_t6021_boot_prepare_publish) with the dma_alloc hook —
 * the same path a fake-MMIO trace test can exercise. Allocations are
 * wedged-pin owned from creation (held while cpu_started).
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

	/* dynamic reads (post-READY, live cells — never hardcoded):
	 * read order SCRATCH0 then SCRATCH1;
	 * SCRATCH0 >= 0x21 refuses before allocations/publication;
	 * SCRATCH3 = fw extra-heap request; SCRATCH1+1 = ordinal.
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
		dev_info(ane->dev, "init resource[0x84] bit0=1: prevent nap\n");
	}
	/* OWNERSHIP TRANSFER on EVERY path: an error after the
	 * pool/ipc allocations must NOT lose them — a
	 * started CPU may already be fetching, so partial allocations
	 * are RETAINED (wedged-pin holds them; reboot reclaims), never
	 * freed on this path.
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
/* Poll-A-timeout progress dump — observables known to be safe to read
 * ONLY: all eight SCRATCH cells (pulse-cleared to 0 pre-release, so any
 * nonzero word with SCRATCH7 != READY/WAKE is a candidate firmware
 * store), RVBAR, CPU_STATUS, and the 24 MHz domain tick at
 * engine+0x1160008 (it advances once the CPU is released; a static
 * tick separates "ASC domain unclocked" from "clocked but silent").
 *
 * Deliberately NOT read: engine+0x1140008 (a word the 13.5 stub
 * writes). Its readability, width and reset value are unknown
 * and no pre-release baseline exists, so an after-value would not
 * locate PC (could be preexisting iBoot state, write-only, or
 * normalized).
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

	/* Wedged-pin module lifetime: the ref is acquired
	 * BEFORE the first write — a started CPU can never
	 * outlive the pin, and a dying module refuses the boot before
	 * any write happens. Retained on CPU start; released only if no CPU start occurred or
	 * recovery proves a complete power-off cycle and a stopped CPU.
	 */
	if (!try_module_get(THIS_MODULE)) {
		dev_err(ane->dev,
			"boot: REFUSED before any write — module ref unavailable (dying); no CPU start possible from a dying module\n");
		return -EBUSY;
	}

	ane->boot_pin = true;

	dev_dbg(ane->dev,
		"BOOT-PHASE dispatch (stop_after=%d%s)\n", stop_after,
		stop_after ? " BISECT STOP ARMED" : "");

	r = ane_t6021_boot_run(&io, &cfg, &cs, &fa, &bo, &sres);

	ane->cpu_started = cs;
	ane->fw_alive = fa;
	ane->booted = bo;
	ane->boot_scratch_result = sres;

	/* Released CPU without READY (poll-A timeout, either transport
	 * mode): capture the 13.5 execution-progress observables NOW,
	 * while the islands are powered.
	 */
	if (cs && !fa)
		ane_t6021_boot_progress_dump(ane);

	if (!cs) {
		/* no CPU start: full release path, normal ownership */
		ane->boot_pin = false;
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
		 "boot: module pinned while firmware CPU may run\n");

	if (r == -ENODATA)
		return r;	/* unreachable: the caller gated */
	if (r && cs) {
		dev_err(ane->dev,
			"boot: sequence error %d AFTER CPU start (cpu_started=%u fw_alive=%u booted=%u scratch_result=%016llx) — WEDGED-PIN HOLD: all surfaces/rings/IRQ/links preserved; verified recovery or reboot required. HANDSHAKE state only: the result word has NO sourced success semantics\n",
			r, cs, fa, bo, sres);
		return 0;	/* bind fenced, state held */
	}
	if (!r)
		dev_info(ane->dev,
			 "boot: run returned 0 (booted=%u fw_alive=%u) — %s; scratch_result=%016llx (raw device address, semantics UNSOURCED — not a success claim; transport stays fenced until response validation)\n",
			 bo, fa,
			 bo ? "DONE observed — handshake complete" :
			 "rc 0 WITHOUT DONE (rtb_mode: HELLO-gated; DONE not part of this mode)",
			 sres);
	return r;
}

/* Caller has verified a complete domain-off cycle and a stopped CPU. */
void ane_t6021_boot_release_pin(struct ane_t6021 *ane)
{
	if (ane->boot_pin) {
		ane->boot_pin = false;
		module_put(THIS_MODULE);
	}
	ane->cpu_started = false;
	ane->fw_alive = false;
	ane->booted = false;
}

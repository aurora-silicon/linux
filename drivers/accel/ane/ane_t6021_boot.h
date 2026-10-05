/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T6021 boot layout and sequencing helpers shared by the kernel and
 * host regression. RVBAR is u64; bit 0 set preserves the current entry.
 * Otherwise write ENTRY_BASE | (firmware DVA & ADDR_MASK). CPU_CONTROL
 * receives u32 writes of zero then 0x10 on either path.
 *
 * The 0x174-byte init allocation carries the firmware DVA at +0, count
 * 64 at +0x68 and a 256-byte template at +0x6c. Template +0xc0 is 4.
 * Publish its DVA as SCRATCH0 low32 then SCRATCH1 high32, after ordering
 * the coherent writes. Consumers supply types and memory accessors.
 */
#ifndef __ANE_T6021_BOOT_H__
#define __ANE_T6021_BOOT_H__

/*
 * RVBAR composition clears bits 0..10, 48 and 55, retaining bit 11.
 * The base supplies bit 0 and the fixed upper pattern.
 */
#define ANE_T6021_RVBAR_ENTRY_BASE	0x0081000000000001ULL
#define ANE_T6021_RVBAR_ADDR_MASK	0xff7efffffffff800ULL

/* CPU_CONTROL release order: write32 zero then 0x10 (RUN bit 4). */
#define ANE_T6021_CPU_RUN_RELEASE	0x10

/* SCRATCH7 wake request and acknowledgment values. */
/* Post-READY SCRATCH0 >= 0x21 refuses preparation before allocations. */
#define ANE_T6021_BOOT_IPC_REQ_THRESHOLD	0x21U

#define ANE_T6021_BOOT_WAKE_REQ		0xf7fbdff9U
#define ANE_T6021_BOOT_ACK		0x08042006U

/* Init allocation size and field offsets. */
#define ANE_T6021_INIT_STRUCT_SIZE	0x174
#define ANE_T6021_INIT_FW_DVA_OFF	0x00	/* u64, legacy branch */
#define ANE_T6021_INIT_COUNT_OFF	0x68	/* u32 = 64 */
#define ANE_T6021_INIT_COUNT		0x40
#define ANE_T6021_INIT_TEMPLATE_OFF	0x6c	/* 256 B at [0x6C,0x16C) */
#define ANE_T6021_INIT_TEMPLATE_SIZE	0x100
#define ANE_T6021_INIT_TBIT_OFF		0xc0	/* template-relative */
/* Initial template control value; the optional bit 0x10 remains clear. */
#define ANE_T6021_INIT_TBIT_VAL		0x4

static inline u64 ane_t6021_rvbar_compose(u64 iova)
{
	return ANE_T6021_RVBAR_ENTRY_BASE | (iova & ANE_T6021_RVBAR_ADDR_MASK);
}

/* A set RVBAR bit 0 skips entry programming; this helper only tests that bit. */
static inline bool ane_t6021_rvbar_latched(u64 rd)
{
	return rd & 1;
}

/* RVBAR entry address bits. */
static inline u64 ane_t6021_rvbar_entry_bits(u64 rd)
{
	return rd & ANE_T6021_RVBAR_ADDR_MASK;
}

/*
 * The staged DMA address must survive RVBAR composition without loss.
 * Reject set bits 0..10, 48 or 55; bit 11 is retained.
 */
static inline bool ane_t6021_rvbar_entry_ok(u64 iova)
{
	return (iova & (u64)~ANE_T6021_RVBAR_ADDR_MASK) == 0;
}

/*
 * Order coherent writes before publishing low32 to SCRATCH0, then high32 to
 * SCRATCH1.
 */
static inline void ane_t6021_scratch64_split(u64 v, u32 *lo, u32 *hi)
{
	*lo = (u32)(v & 0xffffffffU);
	*hi = (u32)(v >> 32);
}

static inline u64 ane_t6021_scratch64_join(u32 lo, u32 hi)
{
	return ((u64)hi << 32) | lo;
}

/*
 * Boot inputs used to fill every init field. Static inputs must be
 * valid before preflight; allocation addresses and SCRATCH values are
 * filled after READY. The complete allocation is zeroed first.
 */
/*
 * Init fields: firmware DVA at +0, IPC DVA at +8, configuration size
 * 0x500000 at +0x10 and its 0x10000000 complement at +0x18. Heap DVA
 * and size occupy +0x20/+0x28; previous image length at +0x30 is zero
 * on first boot and updated on reload. The pool DVA is at +0x58 and
 * its requested size, 0x40000, at +0x60.
 *
 * After READY, SCRATCH3 supplies the heap request and SCRATCH1 + 1
 * supplies the u32 boot ordinal at +0x50. Validate heap and IPC size
 * bounds before allocating; use the returned device DMA addresses.
 */
struct ane_t6021_init_sources {
	u64 fw_dva;
	u64 ipc_dva;
	u32 cfg_size;
	u32 prev_fw_len;
	u64 heap_floor;
	u64 pool_dma;
	u64 pool_word0;
};

/* IPC allocation size is max(DART page size, captured SCRATCH1 + 1). */
/* SCRATCH0 request-band validation is separate from IPC sizing. */
static inline u64 ane_t6021_ipc_size(u64 page_size, u32 captured)
{
	/*
	 * Increment captured SCRATCH1 in u32 arithmetic, including wraparound.
	 * The result is the boot ordinal used for the IPC allocation size.
	 */
	u32 ord = captured + 1;

	return page_size > (u64)ord ? page_size : (u64)ord;
}

/* Operational ceiling for the fw-influenced 'IPC ' allocation
 * (captured is untrusted; the max() can otherwise be forced to an
 * arbitrary size). Distinct from ABI width — documented budget.
 */
#define ANE_T6021_BOOT_IPC_CEILING	0x100000ULL	/* 1 MiB */

/*
 * A zero heap request disables that allocation; otherwise use
 * max(request, floor) and reject requests above the 32 MiB ceiling.
 * This resource budget is independent of the u64 ABI field width and
 * DART addressability.
 */
#define ANE_T6021_BOOT_HEAP_CEILING	0x02000000ULL	/* 32 MiB budget */

static inline long long
ane_t6021_heap_size(u32 scratch3_req, u64 floor, u64 max_size)
{
	u64 req = scratch3_req;

	if (!req)
		return 0;
	if (req > max_size)
		return -E2BIG;
	return (long long)(req > floor ? req : floor);
}

/*
 * Fill the complete init allocation after zeroing all 0x174 bytes.
 * The gaps at +0x34..0x37 and +0x64..0x67 remain zero.
 */
static inline void
ane_t6021_init_struct_fill(u8 *buf, const struct ane_t6021_init_sources *s,
			   long long heap_size, u64 heap_dva,
			   u32 boot_ordinal)
{
	u64 size = s->cfg_size;
	int i;

	for (i = 0; i < 8; i++) {
		buf[ANE_T6021_INIT_FW_DVA_OFF + i] =
			(u8)(s->fw_dva >> (8 * i));
		buf[0x08 + i] = (u8)(s->ipc_dva >> (8 * i));
		buf[0x10 + i] = (u8)(size >> (8 * i));
		buf[0x18 + i] = (u8)((0x10000000ULL - size) >> (8 * i));
		buf[0x20 + i] = (u8)(heap_dva >> (8 * i));
		buf[0x50 + i] = (u8)((u64)boot_ordinal >> (8 * i));
		buf[0x58 + i] = (u8)(s->pool_dma >> (8 * i));
		buf[0x60 + i] = (u8)(s->pool_word0 >> (8 * i));
	}

	/* [0x28] u64 heap size (0 when the fw requested none) */
	for (i = 0; i < 8; i++)
		buf[0x28 + i] = (u8)((u64)heap_size >> (8 * i));

	/* Previous image length at +0x30 is serialized as a full u32. */
	buf[0x30] = (u8)s->prev_fw_len;
	buf[0x31] = (u8)(s->prev_fw_len >> 8);
	buf[0x32] = (u8)(s->prev_fw_len >> 16);
	buf[0x33] = (u8)(s->prev_fw_len >> 24);

	buf[ANE_T6021_INIT_COUNT_OFF + 0] = (u8)ANE_T6021_INIT_COUNT;
	buf[ANE_T6021_INIT_COUNT_OFF + 1] = 0;
	buf[ANE_T6021_INIT_COUNT_OFF + 2] = 0;
	buf[ANE_T6021_INIT_COUNT_OFF + 3] = 0;

	/* Template word 0 is zero; bytes +4..+0xb remain zero. */
	buf[ANE_T6021_INIT_TEMPLATE_OFF + 0] = 0;
	buf[ANE_T6021_INIT_TEMPLATE_OFF + 1] = 0;
	buf[ANE_T6021_INIT_TEMPLATE_OFF + 2] = 0;
	buf[ANE_T6021_INIT_TEMPLATE_OFF + 3] = 0;

	/* Template +0xc0 starts at 4; optional bit 0x10 remains clear. */
	buf[ANE_T6021_INIT_TEMPLATE_OFF + ANE_T6021_INIT_TBIT_OFF + 0] =
		ANE_T6021_INIT_TBIT_VAL;
}

/*
 * Shared allocation sizing, init fill and publication-value assembly.
 * Allocate the 0x40000-byte pool, IPC sized from the page floor and
 * ordinal, then an optional heap. Each allocation returns NULL on
 * failure and supplies its device DMA address. Reject untrusted heap
 * or IPC sizes exceeding their budgets before any allocation.
 */
struct ane_t6021_boot_allocs {
	u64 pool_dva, ipc_dva, heap_dva;
	u64 ipc_size, heap_size;
	void *pool, *ipc, *heap;
};

static inline int
ane_t6021_boot_prepare_publish(const struct ane_t6021_init_sources *s,
			       u32 scratch3_req, u32 scratch0_read,
			       u32 scratch1_read,
			       u64 page_size, u64 ipc_ceiling,
			       u64 heap_ceiling,
			       void *alloc_ctx,
			       void *(*alloc)(void *ctx, u64 size,
					      u64 *iova),
			       struct ane_t6021_boot_allocs *a,
			       u32 *lo, u32 *hi)
{
	int i;

	for (i = 0; i < (int)(sizeof(*a) / sizeof(u64)); i++)
		((u64 *)a)[i] = 0;

	/* Reject SCRATCH0 >= 0x21 before allocation or publication. */
	if (scratch0_read >= ANE_T6021_BOOT_IPC_REQ_THRESHOLD)
		return -EPROTO;
	a->ipc_size = ane_t6021_ipc_size(page_size, scratch1_read);
	if (a->ipc_size > ipc_ceiling)
		return -E2BIG;
	{
		long long heap = ane_t6021_heap_size(scratch3_req,
						     s->heap_floor,
						     heap_ceiling);

		if (heap < 0)
			return (int)heap;
		a->heap_size = (u64)heap;
	}

	a->pool = alloc(alloc_ctx, 0x40000, &a->pool_dva);
	if (!a->pool)
		return -ENOMEM;
	a->ipc = alloc(alloc_ctx, a->ipc_size, &a->ipc_dva);
	if (!a->ipc)
		return -ENOMEM;
	if (a->heap_size) {
		a->heap = alloc(alloc_ctx, a->heap_size, &a->heap_dva);
		if (!a->heap)
			return -ENOMEM;
	}

	/*
	 * Increment the captured SCRATCH1 once using u32 arithmetic. Header
	 * DMA addresses come from successful allocations, not input
	 * placeholders.
	 */
	{
		struct ane_t6021_init_sources filled = *s;

		filled.ipc_dva = a->ipc_dva;
		filled.pool_dma = a->pool_dva;
		ane_t6021_init_struct_fill(a->pool, &filled, a->heap_size,
					   a->heap_dva,
					   (u32)(scratch1_read + 1));
	}
	*lo = (u32)(a->pool_dva & 0xffffffffU);
	*hi = (u32)(a->pool_dva >> 32);
	return 0;
}

/*
 * After DONE, read SCRATCH1 then SCRATCH0 to capture a device address.
 * Never dereference it directly. Any use requires range and length
 * validation against the owned firmware or IPC window, excluding the
 * pool. Until then the driver only stores and logs the raw value.
 */

/*
 * Engine-relative register offsets and shared sequence executor.
 * Kernel and host-test backends execute the same ordering, including
 * preflight before writes and publication only after READY.
 */

#define ANE_T6021_BOOT_REG_TABLE0	0x00000b38
#define ANE_T6021_BOOT_REG_TABLE1	0x00000b98
#define ANE_T6021_BOOT_REG_TABLE2	0x00000bf8
#define ANE_T6021_BOOT_REG_RVBAR	0x01050000
#define ANE_T6021_BOOT_REG_CPUCTRL	0x01400044
#define ANE_T6021_BOOT_REG_SCRATCH0	0x01840048
#define ANE_T6021_BOOT_REG_SCRATCH1	0x0184004c
#define ANE_T6021_BOOT_REG_SCRATCH6	0x01840060
#define ANE_T6021_BOOT_REG_SCRATCH7	0x01840064
/*
 * Progress sampling uses the eight SCRATCH cells, RVBAR, CPU_STATUS
 * and the 24 MHz tick at +0x1160008. The register at +0x1140008 is not
 * read because its access width and readability are not established.
 */
#define ANE_T6021_BOOT_REG_TICK		0x01160008
#define ANE_T6021_BOOT_TABLE_VALUE	0x01ff01ffU
#define ANE_T6021_BOOT_TABLE_POLLS	1000

struct ane_t6021_boot_io {
	void *ctx;
	u32 (*rd32)(void *ctx, unsigned int off);
	u64 (*rd64)(void *ctx, unsigned int off);
	void (*wr32)(void *ctx, unsigned int off, u32 v);
	void (*wr64)(void *ctx, unsigned int off, u64 v);
	/*
	 * dma_wmb() orders coherent writes before device publication without
	 * requiring store completion.
	 */
	void (*publish_barrier)(void *ctx);
	/* Bounded phase log: emitted ONCE before each write/poll block so a
	 * crash source survives netconsole (never per-poll).
	 */
	void (*phase)(void *ctx, const char *what);
	void (*poll_wait)(void *ctx);    /* 1 ms poll delay */
	/* prepare(): called once, strictly AFTER poll A and BEFORE the
	 * SCRATCH0/1 publish; returns the suballoc DVA halves. Kernel
	 * backend: allocate pool/IPC + fill from pinned sources.
	 */
	int (*prepare)(void *ctx, u32 *lo, u32 *hi);
};

struct ane_t6021_boot_cfg {
	int preflight_ok;	/* All prerequisites must pass before any boot write. */
	/*
	 * Table mode: 0 aborts with -EAGAIN before writes; 1 writes; 2 skips.
	 */
	int preboot_table_mode;
	u64 fw_dva;		/* staged surface DVA (fold input) */
	/*
	 * Nonzero selects SCRATCH6=0 and mailbox management; zero selects
	 * SCRATCH6=1 and MBI. Mailbox mode records READY without requiring it,
	 * skips legacy publication/wake, and requires HELLO for aliveness.
	 */
	int rtb_mode;
	/*
	 * Diagnostic stop: zero runs all steps; 1..4 stops after that step
	 * with -ECANCELED (aperture, scratch, RVBAR, CPU release/READY poll).
	 * A READY timeout still takes precedence over the step-4 stop.
	 */
	int stop_after;
};

/* Ownership: a started CPU may be fetching from the staged surfaces —
 * the DMA memory is NOT reclaimable on failure/remove while
 * cpu_started; reclaimable only via the domain-off reset (reboot).
 */
static inline int ane_t6021_boot_dma_reclaimable(int cpu_started)
{
	return !cpu_started;
}

/*
 * While cpu_started, retain DMA surfaces, rings, IRQ and power links until
 * reboot.
 */
static inline int ane_t6021_boot_remove_held(int cpu_started)
{
	return cpu_started;
}

/* Returns 0 (DONE observed, booted), -ENODATA (preflight closed: ZERO
 * io writes), or -ETIMEDOUT (poll A/B: cpu_started holds, DMA stays
 * unreclaimable, no publish/wake happened on poll A timeout).
 */
static inline int
ane_t6021_boot_run(const struct ane_t6021_boot_io *io,
		   const struct ane_t6021_boot_cfg *cfg,
		   int *cpu_started, int *fw_alive, int *booted,
		   u64 *scratch_result)
{
	unsigned int i;
	u32 v;
	u64 rvbar;

	*cpu_started = 0;
	*fw_alive = 0;
	*booted = 0;
	*scratch_result = 0;

	/* Check every prerequisite before the first write. */
	if (!cfg->preflight_ok)
		return -ENODATA;

	/*
	 * Pre-CPU engine table. Mode 1 explicitly enables these writes;
	 * mode 2 skips them. Hardware hangs occurred with this block enabled,
	 * so mode 0 refuses the sequence before any write.
	 */
	switch (cfg->preboot_table_mode) {
	case 0:
		return -EAGAIN;	/* Abort before the first write. */
	case 1:
		io->phase(io->ctx, "P0 preboot-table");
		io->phase(io->ctx, "P0-1 eng+0xb38");
		io->wr32(io->ctx, ANE_T6021_BOOT_REG_TABLE0,
			 ANE_T6021_BOOT_TABLE_VALUE);
		io->phase(io->ctx, "P0-2 eng+0xb98");
		io->wr32(io->ctx, ANE_T6021_BOOT_REG_TABLE1,
			 ANE_T6021_BOOT_TABLE_VALUE);
		io->phase(io->ctx, "P0-3 eng+0xbf8");
		io->wr32(io->ctx, ANE_T6021_BOOT_REG_TABLE2,
			 ANE_T6021_BOOT_TABLE_VALUE);
		break;
	case 2:
	default:
		/* Skip the pre-CPU table and run the remaining sequence. */
		io->phase(io->ctx, "P0 table SKIPPED (diagnostic)");
		break;
	}

	/*
	 * Program the aperture with twelve ordered writes. The backend logs
	 * and reads around each write so a failure identifies the last step.
	 */
	{
		static const struct { u32 off; u32 val; } tun[] = {
			{ 0x000, 0x00000010 }, { 0x038, 0x00050020 },
			{ 0x03c, 0x000a0030 }, { 0x400, 0x40010001 },
			{ 0x600, 0x01ffffff }, { 0x738, 0x00200020 },
			{ 0x798, 0x00100030 }, { 0x7f8, 0x0100000a },
			{ 0x900, 0x00000101 }, { 0x410, 0x00001100 },
			{ 0x420, 0x00001100 }, { 0x430, 0x00001100 },
		};
		unsigned int ti;

		io->phase(io->ctx, "P-1 grant-tunables begin");
		for (ti = 0; ti < ARRAY_SIZE(tun); ti++) {
			io->phase(io->ctx, tun[ti].off == 0x000 ?
				  "P-1a eng+0x000" :
				  tun[ti].off == 0x038 ? "P-1b eng+0x038" :
				  tun[ti].off == 0x03c ? "P-1c eng+0x03c" :
				  tun[ti].off == 0x400 ? "P-1d eng+0x400" :
				  tun[ti].off == 0x600 ? "P-1e eng+0x600" :
				  tun[ti].off == 0x738 ? "P-1f eng+0x738" :
				  tun[ti].off == 0x798 ? "P-1g eng+0x798" :
				  tun[ti].off == 0x7f8 ? "P-1h eng+0x7f8" :
				  tun[ti].off == 0x900 ? "P-1i eng+0x900" :
				  tun[ti].off == 0x410 ? "P-1j eng+0x410" :
				  tun[ti].off == 0x420 ? "P-1k eng+0x420" :
				  "P-1l eng+0x430");
			io->wr32(io->ctx, tun[ti].off, tun[ti].val);
			io->phase(io->ctx, "P-1 write done");
		}
		io->phase(io->ctx, "P-1 grant-tunables end");
	}
	if (cfg->stop_after == 1)
		return -ECANCELED;	/* tunables done, nothing else fired */

	io->phase(io->ctx, "P1 scratch-clear+pulse");
	/*
	 * Clear all SCRATCH cells, set SCRATCH6 by transport mode, and pulse
	 * SCRATCH7 1 -> 0.
	 */
	for (i = 0; i < 8; i++)
		io->wr32(io->ctx,
			 ANE_T6021_BOOT_REG_SCRATCH0 + 4 * i, 0);
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH6,
		 cfg->rtb_mode ? 0 : 1);
	io->phase(io->ctx, cfg->rtb_mode ?
		  "P1 S1 SCRATCH6=0 (mailbox select)" :
		  "P1 S1 SCRATCH6=1 (MBI select)");
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7, 1);
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7, 0);
	if (cfg->stop_after == 2)
		return -ECANCELED;	/* scratch programmed, CPU untouched */

	io->phase(io->ctx, "P2 rvbar");
	/*
	 * RVBAR bit 0 set preserves the latched entry; otherwise compose and
	 * write it.
	 */
	rvbar = io->rd64(io->ctx, ANE_T6021_BOOT_REG_RVBAR);
	if (!ane_t6021_rvbar_latched(rvbar))
		io->wr64(io->ctx, ANE_T6021_BOOT_REG_RVBAR,
			 ane_t6021_rvbar_compose(cfg->fw_dva));
	if (cfg->stop_after == 3)
		return -ECANCELED;	/* entry decision recorded, no RUN */

	io->phase(io->ctx, "P3 cpu-release");
	/* S3: CPU release — both paths, strictly 0 then 0x10. */
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_CPUCTRL, 0);
	io->phase(io->ctx, "P3b cpu-release RUN=0x10");
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_CPUCTRL,
		 ANE_T6021_CPU_RUN_RELEASE);
	*cpu_started = 1;

	io->phase(io->ctx, "P4 pollA-READY");
	/*
	 * A fresh SCRATCH7 READY after CPU release sets fw_alive. In mailbox
	 * mode a missing READY is nonfatal and leaves fw_alive clear; in MBI
	 * mode it returns -ETIMEDOUT.
	 */
	for (i = 0; i < ANE_T6021_BOOT_TABLE_POLLS; i++) {
		v = io->rd32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7);
		if (v == ANE_T6021_BOOT_ACK)
			break;
		io->poll_wait(io->ctx);
	}
	if (v == ANE_T6021_BOOT_ACK) {
		*fw_alive = 1;
		io->phase(io->ctx, "P4 pollA READY observed");
	} else if (cfg->rtb_mode) {
		io->phase(io->ctx,
			  "P4 pollA no READY (mailbox: HELLO required)");
		return 0;	/* Mailbox mode requires HELLO to establish aliveness. */
	} else {
		return -ETIMEDOUT;
	}
	if (cfg->stop_after == 4)
		return -ECANCELED;	/* fw alive, publish/wake withheld */
	if (cfg->rtb_mode)
		/*
		 * Mailbox mode uses management messages instead of legacy
		 * publication and wake.
		 */
		return 0;

	io->phase(io->ctx, "P5 prepare+publish");
	/*
	 * Prepare allocations and fill coherent memory before publication.
	 * Order DMA writes, then publish SCRATCH0 low32 and SCRATCH1 high32;
	 * only then write the wake request.
	 */
	{
		u32 lo = 0, hi = 0;
		int err = io->prepare(io->ctx, &lo, &hi);

		if (err)
			return err;
		io->publish_barrier(io->ctx);
		io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH0, lo);
		io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH1, hi);
	}
	io->phase(io->ctx, "P6 wake");
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7,
		 ANE_T6021_BOOT_WAKE_REQ);

	io->phase(io->ctx, "P7 pollB-DONE");
	/* S7: poll B — DONE; read back the SCRATCH0/1 result u64. */
	for (i = 0; i < ANE_T6021_BOOT_TABLE_POLLS; i++) {
		v = io->rd32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7);
		if (v == ANE_T6021_BOOT_ACK)
			break;
		io->poll_wait(io->ctx);
	}
	if (v != ANE_T6021_BOOT_ACK)
		return -ETIMEDOUT;
	*booted = 1;
	io->phase(io->ctx, "P7 pollB DONE observed");

	/*
	 * DONE records booted state; the returned address still requires
	 * separate validation.
	 */
	*scratch_result =
		((u64)io->rd32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH1) << 32) |
		 io->rd32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH0);
	return 0;
}

/*
 * Eight channel descriptors: type zero is host-to-device, type one
 * is device-to-host. Ring slots are 64 bytes; table entries are 256 bytes.
 */
#define ANE_T6021_CHMAN_ENTRY_SIZE	0x100
#define ANE_T6021_CHMAN_NAME_LEN	0x40
#define ANE_T6021_CHMAN_COUNT		8
#define ANE_T6021_CHMAN_TOTAL		0x10440

struct ane_t6021_chman_desc {
	char name[ANE_T6021_CHMAN_NAME_LEN];
	u32 type;
	u32 bit;
	u64 size;
	u64 ring;
	u8 pad[ANE_T6021_CHMAN_ENTRY_SIZE - ANE_T6021_CHMAN_NAME_LEN - 0x18];
};

struct ane_t6021_chman_static {
	const char *name;
	u32 type;
	u32 bit;
	u64 size;
	u32 off;	/* ring offset from the IPC DVA */
};

static const struct ane_t6021_chman_static
ane_t6021_chman_layout[ANE_T6021_CHMAN_COUNT] = {
	{ "TERMINAL",       2, 0, 0x300, 0x0800 },
	{ "IO",             0, 1, 0x010, 0xc800 },
	{ "DEBUG",          0, 2, 0x008, 0xcc00 },
	{ "BUF_H2T",        0, 3, 0x040, 0xce00 },
	{ "BUF_T2H",        1, 4, 0x040, 0xde00 },
	{ "SHAREDMALLOC",   1, 5, 0x008, 0xee00 },
	{ "IO_T2H",         1, 6, 0x040, 0xf000 },
	{ "DATA_CHAIN_H2T", 0, 7, 0x010, 0x10000 },
};

/* True when entry i of a table read from the IPC surface matches the
 * static layout with the rings based at ipc_dva.
 */
static inline bool
ane_t6021_chman_entry_ok(const struct ane_t6021_chman_desc *d,
			 unsigned int i, u64 ipc_dva)
{
	const struct ane_t6021_chman_static *s = &ane_t6021_chman_layout[i];

	return !strncmp(d->name, s->name, ANE_T6021_CHMAN_NAME_LEN) &&
	       d->type == s->type && d->bit == s->bit && d->size == s->size &&
	       d->ring == ipc_dva + s->off;
}

/* Bitmask of mismatching entries (0 = the whole table validated). */
static inline unsigned int
ane_t6021_chman_check(const struct ane_t6021_chman_desc *t, u64 ipc_dva)
{
	unsigned int i, bad = 0;

	for (i = 0; i < ANE_T6021_CHMAN_COUNT; i++)
		if (!ane_t6021_chman_entry_ok(&t[i], i, ipc_dva))
			bad |= 1U << i;
	return bad;
}

/* Before ACK, ownership value 1 prevents firmware from consuming empty H2T slots. */
static inline bool ane_t6021_chman_host_init(void *ipc, u64 size, u64 dva)
{
	unsigned int i, slot;

	if (!ipc || size < ANE_T6021_CHMAN_TOTAL ||
	    dva > ~(u64)0 - ANE_T6021_CHMAN_TOTAL ||
	    ane_t6021_chman_check(ipc, dva))
		return false;
	for (i = 0; i < ANE_T6021_CHMAN_COUNT; i++) {
		const struct ane_t6021_chman_static *s = &ane_t6021_chman_layout[i];

		if (s->type != 0)
			continue;
		for (slot = 0; slot < s->size; slot++) {
			u8 *entry = (u8 *)ipc + s->off + slot * 0x40;

			memset(entry, 0, 24);
			entry[0] = 1;
		}
	}
	return true;
}

#endif /* __ANE_T6021_BOOT_H__ */

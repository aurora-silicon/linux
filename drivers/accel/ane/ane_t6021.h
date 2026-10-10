/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* T6021 (H14 / J414c) Apple Neural Engine — skeleton constants and types.
 *
 * Where the constants below come from (none of them needs a device write):
 *  - reg windows, IRQ, pmgr islands: ADT j414c (DeviceTree.j414cap.im4p,
 *    receipts/2026-09-18-t6021-engine-layout-mined §2), Linux translation
 *    +0x200000000 (proven class, pmgr low-32 match).
 *  - ASC cpu block (+0x1400000 h14g) + MBI transport (SCRATCH, channel
 *    table, +0x1844000 doorbell): the W4-fix receipt plus the live reads
 *    and the handshake described below; m1n1 ASCRegs = t8103 cross-ref
 *    only.
 *  - RTKit MGMT protocol: Asahi rtkit.c semantics, u64 message halves as
 *    staged in omarchy-ane rtkit/h14_rtkit_hello.py (commit 6ad26b7).
 *  - RTBuddy endpoint table + doorbell word: the per-endpoint ring sizes,
 *    fourccs and doorbell bits listed with ane_t6021_eps below
 *    (receipts/2026-09-18-h14-w2-protocol-decode §3).
 *
 * DT binding (driver + packaging/dt/t6021-ane.dts are the two halves):
 *  compatible    = "apple,t6021-ane" (also "apple,t6020-ane", the
 *                  T6022 die-0 "apple,t6022-ane" and "apple,t8112-ane":
 *                  struct ane_t602x_soc). The T6021 values follow; T8112
 *                  (packaging/dt/t8112-ane.dts) has engine 0x26a000000,
 *                  pmgr 0x23b700000+0x18000, set 0x23b724000+0x4000,
 *                  IRQ 520, seven power states 0xc008..0xc038, and a
 *                  fourth window "fuse" (0x23d2c8060+8, chip revision).
 *  reg/reg-names = "engine" (whole 32 MiB ADT range0, 0x284000000;
 *                  the H13-style +0x1c04000 engine delta does not exist
 *                  on this SoC — it is never applied here; a first touch
 *                  external-aborted, proven 2026-09-18),
 *                  "pmgr" (0x28e080000+0x4034, pmgr1,t6021 island words),
 *                  "set"  (0x28e08c000+0x4000, SET window — read-only by
 *                  repo rule: direct SET writes external-abort the SoC)
 *  interrupts    = one AIC2 level-high interrupt named "ane" (raw 884;
 *                  [INFERENCE W2 §3: the ANE MBI doorbell IRQ].
 *                  dart-ane0 carries raw 885, provider-owned, never
 *                  fetched here)
 *  iommus        = dart-ane0 streams (apple,t6020-dart/apple,t8110-dart
 *                  nodes). The ADT dart-ane0 is one hardware block with a
 *                  four-window quartet (0x85800000/85810000/85820000/
 *                  85804000, each 0x4000); the installed overlay's
 *                  three-node split (t6001-proven pattern) is kept.
 *  power-domains = eight, phase1 raise order (sys_mpm→td→base→set1..4,
 *                  ane_cpu last): ane_sys_mpm@4000, ane_td@4008,
 *                  ane_base@4010, ane_set1..4@4018-4030, ane_cpu@2e0.
 *                  ane_td/ane_base were the W3 chain gap (six consumed,
 *                  block access reset the machine); see the
 *                  2026-09-19 init-sequence receipt §3.
 */

#ifndef __ANE_T6021_H__
#define __ANE_T6021_H__

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/math.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/mutex.h>

#include "ane_t6021_boot.h"

/* reg windows (packaging/dt/t6021-ane.dts reg-names order) */
enum {
	ANE_T6021_REG_ENGINE,
	ANE_T6021_REG_PMGR,
	ANE_T6021_REG_SET,
	ANE_T6021_REG_COUNT
};

/* pmgr ps-word fields (apple-pmgr-pwrstate layout). */
#define ANE_PS_ON		0xf
#define ANE_PS_TARGET		GENMASK(3, 0)
#define ANE_PS_ACTUAL		GENMASK(7, 4)
#define ANE_PS_WAS_GATED	GENMASK(9, 8)
#define ANE_PS_BUSY		BIT(11)
#define ANE_PS_AUTO_ENABLE	BIT(28)

/* ANE-block-relative ASC/RTBuddy addresses.  CPU block = ANE+0x1400000
 * for h14g: CPU_CONTROL is 0x1400044, without the 0x200000 offset of
 * the h16g/h17/h18g parts (whose block is the 0x1600044 one that the
 * phase-1 §2 note quoted).
 * RUN is set at 0x1400044 on this part.
 * CPU_STATUS +0x48 keeps the m1n1 ASCRegs +4 shape (0x1400048, polled
 * while the ASC CPU starts).
 *
 * W10 (2026-09-19) CORRECTS the claim that stood here — that the m1n1
 * t8103 mailbox (INBOX_CTRL +0x8110 / INBOX0 +0x8800) "has NO h14g
 * analog".  It has one, and it is live: see ANE_ASC_MBOX_* below.  The
 * earlier live test read +0x1608114, the h16g base, which is simply
 * the wrong address on this part.  The mailbox belongs to the RTBuddy
 * transport rather than to the ANE engine interface, which is why the
 * engine-side constants never refer to 0x1408xxx.
 *
 * W10 also read the CPU block for the first time, on a Linux boot with
 * all eight pmgr domains at ACTUAL=0xf and the W8 grant applied:
 *   CPU_CONTROL 0x285400044 = 0x00000000  RUN=0
 *   CPU_STATUS  0x285400048 = 0x0000002a  RUNNING=0 STOPPED=1 IDLE=1
 *   RVBAR       0x285050000 = 0x00000001  valid bit set, entry addr 0
 * The IOP has never been started and no firmware image is programmed,
 * so nothing on this transport can answer the host until it is.
 */
#define ANE_ASC_CPU_CONTROL	0x1400044	/* RUN = BIT(4) */
#define ANE_ASC_CPU_STATUS	0x1400048	/* m1n1 R_CPU_STATUS shape */
#define ANE_ASC_RVBAR		0x1050000	/* fw entry | valid bit0 */
#define ANE_ASC_EDPRCR		0x1010310	/* phase1 S2 whitelist */
#define ANE_ASC_VERS		0x1840000
#define ANE_ASC_RTB_STATUS	0x1840088	/* polled: value < 2 */
#define ANE_ASC_RTB_STATUS_UNK7C 0x184007c	/* phase1 S2 whitelist */

/* ASC blocks iBoot names in the firmware's __rtk_patch records
 * (RTK_cpu_physical_address 0x285000000 and RTK_cpu_wrapper_physical_address
 * 0x285400000 on the T6021 preload), as engine-relative offsets.
 */
#define ANE_ASC_CPU_BASE	0x1000000
#define ANE_ASC_WRAPPER_BASE	0x1400000

/* The real ASC mailbox, mapped live by W10 (read-only) at ASC+0x8000.
 * Three independent sources agree on the offsets: the live DT sibling
 * mbox@2a2408000 ("apple,t6020-asc-mailbox" / "apple,asc-mailbox-v4",
 * size 0x4000, IRQs send-empty/send-not-empty/recv-empty/
 * recv-not-empty) fixes mailbox = asc_base + 0x8000; upstream
 * drivers/soc/apple/mailbox.c apple_mbox_asc_hw fixes the register
 * offsets; m1n1 proxyclient/m1n1/hw/asc.py ASCRegs gives the same set
 * ASC-relative plus the R_MBOX_CTRL field layout.
 *
 * Observed on jw14m2 (2026-09-19), both directions identical:
 *   a2i_control 0x285408110 = 0x00020001
 *   i2a_control 0x285408114 = 0x00020001
 *     -> ENABLE=1 EMPTY=1 FULL=0 OVERFLOW=0 FIFOCNT=0 WPTR=0 RPTR=0
 * Enabled, and never used: both pointer pairs are still at the origin,
 * so not one message has crossed either way since reset.  600 polls of
 * i2a_control over 30 s produced zero changes.
 *
 * Receive is POP-ON-READ and 64-bit: upstream does
 *   while (!(i2a_control & EMPTY)) { readq(RECV0); readq(RECV1); }
 * A 32-bit or memcpy-based read would eat messages.
 */
#define ANE_ASC_MBOX		0x1408000
#define ANE_ASC_MBOX_A2I_CTRL	0x1408110	/* m1n1 INBOX_CTRL  */
#define ANE_ASC_MBOX_I2A_CTRL	0x1408114	/* m1n1 OUTBOX_CTRL */
#define ANE_ASC_MBOX_A2I_SEND0	0x1408800	/* m1n1 INBOX0,  u64 */
#define ANE_ASC_MBOX_A2I_SEND1	0x1408808	/* m1n1 INBOX1,  u64 */
#define ANE_ASC_MBOX_I2A_RECV0	0x1408830	/* m1n1 OUTBOX0, u64 */
#define ANE_ASC_MBOX_I2A_RECV1	0x1408838	/* m1n1 OUTBOX1, u64 */

/* R_MBOX_CTRL fields (m1n1 hw/asc.py) */
#define ANE_ASC_MBOX_CTRL_FIFOCNT	GENMASK(23, 20)
#define ANE_ASC_MBOX_CTRL_OVERFLOW	BIT(18)
#define ANE_ASC_MBOX_CTRL_EMPTY		BIT(17)
#define ANE_ASC_MBOX_CTRL_FULL		BIT(16)
#define ANE_ASC_MBOX_CTRL_RPTR		GENMASK(15, 12)
#define ANE_ASC_MBOX_CTRL_WPTR		GENMASK(11, 8)
#define ANE_ASC_MBOX_CTRL_ENABLE	BIT(0)

/* W10 hazard: a plain READ of ANE+0x1854000 is fabric-fatal (watchdog
 * reset ~63 s later, netconsole pinned the pre-log and no value line).
 * Nothing may touch 0x1854000..0x1c04000; the old engine kill window
 * 0x1c04000..0x1c28000 still stands.
 */
#define ANE_FATAL_READ_LO	0x1854000
#define ANE_FATAL_READ_HI	0x1c04000

/* MBI transport (h14g): the SCRATCH registers and the message
 * registers below are the host side of the handshake.  The host writes
 * the command buffer base to SCRATCH0/1, then the wake word 0xf7fbdff9
 * to SCRATCH7, and polls until SCRATCH7 == 0x08042006 ("channel
 * description table ready").  It then reads the table base back from
 * SCRATCH0/1; the table holds per-channel {type,bit,size,phys}
 * entries, and a channel is signalled by writing (1 << bit) to the
 * +0x1844000 doorbell.
 * Host ack = SCRATCH3 = 0x08042006.  This driver runs the handshake
 * only behind the transport opt-in (struct ane_t6021, transport).
 *
 * RTBuddy mode: each ANE channel is an RTBuddy endpoint.  A message to
 * an endpoint is sent by copying it into that endpoint's shared ring
 * and then ringing the AP mailbox doorbell (+0x1844000) with the
 * endpoint's bit; the firmware's mailbox block has inbox and outbox
 * control registers and refuses a message while its inbox is not
 * ready or would overflow.
 *
 * Endpoint number = doorbell bit: EP0 is RTBuddy management (hello,
 * endpoint roll call, power acknowledgement), EP1..5 are the ANE data
 * channels.  The host side of each endpoint is a ring allocated by
 * this driver (struct ane_t6021_ep below), with the write cursor
 * advanced as described further down.
 *
 * Responses travel the other way on the fw->host channels (T2F*,
 * see ane_t6021_eps), which this driver does not walk yet.
 *
 * [INFERENCE: the fw->host doorbell/IRQ bit numbering mirrors the
 * host->fw SET bit per Asahi rtkit semantics; pinned live by W5.]
 *
 */
#define ANE_MBI_SCRATCH0	0x1840048	/* SCRATCH0..7 = +0x48..+0x64 */
#define ANE_MBI_SCRATCH6	0x1840060
#define ANE_MBI_SCRATCH7	0x1840064
/* Wake/ack handshake words live in ane_t6021_boot.h
 * (ANE_T6021_BOOT_WAKE_REQ / ANE_T6021_BOOT_ACK) — single source, shared
 * with the userspace boot regression.
 */
#define ANE_MBI_DOORBELL	0x1844000	/* write32 (1 << endpoint id) */
/* NOT a message pair.  W10 proved this is a mirror of the 24 MHz
 * architectural counter: across 32 samples the absolute difference
 * against CNTVCT_EL0 is a constant 13-18 counts (the MRS-to-MMIO
 * instruction gap), aggregate drift 0.054 ppm over 55.2M counts.  The
 * "type-0 heartbeat, ~29 ticks/3 s" that W6/W9 recorded as firmware
 * liveness was this clock plus a poll count: the RTKit type field
 * GENMASK_ULL(59,52) is structurally zero until the counter passes
 * 2^52 (~6 years of uptime).  Kept only so the old reads stay
 * identifiable in the logs; do not treat as transport.
 */
#define ANE_MBI_TIMEBASE_LO	0x1170000	/* was ANE_MBI_MSG_I2A_LO */
#define ANE_MBI_TIMEBASE_HI	0x1170004	/* was ANE_MBI_MSG_I2A_HI */
#define ANE_MBI_MSG_I2A_LO	ANE_MBI_TIMEBASE_LO
#define ANE_MBI_MSG_I2A_HI	ANE_MBI_TIMEBASE_HI
#define ANE_MBI_MSG_A2I_RD	0x184c000	/* host->fw message read peer */
#define ANE_MBI_MSG_A2I_WR	0x1850000	/* host->fw message write */

/* Per-message MBI word (48-bit ring notification, sent once for every
 * endpoint message): offset = ring write
 * cursor [23:0], length [47:24].  NOT the 54-bit surface-announce
 * word (ane_ep_doorbell_encode below) — that one rides the endpoint
 * buffer setup; this one is sent once per command, after the command
 * bytes are already copied into the shared ring at ring_base +
 * cursor.
 * Length is capped at 0xffffff by the encoder field.
 */
#define ANE_MBI_MSG48_OFF	GENMASK_ULL(23, 0)
#define ANE_MBI_MSG48_LEN	GENMASK_ULL(47, 24)

static inline u64 ane_mbi_msg48_encode(u32 cursor, u32 len)
{
	return (cursor & ANE_MBI_MSG48_OFF) |
	       FIELD_PREP(ANE_MBI_MSG48_LEN, len);
}

/* Host->fw TX sequence (RTBuddy mode):
 *   1. fail if len > ring_size
 *   2. cursor = write_cursor; if (cursor + len >= ring_size) cursor = 0
 *      (an exact fit wraps too)
 *   3. memcpy(ring + cursor, cmd, len)   (DMA-coherent ring)
 *   4. dma_wmb()                          (ring visible before bell)
 *   5. write32(ANE_MBI_MSG_A2I_WR, lo) + write32(+4, hi) — 32-bit
 *      halves (the AKF message register is word-shaped; W6 proved the
 *      W5-live abort was NOT the 64-bit writeq — the halves abort too)
 *   6. write32(ANE_MBI_DOORBELL, 1 << ep)
 *   7. on send success only: write_cursor = cursor + len, so a
 *      failed send leaves the cursor at the last
 *      issued slot.
 * CSNE_CMD_PING = header-only 0x11 on EP1 (INIT) -> doorbell bit
 * 1 << 1 = 0x2.  Only the a2i message register + doorbell are
 * touched, and only behind the mbi_doorbell opt-in (both stay
 * host-write-fatal even behind the W8 grant, W9; SCRATCH family is
 * host-writable granted, W9 nonzero latch).
 */

/* MBI channel-table entry as the firmware publishes it (stride 0x100):
 * type @+0x40, doorbell bit @+0x44, size @+0x48, phys @+0x50 within
 * each entry.
 */
#define ANE_MBI_CHAN_STRIDE	0x100
#define ANE_MBI_CHAN_MAX_DUMP	8

/* RTKit MGMT (EP 0); type bits [59:52], u64 message halves */
#define ANE_RTKIT_TYPE			GENMASK_ULL(59, 52)
#define ANE_RTKIT_MGMT_HELLO		1
#define ANE_RTKIT_MGMT_HELLO_REPLY	2
#define ANE_RTKIT_MGMT_STARTEP		5
#define ANE_RTKIT_MGMT_SET_IOP_PWR_STATE	6
#define ANE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK	7
#define ANE_RTKIT_MGMT_EPMAP		8
#define ANE_RTKIT_MGMT_SET_AP_PWR_STATE		0xb
#define ANE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK	0xb

#define ANE_RTKIT_HELLO_MINVER		GENMASK_ULL(15, 0)
#define ANE_RTKIT_HELLO_MAXVER		GENMASK_ULL(31, 16)
#define ANE_RTKIT_EPMAP_LAST		BIT_ULL(51)
#define ANE_RTKIT_EPMAP_BASE		GENMASK_ULL(34, 32)
#define ANE_RTKIT_EPMAP_BITMAP		GENMASK_ULL(31, 0)
#define ANE_RTKIT_EPMAP_REPLY_MORE	BIT_ULL(0)
#define ANE_RTKIT_STARTEP_EP		GENMASK_ULL(39, 32)
#define ANE_RTKIT_STARTEP_FLAG		BIT_ULL(1)
#define ANE_RTKIT_PWR_STATE		GENMASK_ULL(15, 0)
#define ANE_RTKIT_PWR_STATE_ON		0x20

#define ANE_RTKIT_VER_MIN	11
#define ANE_RTKIT_VER_MAX	12

/* RTKit system endpoints rtkit.c starts when announced */
#define ANE_RTKIT_EP_CRASHLOG	1
#define ANE_RTKIT_EP_SYSLOG	2
#define ANE_RTKIT_EP_DEBUG	3
#define ANE_RTKIT_EP_IOREPORT	4
#define ANE_RTKIT_EP_OSLOG	8
#define ANE_RTKIT_EP_TRACEKIT	0xa

/* RTBuddy app endpoints — the host opens ids 1..6;
 * ring sizes + fourccs from the per-EP config table (W2 §3). fourcc is
 * byte-reversed in the table (0x54324643 = "T2FC").
 */
enum ane_t6021_eps {
	ANE_T6021_EP_INIT = 1,	/* INIT — CSNE_CMD controller channel (W4) */
	ANE_T6021_EP_T2FC,	/* fw->host commands */
	ANE_T6021_EP_T2FH,	/* fw->host commands */
	ANE_T6021_EP_T2HS,
	ANE_T6021_EP_T2HC,
	ANE_T6021_EP_T2HT,	/* polled on the host */
	ANE_T6021_EP_COUNT = ANE_T6021_EP_T2HT + 1	/* arrays index by id */
};

/* CSNE command ids — the firmware knows 96 of them
 * (full set: receipts/2026-09-18-h14-w2-protocol-decode §4 and
 * receipts/2026-09-18-h14-w2-protocol-decode/fw_cmd_table.json). The
 * fw parses the id as u16 at wire offset +4 (W4 correction of the W2
 * §4 "offset 0" claim); the
 * controller header for the fw->host direction is a different shape
 * (u32 id @ +0x8, 0x24 bytes) and never rides host->fw submission.
 */
enum ane_t6021_csne_cmd {
	CSNE_CMD_START		= 0x0000,
	CSNE_CMD_STOP		= 0x0001,
	CSNE_CMD_REG_FILE_LOAD	= 0x0005,	/* fw _rtk_tunables 1456 B */
	CSNE_CMD_BUILDINFO	= 0x0006,
	CSNE_CMD_BOOT		= 0x0010,
	CSNE_CMD_PING		= 0x0011,
	CSNE_CMD_POWER_DEVICE_ON	= 0x0013,
	CSNE_CMD_IPC_ENDPOINT_SET	= 0x0015,
	CSNE_CMD_IPC_ENDPOINT_UNSET	= 0x0016,
	CSNE_CMD_LOAD_PROGRAM		= 0x0200,
	CSNE_CMD_CREATE_PROCESS		= 0x0202,
	CSNE_CMD_PROCEDURE_CALL	= 0x0204,
	CSNE_CMD_INFERENCE_CALL	= 0x0404,
	CSNE_CMD_BACK_CHANNEL_RPC	= 0x7000,
};

/* fw buffer word (the firmware encodes and decodes buffer words this
 * way, and the endpoint setup word has the same shape): addr[43:0] |
 * size_code[51:44] | unit[53:52], unit 0 = no size, 1 = code*4K,
 * 2 = code*1M, 3 = code*2M. The fw encoder picks unit 1 below 1 MiB
 * and unit 2 at or above, size code = CEILING division (rounded up,
 * never down), so the decoded size is the rounded-up value. rtkit.c's
 * BUFFER_REQUEST is this word with unit 1.
 */
#define ANE_EP_DOORBELL_OFFSET	GENMASK_ULL(43, 0)
#define ANE_EP_DOORBELL_SIZE	GENMASK_ULL(51, 44)
#define ANE_EP_DOORBELL_UNIT	GENMASK_ULL(53, 52)

static const u8 ane_ep_doorbell_shift[] = { 0, 12, 20, 21 };

static inline u64 ane_ep_doorbell_encode(u64 offset, u32 size)
{
	u64 unit = (size >= SZ_1M) ? 2 : 1;
	u32 code = DIV_ROUND_UP(size, 1u << ane_ep_doorbell_shift[unit]);

	return (offset & ANE_EP_DOORBELL_OFFSET) |
	       FIELD_PREP(ANE_EP_DOORBELL_SIZE, code) |
	       FIELD_PREP(ANE_EP_DOORBELL_UNIT, unit);
}

static inline u32 ane_ep_doorbell_size(u64 msg)
{
	u32 unit = FIELD_GET(ANE_EP_DOORBELL_UNIT, msg);

	return FIELD_GET(ANE_EP_DOORBELL_SIZE, msg) << ane_ep_doorbell_shift[unit];
}

struct ane_t6021_ep {
	u8 id;
	const char *name;
	u32 fourcc;
	u32 ring_size;
	u32 write_cursor;	/* next ring slot */
	void *ring;			/* dma_alloc_coherent, ring_size */
	dma_addr_t ring_iova;
	bool started;
};

struct reset_control;

struct ane_t6021 {
	struct device *dev;
	void __iomem *base[ANE_T6021_REG_COUNT];
	int irq;

	struct device **pd_dev;
	struct device_link **pd_link;
	int pd_count;

	/* Single MBI consumer (threaded IRQ + probe drain serialize
	 * here)
	 */
	struct mutex mbox_lock;

	/* Bring-up state machine — honest semantics (W15):
	 * power_gated: first_resume passed (eight-island gate +
	 *              whitelist; block access proven-safe),
	 * cpu_started: RVBAR programmed + CPU RUN released (fw_boot=1),
	 * fw_alive:    fw first-alive ack observed on SCRATCH7 — NOT
	 *              the init handshake,
	 * booted:      full init handshake observed. CSNE/MGMT sessions
	 *              gate on this flag only.
	 */
	bool power_gated;
	bool cpu_started;
	bool fw_alive;
	bool booted;

	/* W10: the mailbox does exist — at +0x1408xxx, not the h16g
	 * +0x1608xxx that read-aborted in 2026-09-19 — and it reads a
	 * live, enabled, permanently-empty ASC v4 control pair.  It is
	 * empty because the ASC CPU is STOPPED with RUN clear and RVBAR
	 * entry 0, i.e. no firmware was ever started.  Default off =
	 * status-only bring-up.  Opt-in runs the documented MBI
	 * handshake (SCRATCH wake -> fw channel table), capture-only.
	 */
	bool transport;
	bool doorbell;	/* mbi_doorbell=1: EP rings may write the +0x1844000
			 * doorbell + a2i message register (decoded 2026-09-19)
			 */
	bool irq_requested;

	struct ane_t6021_ep ep[ANE_T6021_EP_COUNT];

	/* W13 fw surface (fw_load=1): coherent, DART-mapped via the
	 * device's iommu group. NULL unless loaded.
	 */
	void *fw_buf;
	dma_addr_t fw_iova;
	u32 fw_size;
	/* W16 entry alias: IOVA the fw pages are aliased at (the latched
	 * RVBAR entry); 0 = no alias mapped. The DMA allocator has no
	 * runtime reservation API on this kernel, so instead of a prose
	 * collision bound every DMA allocation site must pass its iova
	 * through ane_t6021_fw_alias_iova_ok() and refuse overlaps.
	 */
	u64 fw_alias_iova;
	/* Per-window mapped extents (iova, bytes) — teardown unmaps
	 * exactly these, because the windows are not assumed adjacent
	 * (live trace: hole between SEG0 and SEGi) and unmapping a page
	 * that was never mapped trips dart_unmap_pages (io-pgtable-dart
	 * .c:319 WARN, 2026-09-26 state-report unwind). Set only on
	 * successful map.
	 */
#define ANE_FW_ALIAS_MAX_WIN	3
	u64 fw_alias_ext_iova[ANE_FW_ALIAS_MAX_WIN];
	size_t fw_alias_ext_len[ANE_FW_ALIAS_MAX_WIN];
	int fw_alias_extn;
	/* W16 pass-3: ane_cpu reset controller (ps RESET via the pmgr
	 * pwrstate reset_controller ops); NULL when the DT carries no
	 * resets property.
	 */
	struct reset_control *cpu_rst;

	/* W15 boot allocations (gate-gated: never exist until the
	 * preflight opens and the sequence passes poll A). Ownership per
	 * the wedged-pin rule: held, never freed, while cpu_started.
	 */
	void *boot_pool;		/* 'DDM ' pool, 0x40000 (Params word0) */
	dma_addr_t boot_pool_iova;
	void *boot_ipc;			/* 'IPC ' surface, max(0x4000, ord+1) */
	dma_addr_t boot_ipc_iova;
	u64 boot_ipc_size;
	void *boot_heap;		/* fw-requested HEAP surface or NULL */
	dma_addr_t boot_heap_iova;
	u64 boot_heap_size;
	u32 prev_fw_len;		/* [0x30]: previous fw image length
					 * (0 first boot; updated per reload)
					 */
	u64 boot_scratch_result;	/* SCRATCH1<<32 | SCRATCH0 captured
					 * at DONE — raw; success semantics
					 * UNSOURCED (never inferred)
					 */
	bool response_validated;
	bool hybrid_pinned;	/* FALSE until the DONE response
				 * semantics are sourced AND the
				 * raw address is range/length
				 * validated against owned
				 * windows — transport/CSNE
				 * sessions stay fenced on first
				 * boot regardless of booted
				 */
};

/* Enforced alias-window invariant (W16): the DMA allocator has no
 * runtime IOVA reservation on this kernel, so every DMA allocation
 * site must refuse a mapping that overlaps the fw entry alias.
 */
static inline bool ane_t6021_fw_alias_iova_ok(const struct ane_t6021 *ane,
					      dma_addr_t iova, size_t size)
{
	u64 lo = ane->fw_alias_iova;

	if (!lo)
		return true;
	return iova + size <= lo || iova >= lo + ane->fw_size;
}

/* ane_t6021_rtkit.c */
int ane_t6021_rtkit_init(struct ane_t6021 *ane);
void ane_t6021_rtkit_shutdown(struct ane_t6021 *ane);
void ane_t6021_rtkit_drain(struct ane_t6021 *ane);
irqreturn_t ane_t6021_rtkit_irq_thread(int irq, void *data);

/* ane_t6021_boot.c — W15 boot state resolution (fw_boot=1). An
 * explicit boot request fails the probe while the prerequisites in
 * ane_t6021_boot.c hold (-ENODATA); fw_boot=0 binds status-only.
 * ane_t6021_boot_start() is the sequence dispatcher proper (all gates
 * must already hold): it owns the wedged-pin module lifetime once the
 * CPU is released; stop_after = 0 full run, 1..4 = fw-start-debug
 * step bisect (stop after that step, -ECANCELED, clean unwind while
 * no CPU started).
 */
int ane_t6021_boot_start(struct ane_t6021 *ane, int stop_after, int table_mode,
			 int rtb_mode);

/* ---- CSNE_CMD wire structs (host->fw on the INIT channel) ----
 *
 * W4, extending W2 §4 (the receipt's "fw parses a u16 id
 * at header offset 0" was WRONG — the firmware reads
 * the id as u16 at offset +4, for generic commands as well as for
 * LOAD_PROGRAM/CREATE_PROCESS/PROCEDURE_CALL).
 * Wire is
 * little-endian; this module is arm64-only so host order is wire order.
 *
 * The firmware takes commands < 0x1b89 bytes and writes completion
 * state back into the
 * command block (byte +6 and the qword at +8) — the
 * ring slot doubles as the response area. The firmware handles
 * PING/BUILDINFO/BOOT/REG_FILE_LOAD/IPC_ENDPOINT_SET on its
 * default path, without parsing their fields, so
 * their payloads beyond the header are opaque until the W1 live
 * exchange pins them.
 */
struct ane_csne_hdr {
	u32 rsvd0;	/* bytes 0..3: never read by the fw processor; 0 */
	u16 id;		/* PROVEN u16 @ +4 (see above) */
	u8 flags;	/* fw-written byte @ +6 (status/scratch) [INFERENCE] */
	u8 rsvd7;
};

static_assert(sizeof(struct ane_csne_hdr) == 8);

static inline void ane_csne_hdr_init(struct ane_csne_hdr *h, u16 id)
{
	memset(h, 0, sizeof(*h));
	h->id = id;
}

/* Header-only CSNE_CMDs: BOOT (0x10 — boot-arg surfaces ride
 * SCRATCH0-7 / the firmware init boot args, not the command, phase1 §2.5),
 * PING (0x11), BUILDINFO (0x06). sizeof(struct ane_csne_hdr) bytes.
 */

/* REG_FILE_LOAD (0x05) payload: the 1456 B blob is the fw's own
 * _rtk_tunables section (0x5b0 bytes) — its transport
 * (inline vs shared-memory iova) is [INFERENCE], pinned by W1.
 */
struct ane_csne_cmd_reg_file_load {
	struct ane_csne_hdr hdr;
	u8 blob[];
};

/* IPC_ENDPOINT_SET (0x15) payload: binds a host RTBuddy endpoint to a
 * fw channel. No field parsed by the fw processor — layout
 * [INFERENCE], pinned by W1.
 */
struct ane_csne_cmd_ipc_endpoint_set {
	struct ane_csne_hdr hdr;
	u8 payload[];
};

/* PROCEDURE_CALL (0x204) / INFERENCE_CALL (0x404) — the procedure-call
 * command family. Field offsets follow what the firmware accepts;
 * field names (programId, procedureId, numIoBuffers, W2 §4) are given
 * only where the field's role is known:
 *   +0x08/+0x0c u32 pair validated together
 *   +0x10        u64 passed to the fw validator
 *   +0x18        u32 stats/priority, fw-valid [2,7]; 2 is the value
 *                proven end-to-end (binding.json stats_type; the
 *                first-inference receipt's bare calls)
 *   +0x28        u32 element count
 *   +0x60        count × 0x30-byte io-buffer records
 * Commands are whole multiples of the procedure-call size, so they
 * form same-shape arrays in the ring; this driver submits one
 * command per slot. INFERENCE_CALL shares the shape [INFERENCE: the
 * 0x404 id is not among the decoded ids — it is the W4 submission
 * endpoint per the phase-1 workstream plan].
 */
struct ane_csne_io_elem {
	u8 bytes[0x30];	/* internal layout not decoded */
};

struct ane_csne_cmd_procedure_call {
	struct ane_csne_hdr hdr;
	u32 program_id;		/* +0x08 */
	u32 procedure_id;	/* +0x0c */
	u64 field_10;
	u32 field_18;		/* fw requires 8..15 */
	u32 rsvd_1c;
	u64 field_20;
	u32 num_io_buffers;	/* +0x28 = element count */
	u32 rsvd_2c;
	u8 gap_30[0x30];
	struct ane_csne_io_elem io[];
};

static_assert(offsetof(struct ane_csne_cmd_procedure_call, program_id) == 0x08);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, procedure_id) == 0x0c);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, field_10) == 0x10);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, field_18) == 0x18);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, num_io_buffers) == 0x28);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, io) == 0x60);

static inline size_t
ane_csne_cmd_procedure_call_size(unsigned int num_io_buffers)
{
	return sizeof(struct ane_csne_cmd_procedure_call) +
	       num_io_buffers * sizeof(struct ane_csne_io_elem);
}

/* Fw-side bound: the firmware rejects work items of
 * 0x1b89 bytes and above. That the bound applies to the whole command
 * and not to some other size is [INFERENCE] — enforced here as a
 * fail-fast so an oversized command cannot enter the ring.
 */
#define ANE_CSNE_CMD_MAX_SIZE	0x1b88

/* ---- LOAD_PROGRAM (0x200) wire contract (2026-09-26, entry item
 * 23/23b/23c/23e): the payload is NINE 0x30-byte named section
 * records at +0x08. Loading publishes "<name>.bin" for each section
 * into the fw program-info symbol table, with value = record+0x18
 * and aux = u32(record+0x20).
 *
 * The fw dereferences record+0x18, so it
 * carries a fw-addressable pointer — in this driver, the IOVA of a
 * host-authored section object (below). Absent sections (flags bit0
 * clear) are skipped by the firmware. ----
 */
enum ane_t6021_load_section {
	ANE_SEC_GENERIC = 0,
	ANE_SEC_KERNEL,
	ANE_SEC_TEXT,
	ANE_SEC_OPERATION,
	ANE_SEC_PROCEDURE,
	ANE_SEC_KERNELPROP,
	ANE_SEC_TEXTPROP,
	ANE_SEC_OPDBG,
	ANE_SEC_PROCPROP,
	ANE_SEC_COUNT			/* 9 */
};

#define ANE_T6021_LOAD_SEC_COUNT	9

static const char * const
ane_t6021_load_sec_name[ANE_T6021_LOAD_SEC_COUNT] = {
	"genericSection", "kernelSection", "textSection",
	"operationSection", "procedureSection", "kernelPropSection",
	"textPropSection", "opDbgSection", "procPropSection",
};

struct ane_csne_cmd_load_program {
	struct ane_csne_hdr hdr;			/* id 0x200 @ +4 */
	struct ane_csne_io_elem sec[ANE_T6021_LOAD_SEC_COUNT];
};

static_assert(sizeof(struct ane_csne_cmd_load_program) ==
	      0x08 + 9 * 0x30);

/* Section-record wire accessors over the 0x30-byte io_elem (the only
 * bytes the firmware reads; the rest is unread pass-through):
 * +0x00 u8 flags — bit0 present; +0x18 u64 obj — fw-addressable
 * pointer, which the firmware checks lands inside a registered program
 * object's entry table; +0x20 u64 key — per-section lookup key.
 */
#define ANE_SEC_F_PRESENT	BIT(0)

static inline void ane_sec_record_init(void *rec, u64 obj, u64 key)
{
	u8 *r = rec;

	memset(r, 0, 0x30);
	r[0] |= ANE_SEC_F_PRESENT;
	*(u64 *)(r + 0x18) = obj;
	*(u64 *)(r + 0x20) = key;
}

/* Host-authored PROGRAM OBJECT (the thing section records point at;
 * the firmware validates: magic 1 @+0, count <= 0x10 @+4, table entry
 * count in [0x201, 0x400] @+0x204, 0x30-byte entry table @+0x208 with
 * the recorded pointer bounded inside it). Minimum object = header +
 * 0x201 zeroed entries; entries are the same 0x30-byte shape.
 */
#define ANE_PROGOBJ_MIN_ENTRIES	0x201
#define ANE_PROGOBJ_MAX_ENTRIES	0x400
#define ANE_PROGOBJ_TABLE_OFF	0x208
#define ANE_PROGOBJ_HDR_SZ	0x208

static inline size_t ane_progobj_size(u32 entries)
{
	return ANE_PROGOBJ_TABLE_OFF + (size_t)entries * 0x30;
}

static inline void ane_progobj_init(void *obj, u32 entries)
{
	struct { u32 magic; u32 count; u32 rsvd[2]; u32 entries; } *h = obj;

	h->magic = 1;
	h->count = 0;
	h->rsvd[0] = 0;
	h->rsvd[1] = 0;
	h->entries = entries;
	/* caller zeroes the tail: entries table starts at +0x208 */
}

/* Host-authored OPERATION-SECTION image (u32 op_count @+0,
 * <= 0x80; then op_count 0x40c-byte operation records at +4; the
 * section KEY is an offset past the array, >= op_count*0x40c + 4).
 * Operation record fields decoded: +0x00 u32 type <= 4; +0x04 u16
 * <= 0x10; +0x08 u32 procedure_count <= 0x80 (non-zero); +0x0c..
 * u32 procedure indices, each <= 0x3c.
 */
#define ANE_OPSEC_OP_REC_SIZE	0x40c
#define ANE_OPSEC_MAX_OPS	0x80

static inline size_t ane_opsec_size(u32 ops)
{
	return 4 + (size_t)ops * ANE_OPSEC_OP_REC_SIZE;
}

/* Submit one CSNE command block on the INIT (EP1) ring: RTBuddy
 * endpoint send semantics (W2 §3) — slot alloc with wrap,
 * memcpy into the ring, 54-bit doorbell word, cursor
 * advanced only on doorbell success. No synchronous response matching:
 * fw->host responses arrive on the T2F* channels and are not walked
 * yet (W2 §3). Sleeps (mutex) — process context only.
 */
int ane_t6021_csne_submit(struct ane_t6021 *ane, const void *cmd, size_t size);

/* Probe-time one-shot CSNE_CMD_PING on EP1 (W5-live), behind
 * mbi_doorbell=1 only; watches the fw response surfaces for 3 s.
 */
void ane_t6021_csne_ping_attempt(struct ane_t6021 *ane);

/* W13/W14 firmware loader (ane_t6021_fwload.c): validate + stage +
 * dart-ane0-map the 13.5 PRELOAD payload behind fw_load=1. This is
 * the staging half of the boot contract: the fw DVA source
 * and the RVBAR fold are closed (audit commits
 * 3762aee/12be074); ane_t6021_boot.c consumes the staged surface.
 */
int ane_t6021_fwload_probe(struct ane_t6021 *ane);
void ane_t6021_fwload_remove(struct ane_t6021 *ane);
bool ane_t6021_fwload_options_ok(void);
bool ane_t6021_fwload_placement_ok(struct device *dev);
bool ane_t6021_fwload_requested(void);

struct ane_fw_image;
struct ane_asc_tunables;

/* Per-SoC of_match data (defined in ane_t6021_fwload.c).
 *  soc, soc_revision: what iBoot writes to RTK_soc and RTK_soc_revision.
 *    revision_fuse: read the revision from the DT "fuse" window as iBoot
 *    does instead (T8112).
 *  preload_placement: the iBoot SEG0/SEGi physical placement is recorded
 *    for this SoC (T6021 only), so fw_alias_reserved=1 may map it; the
 *    other SoCs always run the firmware from driver-owned memory.
 *  ps_cpu_off: the ANE CPU ps word in the DT "pmgr" window (probe guard).
 *  pwgate_off: the PWGATE word in the DT "set" window, checked
 *    open before any engine read; 0 = not checked.
 *  pmu_pa: the page of the seven ANE ps words; the firmware's power
 *    service writes them through its DART at IOVA == PA. ps_off: the
 *    first of the seven in that page.
 *  trace_td_off: engine offset of the TM last-committed-TD word read by
 *    trace_td; 0 = trace_td unsupported.
 */
struct ane_t602x_soc {
	u32 soc;
	u32 soc_revision;
	bool revision_fuse;
	bool preload_placement;
	const struct ane_fw_image *fw;
	const struct ane_asc_tunables *tunables;
	u32 ps_cpu_off;
	u32 pwgate_off;
	u64 pmu_pa;
	u32 ps_off;
	u32 trace_td_off;
};

extern const struct ane_t602x_soc ane_t6020_soc, ane_t6021_soc,
	ane_t6022_soc, ane_t8112_soc;

#endif /* __ANE_T6021_H__ */

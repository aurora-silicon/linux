/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T602x/T8112 ANE register layout, transport and device state.
 *
 * DT supplies the engine, PMGR and SET windows, interrupt, IOMMU
 * streams and power domains; T8112 also supplies a revision fuse window.
 * Engine accesses require non-posted MMIO. Power domains are raised in
 * sys_mpm, td, base, set1..4 order, with ane_cpu last. Before engine
 * access, all eight domains must have ACTUAL=0xf and BUSY=0, and the
 * CPU domain must have AUTO_ENABLE clear. The driver uses genpd for
 * power transitions; direct PMGR and SET writes can hang the SoC.
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

/*
 * Engine-relative ASC control registers. CPU_CONTROL is at +0x1400044
 * and CPU_STATUS at +0x1400048. RUN is bit 4. RVBAR is accessed as u64;
 * CPU control and status use 32-bit accesses.
 */
#define ANE_ASC_CPU_CONTROL	0x1400044	/* RUN = BIT(4) */
#define ANE_ASC_CPU_STATUS	0x1400048	/* CPU status fields */
#define ANE_ASC_RVBAR		0x1050000	/* fw entry | valid bit0 */
#define ANE_ASC_EDPRCR		0x1010310	/* Readable status register */
#define ANE_ASC_VERS		0x1840000
#define ANE_ASC_RTB_STATUS	0x1840088	/* Status poll accepts values below 2 */
#define ANE_ASC_RTB_STATUS_UNK7C 0x184007c	/* Readable status register */

/* Engine-relative ASC core and wrapper offsets. */
#define ANE_ASC_CPU_BASE	0x1000000
#define ANE_ASC_WRAPPER_BASE	0x1400000

/*
 * ASC mailbox at engine +0x1408000. Control registers are at +0x110
 * and +0x114. Message halves require 64-bit accesses. Receiving pops
 * the queue: read RECV0 before RECV1, without 32-bit reads or memcpy.
 */
#define ANE_ASC_MBOX		0x1408000
#define ANE_ASC_MBOX_A2I_CTRL	0x1408110	/* Transmit control */
#define ANE_ASC_MBOX_I2A_CTRL	0x1408114	/* Receive control */
#define ANE_ASC_MBOX_A2I_SEND0	0x1408800	/* Transmit low half, u64 */
#define ANE_ASC_MBOX_A2I_SEND1	0x1408808	/* Transmit high half, u64 */
#define ANE_ASC_MBOX_I2A_RECV0	0x1408830	/* Receive low half, u64 */
#define ANE_ASC_MBOX_I2A_RECV1	0x1408838	/* Receive high half, u64 */

/* Mailbox control fields */
#define ANE_ASC_MBOX_CTRL_FIFOCNT	GENMASK(23, 20)
#define ANE_ASC_MBOX_CTRL_OVERFLOW	BIT(18)
#define ANE_ASC_MBOX_CTRL_EMPTY		BIT(17)
#define ANE_ASC_MBOX_CTRL_FULL		BIT(16)
#define ANE_ASC_MBOX_CTRL_RPTR		GENMASK(15, 12)
#define ANE_ASC_MBOX_CTRL_WPTR		GENMASK(11, 8)
#define ANE_ASC_MBOX_CTRL_ENABLE	BIT(0)

/*
 * Reads in engine +0x1854000..0x1c04000 can hang the SoC. The region
 * at +0x1c04000..0x1c28000 is also excluded from engine access.
 */
#define ANE_FATAL_READ_LO	0x1854000
#define ANE_FATAL_READ_HI	0x1c04000

/*
 * MBI handshake: publish the command buffer DMA address in SCRATCH0/1,
 * write 0xf7fbdff9 to SCRATCH7, and wait for 0x08042006. SCRATCH0/1 then
 * contain the channel-table address. Each channel describes its type,
 * doorbell bit, size and physical address. Notify with a 32-bit write
 * of BIT(endpoint) to engine +0x1844000. Host acknowledgment writes
 * 0x08042006 to SCRATCH3. Mailbox management and MBI are separate
 * transport modes.
 */
#define ANE_MBI_SCRATCH0	0x1840048	/* SCRATCH0..7 = +0x48..+0x64 */
#define ANE_MBI_SCRATCH6	0x1840060
#define ANE_MBI_SCRATCH7	0x1840064
/* Wake/ack handshake words live in ane_t6021_boot.h
 * (ANE_T6021_BOOT_WAKE_REQ / ANE_T6021_BOOT_ACK) — single source, shared
 * with the userspace boot regression.
 */
#define ANE_MBI_DOORBELL	0x1844000	/* write32 (1 << endpoint id) */
/* Domain tick counter at engine +0x1160008, advancing at 24 MHz. */
#define ANE_MBI_TIMEBASE_LO	0x1170000
#define ANE_MBI_TIMEBASE_HI	0x1170004
#define ANE_MBI_MSG_I2A_LO	ANE_MBI_TIMEBASE_LO
#define ANE_MBI_MSG_I2A_HI	ANE_MBI_TIMEBASE_HI
#define ANE_MBI_MSG_A2I_RD	0x184c000	/* host->fw message read peer */
#define ANE_MBI_MSG_A2I_WR	0x1850000	/* host->fw message write */

/*
 * MBI ring notification: cursor in bits 23:0, length in bits 47:24.
 * Both fields are limited to 0xffffff. This differs from the 54-bit
 * surface announcement encoded below.
 */
#define ANE_MBI_MSG48_OFF	GENMASK_ULL(23, 0)
#define ANE_MBI_MSG48_LEN	GENMASK_ULL(47, 24)

static inline u64 ane_mbi_msg48_encode(u32 cursor, u32 len)
{
	return (cursor & ANE_MBI_MSG48_OFF) |
	       FIELD_PREP(ANE_MBI_MSG48_LEN, len);
}

/*
 * Host transmit order: reject lengths exceeding the ring; wrap the
 * cursor to zero when cursor + length >= ring_size, including exact
 * fits. Copy the command into coherent memory, then dma_wmb(), write
 * the low and high message halves as u32, and ring BIT(endpoint).
 * Advance the cursor only after a successful send. PING uses EP1,
 * doorbell bit 1. Message and doorbell writes require mbi_doorbell;
 * these registers can fault even after power and aperture setup.
 */

/*
 * Channel records have stride 0x100: type at +0x40, doorbell bit at
 * +0x44, size at +0x48 and physical address at +0x50.
 */
#define ANE_MBI_CHAN_STRIDE	0x100
#define ANE_MBI_CHAN_MAX_DUMP	8

/* Mailbox management (EP 0); type bits [59:52], u64 message halves */
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

/* Start mailbox system endpoints when announced. */
#define ANE_RTKIT_EP_CRASHLOG	1
#define ANE_RTKIT_EP_SYSLOG	2
#define ANE_RTKIT_EP_DEBUG	3
#define ANE_RTKIT_EP_IOREPORT	4
#define ANE_RTKIT_EP_OSLOG	8
#define ANE_RTKIT_EP_TRACEKIT	0xa

/* Application endpoint IDs, ring sizes and channel tags. */
enum ane_t6021_eps {
	ANE_T6021_EP_INIT = 1,	/* INIT controller channel */
	ANE_T6021_EP_T2FC,	/* fw->host commands */
	ANE_T6021_EP_T2FH,	/* fw->host commands */
	ANE_T6021_EP_T2HS,
	ANE_T6021_EP_T2HC,
	ANE_T6021_EP_T2HT,	/* polled on the host */
	ANE_T6021_EP_COUNT = ANE_T6021_EP_T2HT + 1	/* arrays index by id */
};

/*
 * Host command IDs are u16 at wire offset +4. Target-to-host controller
 * headers instead carry a u32 ID at +8 in a 0x24-byte header.
 */
enum ane_t6021_csne_cmd {
	CSNE_CMD_START		= 0x0000,
	CSNE_CMD_STOP		= 0x0001,
	CSNE_CMD_REG_FILE_LOAD	= 0x0005,	/* Register-file payload, 1456 bytes */
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

/*
 * Surface announcement: address in bits 43:0, size code in bits 51:44,
 * unit in bits 53:52. Units encode no size, 4 KiB, 1 MiB or 2 MiB.
 * Round sizes up; use 4 KiB units below 1 MiB and 1 MiB units otherwise.
 * BUFFER_REQUEST uses 4 KiB units.
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
	u32 write_cursor;	/* Next ring slot */
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

	/*
	 * Bring-up state: power_gated means the domain checks passed;
	 * cpu_started means CPU RUN was released, so DMA memory remains owned;
	 * fw_alive means a fresh READY was observed; booted means DONE was
	 * observed. DONE alone does not enable a transport session.
	 */
	bool power_gated;
	bool cpu_started;
	bool fw_alive;
	bool booted;

	/*
	 * Mailbox receive is opt-in. The mailbox controls can remain enabled
	 * and empty while the CPU is stopped. MBI requires its own SCRATCH
	 * handshake; enabling mailbox receive does not complete that
	 * handshake.
	 */
	bool transport;
	bool doorbell;	/* Opt-in endpoint message and doorbell writes. */
	bool irq_requested;

	struct ane_t6021_ep ep[ANE_T6021_EP_COUNT];

	/*
	 * Coherent firmware surface mapped through the device IOMMU; NULL
	 * until loaded.
	 */
	void *fw_buf;
	dma_addr_t fw_iova;
	u32 fw_size;
	/*
	 * Firmware alias IOVA at the latched RVBAR entry; zero means no alias.
	 * Every DMA allocation must reject overlap with this window because
	 * it is not reserved in the DMA allocator.
	 */
	u64 fw_alias_iova;
	/*
	 * Record each mapped window separately. Teardown must unmap only
	 * these extents; the windows need not be adjacent.
	 */
#define ANE_FW_ALIAS_MAX_WIN	3
	u64 fw_alias_ext_iova[ANE_FW_ALIAS_MAX_WIN];
	size_t fw_alias_ext_len[ANE_FW_ALIAS_MAX_WIN];
	int fw_alias_extn;
	/* Optional CPU reset controller supplied by DT; NULL when absent. */
	struct reset_control *cpu_rst;

	/*
	 * Boot pool and IPC allocations are created after preflight and READY.
	 * Keep them while cpu_started; reboot reclaims that memory.
	 */
	void *boot_pool;		/* 'DDM ' pool, 0x40000 (Params word0) */
	dma_addr_t boot_pool_iova;
	void *boot_ipc;			/* 'IPC ' surface, max(0x4000, ord+1) */
	dma_addr_t boot_ipc_iova;
	u64 boot_ipc_size;
	void *boot_heap;		/* fw-requested HEAP surface or NULL */
	dma_addr_t boot_heap_iova;
	u64 boot_heap_size;
	/*
	 * Previous image length: zero on first boot, updated on reload.
	 */
	u32 prev_fw_len;
	/*
	 * Raw SCRATCH1:SCRATCH0 captured at DONE; never dereferenced directly.
	 */
	u64 boot_scratch_result;
	bool response_validated;
	/*
	 * Transport remains fenced until the DONE address and length are
	 * validated.
	 */
	bool hybrid_pinned;
};

/*
 * Every DMA allocation must reject overlap with the firmware entry
 * alias, which is not reserved in the DMA allocator.
 */
static inline bool ane_t6021_fw_alias_iova_ok(const struct ane_t6021 *ane,
					      dma_addr_t iova, size_t size)
{
	u64 lo = ane->fw_alias_iova;

	if (!lo)
		return true;
	return iova + size <= lo || iova >= lo + ane->fw_size;
}

/* Mailbox initialization, shutdown and receive helpers. */
int ane_t6021_rtkit_init(struct ane_t6021 *ane);
void ane_t6021_rtkit_shutdown(struct ane_t6021 *ane);
void ane_t6021_rtkit_drain(struct ane_t6021 *ane);
irqreturn_t ane_t6021_rtkit_irq_thread(int irq, void *data);

/*
 * Boot sequencing and state reporting. fw_boot=0 binds status-only.
 * A boot request requires all preflight conditions. The module is
 * pinned before CPU release; diagnostic stops return -ECANCELED.
 */
int ane_t6021_boot_start(struct ane_t6021 *ane, int stop_after, int table_mode,
			 int rtb_mode);

/*
 * Host command wire layout on INIT: little-endian, u16 ID at +4,
 * completion byte at +6 and response qword at +8. The ring slot also
 * holds the response. Commands must be shorter than 0x1b89 bytes.
 */
struct ane_csne_hdr {
	u32 rsvd0;	/* Reserved bytes 0..3, zeroed */
	u16 id;		/* Command ID at +4 */
	u8 flags;	/* Completion byte at +6 */
	u8 rsvd7;
};

static_assert(sizeof(struct ane_csne_hdr) == 8);

static inline void ane_csne_hdr_init(struct ane_csne_hdr *h, u16 id)
{
	memset(h, 0, sizeof(*h));
	h->id = id;
}

/*
 * BOOT, PING and BUILDINFO carry only the command header. Boot
 * arguments are published through SCRATCH rather than this command.
 */

/* REG_FILE_LOAD carries the register-file payload after the header. */
struct ane_csne_cmd_reg_file_load {
	struct ane_csne_hdr hdr;
	u8 blob[];
};

/* IPC_ENDPOINT_SET endpoint-binding payload; field layout remains provisional. */
struct ane_csne_cmd_ipc_endpoint_set {
	struct ane_csne_hdr hdr;
	u8 payload[];
};

/*
 * PROCEDURE_CALL fields: program/procedure IDs at +8/+0xc, opaque u64
 * at +0x10, priority/stats at +0x18 (valid values 2..7), buffer count at
 * +0x28 and 0x30-byte buffer records at +0x60. This driver submits one
 * command per ring slot. INFERENCE_CALL shares the declared layout;
 * its submission remains provisional.
 */
struct ane_csne_io_elem {
	u8 bytes[0x30];	/* Reserved internal fields */
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

/* Reject command sizes of 0x1b89 bytes or larger before submission. */
#define ANE_CSNE_CMD_MAX_SIZE	0x1b88

/*
 * LOAD_PROGRAM carries nine 0x30-byte section records starting at +8.
 * A present record contains a device-addressable object pointer at
 * +0x18 and its key at +0x20. Records with flags bit 0 clear are skipped.
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

/*
 * Section record accessors: flags byte at +0, bit 0 marks presence;
 * u64 object IOVA at +0x18, u64 key at +0x20.
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

/*
 * Program object layout: magic 1 at +0, count <= 0x10 at +4, entry
 * count in [0x201, 0x400] at +0x204, and 0x30-byte entries at +0x208.
 * The recorded pointer must stay within the entry table. The minimum
 * object includes a header and 0x201 zeroed entries.
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

/*
 * Operation section: u32 count <= 0x80 at +0, followed by count
 * 0x40c-byte records at +4. The section key is an offset past the array,
 * at least count * 0x40c + 4. Each record has type <= 4 at +0, a u16
 * <= 0x10 at +4, nonzero procedure count <= 0x80 at +8, and procedure
 * indices <= 0x3c starting at +0xc.
 */
#define ANE_OPSEC_OP_REC_SIZE	0x40c
#define ANE_OPSEC_MAX_OPS	0x80

static inline size_t ane_opsec_size(u32 ops)
{
	return 4 + (size_t)ops * ANE_OPSEC_OP_REC_SIZE;
}

/*
 * Submit an INIT ring command: allocate a slot with wrapping, copy the
 * command, notify with the surface word, and advance only on success.
 * The caller matches the asynchronous response.
 */
int ane_t6021_csne_submit(struct ane_t6021 *ane, const void *cmd, size_t size);

/* Opt-in EP1 PING; observe response surfaces for up to three seconds. */
void ane_t6021_csne_ping_attempt(struct ane_t6021 *ane);

/*
 * Validate and stage the image in device DMA memory. Boot sequencing
 * consumes the staged surface separately; staging alone does not
 * release the CPU.
 */
int ane_t6021_fwload_check(struct device *dev);
int ane_t6021_fwload_probe(struct ane_t6021 *ane);
void ane_t6021_fwload_remove(struct ane_t6021 *ane);
bool ane_t6021_fwload_options_ok(void);
bool ane_t6021_fwload_placement_ok(struct device *dev);
bool ane_t6021_fwload_requested(void);

struct ane_fw_image;
struct ane_asc_tunables;

/*
 * Per-SoC data: SoC/revision fields select boot parameters; revision_fuse
 * uses the DT fuse window on T8112. preload_placement permits the
 * optional reserved-memory mapping on T6021; other SoCs use owned RAM.
 * ps_cpu_off is the CPU power-state word in the PMGR window; pwgate_off
 * is a SET gate checked before engine reads (zero disables this check).
 * pmu_pa and ps_off locate the seven power-state words, mapped for the
 * device with IOVA == PA. trace_td_off locates the last-committed TD
 * word; zero disables TD sampling.
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

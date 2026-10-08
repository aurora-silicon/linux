// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T602x/T8112 ANE boot, transport and DRM accel driver.
 *
 * Boot staging and sequencing precede the MBI command transport. The
 * ABI exposes buffer allocation, program loading, process creation
 * and execution; userspace supplies section data in BOs. A global mutex
 * serializes commands. Execution completion requires both the reply
 * and the IO_T2H finish event; return target-to-host slots afterward.
 *
 * Power domains are managed through genpd and checked before engine
 * access. Non-posted MMIO is mandatory. Firmware memory is aliased at
 * a latched RVBAR entry when necessary. Each completed exchange
 * advances to the next 64-byte slot; repeatedly using one slot hangs
 * the ring.
 */

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/util_macros.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <drm/drm_accel.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mm.h>
#include <crypto/sha2.h>

#include "ane_stats.h"
#include "ane_t6021.h"
#include "ane_t6021_boot.h"

#include <drm/ane_accel.h>

/*
 * Doorbell block at engine +0x1844000: set at +0, pending at +0x8000, ack at
 * +0xc000.
 */
#define ANE_IPI_OFF			0x1844000

/* CPU_STATUS fields. */
#define ANE_ASC_CPU_STATUS_RUNNING	BIT(0)
#define ANE_ASC_CPU_STATUS_STOPPED	BIT(1)

/*
 * Boot parameters select staging, transport and diagnostic steps.
 * fw_load, fw_extra_ram and fw_alias_reserved are registered by the
 * loader; boot_prevent_nap is registered by the boot backend.
 */
static bool fw_start = true;
module_param(fw_start, bool, 0444);
MODULE_PARM_DESC(fw_start,
		 "Stage and map the image, then run the gated boot sequence (default on)");

static int fw_start_table_mode = 2;
module_param(fw_start_table_mode, int, 0444);
MODULE_PARM_DESC(fw_start_table_mode,
		 "Pre-CPU engine table: 0 abort, 1 write, 2 skip (default)");

static bool fw_start_rtb_mode;
module_param(fw_start_rtb_mode, bool, 0444);
MODULE_PARM_DESC(fw_start_rtb_mode,
		 "Mailbox application endpoints: 1 enabled, 0 MBI transport (default)");

/*
 * Poll mailbox RX from a workqueue to service HELLO and endpoint
 * announcements.
 */
static bool poll_rx = true;
module_param(poll_rx, bool, 0444);
MODULE_PARM_DESC(poll_rx,
		 "Drive mailbox RX from a polling workqueue (default on)");

/*
 * Start every announced application endpoint (>= 0x20) after the management
 * handshake.
 */
static bool start_app_eps = true;
module_param(start_app_eps, bool, 0444);
MODULE_PARM_DESC(start_app_eps,
		 "STARTEP every fw-announced app endpoint after the handshake (default on).");

static bool scratch3_ack = true;
module_param(scratch3_ack, bool, 0444);
MODULE_PARM_DESC(scratch3_ack,
		 "After DONE, write SCRATCH3 = 0x08042006 (default on)");

static bool legacy_only = true;
module_param(legacy_only, bool, 0444);
MODULE_PARM_DESC(legacy_only,
		 "Use the MBI command transport (default on)");

static bool legacy_query = true;
module_param(legacy_query, bool, 0444);
MODULE_PARM_DESC(legacy_query,
		 "Service bounded startup allocations and CONFIG_GET (default on)");

/*
 * The default zero skips mailbox management in MBI mode. Enabling
 * mailbox RX without HELLO can leave the level interrupt asserted and
 * cause an interrupt storm. Set a nonzero wait only when the image
 * supports the management handshake.
 */
static unsigned int hello_wait_ms;
module_param(hello_wait_ms, uint, 0444);
MODULE_PARM_DESC(hello_wait_ms,
		 "mailbox management HELLO wait in legacy mode; 0 (default) skips mailbox management and leaves the mailbox stopped. A firmware that speaks mailbox management needs 1000.");

/*
 * Producer-side stats (ane_stats sysfs, ane_timeline debugfs), read
 * once at probe. stats=0 keeps the hot path at its single predictable
 * branch (the NULL stats_slots check) and no files are created. Same
 * name and meaning as ane.ko's parameter.
 */
static bool stats = true;
module_param(stats, bool, 0444);
MODULE_PARM_DESC(stats,
		 "Expose ane_stats sysfs and ane_timeline debugfs (default 1; 0 = no files and the hot path skips the counters)");

#define ANE_LEGACY_ALLOCS 8192
#define ANE_LEGACY_BYTES SZ_512M

struct ane_legacy_buffer {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct ane_rtclient {
	struct device *dev;
	void __iomem *engine;
	void __iomem *pmgr;
	const struct ane_t602x_soc *soc;
	struct apple_rtkit *rtk;
	struct reset_control *cpu_rst;
	struct delayed_work poll_work;

	/* fw_start=1 view over the shared boot/fwload contract units. */
	struct ane_t6021 *fw;

	/* A CPU we released is (or may be) running: state HELD — surfaces,
	 * rings, IRQ, power links preserved; no unwind; reboot reclaims
	 * (wedged-pin rule).
	 */
	bool held;

	bool boot_done;

	/* MBI descriptor table validated against the 'IPC ' surface. */
	bool chman_ok;
	struct ane_legacy_buffer *legacy_buffers;
	u32 legacy_allocated;
	size_t legacy_bytes;
	u32 legacy_malloc_cursor;
	u32 legacy_cmd_cursor[ANE_T6021_CHMAN_COUNT];
	/*
	 * Reuse a 16 KiB command buffer only after the IO slot returns to
	 * host ownership with zero status. Allocation reply buffers stay
	 * mapped until reboot because the device may keep referencing them.
	 */
	struct ane_legacy_buffer *cmd_buf;

	/*
	 * Producer-side stats (see ane_stats.h). stats=0 or a failed
	 * ring allocation leaves stats_slots NULL, which is the one
	 * hot-path gate: no files, no counter updates.
	 */
	struct ane_stats_counters stats_ctrs;
	struct ane_stats_ring stats_ring;
	struct ane_stats_ring_entry *stats_slots;
};

/* Per-open BO ownership (drm_file->driver_priv). Handles live in the
 * fd's list; closing the fd drops its handles (postclose). The
 * coherent buffers themselves live as long as something references
 * them: a user mapping, or the firmware (see ane_t6021_bo_release).
 */
struct ane_t6021_fd {
	struct list_head bos;
};

/*
 * Per-file locking protects the handle counter and lists against
 * concurrent ioctls. Device-visible program BOs remain held until
 * reboot; IO BOs are parked for reuse. Other BOs free at their last
 * reference. A per-BO size limit bounds individual allocations, while
 * an atomic counter enforces the total-memory limit. A zero total
 * limit refuses BO_INIT. Every DMA allocation must be 16 KiB aligned.
 */
#define ANE_T6021_BO_MAX		SZ_1G
#define ANE_T6021_BO_HASH_CHUNK		SZ_1M

struct ane_t6021_bo {
	struct list_head node;
	struct ane_t6021_fd *owner;
	u32 handle;
	struct kref refcount;		/* handle + user mappings */
	void *cpu;			/* coherent mapping */
	dma_addr_t dma;
	size_t size;
	bool fw_ref;			/* the firmware received this IOVA */
	bool fw_program;		/* a loaded program keeps this IOVA */
	struct device *dev;
};

static DEFINE_MUTEX(ane_t6021_bo_lock);
static u32 ane_t6021_next_handle = 1;
static atomic64_t ane_t6021_bo_total_bytes = ATOMIC64_INIT(0);

static unsigned int bo_total_max_mb = 12288;
module_param(bo_total_max_mb, uint, 0444);
MODULE_PARM_DESC(bo_total_max_mb,
		 "Cap on the BO bytes held at one time, in MiB (default 12288)");

static int ane_t6021_bo_total_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&ane_t6021_bo_total_bytes));
}

static int ane_t6021_bo_total_set(const char *val,
				  const struct kernel_param *kp)
{
	return -EPERM;
}

static const struct kernel_param_ops ane_t6021_bo_total_ops = {
	.set = ane_t6021_bo_total_set,
	.get = ane_t6021_bo_total_get,
};
module_param_cb(bo_total_bytes, &ane_t6021_bo_total_ops, NULL, 0444);
MODULE_PARM_DESC(bo_total_bytes,
		 "Read only: the BO bytes counted against bo_total_max_mb now");

/* Mark the device quarantined: a timed-out command left the firmware
 * queue state unknown. The only safe next step is to refuse further
 * ioctls until a reboot reclaims the surfaces (wedged-pin rule).
 */
static atomic_t ane_t6021_quarantined = ATOMIC_INIT(0);

/* Parked io BOs: their last user is gone, the IOVA stays mapped and the
 * bytes stay counted. BO_INIT of the same page-aligned size takes one,
 * so held memory stays at the peak of concurrent io BOs instead of
 * growing with every process until the BO cap refuses BO_INIT.
 */
static LIST_HEAD(ane_t6021_bo_pool);
static DEFINE_SPINLOCK(ane_t6021_bo_pool_lock);

/*
 * On final user reference, keep device-visible program sections
 * mapped. Park IO BOs for reuse unless the device is quarantined and
 * may still write them. Free only BOs with no device reference, then
 * release their memory accounting.
 */
static void ane_t6021_bo_release(struct kref *ref)
{
	struct ane_t6021_bo *bo = container_of(ref, struct ane_t6021_bo,
					       refcount);

	if (!bo->fw_ref) {
		atomic64_sub(PAGE_ALIGN(bo->size), &ane_t6021_bo_total_bytes);
		dma_free_coherent(bo->dev, bo->size, bo->cpu, bo->dma);
		kfree(bo);
	} else if (bo->fw_program || atomic_read(&ane_t6021_quarantined)) {
		kfree(bo);
	} else {
		spin_lock(&ane_t6021_bo_pool_lock);
		list_add(&bo->node, &ane_t6021_bo_pool);
		spin_unlock(&ane_t6021_bo_pool_lock);
	}
}

/* A parked io BO of SIZE's page-aligned size, or NULL. */
static struct ane_t6021_bo *ane_t6021_bo_pool_take(size_t size)
{
	struct ane_t6021_bo *bo;

	if (atomic_read(&ane_t6021_quarantined))
		return NULL;
	spin_lock(&ane_t6021_bo_pool_lock);
	list_for_each_entry(bo, &ane_t6021_bo_pool, node) {
		if (PAGE_ALIGN(bo->size) == PAGE_ALIGN(size)) {
			list_del(&bo->node);
			spin_unlock(&ane_t6021_bo_pool_lock);
			return bo;
		}
	}
	spin_unlock(&ane_t6021_bo_pool_lock);
	return NULL;
}

/* A user mapping holds its BO's memory until it is torn down: open
 * (fork, mremap split) takes a reference, close drops it. A BO the
 * user freed while mapped stays allocated until the last mapping
 * goes away.
 */
static void ane_t6021_vm_open(struct vm_area_struct *vma)
{
	kref_get(&((struct ane_t6021_bo *)vma->vm_private_data)->refcount);
}

static void ane_t6021_vm_close(struct vm_area_struct *vma)
{
	kref_put(&((struct ane_t6021_bo *)vma->vm_private_data)->refcount,
		 ane_t6021_bo_release);
}

static const struct vm_operations_struct ane_t6021_vm_ops = {
	.open = ane_t6021_vm_open,
	.close = ane_t6021_vm_close,
};

/* Serializes every firmware command (LOAD/CREATE/CALL) so the
 * cursor-on-next-64-byte-slot rule and the PMGR/TM gate cannot race.
 */
static DEFINE_MUTEX(ane_t6021_fw_lock);

/* Forward declaration (defined below). */
static int ane_rtclient_legacy_exchange(struct ane_rtclient *ane,
					struct ane_legacy_buffer *command,
				       size_t length, u16 opcode,
				       unsigned int channel,
				       unsigned int timeout_ms);

/* ---- boot-contract helpers (shared with boot.c / fwload.c) ---- */

static int ane_rtclient_legacy_alloc(struct ane_rtclient *ane, u64 size)
{
	struct ane_legacy_buffer *buffer;

	if (!size || !IS_ALIGNED(size, SZ_16K) || size > SZ_2M ||
	    ane->legacy_allocated == ANE_LEGACY_ALLOCS ||
	    size > ANE_LEGACY_BYTES - ane->legacy_bytes)
		return -E2BIG;
	buffer = &ane->legacy_buffers[ane->legacy_allocated];
	buffer->cpu = dma_alloc_coherent(ane->dev, size, &buffer->dma,
					 GFP_KERNEL);
	if (!buffer->cpu)
		return -ENOMEM;
	if (!IS_ALIGNED(buffer->dma, SZ_16K) ||
	    (ane->fw && !ane_t6021_fw_alias_iova_ok(ane->fw, buffer->dma, size))) {
		dma_free_coherent(ane->dev, size, buffer->cpu, buffer->dma);
		buffer->cpu = NULL;
		return -ERANGE;
	}
	memset(buffer->cpu, 0, size);
	buffer->size = size;
	ane->legacy_bytes += size;
	ane->legacy_allocated++;
	return 0;
}

/* Validate the MBI table the fw published in the 'IPC ' surface. */
static void ane_rtclient_validate_chman(struct ane_rtclient *ane)
{
	struct ane_t6021 *a = ane->fw;
	const struct ane_t6021_chman_desc *t;
	unsigned int i, bad;

	if (!a || !a->boot_ipc) {
		dev_info(ane->dev,
			 "channel table: no host IPC surface — table not validated\n");
		return;
	}
	if (a->boot_ipc_size < ANE_T6021_CHMAN_TOTAL) {
		dev_warn(ane->dev,
			 "channel table: IPC surface %#llx bytes < fw layout %#x — table not validated\n",
			 a->boot_ipc_size, ANE_T6021_CHMAN_TOTAL);
		return;
	}

	dma_rmb();
	t = a->boot_ipc;
	bad = ane_t6021_chman_check(t, a->boot_ipc_iova);
	for (i = 0; i < ANE_T6021_CHMAN_COUNT; i++) {
		const struct ane_t6021_chman_desc *d = &t[i];
		const struct ane_t6021_chman_static *s = &ane_t6021_chman_layout[i];

		dev_dbg(ane->dev,
			"channel table[%u]: name=\"%.*s\" type=%u bit=%u size=%#llx %s (static: %s/%u/%u/%#llx/ipc+%#x)\n",
			i, ANE_T6021_CHMAN_NAME_LEN, d->name, d->type, d->bit,
			d->size,
			(bad & BIT(i)) ? "MISMATCH" : "OK",
			s->name, s->type, s->bit, s->size, s->off);
	}

	ane->chman_ok = !bad;
	dev_info(ane->dev, "channel table: %s (mismatch mask %#x)\n",
		 bad ? "NOT VALIDATED" : "VALIDATED", bad);
}

/*
 * Post-DONE MBI exchange. Each completed command advances to the next
 * 64-byte IO slot; repeatedly submitting to one slot can hang the ring.
 * Bit 0 marks host ownership.
 */
static int ane_rtclient_legacy_exchange(struct ane_rtclient *ane,
					struct ane_legacy_buffer *command,
				       size_t length, u16 opcode,
				       unsigned int channel,
				       unsigned int timeout_ms)
{
	struct ane_t6021 *a = ane->fw;
	u64 *io, *malloc_ring, header;
	u64 *t2h_buf, *t2h_ioq;
	void __iomem *ipi = ane->engine + ANE_IPI_OFF;
	unsigned int cursor = ane->legacy_malloc_cursor;
	unsigned long deadline;
	u32 *reply;
	int result = -ETIMEDOUT;

	if (!ane->held || !ane->chman_ok ||
	    ane_t6021_chman_check(a->boot_ipc, a->boot_ipc_iova))
		return -EPROTO;
	if (length < 8 || length > command->size ||
	    channel >= ANE_T6021_CHMAN_COUNT) {
		result = -EINVAL;
		goto out;
	}
	io = a->boot_ipc + ane_t6021_chman_layout[channel].off +
	     (size_t)ane->legacy_cmd_cursor[channel] * 64;
	malloc_ring = a->boot_ipc + ane_t6021_chman_layout[5].off;
	t2h_buf = a->boot_ipc + ane_t6021_chman_layout[4].off;
	t2h_ioq = a->boot_ipc + ane_t6021_chman_layout[6].off;

	if (!(READ_ONCE(io[0]) & 1)) {
		result = -EBUSY;
		goto out;
	}
	reply = command->cpu;
	((u16 *)reply)[2] = opcode;
	WRITE_ONCE(io[1], length);
	WRITE_ONCE(io[2], length);
	dma_wmb();
	WRITE_ONCE(io[0], command->dma);
	dma_wmb();
	writel(BIT(ane_t6021_chman_layout[channel].bit), ipi);
	deadline = jiffies + msecs_to_jiffies(timeout_ms);
	while (time_before(jiffies, deadline)) {
		u32 pending = readl(ipi + 0x8000);

		if (pending) {
			writel(pending, ipi + 0xc000);
			/* Complete the IPI acknowledgment before the
			 * next reads of the command and malloc rings.
			 */
			mb();
		}
		header = READ_ONCE(io[0]);
		if (!(header & 1)) {
			/* malloc-ring service: the fw may ask for an
			 * allocation while we wait; service those until
			 * the command slot flips back.
			 */
			u64 *slot = malloc_ring + cursor * 8;
			u64 alloc_hdr = READ_ONCE(slot[0]);

			if (!(alloc_hdr & 1)) {
				u64 size, tag;
				struct ane_legacy_buffer *buf;

				dma_rmb();
				size = READ_ONCE(slot[1]);
				tag = READ_ONCE(slot[2]);
				/*
				 * A valid allocation slot has header zero and
				 * a u32 tag; stop on malformed entries.
				 */
				if (alloc_hdr || tag > U32_MAX) {
					result = -EOPNOTSUPP;
					goto out;
				}
				if (ane_rtclient_legacy_alloc(ane, size)) {
					result = -ENOMEM;
					goto out;
				}
				buf = &ane->legacy_buffers[ane->legacy_allocated - 1];
				WRITE_ONCE(slot[1], 0);
				WRITE_ONCE(slot[2], ane->legacy_allocated);
				dma_wmb();
				WRITE_ONCE(slot[0], buf->dma | 1);
				dma_wmb();
				writel(BIT(5), ipi);
				cursor = (cursor + 1) %
					ane_t6021_chman_layout[5].size;
			}
		}
		header = READ_ONCE(io[0]);
		if (header & 1) {
			dma_rmb();
			result = (header == (command->dma | 1) &&
				  READ_ONCE(io[1]) == length &&
				  READ_ONCE(io[2]) == 0 &&
				  ((u16 *)reply)[2] == opcode &&
				  ((u16 *)reply)[3] == 0) ? 0 : -EPROTO;
			ane->legacy_cmd_cursor[channel] =
				(ane->legacy_cmd_cursor[channel] + 1) %
				ane_t6021_chman_layout[channel].size;
			goto out;
		}
		/*
		 * Fast polling checks the rings every 50 us instead of
		 * sleeping 1..2 ms.
		 */
		udelay(50);
	}
	dev_info(ane->dev, "MBI timeout ch=%u io=%016llx\n",
		 channel, READ_ONCE(io[0]));
out:
	ane->legacy_malloc_cursor = cursor;
	return result;
}

/*
 * Extra sleep after a CALL's finish event. 0 by default: the finish event
 * already marks the output in DRAM (ane_rtclient_call_wait). The 1 ms
 * default it replaces dated from a wait on dispatch signals; set it only
 * to test a suspected late output write.
 */
static unsigned int call_settle_us;
module_param(call_settle_us, uint, 0644);
MODULE_PARM_DESC(call_settle_us,
		 "Microseconds to sleep after a CALL's finish event (default 0: the event marks the output in DRAM)");

/*
 * Read-only per-CALL TD timeline, disabled by default. When enabled,
 * poll completion every 20..40 us and sample the last-committed TD word
 * only while all seven power-state words read 0x3ff. TM reads while
 * compute domains are off can hang the SoC. T8112 has no supported TD
 * offset and records nothing.
 *
 * Records carry ktime timestamps for submission, acknowledgment,
 * changed TD values, IO_T2H events, power-gate failure and completion.
 * The TD word holds call ID in bits 23:16 and task index in bits 15:0.
 * Allocate the buffer on first enable and retain it until unload; each
 * enable clears it and excess records increment dropped. Read debugfs
 * only when no CALL runs. ane_t6021_fw_lock protects all timeline state.
 */
#define ANE_TRACE_MAGIC		0x31445441	/* "ATD1" */
#define ANE_TRACE_RECS		BIT(18)
#define ANE_PMGR_PS_LAST_OFF	0x30

enum {
	ANE_TR_CALL = 1,
	ANE_TR_ACK,
	ANE_TR_TD,
	ANE_TR_EVENT,
	ANE_TR_GATE,
	ANE_TR_DONE,
};

struct ane_t6021_trace_rec {
	u64 t_ns;
	u32 word;
	u16 kind;
	u16 call;
};

struct ane_t6021_trace {
	u32 magic;
	u32 rec_size;
	u32 capacity;
	u32 n;
	u32 dropped;
	u32 calls;
	u64 reserved;
	struct ane_t6021_trace_rec r[];
};

static bool trace_td;
static bool ane_t6021_tracing;	/* the running CALL is traced */
static struct ane_t6021_trace *ane_t6021_trace;
static struct debugfs_blob_wrapper ane_t6021_trace_blob;
static struct dentry *ane_t6021_trace_dir;

static void ane_t6021_trace_add(u16 kind, u32 word)
{
	struct ane_t6021_trace *t = ane_t6021_trace;

	if (t->n == t->capacity) {
		t->dropped++;
		return;
	}
	t->r[t->n++] = (struct ane_t6021_trace_rec){
		.t_ns = ktime_get_ns(), .word = word, .kind = kind,
		.call = t->calls,
	};
}

static int ane_t6021_trace_set(const char *val, const struct kernel_param *kp)
{
	size_t size = struct_size_t(struct ane_t6021_trace, r, ANE_TRACE_RECS);
	bool on;
	int ret;

	ret = kstrtobool(val, &on);
	if (ret)
		return ret;
	mutex_lock(&ane_t6021_fw_lock);
	if (on && !ane_t6021_trace) {
		ane_t6021_trace = vzalloc(size);
		if (!ane_t6021_trace) {
			ret = -ENOMEM;
			goto out;
		}
		ane_t6021_trace->magic = ANE_TRACE_MAGIC;
		ane_t6021_trace->rec_size = sizeof(struct ane_t6021_trace_rec);
		ane_t6021_trace->capacity = ANE_TRACE_RECS;
		ane_t6021_trace_blob.data = ane_t6021_trace;
		ane_t6021_trace_blob.size = size;
		ane_t6021_trace_dir = debugfs_create_dir("ane_t6021", NULL);
		debugfs_create_blob("trace_td", 0400, ane_t6021_trace_dir,
				    &ane_t6021_trace_blob);
	}
	if (on && !trace_td) {
		ane_t6021_trace->n = 0;
		ane_t6021_trace->dropped = 0;
		ane_t6021_trace->calls = 0;
	}
	trace_td = on;
out:
	mutex_unlock(&ane_t6021_fw_lock);
	return ret;
}

static const struct kernel_param_ops ane_t6021_trace_ops = {
	.set = ane_t6021_trace_set,
	.get = param_get_bool,
};
module_param_cb(trace_td, &ane_t6021_trace_ops, &trace_td, 0644);
MODULE_PARM_DESC(trace_td,
		 "Record a read-only per-CALL TD-word timeline in debugfs ane_t6021/trace_td (default 0; T602x only)");

static void ane_t6021_trace_free(void)
{
	debugfs_remove_recursive(ane_t6021_trace_dir);
	vfree(ane_t6021_trace);
}

/* The CALL cookie (CALL +0x20); the firmware returns it in the call's
 * IO_T2H events.
 */
#define ANE_CALL_COOKIE		0xADD0

/*
 * PROCEDURE_CALL IO_T2H event, 0x28 bytes: sequence, type 0x300,
 * u64 cookie, program ID, process ID, reserved zero and state. State
 * zero acknowledges acceptance; state one signals procedure completion.
 */
#define ANE_T2H_CALL_COOKIE_OFF		0x08
#define ANE_T2H_CALL_STATE_OFF		0x1c
#define ANE_T2H_CALL_FINISHED		1

/* The CPU address of LEN bytes at the firmware IOVA, or NULL. The firmware
 * places T2H payloads in memory the host gave it: a SHAREDMALLOC buffer or
 * the 'IPC ' surface.
 */
static const void *ane_rtclient_fw_cpu(struct ane_rtclient *ane, u64 iova,
				       size_t len)
{
	u32 i;

	for (i = 0; i < ane->legacy_allocated; i++) {
		const struct ane_legacy_buffer *b = &ane->legacy_buffers[i];

		if (iova >= b->dma && iova - b->dma + len <= b->size)
			return b->cpu + (iova - b->dma);
	}
	if (iova >= ane->fw->boot_ipc_iova &&
	    iova - ane->fw->boot_ipc_iova + len <= ane->fw->boot_ipc_size)
		return ane->fw->boot_ipc + (iova - ane->fw->boot_ipc_iova);
	return NULL;
}

/*
 * Return target-to-host slots when legacy_t2h_ack is enabled. The
 * device publishes a DMA address with bit 0 clear; the host returns
 * ownership by setting bit 0, after processing the payload.
 */
static bool ane_rtclient_drain_t2h(struct ane_rtclient *ane,
				   unsigned int channel)
{
	const struct ane_t6021_chman_static *c = &ane_t6021_chman_layout[channel];
	unsigned int n = 0, slot_i = ane->legacy_cmd_cursor[channel];
	void __iomem *ipi = ane->engine + ANE_IPI_OFF;
	bool finished = false;

	if (!ane->fw || !ane->fw->boot_ipc)
		return false;
	while (n < c->size) {
		u64 *slot = ane->fw->boot_ipc + c->off + (size_t)slot_i * 64;
		u64 hdr = READ_ONCE(slot[0]), len = READ_ONCE(slot[1]);

		if (hdr & 1)
			break;
		dma_rmb();
		dev_dbg(ane->dev, "T2H ch=%s slot=%u hdr=%016llx len=%#llx\n",
			c->name, slot_i, hdr, len);
		if (channel == 6 && len >= ANE_T2H_CALL_STATE_OFF + 4) {
			const u8 *ev = ane_rtclient_fw_cpu(ane, hdr,
							   ANE_T2H_CALL_STATE_OFF + 4);

			if (ev &&
			    get_unaligned_le64(ev + ANE_T2H_CALL_COOKIE_OFF) ==
			    ANE_CALL_COOKIE) {
				u32 state = get_unaligned_le32(ev +
							       ANE_T2H_CALL_STATE_OFF);

				if (ane_t6021_tracing)
					ane_t6021_trace_add(ANE_TR_EVENT, state);
				if (state == ANE_T2H_CALL_FINISHED)
					finished = true;
			}
		}
		n++;
		WRITE_ONCE(slot[0], hdr | 1);
		dma_wmb();
		writel(BIT(c->bit), ipi);
		slot_i = (slot_i + 1) % c->size;
		ane->legacy_cmd_cursor[channel] = slot_i;
	}
	return finished;
}

/*
 * Completion polling with TD sampling uses the same finish event and power-
 * state guard.
 */
static int ane_rtclient_call_wait_traced(struct ane_rtclient *ane,
					 unsigned long deadline)
{
	void __iomem *td = ane->engine + ane->soc->trace_td_off;
	void __iomem *ps = ioremap_np(ane->soc->pmu_pa + ane->soc->ps_off,
				      ANE_PMGR_PS_LAST_OFF + 4);
	unsigned int i, gate = ANE_PMGR_PS_LAST_OFF + 8;
	u32 last = U32_MAX, samples = 0;
	int ret = -ETIMEDOUT;

	do {
		if (ane_rtclient_drain_t2h(ane, 6)) {
			ret = 0;
			break;
		}
		if (ps) {
			for (i = 0; i <= ANE_PMGR_PS_LAST_OFF; i += 8)
				if ((readl(ps + i) & 0x3ff) != 0x3ff)
					break;
			if (i > ANE_PMGR_PS_LAST_OFF) {
				u32 w = readl(td);

				samples++;
				if (w != last)
					ane_t6021_trace_add(ANE_TR_TD, w);
				last = w;
			} else if (i != gate) {
				ane_t6021_trace_add(ANE_TR_GATE, i);
			}
			gate = i;
		}
		usleep_range(20, 40);
	} while (time_before(jiffies, deadline));
	if (ret && ane_rtclient_drain_t2h(ane, 6))
		ret = 0;
	ane_t6021_trace_add(ANE_TR_DONE, samples);
	if (ps)
		iounmap(ps);
	return ret;
}

/*
 * Wait for the PROCEDURE_CALL finish event on IO_T2H channel 6.
 * Acknowledgment, TQ status and the last-committed TD word indicate
 * dispatch rather than execution completion. On our hardware, returning
 * at dispatch could expose zero output before the finish event.
 * The finish event is also the output-landed signal, so call_settle_us
 * defaults to 0: measured 2026-10-06, 6000 add calls with new inputs
 * per call each read their own result right after the event, and
 * program 20 and the Parakeet encoder stayed bit-identical.
 * Return zero on the event, or -ETIMEDOUT.
 */
static int ane_rtclient_call_wait(struct ane_rtclient *ane,
				  unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	if (ane_t6021_tracing)
		return ane_rtclient_call_wait_traced(ane, deadline);

	do {
		if (ane_rtclient_drain_t2h(ane, 6))
			return 0;
		usleep_range(50, 100);
	} while (time_before(jiffies, deadline));
	return ane_rtclient_drain_t2h(ane, 6) ? 0 : -ETIMEDOUT;
}

static int ane_rtclient_command(struct ane_rtclient *ane,
				struct ane_legacy_buffer *command,
						     size_t length, u16 opcode,
						     unsigned int channel,
						     unsigned int timeout_ms)
{
	int ret;
	u64 stats_ticket = 0;
	u64 stats_submit_ns = 0;

	ane_t6021_tracing = opcode == CSNE_CMD_PROCEDURE_CALL && trace_td &&
			    ane->soc->trace_td_off;
	if (ane_t6021_tracing) {
		ane_t6021_trace->calls++;
		ane_t6021_trace_add(ANE_TR_CALL, 0);
	}
	/*
	 * Record submission at enqueue and completion after the call returns.
	 * Count only PROCEDURE_CALL as engine work; other commands are control
	 * exchanges. Overlapping calls share a busy period. tmst is zero,
	 * tasks is one, rc is the return value. Hooks require a stats ring.
	 */
	bool stats_call = ane->stats_slots &&
			  opcode == CSNE_CMD_PROCEDURE_CALL;

	if (stats_call) {
		stats_submit_ns = ktime_get_ns();
		stats_ticket = ane_stats_begin(&ane->stats_ctrs,
					       &ane->stats_ring,
					       stats_submit_ns, 1);
	}
	ret = ane_rtclient_legacy_exchange(ane, command, length, opcode,
					   channel, timeout_ms);
	if (ret) {
		ane_t6021_tracing = false;
		if (stats_call)
			ane_stats_complete(&ane->stats_ctrs,
					   &ane->stats_ring, stats_ticket,
					   ktime_get_ns(), (u32)ret, 0);
		dev_info(ane->dev, "EXCH op=%#x failed %d (fw allocs %u, %zu bytes)\n",
			 opcode, ret, ane->legacy_allocated, ane->legacy_bytes);
		atomic_set(&ane_t6021_quarantined, 1);
		return ret;
	}
	if (opcode == CSNE_CMD_PROCEDURE_CALL) {
		if (ane_t6021_tracing)
			ane_t6021_trace_add(ANE_TR_ACK, 0);
		ret = ane_rtclient_call_wait(ane, timeout_ms);
		ane_t6021_tracing = false;
		if (!ret && call_settle_us)
			usleep_range(call_settle_us, call_settle_us + 100);
		if (ret) {
			if (stats_call)
				ane_stats_complete(&ane->stats_ctrs,
						   &ane->stats_ring,
						   stats_ticket,
						   ktime_get_ns(),
						   (u32)ret, 0);
			dev_err(ane->dev,
				"call completion wait failed %d: no finish event in %u ms\n",
				ret, timeout_ms);
			atomic_set(&ane_t6021_quarantined, 1);
			return ret;
		}
	}
	if (stats_call)
		ane_stats_complete(&ane->stats_ctrs, &ane->stats_ring,
				   stats_ticket, ktime_get_ns(), 0, 0);
	/*
	 * Return target-to-host slots on channels 4 and 6 so the rings do not
	 * fill.
	 */
	ane_rtclient_drain_t2h(ane, 4);
	ane_rtclient_drain_t2h(ane, 6);
	/* BOs are dma_alloc_coherent memory mapped write-combined for the
	 * CPU, so no cache maintenance is needed on either side.
	 */
	return 0;
}

/* ---- LOAD / CREATE / CALL wrappers ---- */

/*
 * The program table holds 256 entries; exceeding it returned a
 * protocol error on our hardware. Identical sections share one program
 * and process, keyed by SHA-256 over section IDs, sizes and bytes.
 * ane_t6021_fw_lock protects the table.
 */
#define ANE_T6021_MAX_PROGRAMS 250

struct ane_t6021_prog {
	u8 digest[SHA256_DIGEST_SIZE];
	u32 prog_id;
	u32 proc_id;		/* U32_MAX until a process exists */
};

static struct ane_t6021_prog ane_t6021_progs[ANE_T6021_MAX_PROGRAMS];
static unsigned int ane_t6021_nprogs;

static struct ane_t6021_prog *ane_t6021_prog_find(const u8 *digest)
{
	unsigned int i;

	for (i = 0; i < ane_t6021_nprogs; i++)
		if (!memcmp(ane_t6021_progs[i].digest, digest,
			    SHA256_DIGEST_SIZE))
			return &ane_t6021_progs[i];
	return NULL;
}

static struct ane_t6021_prog *ane_t6021_prog_by_id(u32 prog_id)
{
	unsigned int i;

	for (i = 0; i < ane_t6021_nprogs; i++)
		if (ane_t6021_progs[i].prog_id == prog_id)
			return &ane_t6021_progs[i];
	return NULL;
}

/* Build a LOAD_PROGRAM (0x200) message in a kernel-owned buffer. The
 * section bytes live in the BOs the user supplied (section_ptr), the
 * generic binds (bufferId, bo_handle, size) patch the generic section
 * at iova +0x20 / +0x28 once the BO is copied. Returns 0 with
 * *prog_id set on success.
 */
static int ane_rtclient_load_program(struct ane_rtclient *ane,
				     struct drm_file *file,
				     const struct drm_ane_prog_load *user,
				     __u32 *prog_id)
{
	struct drm_ane_section *sections;
	struct drm_ane_generic_bind *binds;
	struct ane_legacy_buffer *command;
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_t6021_prog *cached;
	u8 digest[SHA256_DIGEST_SIZE];
	size_t binds_size;
	int ret, i, j;

	BUILD_BUG_ON(DRM_ANE_MAX_SECTIONS != ANE_T6021_LOAD_SEC_COUNT);
	if (user->pad || user->section_count < 1 ||
	    user->section_count > ANE_T6021_LOAD_SEC_COUNT ||
	    user->generic_count > DRM_ANE_MAX_BINDS || !fd)
		return -EINVAL;

	sections = kmalloc_array(user->section_count, sizeof(*sections),
				 GFP_KERNEL);
	binds = kmalloc_array(user->generic_count, sizeof(*binds),
			      GFP_KERNEL);
	if (!sections || !binds) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(sections,
			   u64_to_user_ptr(user->sections_ptr),
			   user->section_count * sizeof(*sections))) {
		ret = -EFAULT;
		goto out;
	}
	binds_size = user->generic_count * sizeof(*binds);
	if (binds_size && copy_from_user(binds,
					 u64_to_user_ptr(user->generic_ptr),
					binds_size)) {
		ret = -EFAULT;
		goto out;
	}
	for (i = 0; i < user->generic_count; i++) {
		if (binds[i].pad) {
			ret = -EINVAL;
			goto out;
		}
	}

	/*
	 * Section slot is ID - 1; reject duplicate and out-of-range IDs before
	 * encoding.
	 */
	for (i = 0; i < user->section_count; i++) {
		if (sections[i].id < 1 ||
		    sections[i].id > ANE_T6021_LOAD_SEC_COUNT) {
			ret = -EINVAL;
			goto out;
		}
		for (j = i + 1; j < user->section_count; j++) {
			if (sections[i].id == sections[j].id) {
				ret = -EINVAL;
				goto out;
			}
		}
	}

	/*
	 * Identical sections share a program. Hash large sections in bounded
	 * chunks under bo_lock to avoid oversized stack arguments.
	 */
	{
		struct sha256_ctx sha;
		void *scratch;

		sha256_init(&sha);
		scratch = kvmalloc(ANE_T6021_BO_HASH_CHUNK, GFP_KERNEL);
		if (!scratch) {
			ret = -ENOMEM;
			goto out;
		}
		for (i = 0; i < user->section_count; i++) {
			struct ane_t6021_bo *bo = NULL, *b;
			u64 hdr[2] = { sections[i].id, sections[i].size };
			size_t left;

			mutex_lock(&ane_t6021_bo_lock);
			list_for_each_entry(b, &fd->bos, node) {
				if (b->handle == sections[i].bo_handle) {
					bo = b;
					break;
				}
			}
			if (!bo || sections[i].size > bo->size ||
			    sections[i].offset > bo->size - sections[i].size) {
				mutex_unlock(&ane_t6021_bo_lock);
				kvfree(scratch);
				ret = -EINVAL;
				goto out;
			}
			sha256_update(&sha, (const u8 *)hdr, sizeof(hdr));
			left = sections[i].size;
			while (left) {
				size_t n = min_t(size_t, left,
						 ANE_T6021_BO_HASH_CHUNK);

				memcpy(scratch, (const u8 *)bo->cpu +
				       sections[i].offset +
				       (sections[i].size - left), n);
				sha256_update(&sha, scratch, n);
				left -= n;
			}
			mutex_unlock(&ane_t6021_bo_lock);
		}
		kvfree(scratch);
		sha256_final(&sha, digest);
	}
	cached = ane_t6021_prog_find(digest);
	if (cached) {
		*prog_id = cached->prog_id;
		ret = 0;
		goto out;
	}
	if (ane_t6021_nprogs == ANE_T6021_MAX_PROGRAMS) {
		ret = -ENOSPC;
		goto out;
	}

	command = ane->cmd_buf;
	if (!command) {
		ret = -ENODEV;
		goto out;
	}
	memset(command->cpu, 0, SZ_16K);
	/*
	 * Place each 0x30-byte section record at slot ID - 1. Unused records
	 * remain zero. Present records have flags bit 0 set, ID at +4, IOVA
	 * at +0x18 and size at +0x20.
	 */
	for (i = 0; i < user->section_count; i++) {
		struct ane_t6021_bo *bo = NULL, *b;
		u64 slot_base, iova;
		u8 *cmd = command->cpu;

		mutex_lock(&ane_t6021_bo_lock);
		list_for_each_entry(b, &fd->bos, node) {
			if (b->handle == sections[i].bo_handle) {
				bo = b;
				break;
			}
		}
		if (bo && sections[i].size <= bo->size &&
		    sections[i].offset <= bo->size - sections[i].size) {
			/* The IOVA below is about to be published to the
			 * firmware, so mark the BO before the exchange:
			 * the fw may read the section any time after the
			 * doorbell rings, including during a timeout.
			 * Under bo_lock the handle reference cannot go
			 * away, so no kref is needed here.
			 */
			bo->fw_ref = true;
			bo->fw_program = true;
			iova = bo->dma + sections[i].offset;
		} else {
			bo = NULL;
		}
		mutex_unlock(&ane_t6021_bo_lock);
		if (!bo) {
			ret = -EINVAL;
			goto out;
		}
		slot_base = 0x8 + (u64)(sections[i].id - 1) * 0x30;
		*(u32 *)(cmd + slot_base + 0x00) = cpu_to_le32(1);
		*(u32 *)(cmd + slot_base + 0x04) =
			cpu_to_le32(sections[i].id);
		*(u64 *)(cmd + slot_base + 0x18) = cpu_to_le64(iova);
		*(u64 *)(cmd + slot_base + 0x20) =
			cpu_to_le64(sections[i].size);
	}
	/*
	 * Generic binds remain accepted for ABI compatibility. Section records
	 * already carry the IOVA and size; userspace supplies the generic
	 * section contents.
	 */

	/* ProgramId placeholder; the firmware writes the assigned id
	 * back at +0x1b8.
	 */
	{
		u8 *cmd = command->cpu;

		*(u32 *)(cmd + 0x1b8) = cpu_to_le32(U32_MAX);
	}

	/*
	 * LOAD command length is 0x1c0: nine records end at +0x1b8, followed
	 * by the program ID and zero tail. The declared structure is 0x1b8,
	 * so the transport buffer explicitly includes the extra eight bytes.
	 */
	BUILD_BUG_ON(sizeof(struct ane_csne_cmd_load_program) != 0x1b8);
	ret = ane_rtclient_command(ane, command,
				   0x1c0,
						       CSNE_CMD_LOAD_PROGRAM,
						       1, 5000);
	if (!ret) {
		u8 *cmd = command->cpu;

		*prog_id = le32_to_cpu(*(u32 *)(cmd + 0x1b8));
		if (*prog_id == U32_MAX) {
			ret = -EPROTO;
			goto out;
		}
		memcpy(ane_t6021_progs[ane_t6021_nprogs].digest, digest,
		       SHA256_DIGEST_SIZE);
		ane_t6021_progs[ane_t6021_nprogs].prog_id = *prog_id;
		ane_t6021_progs[ane_t6021_nprogs].proc_id = U32_MAX;
		ane_t6021_nprogs++;
	}

out:
	kfree(sections);
	kfree(binds);
	return ret;
}

static int ane_rtclient_create_process(struct ane_rtclient *ane,
				       __u32 prog_id, __u32 *proc_id)
{
	struct ane_legacy_buffer *command;
	struct ane_t6021_prog *prog = ane_t6021_prog_by_id(prog_id);
	int ret;

	if (!prog)
		return -ENOENT;
	if (prog->proc_id != U32_MAX) {
		*proc_id = prog->proc_id;
		return 0;
	}
	command = ane->cmd_buf;
	if (!command)
		return -ENODEV;
	memset(command->cpu, 0, SZ_16K);
	{
		u8 *cmd = command->cpu;

		*(u32 *)(cmd + 0x08) = cpu_to_le32(prog_id);
		*(u32 *)(cmd + 0x0c) = cpu_to_le32(U32_MAX);
	}
	ret = ane_rtclient_command(ane, command, 0x10,
				   CSNE_CMD_CREATE_PROCESS,
						       1, 3000);
	if (!ret) {
		u8 *cmd = command->cpu;

		*proc_id = le32_to_cpu(*(u32 *)(cmd + 0x0c));
		if (*proc_id == U32_MAX)
			ret = -EPROTO;
		else
			prog->proc_id = *proc_id;
	}
	return ret;
}

static int ane_rtclient_procedure_call(struct ane_rtclient *ane,
				       struct drm_file *file,
				       const struct drm_ane_exec *user)
{
	struct drm_ane_exec_io *ios;
	struct ane_legacy_buffer *command;
	struct ane_t6021_fd *fd = file->driver_priv;
	size_t ios_size;
	size_t cmd_size;
	int ret, i;

	if (user->pad || user->count < 1 || user->count > DRM_ANE_MAX_BINDS ||
	    user->priority < 2 || user->priority > 7 || !fd)
		return -EINVAL;

	ios_size = (size_t)user->count * sizeof(*ios);
	ios = kmalloc(ios_size, GFP_KERNEL);
	if (!ios)
		return -ENOMEM;
	if (copy_from_user(ios, u64_to_user_ptr(user->io_ptr), ios_size)) {
		kfree(ios);
		return -EFAULT;
	}
	for (i = 0; i < user->count; i++) {
		if (ios[i].flags || ios[i].reserved) {
			kfree(ios);
			return -EINVAL;
		}
	}

	cmd_size = sizeof(struct ane_csne_cmd_procedure_call) +
		   (size_t)user->count * sizeof(struct ane_csne_io_elem);
	if (cmd_size > SZ_16K) {
		kfree(ios);
		return -E2BIG;
	}

	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined)) {
		mutex_unlock(&ane_t6021_fw_lock);
		kfree(ios);
		return -ETIMEDOUT;
	}
	command = ane->cmd_buf;
	if (!command) {
		mutex_unlock(&ane_t6021_fw_lock);
		kfree(ios);
		return -ENODEV;
	}
	memset(command->cpu, 0, SZ_16K);
	{
		u8 *cmd = command->cpu;

		*(u32 *)(cmd + 0x08) = cpu_to_le32(user->prog_id);
		*(u32 *)(cmd + 0x0c) = cpu_to_le32(user->proc_id);
		*(u64 *)(cmd + 0x10) = cpu_to_le64(0);
		*(u32 *)(cmd + 0x18) = cpu_to_le32(user->priority);
		*(u64 *)(cmd + 0x20) = cpu_to_le64(ANE_CALL_COOKIE);
		*(u32 *)(cmd + 0x28) = cpu_to_le32(user->count);
		for (i = 0; i < user->count; i++) {
			struct ane_t6021_bo *bo = NULL, *b;
			u64 slot_base = 0x60 + (u64)i * 0x30;
			u64 iova;

			mutex_lock(&ane_t6021_bo_lock);
			list_for_each_entry(b, &fd->bos, node) {
				if (b->handle == ios[i].bo_handle) {
					bo = b;
					break;
				}
			}
			if (!bo || ios[i].size > bo->size) {
				mutex_unlock(&ane_t6021_bo_lock);
				ret = -EINVAL;
				goto unlock;
			}
			/* The IOVA below is about to be published to the
			 * firmware, so mark the BO before the exchange.
			 * Under bo_lock the handle reference cannot go
			 * away, so no kref is needed here.
			 */
			bo->fw_ref = true;
			iova = bo->dma;
			mutex_unlock(&ane_t6021_bo_lock);
			*(u32 *)(cmd + slot_base + 0x00) = cpu_to_le32(1);
			*(u32 *)(cmd + slot_base + 0x04) =
				cpu_to_le32(ios[i].buffer_id);
			*(u32 *)(cmd + slot_base + 0x08) =
				cpu_to_le32(ios[i].type);
			*(u64 *)(cmd + slot_base + 0x18) =
				cpu_to_le64(iova);
			*(u64 *)(cmd + slot_base + 0x20) =
				cpu_to_le64(ios[i].size);
		}
		/* Drain the CPU write buffers so the input BOs the user
		 * filled through its uncached mapping are in DRAM before
		 * the fw starts reading them.
		 */
		wmb();
		ret = ane_rtclient_command(ane, command,
					   cmd_size,
							       CSNE_CMD_PROCEDURE_CALL,
							       1,
							       user->timeout_ms ?
							       user->timeout_ms : 5000);
	}
unlock:
	mutex_unlock(&ane_t6021_fw_lock);
	kfree(ios);
	return ret;
}

/* ---- DRM accel glue (minimal BO + ioctl table) ---- */

struct ane_t6021_drm {
	struct drm_device drm;
	struct device *dev;
	struct ane_rtclient *ane;
};

static struct ane_t6021_drm *to_ane_t6021_drm(struct drm_device *drm)
{
	return container_of(drm, struct ane_t6021_drm, drm);
}

static int ane_t6021_bo_init_ioctl(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct drm_ane_bo_init *args = data;
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_t6021_bo *bo;
	struct ane_rtclient *ane;

	if (args->pad || args->size == 0 || args->size > ANE_T6021_BO_MAX ||
	    !fd)
		return -EINVAL;
	/* A parked io BO is already mapped and counted; its old contents
	 * belong to another process, so it is zeroed like a new one.
	 */
	bo = ane_t6021_bo_pool_take(args->size);
	if (bo) {
		memset(bo->cpu, 0, PAGE_ALIGN(args->size));
		goto publish;
	}
	/* Global coherent-memory accounting. Each BO is 16 KiB-aligned;
	 * a BO whose IOVA reaches the firmware is never freed (held or
	 * pooled), so this bound caps the memory that outlives its
	 * users.
	 */
	if (atomic64_add_return(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes) >
	    (s64)bo_total_max_mb << 20) {
		atomic64_sub(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes);
		return -ENOSPC;
	}
	ane = to_ane_t6021_drm(drm)->ane;
	bo = kzalloc_obj(*bo);
	if (!bo) {
		atomic64_sub(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes);
		return -ENOMEM;
	}
	bo->cpu = dma_alloc_coherent(drm->dev, args->size, &bo->dma,
				     GFP_KERNEL);
	if (!bo->cpu) {
		atomic64_sub(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes);
		kfree(bo);
		return -ENOMEM;
	}
	/*
	 * Device-visible DMA surfaces must be 16 KiB aligned and not overlap
	 * the entry alias.
	 */
	if (!IS_ALIGNED(bo->dma, SZ_16K) ||
	    (ane->fw && !ane_t6021_fw_alias_iova_ok(ane->fw, bo->dma,
						    args->size))) {
		dma_free_coherent(drm->dev, args->size, bo->cpu, bo->dma);
		atomic64_sub(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes);
		kfree(bo);
		return -ERANGE;
	}
publish:
	bo->size = args->size;
	bo->owner = fd;
	bo->dev = drm->dev;
	kref_init(&bo->refcount); /* the handle holds this reference */
	mutex_lock(&ane_t6021_bo_lock);
	bo->handle = ane_t6021_next_handle++;
	if (bo->handle == 0)
		bo->handle = ane_t6021_next_handle++;
	list_add_tail(&bo->node, &fd->bos);
	mutex_unlock(&ane_t6021_bo_lock);
	args->handle = bo->handle;
	/* mmap offset = the handle; libane mmaps the fd at exactly this
	 * offset and ane_t6021_mmap resolves the BO from vm_pgoff.
	 */
	args->offset = (u64)bo->handle << PAGE_SHIFT;
	return 0;
}

/* Drop one handle owned by this fd. The kref may keep the memory
 * alive past this call — a user mapping still holds a reference — so
 * the final ane_t6021_bo_release makes the free-or-hold decision.
 */
static void ane_t6021_bo_drop(struct ane_t6021_bo *bo)
{
	list_del(&bo->node);
	kref_put(&bo->refcount, ane_t6021_bo_release);
}

static int ane_t6021_bo_free_ioctl(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct drm_ane_bo_free *args = data;
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_t6021_bo *bo = NULL, *b;

	if (!fd)
		return -ENODEV;
	if (args->pad)
		return -EINVAL;
	mutex_lock(&ane_t6021_bo_lock);
	list_for_each_entry(b, &fd->bos, node) {
		if (b->handle == args->handle) {
			bo = b;
			break;
		}
	}
	if (bo)
		ane_t6021_bo_drop(bo);
	mutex_unlock(&ane_t6021_bo_lock);
	return bo ? 0 : -ENOENT;
}

/* Resolve the BO a user mmap names (BO_INIT returned offset = handle
 * << PAGE_SHIFT) and map the coherent buffer cacheably — the device
 * half coheres through the DART (IOMMU_CACHE), so reads after EXEC
 * observe the firmware's writes without extra sync. The mapping takes
 * one BO reference: it survives BO_FREE until the vma is gone.
 */
static int ane_t6021_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *file = filp->private_data;
	struct ane_t6021_fd *fd = file->driver_priv;
	struct drm_device *drm = file->minor->dev;
	struct ane_t6021_bo *bo = NULL, *b;
	size_t size = vma->vm_end - vma->vm_start;
	u32 handle;
	int ret;

	if (!fd)
		return -ENODEV;
	/* vm_pgoff is the BO_INIT offset in pages: handle << PAGE_SHIFT
	 * >> PAGE_SHIFT == handle.
	 */
	handle = (u32)vma->vm_pgoff;
	if (!handle)
		return -EINVAL;
	mutex_lock(&ane_t6021_bo_lock);
	list_for_each_entry(b, &fd->bos, node) {
		if (b->handle == handle && b->owner == fd) {
			kref_get(&b->refcount);
			bo = b;
			break;
		}
	}
	if (!bo || size > PAGE_ALIGN(bo->size)) {
		mutex_unlock(&ane_t6021_bo_lock);
		if (bo)
			kref_put(&bo->refcount, ane_t6021_bo_release);
		return -ENOENT;
	}
	vma->vm_pgoff = 0;
	ret = dma_mmap_coherent(drm->dev, vma, bo->cpu, bo->dma, size);
	mutex_unlock(&ane_t6021_bo_lock);
	if (ret) {
		kref_put(&bo->refcount, ane_t6021_bo_release);
		return ret;
	}
	/* The vma owns the lookup reference: close (and open on fork)
	 * go through ane_t6021_vm_ops.
	 */
	vma->vm_private_data = bo;
	vma->vm_ops = &ane_t6021_vm_ops;
	return 0;
}

static int ane_t6021_open(struct drm_device *drm, struct drm_file *file)
{
	struct ane_t6021_fd *fd;

	fd = kzalloc_obj(*fd);
	if (!fd)
		return -ENOMEM;
	INIT_LIST_HEAD(&fd->bos);
	file->driver_priv = fd;
	return 0;
}

static void ane_t6021_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_t6021_bo *bo, *tmp;

	if (!fd)
		return;
	mutex_lock(&ane_t6021_bo_lock);
	list_for_each_entry_safe(bo, tmp, &fd->bos, node)
		ane_t6021_bo_drop(bo);
	mutex_unlock(&ane_t6021_bo_lock);
	kfree(fd);
	file->driver_priv = NULL;
}

static int ane_t6021_get_caps_ioctl(struct drm_device *drm, void *data,
				    struct drm_file *file)
{
	struct drm_ane_get_caps *args = data;

	if (args->flags || args->pad)
		return -EINVAL;

	*args = (struct drm_ane_get_caps) {
		.size = sizeof(*args),
		.abi_version = DRM_ANE_ABI_V2,
		.chip_family = DRM_ANE_CHIP_H14,
		.section_size = sizeof(struct drm_ane_section),
		.bind_size = sizeof(struct drm_ane_generic_bind),
		.exec_io_size = sizeof(struct drm_ane_exec_io),
	};
	return 0;
}

static int ane_t6021_submit_ioctl(struct drm_device *drm, void *data,
				  struct drm_file *file)
{
	return -ENOTTY; /* ABI 1 SUBMIT is rejected on T6021/M2 */
}

static int ane_t6021_prog_load_ioctl(struct drm_device *drm, void *data,
				     struct drm_file *file)
{
	struct ane_t6021_drm *adrm = to_ane_t6021_drm(drm);
	struct drm_ane_prog_load *args = data;
	int ret;

	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined)) {
		mutex_unlock(&ane_t6021_fw_lock);
		return -ETIMEDOUT;
	}
	ret = ane_rtclient_load_program(adrm->ane, file, args,
					&args->prog_id_out);
	mutex_unlock(&ane_t6021_fw_lock);
	return ret;
}

/*
 * Performance property write: command 0x1f, channel zero, property
 * 0x10aa, value one. The runtime parameter accepts only value one;
 * other values are rejected.
 */
static struct ane_rtclient *ane_t6021_perf_ane;
static bool fw_perf_mode;

static int ane_t6021_perf_mode_set(const char *val,
				   const struct kernel_param *kp)
{
	struct ane_rtclient *ane = READ_ONCE(ane_t6021_perf_ane);
	struct ane_legacy_buffer *command;
	bool on;
	int ret;

	ret = kstrtobool(val, &on);
	if (ret)
		return ret;
	if (!on || fw_perf_mode)
		return on ? 0 : -EINVAL;
	if (!ane)
		return -ENODEV;
	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined)) {
		mutex_unlock(&ane_t6021_fw_lock);
		return -ETIMEDOUT;
	}
	command = ane->cmd_buf;
	if (!command) {
		mutex_unlock(&ane_t6021_fw_lock);
		return -ENODEV;
	}
	memset(command->cpu, 0, SZ_16K);
	*(u32 *)((u8 *)command->cpu + 0x08) = cpu_to_le32(0);
	*(u32 *)((u8 *)command->cpu + 0x0c) = cpu_to_le32(0x10aa);
	*(u32 *)((u8 *)command->cpu + 0x10) = cpu_to_le32(1);
	ret = ane_rtclient_command(ane, command, 0x14, 0x001f, 1, 3000);
	if (!ret) {
		fw_perf_mode = true;
		dev_info(ane->dev, "fw perf mode set (property 0x10aa = 1)\n");
	}
	mutex_unlock(&ane_t6021_fw_lock);
	return ret;
}

static const struct kernel_param_ops ane_t6021_perf_mode_ops = {
	.set = ane_t6021_perf_mode_set,
	.get = param_get_bool,
};
module_param_cb(fw_perf_mode, &ane_t6021_perf_mode_ops, &fw_perf_mode, 0644);
MODULE_PARM_DESC(fw_perf_mode,
		 "Write performance-mode property value 1 at runtime (other values rejected)");

static int ane_t6021_proc_create_ioctl(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct ane_t6021_drm *adrm = to_ane_t6021_drm(drm);
	struct drm_ane_proc_create *args = data;
	int ret;

	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined)) {
		mutex_unlock(&ane_t6021_fw_lock);
		return -ETIMEDOUT;
	}
	args->proc_id_out = 0;
	ret = ane_rtclient_create_process(adrm->ane, args->prog_id,
					  &args->proc_id_out);
	mutex_unlock(&ane_t6021_fw_lock);
	return ret;
}

static int ane_t6021_exec_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct ane_t6021_drm *adrm = to_ane_t6021_drm(drm);
	struct drm_ane_exec *args = data;

	return ane_rtclient_procedure_call(adrm->ane, file, args);
}

static const struct drm_ioctl_desc ane_t6021_ioctls[] = {
	DRM_IOCTL_DEF_DRV(ANE_GET_CAPS, ane_t6021_get_caps_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_BO_INIT, ane_t6021_bo_init_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_BO_FREE, ane_t6021_bo_free_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_SUBMIT, ane_t6021_submit_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_PROG_LOAD, ane_t6021_prog_load_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_PROC_CREATE, ane_t6021_proc_create_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_EXEC, ane_t6021_exec_ioctl, 0),
};

/* Driver fops: the accel-core entry points plus our BO mmap (the core
 * default maps only GEM objects; this driver keeps its own BO table,
 * so .mmap resolves the handle from BO_INIT's returned offset).
 */
static const struct file_operations ane_t6021_fops = {
	.owner = THIS_MODULE,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	.open = accel_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = ane_t6021_mmap,
};

/* Version reported through DRM_IOCTL_VERSION: ABI 2 (T6021).
 * DRIVER_COMPUTE_ACCEL puts the node at /dev/accel/accelN.
 */
static const struct drm_driver ane_t6021_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_COMPUTE_ACCEL,
	.open = ane_t6021_open,
	.postclose = ane_t6021_postclose,
	.ioctls = ane_t6021_ioctls,
	.num_ioctls = ARRAY_SIZE(ane_t6021_ioctls),
	.fops = &ane_t6021_fops,
	.major = DRM_ANE_ABI_V2,
	.minor = 0,
	.name = "ane",
	.desc = "Apple Neural Engine (T6021/M2)",
};

/*
 * Management callbacks handle boot; MBI serves ioctls after transport
 * validation.
 */

static void ane_rtclient_recv(void *cookie, u8 ep, u64 message)
{
	struct ane_rtclient *ane = cookie;

	dev_dbg(ane->dev,
		"mailbox management app msg: ep=%#x msg=%016llx\n", ep, message);
}

static void ane_rtclient_crashed(void *cookie, const void *crashlog,
				 size_t size)
{
	struct ane_rtclient *ane = cookie;

	dev_err(ane->dev, "mailbox management: coprocessor crashed (crashlog %zu bytes)\n",
		size);
	print_hex_dump(KERN_ERR, "ANE crashlog: ", DUMP_PREFIX_OFFSET, 16, 1,
		       crashlog, min_t(size_t, size, 256), false);
}

static int ane_rtclient_shmem_setup(void *cookie,
				    struct apple_rtkit_shmem *bfr)
{
	struct ane_rtclient *ane = cookie;

	if (bfr->iova) {
		dev_warn(ane->dev,
			 "mailbox management: fw-provided shmem iova=%pad size=%#zx — refused\n",
			 &bfr->iova, bfr->size);
		return -EINVAL;
	}
	bfr->buffer = dma_alloc_coherent(ane->dev, bfr->size, &bfr->iova,
					 GFP_KERNEL);
	if (!bfr->buffer)
		return -ENOMEM;
	if (ane->fw && !ane_t6021_fw_alias_iova_ok(ane->fw, bfr->iova,
						   bfr->size)) {
		dev_err(ane->dev,
			"mailbox management: shmem grant %pad+%#zx overlaps the fw alias — refusing\n",
			&bfr->iova, bfr->size);
		dma_free_coherent(ane->dev, bfr->size, bfr->buffer, bfr->iova);
		bfr->buffer = NULL;
		return -EBUSY;
	}
	return 0;
}

static void ane_rtclient_shmem_destroy(void *cookie,
				       struct apple_rtkit_shmem *bfr)
{
	struct ane_rtclient *ane = cookie;

	if (!bfr->buffer)
		return;
	if (ane->held) {
		dev_warn(ane->dev,
			 "mailbox management: shmem %pad HELD (CPU started) — not freed\n",
			 &bfr->iova);
		return;
	}
	dma_free_coherent(ane->dev, bfr->size, bfr->buffer, bfr->iova);
}

static const struct apple_rtkit_ops ane_rtclient_rtkit_ops = {
	.crashed = ane_rtclient_crashed,
	.recv_message = ane_rtclient_recv,
	.shmem_setup = ane_rtclient_shmem_setup,
	.shmem_destroy = ane_rtclient_shmem_destroy,
};

/*
 * RX fallback polls every 10 ms until the handshake completes, then
 * every second while enabled. Arm it before host acknowledgment so
 * an immediately following HELLO is serviced.
 */
static void ane_rtclient_post_boot(struct work_struct *w)
{
	struct ane_rtclient *ane =
		container_of(to_delayed_work(w), struct ane_rtclient,
			     poll_work);

	apple_rtkit_poll(ane->rtk);

	if (!ane->boot_done) {
		schedule_delayed_work(&ane->poll_work, msecs_to_jiffies(10));
		return;
	}

	if (poll_rx)
		schedule_delayed_work(&ane->poll_work, HZ);
}

/* Start each announced application endpoint (>= 0x20); flag bit 1 enables it. */
static int ane_rtclient_start_app_eps(struct ane_rtclient *ane)
{
	int ep;

	for (ep = 0x20; ep < 0x100; ep++) {
		int ret;

		if (!apple_rtkit_has_endpoint(ane->rtk, ep))
			continue;
		ret = apple_rtkit_start_ep(ane->rtk, ep);
		dev_dbg(ane->dev, "mailbox management: STARTEP app ep %#x -> %pe\n",
			ep, ERR_PTR(ret));
		if (ret)
			return ret;
	}
	return 0;
}

/* Multi-domain genpd attach, ownership-correct and idempotent. */
struct ane_rtclient_pd {
	struct list_head list;
	struct device *dev;
	struct device **pd_dev;
	struct device_link **pd_link;
	int count;
};

static LIST_HEAD(ane_rtclient_pd_list);
static DEFINE_MUTEX(ane_rtclient_pd_lock);
static bool ane_rtclient_pinned;
static bool ane_rtclient_probe_failed_dirty;
static DEFINE_MUTEX(ane_rtclient_probe_guard_lock);

static void ane_rtclient_pd_free(struct ane_rtclient_pd *pd)
{
	list_del(&pd->list);
	kfree(pd->pd_dev);
	kfree(pd->pd_link);
	kfree(pd);
}

static int ane_rtclient_attach_genpd(struct ane_rtclient *ane)
{
	struct device *dev = ane->dev;
	struct ane_rtclient_pd *pd = NULL;
	int count, i, err = 0;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count == -ENOENT)
		return 0;
	if (count < 0)
		return count;
	if (count <= 1)
		return 0;

	mutex_lock(&ane_rtclient_pd_lock);
	list_for_each_entry(pd, &ane_rtclient_pd_list, list)
		if (pd->dev == dev)
			goto found;

	pd = kzalloc_obj(*pd);
	if (!pd) {
		err = -ENOMEM;
		goto out;
	}
	INIT_LIST_HEAD(&pd->list);
	pd->dev = dev;
	pd->pd_dev = kcalloc(count, sizeof(*pd->pd_dev), GFP_KERNEL);
	pd->pd_link = kcalloc(count, sizeof(*pd->pd_link), GFP_KERNEL);
	if (!pd->pd_dev || !pd->pd_link) {
		ane_rtclient_pd_free(pd);
		pd = NULL;
		err = -ENOMEM;
		goto out;
	}
	list_add_tail(&pd->list, &ane_rtclient_pd_list);

found:
	for (i = pd->count; i < count; i++) {
		if (!pd->pd_dev[i]) {
			pd->pd_dev[i] = dev_pm_domain_attach_by_id(dev, i);
			if (IS_ERR_OR_NULL(pd->pd_dev[i])) {
				err = IS_ERR(pd->pd_dev[i]) ?
				      PTR_ERR(pd->pd_dev[i]) : -ENODEV;
				pd->pd_dev[i] = NULL;
				goto out;
			}
		}
		if (!pd->pd_link[i]) {
			if (!ane_rtclient_pinned) {
				if (!try_module_get(THIS_MODULE)) {
					err = -ENODEV;
					goto out;
				}
				ane_rtclient_pinned = true;
			}
			pd->pd_link[i] =
				device_link_add(dev, pd->pd_dev[i],
						DL_FLAG_STATELESS |
						DL_FLAG_PM_RUNTIME |
						DL_FLAG_RPM_ACTIVE);
			if (!pd->pd_link[i]) {
				err = -EINVAL;
				goto out;
			}
		}
		pd->count = i + 1;
	}

	dev_dbg(dev, "BOOT-PHASE genpd domains attached: %d\n", count);
out:
	mutex_unlock(&ane_rtclient_pd_lock);
	return err;
}

static void ane_rtclient_detach_genpd(struct device *dev)
{
	struct ane_rtclient_pd *pd, *tmp;
	int count, i;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count <= 1)
		return;

	mutex_lock(&ane_rtclient_pd_lock);
	list_for_each_entry_safe(pd, tmp, &ane_rtclient_pd_list, list) {
		if (pd->dev != dev)
			continue;
		for (i = count - 1; i >= 0; i--) {
			if (pd->pd_link[i])
				device_link_del(pd->pd_link[i]);
			if (pd->pd_dev[i])
				dev_pm_domain_detach(pd->pd_dev[i], true);
		}
		ane_rtclient_pd_free(pd);
		if (list_empty(&ane_rtclient_pd_list) && ane_rtclient_pinned) {
			module_put(THIS_MODULE);
			ane_rtclient_pinned = false;
		}
		break;
	}
	mutex_unlock(&ane_rtclient_pd_lock);
}

static int ane_rtclient_probe_inner(struct platform_device *pdev,
				    bool *hardware_touched)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct ane_rtclient *ane;
	struct ane_t6021 *a;
	u32 cpu_status, ps_cpu;
	u64 rvbar;
	int ret;

	if (!ane_t6021_fwload_options_ok()) {
		dev_err(dev,
			"invalid firmware RAM-grant options; refusing before power access\n");
		return -EINVAL;
	}

	if (!ane_t6021_fwload_placement_ok(dev)) {
		dev_err(dev,
			"fw_alias_reserved=1 requires DT no-map coverage for both image windows; refusing before power access, ANE off\n");
		return -ENODEV;
	}

	/*
	 * Validate legacy_only before allocation or power, so failure cannot
	 * unwind a running CPU.
	 */
	if (legacy_only && (!fw_start || fw_start_rtb_mode)) {
		dev_err(dev,
			"legacy_only=1 requires fw_start=1 and fw_start_rtb_mode=0\n");
		return -EINVAL;
	}
	if (legacy_query && !legacy_only) {
		dev_err(dev, "legacy_query requires legacy_only=1\n");
		return -EINVAL;
	}
	if (fw_start && ane_t6021_fwload_requested()) {
		ret = ane_t6021_fwload_check(dev);
		if (ret)
			return dev_err_probe(dev, ret, "firmware preflight failed\n");
	}

	ane = devm_kzalloc(dev, sizeof(*ane), GFP_KERNEL);
	if (!ane)
		return -ENOMEM;
	ane->legacy_buffers = devm_kcalloc(dev, ANE_LEGACY_ALLOCS,
					   sizeof(*ane->legacy_buffers),
					   GFP_KERNEL);
	if (!ane->legacy_buffers)
		return -ENOMEM;
	ane->dev = dev;
	ane->soc = of_device_get_match_data(dev);
	platform_set_drvdata(pdev, ane);
	INIT_DELAYED_WORK(&ane->poll_work, ane_rtclient_post_boot);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	if (!(res->flags & IORESOURCE_MEM_NONPOSTED))
		dev_warn(dev, "engine window is not flagged non-posted\n");
	ane->engine = ioremap_np(res->start, resource_size(res));
	if (!ane->engine)
		return -ENOMEM;
	ane->cpu_rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(ane->cpu_rst))
		return dev_err_probe(dev, PTR_ERR(ane->cpu_rst),
				     "ane_cpu reset control\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	*hardware_touched = true;
	ret = ane_rtclient_attach_genpd(ane);
	if (ret)
		return dev_err_probe(dev, ret, "extra genpd attach\n");
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "genpd raise failed\n");
	}

	ane->pmgr = devm_of_iomap(dev, dev->of_node, 1, NULL);
	if (IS_ERR(ane->pmgr)) {
		ret = PTR_ERR(ane->pmgr);
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret,
				     "pmgr window map failed; G1 gate cannot run\n");
	}
	ps_cpu = readl(ane->pmgr + ane->soc->ps_cpu_off);
	dev_dbg(dev, "ane_cpu ACTUAL = 0x%x\n", ps_cpu);
	if (FIELD_GET(ANE_PS_ACTUAL, ps_cpu) != ANE_PS_ON) {
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
		return -EPROBE_DEFER;
	}
	/*
	 * On T8112, require PWGATE bits 29:28 clear before power-state and
	 * engine reads. Only read this gate; refuse access while it is closed.
	 */
	if (ane->soc->pwgate_off) {
		void __iomem *set = devm_of_iomap(dev, dev->of_node, 2, NULL);
		u32 gate = IS_ERR(set) ? U32_MAX :
			   readl(set + ane->soc->pwgate_off);

		dev_dbg(dev, "PWGATE = 0x%x\n", gate);
		if (gate & GENMASK(29, 28)) {
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return dev_err_probe(dev, -ENODEV,
					     "PWGATE closed; refusing before any engine read\n");
		}
	}

	cpu_status = readl(ane->engine + ANE_ASC_CPU_STATUS);
	rvbar = readq(ane->engine + ANE_ASC_RVBAR);
	dev_dbg(dev,
		"BOOT-PHASE engine reads ok: CPU_STATUS = 0x%x, RVBAR = %016llx (bit0=%u)\n",
		cpu_status, rvbar, (u32)(rvbar & 1));

	if (!(cpu_status & ANE_ASC_CPU_STATUS_RUNNING)) {
		if (!fw_start) {
			dev_err(dev,
				"ANE firmware not alive (CPU_STATUS 0x%x) — start it from a quiesce context, or retry with fw_start=1\n",
				cpu_status);
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EPROBE_DEFER;
		}

		/*
		 * Staging requires fw_load and an attached IOMMU before
		 * allocation.
		 */
		if (!ane_t6021_fwload_requested()) {
			dev_err(dev,
				"fw_start: requires fw_load=1 (no staged firmware)\n");
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EINVAL;
		}
		if (!device_iommu_mapped(dev)) {
			dev_err(dev,
				"fw_start: device not IOMMU-mapped — a staged DVA/entry alias would be untranslated; refusing\n");
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EINVAL;
		}

		a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
		if (!a) {
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -ENOMEM;
		}
		a->dev = dev;
		a->base[ANE_T6021_REG_ENGINE] = ane->engine;
		a->irq = -1;
		a->power_gated = true;
		ane->fw = a;

		dev_dbg(dev, "BOOT-PHASE fwload stage+alias begin\n");
		ret = ane_t6021_fwload_probe(a);
		if (ret) {
			dev_err_probe(dev, ret, "fw_start: staging failed\n");
			ane_t6021_fwload_remove(a);
			ane->fw = NULL;
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return ret;
		}
		if (!ane_t6021_rvbar_entry_ok(a->fw_iova)) {
			dev_err(dev, "fw_start: staged iova %pad invalid\n",
				&a->fw_iova);
			ane_t6021_fwload_remove(a);
			ane->fw = NULL;
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EINVAL;
		}

		dev_dbg(dev, "BOOT-PHASE dispatch (table_mode=%d)\n",
			fw_start_table_mode);
		ret = ane_t6021_boot_start(a, 0, fw_start_table_mode,
					   fw_start_rtb_mode);
		if (ret == -ENODATA || ret == -EAGAIN ||
		    ret == -EBUSY || ret == -ECANCELED) {
			ane_t6021_fwload_remove(a);
			ane->fw = NULL;
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return ret;
		}

		ane->held = true;
		cpu_status = readl(ane->engine + ANE_ASC_CPU_STATUS);
		dev_info(dev,
			 "BOOT-PHASE sequence returned %pe (cpu_started=%u fw_alive=%u booted=%u) CPU_STATUS=0x%x\n",
			 ERR_PTR(ret), a->cpu_started, a->fw_alive, a->booted,
			 cpu_status);
		if (!a->fw_alive && !fw_start_rtb_mode) {
			dev_err(dev,
				"fw_start: poll A timeout, no READY — HELD until reboot, mailbox management handshake skipped\n");
			return 0;
		}
	}

	if (legacy_only) {
		int cfg_err = 0;

		ane_rtclient_validate_chman(ane);
		if (ane->fw && ane->fw->booted && scratch3_ack &&
		    ane->chman_ok &&
		    ane_t6021_chman_host_init(ane->fw->boot_ipc,
					      ane->fw->boot_ipc_size,
					     ane->fw->boot_ipc_iova)) {
			int hello_ret = 0;

			dma_wmb();
			dev_dbg(dev,
				"MBI P8 host ack: SCRATCH3 <- %08x\n",
				ANE_T6021_BOOT_ACK);
			/*
			 * For a nonzero HELLO wait, initialize management and
			 * arm RX before host acknowledgment.
			 */
			if (hello_wait_ms && !ane->rtk) {
				ane->rtk = devm_apple_rtkit_init(dev, ane,
								 NULL, 0,
								&ane_rtclient_rtkit_ops);
				if (IS_ERR(ane->rtk)) {
					dev_err(dev,
						"MBI hello: mailbox management init %pe\n",
						ane->rtk);
					ane->rtk = NULL;
				} else {
					schedule_delayed_work(&ane->poll_work,
							      msecs_to_jiffies(10));
				}
			}
			writel(ANE_T6021_BOOT_ACK,
			       ane->engine + ANE_MBI_SCRATCH0 + 4 * 3);
			if (ane->rtk) {
				unsigned long hello_deadline =
					jiffies + msecs_to_jiffies(hello_wait_ms);

				dev_dbg(dev, "MBI hello: boot begin (%u ms)\n",
					hello_wait_ms);
				do {
					hello_ret = apple_rtkit_boot(ane->rtk);
				} while (hello_ret == -ETIME &&
					 time_before(jiffies, hello_deadline));
				dev_info(dev,
					 "MBI hello: boot %pe running=%d crashed=%d\n",
					 ERR_PTR(hello_ret),
					 apple_rtkit_is_running(ane->rtk),
					 apple_rtkit_is_crashed(ane->rtk));
				if (!hello_ret) {
					if (start_app_eps) {
						ret = ane_rtclient_start_app_eps(ane);
						if (ret) {
							cancel_delayed_work_sync(&ane->poll_work);
							goto err_pm_or_hold;
						}
					}
					ane->boot_done = true;
				} else {
					cancel_delayed_work_sync(&ane->poll_work);
				}
			}
		} else {
			dev_warn(dev,
				 "MBI ack withheld (booted=%u scratch3_ack=%u channel_table_valid=%u)\n",
				 ane->fw ? ane->fw->booted : 0,
				 scratch3_ack, ane->chman_ok);
		}
		if (ane->chman_ok) {
			/* One reusable command buffer for the transport
			 * (CONFIG_GET + every ioctl); reuse is legal
			 * because each exchange completes with the slot
			 * host-owned again. The 128-entry table stays
			 * for fw MALLOC replies only.
			 */
			if (ane_rtclient_legacy_alloc(ane, SZ_16K) == 0)
				ane->cmd_buf =
					&ane->legacy_buffers[ane->legacy_allocated - 1];
			else
				cfg_err = -ENOMEM;
		}
		if (!cfg_err && legacy_query && ane->chman_ok) {
			/*
			 * CONFIG_GET keeps MBI available for ioctls; reply
			 * word +8 must be
			 * nonzero. Keep the boot heap and IPC allocations
			 * until reboot.
			 */
			struct ane_legacy_buffer *command = ane->cmd_buf;
			int qret;

			if (command) {
				memset(command->cpu, 0, SZ_16K);
				qret = ane_rtclient_legacy_exchange(ane,
								    command,
								     16, 0x03,
								     1, 3000);
				dev_dbg(dev,
					"MBI CONFIG_GET words %08x %08x result=%d (DMA remains held)\n",
					READ_ONCE(((u32 *)command->cpu)[1]),
					READ_ONCE(((u32 *)command->cpu)[2]),
					qret);
				if (qret) {
					cfg_err = qret;
				} else if (!READ_ONCE(((u32 *)command->cpu)[2])) {
					dev_err(dev,
						"MBI CONFIG_GET reply word +0x08 zero\n");
					cfg_err = -EPROTO;
				}
			} else {
				cfg_err = -ENOMEM;
			}
		}
		ane->boot_done = true;
		if (cfg_err) {
			dev_err(dev,
				"install: CONFIG_GET failed (%d) — refusing to register DRM device (ioctls would run on an unproven ring)\n",
				cfg_err);
			if (!ane->held) {
				pm_runtime_put_sync_suspend(dev);
				pm_runtime_disable(dev);
			}
			return cfg_err;
		}
	} else {
		unsigned long deadline;
		int boot_ret;

		ane->rtk = devm_apple_rtkit_init(dev, ane, NULL, 0,
						 &ane_rtclient_rtkit_ops);
		if (IS_ERR(ane->rtk)) {
			ret = PTR_ERR(ane->rtk);
			ane->rtk = NULL;
			dev_err_probe(dev, ret, "mailbox management initialization failed\n");
			goto err_pm_or_hold;
		}
		/*
		 * Arm RX fallback before acknowledgment so the following HELLO
		 * is serviced.
		 */
		schedule_delayed_work(&ane->poll_work, msecs_to_jiffies(10));
		deadline = jiffies + msecs_to_jiffies(hello_wait_ms);
		do {
			boot_ret = apple_rtkit_boot(ane->rtk);
		} while (boot_ret == -ETIME && time_before(jiffies, deadline));
		if (boot_ret) {
			dev_err(dev, "mailbox management boot handshake failed: %pe\n",
				ERR_PTR(boot_ret));
			cancel_delayed_work_sync(&ane->poll_work);
			ret = boot_ret;
			goto err_pm_or_hold;
		}
		if (start_app_eps) {
			ret = ane_rtclient_start_app_eps(ane);
			if (ret) {
				cancel_delayed_work_sync(&ane->poll_work);
				goto err_pm_or_hold;
			}
		}
		ane->boot_done = true;
	}

	if (!ane->chman_ok) {
		dev_err(dev,
			"install: channel table not validated — refusing to register DRM device (ioctls would stall the ring)\n");
		cancel_delayed_work_sync(&ane->poll_work);
		if (!ane->held) {
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
		}
		return -EPROTO;
	}

	/* Producer-side stats: preallocate the ring at probe; the files
	 * and the hot-path branch key off stats_slots.
	 */
	if (stats) {
		ane->stats_slots = devm_kcalloc(dev,
						1u << ANE_STATS_RING_ORDER_DEFAULT,
						sizeof(*ane->stats_slots),
						GFP_KERNEL);
		if (ane->stats_slots) {
			ane_stats_counters_init(&ane->stats_ctrs,
						&ane->stats_ring,
						ANE_STATS_RING_ORDER_DEFAULT);
			ane->stats_ring.slots = ane->stats_slots;
		} else {
			dev_warn(dev,
				 "ane_stats ring allocation failed; stats disabled\n");
		}
	}

	{
		struct ane_t6021_drm *adrm;
		int drmret;

		/* This kernel dropped drm_dev_init; the resource-managed
		 * alloc registers the same ABI-2 device.
		 */
		adrm = devm_drm_dev_alloc(dev, &ane_t6021_drm_driver,
					  struct ane_t6021_drm, drm);
		if (IS_ERR(adrm)) {
			ret = dev_err_probe(dev, PTR_ERR(adrm),
					    "drm device alloc\n");
			goto err_pm_or_hold;
		}
		adrm->dev = dev;
		adrm->ane = ane;
		if (ane->stats_slots)
			drm_debugfs_add_file(&adrm->drm, "ane_timeline",
					     ane_timeline_show,
					     &ane->stats_ring);
		WRITE_ONCE(ane_t6021_perf_ane, ane);
		drmret = drm_dev_register(&adrm->drm, 0);
		if (drmret) {
			dev_err_probe(dev, drmret, "drm_dev_register\n");
			ret = drmret;
			goto err_pm_or_hold;
		}
		dev_info(dev,
			 "loaded ane_t6021 (DRM major %d minor %d; ABI 2; legacy_only=%u channel_table_valid=%u booted=%u; state %s; BO cap %u MiB)\n",
			 DRM_ANE_ABI_V2, 0,
			 legacy_only, ane->chman_ok,
			 ane->fw ? ane->fw->booted : 0,
			 ane->held ? "HELD" : "ready", bo_total_max_mb);
	}

	return 0;

err_pm_or_hold:
	if (ane->held) {
		dev_warn(dev,
			 "probe failed after CPU start (%pe) — binding fenced; power/rings/IRQ HELD until reboot\n",
			 ERR_PTR(ret));
	} else {
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
	}
	return ret;
}

static int ane_rtclient_probe(struct platform_device *pdev)
{
	bool hardware_touched = false;
	int ret;

	mutex_lock(&ane_rtclient_probe_guard_lock);
	if (ane_rtclient_probe_failed_dirty) {
		mutex_unlock(&ane_rtclient_probe_guard_lock);
		dev_err(&pdev->dev,
			"previous probe failed after touching the hardware: reboot required\n");
		return -EBUSY;
	}

	ret = ane_rtclient_probe_inner(pdev, &hardware_touched);
	if (ret) {
		struct ane_rtclient *ane = platform_get_drvdata(pdev);

		if (hardware_touched) {
			ane_rtclient_probe_failed_dirty = true;
			dev_err(&pdev->dev,
				"probe failed after hardware access; reboot required before retry\n");
		}
		if (!ane || !ane->held)
			ane_rtclient_detach_genpd(&pdev->dev);
	}
	mutex_unlock(&ane_rtclient_probe_guard_lock);
	return ret;
}

static void ane_rtclient_remove(struct platform_device *pdev)
{
	struct ane_rtclient *ane = platform_get_drvdata(pdev);

	if (READ_ONCE(ane_t6021_perf_ane) == ane)
		WRITE_ONCE(ane_t6021_perf_ane, NULL);

	cancel_delayed_work_sync(&ane->poll_work);

	if (ane->held)
		dev_warn(&pdev->dev,
			 "remove HELD: no teardown — reboot reclaims\n");
}

static const struct of_device_id ane_rtclient_of_match[] = {
	{ .compatible = "apple,t6020-ane", .data = &ane_t6020_soc },
	{ .compatible = "apple,t6021-ane", .data = &ane_t6021_soc },
	{ .compatible = "apple,t6022-ane", .data = &ane_t6022_soc },
	{ .compatible = "apple,t8112-ane", .data = &ane_t8112_soc },
	{ }
};
MODULE_DEVICE_TABLE(of, ane_rtclient_of_match);

/*
 * ane_stats: cumulative busy_ns/jobs for this device (mode 0444, no
 * root needed). Formatting lives in ane_stats_emit() (ane_stats.h),
 * shared with ane.ko.
 */
static ssize_t ane_stats_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct ane_rtclient *ane = dev_get_drvdata(dev);

	return ane_stats_emit(buf, &ane->stats_ctrs);
}
static DEVICE_ATTR_RO(ane_stats);

static struct attribute *ane_t6021_stats_attrs[] = {
	&dev_attr_ane_stats.attr,
	NULL,
};

/* ane_stats appears only when stats=1 gave the device a ring. */
static umode_t ane_t6021_stats_is_visible(struct kobject *kobj,
					  struct attribute *attr, int n)
{
	struct ane_rtclient *ane = dev_get_drvdata(kobj_to_dev(kobj));

	if (attr == &dev_attr_ane_stats.attr && !ane->stats_slots)
		return 0;
	return attr->mode;
}

static const struct attribute_group ane_t6021_stats_group = {
	.attrs = ane_t6021_stats_attrs,
	.is_visible = ane_t6021_stats_is_visible,
};

static const struct attribute_group *ane_t6021_stats_groups[] = {
	&ane_t6021_stats_group,
	NULL,
};

static struct platform_driver ane_rtclient_driver = {
	.driver = {
		.name = "ane_t6021",
		.of_match_table = ane_rtclient_of_match,
		.dev_groups = ane_t6021_stats_groups,
		.suppress_bind_attrs = true,
	},
	.probe = ane_rtclient_probe,
	.remove = ane_rtclient_remove,
};

static int __init ane_rtclient_init(void)
{
	int ret;

	WRITE_ONCE(ane_rtclient_probe_failed_dirty, false);
	ret = platform_driver_register(&ane_rtclient_driver);

	if (ret)
		ane_t6021_trace_free();
	return ret;
}
module_init(ane_rtclient_init);

static void __exit ane_rtclient_exit(void)
{
	platform_driver_unregister(&ane_rtclient_driver);
	ane_t6021_trace_free();
}
module_exit(ane_rtclient_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple Neural Engine (T6021/M2) installed module");

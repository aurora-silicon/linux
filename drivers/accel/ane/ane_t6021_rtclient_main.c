// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_t6021_rtclient.c — T6021 (H14 / M2) installed ANE driver.
 *
 * One module, two surfaces:
 *   - boot/transport: fenced Linux-context firmware start (fw_start=1
 *     default), shared boot/fwload helpers, then the legacy 13.5 ChMan
 *     transport for the three load-time commands (LOAD_PROGRAM 0x200,
 *     CREATE_PROCESS 0x202, PROCEDURE_CALL 0x204) driven through the
 *     ioctls in this file;
 *   - DRM accel: BO_INIT/BO_FREE plus PROG_LOAD/PROC_CREATE/EXEC per
 *     include/uapi/drm/ane_accel.h (ABI 2). Sections ride BOs the
 *     user supplies; one global mutex serializes every firmware
 *     command; completion = the legacy exchange's reply PLUS the
 *     firmware's finish event on the IO_T2H ring. EXEC also returns the
 *     fw's target-to-host slots (drained after every exchange).
 *
 * The compiled defaults are the configuration that boots the firmware:
 * a bare `insmod ane_t6021.ko` runs the whole sequence (fw_load=1
 * fw_start=1 legacy_only=1 legacy_query=1 scratch3_ack=1 poll_rx=1 ...).
 * Remaining parameters are overridable from sysfs for bisection only.
 *
 * Division of labor:
 *  - CPU start: from kernel context — the m1n1 RTBuddy path stays
 *    dormant (no MGMT session comes up) and the 13.5 (22G74) selene
 *    image brings up the legacy ChMan transport after DONE without
 *    RTKit. Boot is fenced in ane_t6021_boot.c (entry fold, RVBAR
 *    skip-or-fold, scratch, RUN, poll A/B, publish/wake).
 *  - genpd/pmgr: Runtime PM + the DT power-domains binding owns the
 *    raise; probe verifies ACTUAL on ane_cpu (pmgr window) before
 *    the first engine read (G1 gate). The pre-CPU engine table block
 *    is NOT written by default (preboot_table_mode = 2 = SKIP: the
 *    block has wedged the machine). The macOS-form mpm_off step is
 *    skipped: the boot-raised ane_sys_mpm stays on; the boot writes do
 *    not depend on it.
 *  - mailbox: ane_rtclient_validate_chman sees the 'IPC ' surface the
 *    fw published during boot; the three load-time commands travel
 *    the IO ring (channel 1) over ane_rtclient_legacy_exchange. Each
 *    command advances the cursor on the next 64-byte slot — resending
 *    to slot 0 wedges the ring.
 *  - non-posted MMIO (DT "nonposted-mmio" -> IORESOURCE_MEM_NONPOSTED)
 *    is mandatory throughout the ANE aperture; posted writel hangs the
 *    machine.
 *  - Firmware alias: the staged copy (own memory, default) or, with
 *    fw_alias_reserved=1, the iBoot-reserved SEG0/SEGi phys
 *    is aliased at the latched RVBAR entry; the subsequent kernel writes
 *    into the entry region translate through dart-ane0 instead of
 *    faulting.
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
#include <linux/of_address.h>
#include <linux/of_platform.h>
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

#include "ane_fw_platform.h"
#include "ane_h16_protocol.h"
#include "ane_h16_pending.h"
#include "ane_h16_resources.h"
#include "ane_fw_protocol.h"
#include "ane_stats.h"
#include "ane_t6021.h"
#include "ane_t6021_boot.h"

#include <drm/ane_accel.h>

/* Doorbell (IPI) block, engine + 0x1844000: set +0, pending +0x8000,
 * ack +0xc000 (an interrupt is handled by reading +0x184c000 and writing
 * +0x1850000, with no SoC difference).
 */
#define ANE_IPI_OFF			0x1844000

/* CPU_STATUS bits (m1n1 ASCRegs shape) */
#define ANE_ASC_CPU_STATUS_RUNNING	BIT(0)
#define ANE_ASC_CPU_STATUS_STOPPED	BIT(1)

/* Boot-time module parameters — defaults are the configuration that
 * boots the firmware, with fw_alias_reserved defaulting to own memory.
 * fw_load, fw_extra_ram and fw_alias_reserved live in
 * ane_t6021_fwload.c and boot_prevent_nap in ane_t6021_boot.c (single
 * registration each). Remaining knobs are overridable from sysfs for
 * bisection only.
 */
static bool fw_start = true;
module_param(fw_start, bool, 0444);
MODULE_PARM_DESC(fw_start,
		 "Fenced Linux-context firmware start: stage + alias, then the boot sequence; default on.");

static int fw_start_table_mode = 2;
module_param(fw_start_table_mode, int, 0444);
MODULE_PARM_DESC(fw_start_table_mode,
		 "Pre-CPU engine table block: 0 abort, 1 write, 2 skip (default).");

static bool fw_start_rtb_mode;
module_param(fw_start_rtb_mode, bool, 0444);
MODULE_PARM_DESC(fw_start_rtb_mode,
		 "RTBuddy/RTKit-app-endpoint select; default off (legacy ChMan transport).");

/* Drive RX by apple_rtkit_poll from a workqueue even though a recv
 * IRQ exists (default on: the raw recv line is not relied on, so the
 * worker is what services HELLO/EPMAP).
 */
static bool poll_rx = true;
module_param(poll_rx, bool, 0444);
MODULE_PARM_DESC(poll_rx,
		 "Drive RX by apple_rtkit_poll from a workqueue (default on).");

/* STARTEP every fw-announced app endpoint (>= 0x20) after a successful
 * RTKit handshake (default on).
 */
static bool start_app_eps = true;
module_param(start_app_eps, bool, 0444);
MODULE_PARM_DESC(start_app_eps,
		 "STARTEP every fw-announced app endpoint after the handshake (default on).");

static bool scratch3_ack = true;
module_param(scratch3_ack, bool, 0444);
MODULE_PARM_DESC(scratch3_ack,
		 "After DONE, write SCRATCH3 = 0x08042006 (host ack). Default on; 0 withholds.");

static bool legacy_only = true;
module_param(legacy_only, bool, 0444);
MODULE_PARM_DESC(legacy_only,
		 "Use the pinned 13.5 legacy ChMan transport (default on).");

static bool legacy_query = true;
module_param(legacy_query, bool, 0444);
MODULE_PARM_DESC(legacy_query,
		 "Service bounded startup allocations and CONFIG_GET; default on.");

/* 0 (default) skips RTKit in legacy mode, so the ANE mailbox never
 * starts: the 13.5 firmware sends no HELLO (-ETIME after
 * 1000 ms), and starting the mailbox enables AIC2 884, a level line
 * that then fires ~700,000 times/s (one CPU of hardirq time). A
 * firmware that speaks RTKit needs hello_wait_ms=1000.
 */
static unsigned int hello_wait_ms;
module_param(hello_wait_ms, uint, 0444);
MODULE_PARM_DESC(hello_wait_ms,
		 "RTKit HELLO wait in legacy mode; 0 (default) skips RTKit and leaves the mailbox stopped. A firmware that speaks RTKit needs 1000.");

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

struct ane_t6021_fd;

/* Populated only by an original-container loader and decoded process reply.
 * Those interfaces remain refused until their complete wire contract exists.
 * The firmware mutex protects this list and every pending UUID operation.
 */
struct ane_h16_process {
	struct list_head node;
	struct list_head owner_node;
	struct ane_h16_hwx_snapshot original;
	struct ane_h16_request_key key;
	struct ane_h16_resource resources[ANE_H16_SURFACES];
	struct ane_legacy_buffer mappings[ANE_H16_SURFACES];
	u32 resource_count;
};

struct ane_rtclient {
	struct device *dev;
	void __iomem *engine;
	void __iomem *pmgr;
	const struct ane_t602x_soc *soc;
	struct apple_rtkit *rtk;
	struct reset_control *cpu_rst;
	struct delayed_work poll_work;

	/* fw_start=1 view over the shared boot/fwload helpers. */
	struct ane_t6021 *fw;

	/* A CPU we released is (or may be) running: state HELD — surfaces,
	 * rings, IRQ, power links preserved; no unwind; reboot reclaims
	 * (wedged-pin rule).
	 */
	bool held;

	bool boot_done;

	/* ChMan descriptor table validated against the 'IPC ' surface. */
	bool chman_ok;
	struct ane_fw_channel channels[ANE_FW_CHANNELS];
	u8 channel_snapshot[ANE_FW_TABLE_SIZE];
	u64 channel_table_offset;
	struct ane_h16_pending_table pending;
	struct ane_h16_pending *waiting;
	struct list_head h16_processes;
	struct ane_legacy_buffer *legacy_buffers;
	u32 legacy_allocated;
	size_t legacy_bytes;
	u32 legacy_malloc_cursor;
	u32 legacy_cmd_cursor[ANE_T6021_CHMAN_COUNT];
	/* One reusable 16 KiB command buffer for every host command
	 * (CONFIG_GET + the three ioctls). Protocol-legal to reuse: an
	 * exchange returns 0 only after the IO slot flipped back to
	 * host-owned with a zero status (the completion
	 * predicate), so the firmware has fully consumed the previous
	 * command. The legacy_buffers table stays for fw MALLOC replies,
	 * which the firmware may reference forever (held until reboot).
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
	struct list_head h16_processes;
};

/* Per-file handle counter and every per-fd list against same-fd concurrent
 * ioctls. A BO whose IOVA reached the firmware (fw_ref) never goes back to
 * the kernel: a program section is held until reboot, an io BO is parked
 * for reuse (see ane_t6021_bo_release). Every other BO frees at its last
 * reference.
 *
 * Per-BO cap: matvec weights emitted by the H14 compiler are bounded by
 * `reduction * columns * 2`, which reaches 20 MiB at (K,N)=(2048,5120).
 * The same cap serves an attention constant at (K,N)=(4096,4096), 32 MiB,
 * and any H14 softmax/reduction with an 8 MiB table. Total BO bytes
 * across all fds are capped separately (bo_total_max_mb) by an atomic
 * counter, enforced at alloc and released when the memory is really
 * freed. Program sections stay held until reboot, and one large model's
 * programs alone can hold gigabytes.
 * A cap of 0 refuses every BO_INIT with -ENOSPC; there is no unlimited
 * value. Every BO also needs IOVA below 4 GiB (32-bit DMA mask), so
 * allocations fail with -ENOMEM near that bound whatever the cap.
 * The 16 KiB alignment check is unchanged: every DMA site assumes it.
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

/* Final put: the last handle or mapping is gone. The firmware must never
 * see a freed IOVA, so a fw_ref BO is never freed: a program
 * section stays held, because a cached firmware program keeps reading
 * it; an io BO goes to the pool, unless a quarantined firmware may still
 * write it. Every other BO frees here, so the BO cap bounds only the
 * memory the firmware may touch.
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
				       unsigned long deadline);

static bool ane_rtclient_drain_t2h(struct ane_rtclient *ane,
				   unsigned int channel);

/* ---- boot helpers (shared with boot.c / fwload.c) ---- */

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

/* Validate the chman table the fw published in the 'IPC ' surface. */
static void ane_rtclient_validate_chman(struct ane_rtclient *ane)
{
	struct ane_t6021 *a = ane->fw;
	const struct ane_t6021_chman_desc *t;
	unsigned int i, bad;

	if (!a || !a->boot_ipc) {
		dev_info(ane->dev,
			 "chman: no host IPC surface — table not validated\n");
		return;
	}
	if (ane->soc->platform) {
		int ret;

		if (!ane_fw_contains(a->boot_ipc_iova, a->boot_ipc_size,
				     a->boot_scratch_result, ANE_FW_TABLE_SIZE)) {
			dev_err(ane->dev, "27.x channel table outside IPC\n");
			return;
		}
		ane->channel_table_offset = a->boot_scratch_result - a->boot_ipc_iova;
		dma_rmb();
		memcpy(ane->channel_snapshot,
		       a->boot_ipc + ane->channel_table_offset, ANE_FW_TABLE_SIZE);
		ret = ane_fw_channels_parse(ane->channel_snapshot, a->boot_ipc_iova,
					    a->boot_ipc_size, a->boot_scratch_result,
					    ane->channels);
		dma_rmb();
		if (ret || memcmp(ane->channel_snapshot,
				  a->boot_ipc + ane->channel_table_offset,
				  ANE_FW_TABLE_SIZE)) {
			dev_err(ane->dev, "27.x channel table rejected or changed\n");
			return;
		}
		ane->chman_ok = true;
		return;
	}
	if (a->boot_ipc_size < ANE_T6021_CHMAN_TOTAL) {
		dev_warn(ane->dev,
			 "chman: IPC surface %#llx bytes < fw layout %#x — table not validated\n",
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
			"chman[%u]: name=\"%.*s\" type=%u bit=%u size=%#llx %s (static: %s/%u/%u/%#llx/ipc+%#x)\n",
			i, ANE_T6021_CHMAN_NAME_LEN, d->name, d->type, d->bit,
			d->size,
			(bad & BIT(i)) ? "MISMATCH" : "OK",
			s->name, s->type, s->bit, s->size, s->off);
	}

	ane->chman_ok = !bad;
	if (!bad)
		for (i = 0; i < ANE_FW_CHANNELS; i++) {
			const struct ane_t6021_chman_static *c = &ane_t6021_chman_layout[i];

			ane->channels[i] = (struct ane_fw_channel) {
				.offset = c->off, .slots = c->size,
				.type = c->type, .bit = c->bit,
			};
		}
	dev_info(ane->dev, "chman: table %s (mismatch mask %#x)\n",
		 bad ? "NOT VALIDATED" : "VALIDATED", bad);
}

static bool ane_rtclient_channels_unchanged(struct ane_rtclient *ane)
{
	struct ane_t6021 *a = ane->fw;

	if (!a || !ane->chman_ok)
		return false;
	dma_rmb();
	if (ane->soc->platform)
		return !memcmp(ane->channel_snapshot,
			       a->boot_ipc + ane->channel_table_offset,
			       ANE_FW_TABLE_SIZE);
	return !ane_t6021_chman_check(a->boot_ipc, a->boot_ipc_iova);
}

static void __iomem *ane_rtclient_doorbell(struct ane_rtclient *ane)
{
	return ane->engine + (ane->soc->platform ?
		ane->soc->platform->doorbell : ANE_IPI_OFF);
}

/* Legacy ChMan exchange (post-DONE 13.5 transport). Resends to the same
 * 64-byte IO slot would wedge the ring; cursor advances after each
 * completed command (the fw treats bit0 as host-owned).
 */
static int ane_rtclient_legacy_exchange(struct ane_rtclient *ane,
					struct ane_legacy_buffer *command,
				       size_t length, u16 opcode,
				       unsigned int channel,
				       unsigned long deadline)
{
	struct ane_t6021 *a = ane->fw;
	u64 *io, *malloc_ring, header;
	void __iomem *ipi = ane_rtclient_doorbell(ane);
	unsigned int cursor = ane->legacy_malloc_cursor;
	int result = -ETIMEDOUT;

	if (!ane->held || !ane_rtclient_channels_unchanged(ane))
		return -EPROTO;
	if (length < 8 || length > command->size ||
	    channel >= ANE_T6021_CHMAN_COUNT) {
		result = -EINVAL;
		goto out;
	}
	io = a->boot_ipc + ane->channels[channel].offset +
	     (size_t)ane->legacy_cmd_cursor[channel] * 64;
	malloc_ring = a->boot_ipc + ane->channels[5].offset;

	if (!(READ_ONCE(io[0]) & 1)) {
		result = -EBUSY;
		goto out;
	}
	if (time_after_eq(jiffies, deadline))
		goto out;
	if (ane->soc->platform) {
		result = ane_h16_command_prepare(command->cpu, length, opcode,
						 ane->channels[channel].index);
		if (result)
			goto out;
	} else {
		ane_fw_command_header(command->cpu, opcode, false);
	}
	if (ane->soc->platform)
		memset((u8 *)command->cpu + length, 0xa5, command->size - length);
	WRITE_ONCE(io[1], length);
	WRITE_ONCE(io[2], length);
	dma_wmb();
	WRITE_ONCE(io[0], command->dma);
	dma_wmb();
	writel(BIT(ane->channels[channel].bit), ipi);
	while (time_before(jiffies, deadline)) {
		u32 pending;

		if (atomic_read(&ane_t6021_quarantined)) {
			result = -EIO;
			goto out;
		}
		if (!ane_rtclient_channels_unchanged(ane)) {
			result = -EPROTO;
			goto out;
		}
		if (ane->soc->platform && opcode == ANE_H16_EXECUTE) {
			ane_rtclient_drain_t2h(ane, 6);
			if (ane->pending.quarantined) {
				result = ane->pending.error ?: -EIO;
				goto out;
			}
		}
		pending = readl(ipi + 0x8000);

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

				/* H16 needs its own validated allocation/reply provider.
				 * Never answer it with the legacy H14 allocation index.
				 */
				if (ane->soc->platform) {
					result = -EOPNOTSUPP;
					goto out;
				}
				dma_rmb();
				size = READ_ONCE(slot[1]);
				tag = READ_ONCE(slot[2]);
				/* A valid MALLOC slot carries
				 * header 0 and a u32 tag; anything else
				 * is a malformed ring and we stop.
				 */
				if (alloc_hdr || tag > U32_MAX) {
					result = -EPROTO;
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
				writel(BIT(ane->channels[5].bit), ipi);
				cursor = (cursor + 1) %
					ane->channels[5].slots;
			}
		}
		/* Allocation service cannot extend the command deadline. */
		if (time_after_eq(jiffies, deadline))
			goto out;
		header = READ_ONCE(io[0]);
		if (header & 1) {
			dma_rmb();
			result = ane_fw_reply_status(header, READ_ONCE(io[1]),
						     READ_ONCE(io[2]), command->dma,
						     length, opcode, command->cpu,
						     !!ane->soc->platform);
			/* Corruption does not retire the unknown slot. A refusal
			 * retires ownership but remains a nonzero result.
			 */
			if (!ane_rtclient_channels_unchanged(ane) ||
			    (ane->soc->platform &&
			     memchr_inv((u8 *)command->cpu + length, 0xa5,
					command->size - length)))
				result = -EPROTO;
			if (result == -EREMOTEIO)
				dev_info(ane->dev, "firmware refused op=%#x ack=%#x; no retry\n",
					 opcode, get_unaligned_le16((u8 *)command->cpu + 6));
			if (!result || result == -EREMOTEIO)
				ane->legacy_cmd_cursor[channel] =
					(ane->legacy_cmd_cursor[channel] + 1) %
					ane->channels[channel].slots;
			goto out;
		}
		/* Fast poll: check the rings every 50 us instead of
		 * sleeping 1-2 ms.
		 */
		udelay(50);
	}
	dev_info(ane->dev, "LEGACY timeout ch=%u io=%016llx\n",
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
 * The finish-event wait polls the coherent IO_T2H ring, so the poll cadence is
 * the whole detection lag: a uniform 50..100 us sleep adds 0..100 us of pure
 * wait to every CALL. call_poll_us is how long the head of the wait sleeps
 * only 1..2 us between looks (the real period is the hrtimer wake latency)
 * before it falls back to the 50..100 us sleep, which keeps long programs (the
 * Parakeet encoder, about 254 ms) from spending the wait on hrtimer wakeups.
 * 0 restores the uniform 50..100 us cadence. A very large value makes the
 * whole wait 1..2 us sleeps. Writable at run time (0644), so both cadences can
 * be compared in one boot.
 */
static unsigned int call_poll_us = 1000;
module_param(call_poll_us, uint, 0644);
MODULE_PARM_DESC(call_poll_us,
		 "Microseconds at the head of the CALL finish-event wait that sleep only 1..2 us between looks, then 50..100 us (default 1000; 0 = uniform 50..100 us; a very large value makes the whole wait 1..2 us sleeps)");

/* trace_td: a read-only timeline of each CALL for performance work. Off
 * by default; switch it at runtime with
 * /sys/module/ane_t6021/parameters/trace_td (no device needed). Off, the
 * CALL path is unchanged. On, the completion wait polls every 20-40 us
 * and also reads the last-committed-TD word. It reads that word only
 * while the seven ANE pmgr PS words read 0x3ff. No register is written.
 *
 * - The TD word: engine + ane_t602x_soc.trace_td_off; on T602x TM
 *   0x285c00000 + TD window 0x20400 + 0x58. Its layout: the call's nid
 *   in bits 23:16 (+1 per call), the index of the last task taken in
 *   bits 15:0. Not known on T8112, so trace_td records nothing there.
 * - The seven ANE power-state words at ane_t602x_soc.pmu_pa + ps_off
 *   (T602x: DT power-domains ane_sys_mpm 0x4000 .. ane_set4 0x4030 of
 *   the pmgr at 0x28e080000). The guard: PS words 0x3ff before any TM
 *   read, since a TM read while the compute domains are off hangs the
 *   SoC.
 *
 * Each record has a ktime_get_ns() stamp: CALL before the exchange, ACK
 * when the firmware acked it, TD for each new TD word value (word = the
 * value), EVENT for each IO_T2H event of the CALL (word = its state),
 * GATE when a PS word reads other than 0x3ff (word = its offset), DONE
 * when the wait ends (word = the TD samples taken). The buffer is
 * allocated at the first switch-on and kept until unload; each switch-on
 * empties it, and records past its end are counted in `dropped`. Read
 * it, while no CALL runs, from debugfs ane_t6021/trace_td (0400). All
 * trace state is protected by ane_t6021_fw_lock.
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

/* IO_T2H event of a PROCEDURE_CALL, 0x28 bytes: u32 sequence, u32 0x300,
 * u64 cookie, u32 program id, u32 process id, u32 0, u32 state. The
 * firmware posts two per call: state 0 about 0.2-0.5 ms after the ack
 * whatever the program length (a long program can take its last task
 * hundreds of ms later), and state 1 when the procedure has finished.
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
	const void *found = NULL;
	u32 i;

	for (i = 0; i < ane->legacy_allocated; i++) {
		const struct ane_legacy_buffer *b = &ane->legacy_buffers[i];

		if (ane_fw_contains(b->dma, b->size, iova, len)) {
			if (!ane->soc->platform)
				return b->cpu + (iova - b->dma);
			if (found)
				return NULL;
			found = b->cpu + (iova - b->dma);
		}
	}
	if (ane_fw_contains(ane->fw->boot_ipc_iova, ane->fw->boot_ipc_size,
			    iova, len))
		return found ? NULL : ane->fw->boot_ipc + (iova - ane->fw->boot_ipc_iova);
	return found;
}

/* Return every firmware-owned slot on a target-to-host ring to the fw:
 * the fw publishes by writing the DMA address with bit0 clear; the host
 * returns the slot by setting bit0 and ringing the channel's doorbell
 * bit, as the allocation ring does. The ioctls drain channels 4/6 after
 * every completed exchange. Returns true when
 * an IO_T2H slot held the finish event of a CALL.
 */
static bool ane_rtclient_drain_t2h(struct ane_rtclient *ane,
				   unsigned int channel)
{
	const struct ane_fw_channel *c = &ane->channels[channel];
	unsigned int n = 0, slot_i = ane->legacy_cmd_cursor[channel];
	void __iomem *ipi = ane_rtclient_doorbell(ane);
	bool finished = false;

	if (!ane_rtclient_channels_unchanged(ane)) {
		atomic_set(&ane_t6021_quarantined, 1);
		if (ane->soc->platform) {
			ane->pending.error = -EPROTO;
			ane->pending.quarantined = true;
		}
		return false;
	}
	if (c->type != 1)
		return false;
	while (n < c->slots) {
		u64 *slot = ane->fw->boot_ipc + c->offset + (size_t)slot_i * 64;
		u64 hdr = READ_ONCE(slot[0]), len;

		if (hdr & 1)
			break;
		dma_rmb();
		len = READ_ONCE(slot[1]);
		dev_dbg(ane->dev, "T2H ch=%u slot=%u len=%#llx\n",
			channel, slot_i, len);
		if (ane->soc->platform) {
			u8 snapshot[0x60];
			const void *ev;
			int status;

			/* Other channels have different message types; never apply
			 * the event predicate to logs or allocation traffic.
			 */
			if (c->index != 6 || (len != sizeof(snapshot) && len != 0xc)) {
				atomic_set(&ane_t6021_quarantined, 1);
				ane->pending.error = -EPROTO;
				ane->pending.quarantined = true;
				return false;
			}
			ev = ane_rtclient_fw_cpu(ane, hdr, len);
			if (!ev) {
				atomic_set(&ane_t6021_quarantined, 1);
				ane->pending.error = -EPROTO;
				ane->pending.quarantined = true;
				return false;
			}
			memcpy(snapshot, ev, len);
			status = ane_h16_pending_dispatch(&ane->pending, snapshot,
							  len, c->index, jiffies);
			if (ane->pending.quarantined) {
				atomic_set(&ane_t6021_quarantined, 1);
				return false;
			}
			if (status == ANE_H16_EVENT_COMPLETE)
				finished = true;
		} else if (channel == 6 && len >= ANE_T2H_CALL_STATE_OFF + 4) {
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
		slot_i = (slot_i + 1) % c->slots;
		ane->legacy_cmd_cursor[channel] = slot_i;
	}
	return finished;
}

/* The completion wait with trace_td on: the same finish-event test with a
 * 20-40 us poll, and one TD-word sample per poll under the PS-word guard
 * (see trace_td).
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
 * dispatch rather than execution completion. The finish event marks
 * completion and output visibility, so no extra settle delay is needed.
 * Return zero on the event, or -ETIMEDOUT.
 */
static int ane_rtclient_call_wait(struct ane_rtclient *ane,
				  unsigned long deadline)
{
	unsigned int poll_us = ane->soc->platform ? 0 : READ_ONCE(call_poll_us);
	ktime_t tight_end = ktime_add_ns(ktime_get(),
					 (u64)poll_us * NSEC_PER_USEC);

	if (ane_t6021_tracing)
		return ane_rtclient_call_wait_traced(ane, deadline);
	if (ane->soc->platform && time_after_eq(jiffies, deadline))
		return -ETIMEDOUT;

	do {
		if (atomic_read(&ane_t6021_quarantined))
			return -EIO;
		if (ane->soc->platform) {
			ane_rtclient_drain_t2h(ane, 6);
			if (atomic_read(&ane_t6021_quarantined))
				return ane->pending.error ?: -EIO;
			if (ane->waiting && ane->waiting->terminal)
				return ane->waiting->status;
		} else if (ane_rtclient_drain_t2h(ane, 6)) {
			return 0;
		}
		if (poll_us && ktime_before(ktime_get(), tight_end))
			usleep_range(1, 2);
		else
			usleep_range(50, 100);
	} while (time_before(jiffies, deadline));
	if (atomic_read(&ane_t6021_quarantined))
		return -EIO;
	if (ane->soc->platform)
		return -ETIMEDOUT;
	return ane_rtclient_drain_t2h(ane, 6) ? 0 : -ETIMEDOUT;
}

static int ane_rtclient_command(struct ane_rtclient *ane,
				struct ane_legacy_buffer *command,
						     size_t length, u16 opcode,
						     unsigned int channel,
						     unsigned int timeout_ms)
{
	unsigned long deadline = ane->soc->platform && ane->waiting ?
		ane->waiting->deadline : jiffies + msecs_to_jiffies(timeout_ms);
	int ret;
	u64 stats_ticket = 0;
	u64 stats_submit_ns = 0;
	bool execute = ane->soc->platform ? opcode == ANE_H16_EXECUTE :
					   opcode == CSNE_CMD_PROCEDURE_CALL;

	ane_t6021_tracing = opcode == CSNE_CMD_PROCEDURE_CALL && trace_td &&
			    ane->soc->trace_td_off;
	if (ane_t6021_tracing) {
		ane_t6021_trace->calls++;
		ane_t6021_trace_add(ANE_TR_CALL, 0);
	}
	/* Producer-side stats hot path (ane_stats.h): record submit at
	 * command enqueue, completion after the call returns, one
	 * begin/complete pair per engine submission. Only
	 * CSNE_CMD_PROCEDURE_CALL is engine work; the control-plane
	 * exchanges that ride this function (LOAD_PROGRAM,
	 * CREATE_PROCESS, CH_PROPERTY_WRITE, CONFIG_GET) are not
	 * counted, so jobs matches the engine calls the workload made.
	 * The T6021 path can run concurrently (MBI per-channel rings),
	 * so overlapping calls share a busy period and busy_ns is the
	 * union of the submit-to-completion windows. tmst is 0 (no
	 * host TM on T6021; documented in the file header line).
	 * tasks = 1 (one call per submission); rc = ret. The gate is the
	 * ring, as on ane.ko: stats=0 or a failed probe allocation leaves
	 * stats_slots NULL, and the hooks must not run on a NULL ring.
	 */
	bool stats_call = ane->stats_slots && execute;

	if (ane->soc->platform && execute &&
	    (!ane->waiting || !ane->waiting->active || ane->pending.quarantined))
		return -EIO;

	if (stats_call) {
		stats_submit_ns = ktime_get_ns();
		stats_ticket = ane_stats_begin(&ane->stats_ctrs,
					       &ane->stats_ring,
					       stats_submit_ns, 1);
	}
	ret = ane_rtclient_legacy_exchange(ane, command, length, opcode,
					   channel, ane->soc->platform ? deadline :
					   jiffies + msecs_to_jiffies(timeout_ms));
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
	if (execute) {
		if (ane_t6021_tracing)
			ane_t6021_trace_add(ANE_TR_ACK, 0);
		ret = ane_rtclient_call_wait(ane, ane->soc->platform ? deadline :
					    jiffies + msecs_to_jiffies(timeout_ms));
		ane_t6021_tracing = false;
		if (!ret)
			dma_rmb();
		if (!ret && !ane->soc->platform && call_settle_us)
			usleep_range(call_settle_us, call_settle_us + 100);
		if (ret) {
			if (stats_call)
				ane_stats_complete(&ane->stats_ctrs,
						   &ane->stats_ring,
						   stats_ticket,
						   ktime_get_ns(),
						   (u32)ret, 0);
			dev_err(ane->dev,
				"call completion failed %d within %u ms\n",
				ret, timeout_ms);
			atomic_set(&ane_t6021_quarantined, 1);
			return ret;
		}
	}
	if (stats_call)
		ane_stats_complete(&ane->stats_ctrs, &ane->stats_ring,
				   stats_ticket, ktime_get_ns(), 0, 0);
	/* The fw talks back on the target-to-host rings (fwlog, perf);
	 * hand those slots back so the rings never fill (channels 4
	 * and 6).
	 */
	if (!ane->soc->platform)
		ane_rtclient_drain_t2h(ane, 4);
	ane_rtclient_drain_t2h(ane, 6);
	if (atomic_read(&ane_t6021_quarantined))
		return -EIO;
	/* BOs are dma_alloc_coherent memory mapped write-combined for the
	 * CPU, so no cache maintenance is needed on either side.
	 */
	return 0;
}

/* ---- LOAD / CREATE / CALL wrappers ---- */

/* The firmware never frees a program or a process, and its program table
 * holds 256 entries: the 257th LOAD_PROGRAM after a boot is answered
 * with a protocol error, which quarantines the device. Two
 * loads of byte-identical sections therefore share one firmware program
 * and one process. The key is SHA-256 over every section's id, size and
 * bytes, so a client can only reach a program whose bytes it also
 * supplied. Protected by ane_t6021_fw_lock.
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
	/* H16 requires the original HWX, not caller-asserted section identity. */
	if (ane->soc->platform)
		return -EOPNOTSUPP;
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

	/* The record slot IS the section identity (slot =
	 * id - 1; tdprop id 7 lands at slot 6 and slot 5 stays empty
	 * when id 6 is not supplied). Reject out-of-range and
	 * duplicate ids before building the wire image.
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

	/* Identical sections share one firmware program (see the table).
	 * A constant section can reach 20 MiB (the H14 (K,N)=(2048,5120)
	 * matvec), so feed the hash in ANE_T6021_BO_HASH_CHUNK-sized slices
	 * under bo_lock: a single sha256_update of a 20 MiB buffer would
	 * rely on a 20 MiB stack argument list and has no upper bound on
	 * the chunk that the BO could supply.
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
	/* One 0x30-byte section record per supplied section, placed at
	 * its IDENTITY slot: slot = id - 1 (tdprop id 7 lands at slot
	 * 6, slot 5 stays empty — e.g. an image with
	 * six populated records, ids 1,2,3,4,5 then 7). Unsupplied
	 * slots stay zero-filled (flags = 0). flags bit0 = 1, id at
	 * +0x04, iova at +0x18, size at +0x20.
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
	/* Generic binds are accepted for ABI compatibility and unused:
	 * the LOAD record already carries iova + size, and the generic
	 * section image is prepared by userspace (it ships inside the
	 * section bytes, generic.bin).
	 */

	/* ProgramId placeholder; the firmware writes the assigned id
	 * back at +0x1b8.
	 */
	{
		u8 *cmd = command->cpu;

		*(u32 *)(cmd + 0x1b8) = cpu_to_le32(U32_MAX);
	}

	/* The LOAD command is 0x1C0 bytes: the nine
	 * 0x30 records end at 0x1b8, ProgramId at +0x1b8, 8 bytes of
	 * zero tail. sizeof(struct ane_csne_cmd_load_program) is only
	 * 0x1b8, so the length is pinned here.
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

	/* H16 uses 0x258 bytes; its body is not the H14 0x10-byte body. */
	if (ane->soc->platform)
		return -EOPNOTSUPP;
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

/* H16 consumes only backend-owned process resources. The ABI-2 section
 * loader and short process packet cannot create an entry in h16_processes.
 */
static int ane_rtclient_h16_call(struct ane_rtclient *ane, struct drm_file *file,
				 const struct drm_ane_exec *user)
{
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_h16_process *process = NULL, *p;
	struct ane_h16_execute_buffer buffers[ANE_H16_SURFACES];
	struct ane_t6021_bo *bos[ANE_H16_SURFACES] = {};
	struct drm_ane_exec_io *ios;
	struct ane_h16_pending request = {};
	struct ane_h16_execute call = {};
	unsigned int i, j, pinned = 0;
	int ret = -EOPNOTSUPP;

	if (!fd || user->pad || user->priority != 5 || user->count < 2 ||
	    user->count > ANE_H16_SURFACES || user->timeout_ms > 30000)
		return -EINVAL;
	ios = memdup_user(u64_to_user_ptr(user->io_ptr),
			  array_size(user->count, sizeof(*ios)));
	if (IS_ERR(ios))
		return PTR_ERR(ios);
	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined) || ane->pending.quarantined) {
		ret = -EIO;
		goto out;
	}
	list_for_each_entry(p, &fd->h16_processes, owner_node) {
		if (p->key.program == user->prog_id &&
		    p->key.process == user->proc_id) {
			process = p;
			break;
		}
	}
	if (!process)
		goto out;
	ret = ane_h16_hwx_snapshot_check(&process->original);
	if (!ret)
		ret = ane_h16_resources_validate(&process->original.info,
						 process->resources, process->resource_count);
	if (ret)
		goto out;
	if (user->count != process->resource_count || !ane->cmd_buf ||
	    process->key.program == U32_MAX || process->key.process == U32_MAX ||
	    process->key.procedure == U32_MAX) {
		ret = -EINVAL;
		goto out;
	}
	for (i = 0; i < user->count; i++) {
		struct ane_t6021_bo *b;
		const struct ane_h16_resource *r = &process->resources[i];
		const struct ane_legacy_buffer *m = &process->mappings[i];

		if (ios[i].flags || ios[i].reserved ||
		    ios[i].buffer_id != r->buffer.index || ios[i].type != r->buffer.class ||
		    ios[i].size != r->buffer.size || !m->cpu ||
		    m->dma != r->buffer.mapping_dva || m->size != r->buffer.mapping_size) {
			ret = -EINVAL;
			goto out;
		}
		for (j = 0; j < i; j++) {
			if (ios[j].bo_handle == ios[i].bo_handle) {
				ret = -EINVAL;
				goto out;
			}
		}
		mutex_lock(&ane_t6021_bo_lock);
		list_for_each_entry(b, &fd->bos, node) {
			if (b->handle == ios[i].bo_handle && b->owner == fd &&
			    b->size >= ios[i].size) {
				kref_get(&b->refcount);
				bos[pinned++] = b;
				break;
			}
		}
		mutex_unlock(&ane_t6021_bo_lock);
		if (pinned != i + 1) {
			ret = -ENOENT;
			goto out;
		}
		buffers[i] = r->buffer;
	}
	/* All user-facing BOs are staging only. Concurrent mmap writes cannot
	 * change code, input snapshots or firmware-visible output allocations.
	 */
	for (i = 0; i < user->count; i++) {
		if (process->resources[i].permissions == 1)
			memcpy(process->mappings[i].cpu, bos[i]->cpu, buffers[i].size);
		else
			memset(process->mappings[i].cpu, 0xff, buffers[i].size);
	}
	request.key = process->key;
	request.deadline = jiffies + msecs_to_jiffies(user->timeout_ms ?: 5000);
	ret = ane_h16_pending_insert(&ane->pending, &request, jiffies);
	if (ret)
		goto out;
	call.key = request.key;
	call.buffers = buffers;
	call.buffer_count = user->count;
	/* Custom BAR selectors/values have no validated process map yet. */
	ret = ane_h16_execute_encode(ane->cmd_buf->cpu, ANE_H16_EXECUTE_BASE_SIZE, &call);
	if (ret) {
		ane_h16_pending_remove(&ane->pending, &request);
		goto out;
	}
	ane->waiting = &request;
	/* Coherent allocations require ordering, not streaming-map sync calls. */
	wmb();
	dma_wmb();
	ret = ane_rtclient_command(ane, ane->cmd_buf, ANE_H16_EXECUTE_BASE_SIZE,
				   ANE_H16_EXECUTE, 7, user->timeout_ms ?: 5000);
	ane->waiting = NULL;
	if (ret || !request.terminal || request.status) {
		ret = ret ?: -EIO;
		ane_h16_pending_abort(&ane->pending, &request, ret);
		atomic_set(&ane_t6021_quarantined, 1);
		goto out;
	}
	dma_rmb();
	for (i = 0; i < user->count; i++)
		if (process->resources[i].permissions == 2)
			memcpy(bos[i]->cpu, process->mappings[i].cpu, buffers[i].size);
	/* Private DMA mappings remain resident for the entire process lifetime,
	 * including after terminal failure or an uncertain transport outcome.
	 */
out:
	while (pinned)
		kref_put(&bos[--pinned]->refcount, ane_t6021_bo_release);
	mutex_unlock(&ane_t6021_fw_lock);
	kfree(ios);
	return ret;
}

static int ane_rtclient_procedure_call(struct ane_rtclient *ane,
				       struct drm_file *file,
				       const struct drm_ane_exec *user)
{
	struct drm_ane_exec_io *ios;
	struct ane_t6021_bo **exec_bos;
	struct ane_legacy_buffer *command;
	struct ane_t6021_fd *fd = file->driver_priv;
	size_t ios_size;
	size_t cmd_size;
	int ret, i, pinned = 0;

	if (ane->soc->platform)
		return ane_rtclient_h16_call(ane, file, user);
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

	exec_bos = kcalloc(user->count, sizeof(*exec_bos), GFP_KERNEL);
	if (!exec_bos) {
		kfree(ios);
		return -ENOMEM;
	}

	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined)) {
		ret = -ETIMEDOUT;
		goto unlock;
	}
	command = ane->cmd_buf;
	if (!command) {
		ret = -ENODEV;
		goto unlock;
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
			/* Keep this IO element out of the reuse pool until completion. */
			kref_get(&bo->refcount);
			exec_bos[pinned++] = bo;
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
	/* Command failures latch quarantine before these final puts. */
	while (pinned)
		kref_put(&exec_bos[--pinned]->refcount, ane_t6021_bo_release);
	kfree(exec_bos);
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

	if (atomic_read(&ane_t6021_quarantined))
		return -EIO;
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
	/* Every fw-visible DMA surface must be 16 KiB aligned and clear
	 * of the firmware entry alias (the same invariant
	 * ane_rtclient_legacy_alloc enforces).
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
	INIT_LIST_HEAD(&fd->h16_processes);
	file->driver_priv = fd;
	return 0;
}

static void ane_t6021_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_t6021_bo *bo, *tmp;
	struct ane_h16_process *process, *next;

	if (!fd)
		return;
	mutex_lock(&ane_t6021_fw_lock);
	/* Revoke per-file authority, retaining device-owned DMA allocations. */
	list_for_each_entry_safe(process, next, &fd->h16_processes, owner_node)
		list_del_init(&process->owner_node);
	mutex_lock(&ane_t6021_bo_lock);
	list_for_each_entry_safe(bo, tmp, &fd->bos, node)
		ane_t6021_bo_drop(bo);
	mutex_unlock(&ane_t6021_bo_lock);
	mutex_unlock(&ane_t6021_fw_lock);
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

/* Firmware perf mode: CSNE_CMD_CH_PROPERTY_WRITE (0x1f), channel 0,
 * property 0x10aa, value 1. macOS sends it once during power-on; the
 * 13.5 firmware treats it as its engine perf-mode switch. It is a runtime
 * switch (write 1 to /sys/module/ane_t6021/parameters/fw_perf_mode) so
 * its effect on call time can be measured on one boot. Only 1 is
 * accepted: no other value is known to be safe.
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
		 "Write 1 once to send CH_PROPERTY_WRITE 0x10aa = 1 (fw perf mode); reads back whether it was sent");

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

/* ---- RTKit ops (kept on the boot path; the legacy ChMan transport
 * is used by the ioctls once probe confirms legacy_only / chman_ok).
 */

static void ane_rtclient_recv(void *cookie, u8 ep, u64 message)
{
	struct ane_rtclient *ane = cookie;

	dev_dbg(ane->dev,
		"rtkit app msg: ep=%#x msg=%016llx\n", ep, message);
}

static void ane_rtclient_crashed(void *cookie, const void *crashlog,
				 size_t size)
{
	struct ane_rtclient *ane = cookie;

	dev_err(ane->dev, "rtkit: coprocessor crashed (crashlog %zu bytes)\n",
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
			 "rtkit: fw-provided shmem iova=%pad size=%#zx — refused\n",
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
			"rtkit: shmem grant %pad+%#zx overlaps the fw alias — refusing\n",
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
			 "rtkit: shmem %pad HELD (CPU started) — not freed\n",
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

/* ---- poll worker: RX fallback, the recv line is not relied on ----
 * Runs every 10 ms until the handshake completes,
 * then every second while poll_rx stays on. Armed BEFORE the host ack
 * so the fw's HELLO is never missed.
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

/* STARTEP every fw-announced app endpoint (>= 0x20; the fw mgmt
 * dispatcher starts an endpoint on flag bit 1). Same call every Asahi
 * RTKit client makes.
 */
static void ane_rtclient_start_app_eps(struct ane_rtclient *ane)
{
	int ep;

	for (ep = 0x20; ep < 0x100; ep++) {
		int ret;

		if (!apple_rtkit_has_endpoint(ane->rtk, ep))
			continue;
		ret = apple_rtkit_start_ep(ane->rtk, ep);
		dev_dbg(ane->dev, "rtkit: STARTEP app ep %#x -> %pe\n",
			ep, ERR_PTR(ret));
	}
}

/* Multi-domain genpd attach, ownership-correct and idempotent. */
struct ane_rtclient_pd {
	struct list_head list;
	struct device *dev;
	struct device **pd_dev;
	struct device_link **pd_link;
	int count;

	/* T8112 power gate, kept in step with power-domains[0]. */
	struct notifier_block pwgate_nb;
	void __iomem *pwgate_base;
	void __iomem *pwgate;
	bool pwgate_nb_added;
	bool pwgate_opened;
};

static LIST_HEAD(ane_rtclient_pd_list);
static DEFINE_MUTEX(ane_rtclient_pd_lock);
static bool ane_rtclient_pinned;
static bool ane_rtclient_probe_failed_dirty;
static DEFINE_MUTEX(ane_rtclient_probe_guard_lock);

static void ane_rtclient_pd_free(struct ane_rtclient_pd *pd)
{
	list_del(&pd->list);
	if (pd->pwgate_base)
		iounmap(pd->pwgate_base);
	kfree(pd->pd_dev);
	kfree(pd->pd_link);
	kfree(pd);
}

/*
 * T8112 power gate. A word in the "set" window gates the ANE: bits 29:28
 * set (the word reads 0x30000000) is closed, both clear is open. It must be
 * open before the ANE CPU power state (ane_sys_cpu) turns on and is closed
 * again after that state turns off, with ane_sys, the parent, on both
 * times. power-domains[0] is ane_sys_cpu, and genpd calls this notifier
 * after ane_sys is on and before ane_sys_cpu is written (PRE_ON), and
 * after ane_sys_cpu is off and before ane_sys goes off (OFF), which is that
 * order. Only the two known values are written. Any other word fails the
 * power-up without a write.
 */
#define ANE_PWGATE_CLOSED	GENMASK(29, 28)
#define ANE_PWGATE_POLL_US	10
#define ANE_PWGATE_TIMEOUT_US	100000

static int ane_rtclient_pwgate_notify(struct notifier_block *nb,
				      unsigned long action, void *data)
{
	struct ane_rtclient_pd *pd =
		container_of(nb, struct ane_rtclient_pd, pwgate_nb);
	u32 gate;
	int ret;

	switch (action) {
	case GENPD_NOTIFY_PRE_ON:
		gate = readl(pd->pwgate);
		if (!(gate & ANE_PWGATE_CLOSED))
			return NOTIFY_OK;
		if (gate != ANE_PWGATE_CLOSED) {
			dev_err(pd->dev, "power gate word %#x is not a known state\n",
				gate);
			return notifier_from_errno(-EIO);
		}
		writel(0, pd->pwgate);
		ret = readl_poll_timeout_atomic(pd->pwgate, gate,
						!(gate & ANE_PWGATE_CLOSED),
						ANE_PWGATE_POLL_US,
						ANE_PWGATE_TIMEOUT_US);
		if (ret) {
			writel(ANE_PWGATE_CLOSED, pd->pwgate);
			dev_err(pd->dev, "power gate did not open (word %#x)\n",
				gate);
			return notifier_from_errno(ret);
		}
		pd->pwgate_opened = true;
		return NOTIFY_OK;
	case GENPD_NOTIFY_OFF:
		if (!pd->pwgate_opened)
			return NOTIFY_OK;
		pd->pwgate_opened = false;
		writel(ANE_PWGATE_CLOSED, pd->pwgate);
		ret = readl_poll_timeout_atomic(pd->pwgate, gate,
						(gate & ANE_PWGATE_CLOSED) ==
						ANE_PWGATE_CLOSED,
						ANE_PWGATE_POLL_US,
						ANE_PWGATE_TIMEOUT_US);
		if (ret)
			dev_warn(pd->dev, "power gate did not close (word %#x)\n",
				 gate);
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

/*
 * Register the power gate notifier on power-domains[0] before its device
 * link is created: the link resumes the domain at once.
 */
static int ane_rtclient_pwgate_add(struct ane_rtclient *ane,
				   struct ane_rtclient_pd *pd)
{
	int ret;

	if (!ane->soc->pwgate_off || pd->pwgate_nb_added)
		return 0;
	if (!pd->pwgate_base) {
		pd->pwgate_base = of_iomap(ane->dev->of_node, 2);
		if (!pd->pwgate_base)
			return -ENOMEM;
		pd->pwgate = pd->pwgate_base + ane->soc->pwgate_off;
	}
	pd->pwgate_nb.notifier_call = ane_rtclient_pwgate_notify;
	ret = dev_pm_genpd_add_notifier(pd->pd_dev[0], &pd->pwgate_nb);
	if (ret)
		return ret;
	pd->pwgate_nb_added = true;
	return 0;
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
		if (i == 0) {
			err = ane_rtclient_pwgate_add(ane, pd);
			if (err)
				goto out;
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
			/*
			 * genpd keeps a notifier past detach, so drop it first.
			 * The domain may then go off with the gate open, the
			 * state between the two power-up steps.
			 */
			if (i == 0 && pd->pwgate_nb_added) {
				dev_pm_genpd_remove_notifier(pd->pd_dev[0]);
				pd->pwgate_nb_added = false;
			}
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
			"fw_alias_reserved=1, but no no-map /reserved-memory node covers the iBoot firmware windows (this m1n1 does not reserve them); refusing before power access, ANE off\n");
		return -ENODEV;
	}

	/* Early, BEFORE any allocation/power: legacy_only must never be
	 * rejected after the CPU release, where an unwind could drop
	 * domains under a running ASC.
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
	INIT_LIST_HEAD(&ane->h16_processes);
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
	/* T8112: the power gate notifier opened the gate before ane_sys_cpu
	 * turned on. A gate that still reads closed means the state was
	 * already on without it (no power-up ran), so refuse before any
	 * engine read.
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

		/* fw_start order: staging requires fw_load and an
		 * IOMMU-mapped device BEFORE any allocation/staging.
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
				"fw_start: poll A timeout, no READY — HELD until reboot, RTKit handshake skipped\n");
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
				"LEGACY P8 host ack: SCRATCH3 <- %08x\n",
				ANE_T6021_BOOT_ACK);
			/* hello_wait_ms > 0 only: init rtkit and arm the RX
			 * poll worker BEFORE writing the ack, so a HELLO
			 * after the ack is not missed. The 13.5 fw sends none.
			 */
			if (hello_wait_ms && !ane->rtk) {
				ane->rtk = devm_apple_rtkit_init(dev, ane,
								 NULL, 0,
								&ane_rtclient_rtkit_ops);
				if (IS_ERR(ane->rtk)) {
					dev_err(dev,
						"LEGACY hello: rtkit init %pe\n",
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

				dev_dbg(dev, "LEGACY hello: boot begin (%u ms)\n",
					hello_wait_ms);
				do {
					hello_ret = apple_rtkit_boot(ane->rtk);
				} while (hello_ret == -ETIME &&
					 time_before(jiffies, hello_deadline));
				dev_info(dev,
					 "LEGACY hello: boot %pe running=%d crashed=%d\n",
					 ERR_PTR(hello_ret),
					 apple_rtkit_is_running(ane->rtk),
					 apple_rtkit_is_crashed(ane->rtk));
				if (!hello_ret) {
					ane->boot_done = true;
					if (start_app_eps)
						ane_rtclient_start_app_eps(ane);
				} else {
					cancel_delayed_work_sync(&ane->poll_work);
				}
			}
		} else {
			dev_warn(dev,
				 "LEGACY ack withheld (booted=%u scratch3_ack=%u chman_ok=%u)\n",
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
			/* CONFIG_GET (opcode 0x03) keeps the legacy ChMan
			 * transport armed for the ioctls; the reply's
			 * word +0x08 must be nonzero. The
			 * boot heap + 'IPC ' allocations stay HELD until
			 * reboot.
			 */
			struct ane_legacy_buffer *command = ane->cmd_buf;
			unsigned long query_deadline = jiffies + msecs_to_jiffies(3000);
			int qret;

			if (command) {
				memset(command->cpu, 0, SZ_16K);
				qret = ane_rtclient_legacy_exchange(ane, command, 16, 0x03, 1,
								    query_deadline);
				dev_dbg(dev,
					"LEGACY CONFIG_GET words %08x %08x result=%d (DMA remains held)\n",
					READ_ONCE(((u32 *)command->cpu)[1]),
					READ_ONCE(((u32 *)command->cpu)[2]),
					qret);
				if (qret) {
					cfg_err = qret;
				} else if (!READ_ONCE(((u32 *)command->cpu)[2])) {
					dev_err(dev,
						"LEGACY CONFIG_GET reply word +0x08 zero\n");
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
			dev_err_probe(dev, ret, "apple_rtkit_init failed\n");
			goto err_pm_or_hold;
		}
		/* RX fallback worker first: the fw's HELLO must not be
		 * missed, and the recv line is not relied on.
		 */
		schedule_delayed_work(&ane->poll_work, msecs_to_jiffies(10));
		deadline = jiffies + msecs_to_jiffies(hello_wait_ms);
		do {
			boot_ret = apple_rtkit_boot(ane->rtk);
		} while (boot_ret == -ETIME && time_before(jiffies, deadline));
		if (boot_ret) {
			dev_err(dev, "rtkit boot handshake failed: %pe\n",
				ERR_PTR(boot_ret));
			cancel_delayed_work_sync(&ane->poll_work);
			ret = boot_ret;
			goto err_pm_or_hold;
		}
		ane->boot_done = true;
		if (start_app_eps)
			ane_rtclient_start_app_eps(ane);
	}

	if (!ane->chman_ok) {
		dev_err(dev,
			"install: ChMan table not validated — refusing to register DRM device (ioctls would stall the ring)\n");
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
		drmret = drm_dev_register(&adrm->drm, 0);
		if (drmret) {
			dev_err_probe(dev, drmret, "drm_dev_register\n");
			ret = drmret;
			goto err_pm_or_hold;
		}
		WRITE_ONCE(ane_t6021_perf_ane, ane);
		dev_info(dev,
			 "loaded ane_t6021 (DRM major %d minor %d; ABI 2; legacy_only=%u chman_ok=%u booted=%u; state %s; BO cap %u MiB)\n",
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
	const struct ane_t602x_soc *soc = of_device_get_match_data(&pdev->dev);
	bool hardware_touched = false;
	int ret;

	if (!soc)
		return -ENODEV;
	/* Later generations cannot enter H14 boot or parameter paths. */
	if (soc->platform)
		return ane_fw_platform_preflight(pdev, soc->platform);

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

		if (ane)
			cancel_delayed_work_sync(&ane->poll_work);
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

static const struct ane_t602x_soc ane_h16_soc = {
	.platform = &ane_h16_platform,
};

static const struct ane_t602x_soc ane_h17_soc = {
	.platform = &ane_h17_platform,
};

static const struct of_device_id ane_rtclient_of_match[] = {
	{ .compatible = "apple,t6020-ane", .data = &ane_t6020_soc },
	{ .compatible = "apple,t6021-ane", .data = &ane_t6021_soc },
	{ .compatible = "apple,t6022-ane", .data = &ane_t6022_soc },
	{ .compatible = "apple,t8112-ane", .data = &ane_t8112_soc },
	{ .compatible = "apple,t6041-ane", .data = &ane_h16_soc },
	{ .compatible = "apple,t8140-ane", .data = &ane_h17_soc },
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
	const struct of_device_id *match;
	struct device_node *np;
	int ret;

	/* platform_probe() powers a single genpd, and DMA configuration
	 * happens even earlier. Refuse later generations before registering
	 * the driver, including disabled nodes, so these core paths cannot
	 * precede the ownership and firmware preflight. These SoCs have one
	 * ANE generation; refusing registration cannot hide an H14 engine.
	 */
	for_each_matching_node_and_match(np, ane_rtclient_of_match, &match) {
		const struct ane_t602x_soc *soc = match->data;
		struct platform_device *pdev;

		if (!soc->platform)
			continue;
		if (soc->platform->owner == ANE_FW_OWNER_EXCLAVE) {
			of_node_put(np);
			pr_info("ane: H17 ANE: Linux exclave ownership handoff unavailable; refusing registration\n");
			return -EOPNOTSUPP;
		}
		pdev = of_find_device_by_node(np);
		of_node_put(np);
		if (!pdev) {
			pr_info("ane: H16 ANE: firmware transport unvalidated; keeping disabled\n");
			return -EOPNOTSUPP;
		}
		ret = ane_fw_platform_preflight(pdev, soc->platform);
		put_device(&pdev->dev);
		return ret;
	}

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
MODULE_DESCRIPTION("Apple Neural Engine firmware driver with H16/H17 preflight");

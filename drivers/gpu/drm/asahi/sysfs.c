// SPDX-License-Identifier: GPL-2.0-only OR MIT
//
// Per-device sysfs shim. Rust owns the snapshot through an Arc in the bound
// driver. The attribute wrapper is removed and its readers drained before
// that Arc is dropped. Atomic fields may reflect different sample instants.

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/suspend.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "sysfs.h"

/* Legacy prefix of repr(C) Rust atomics; READ_ONCE accesses current values. */
struct asahi_stats_snapshot {
	u32 util1;
	u32 util2;
	u32 util3;
	u32 util4;
	u32 pstate;
	u32 avg_power_mw;
	u32 temperature_raw;
	u32 temperature_scale;
	u32 temperature_tmin;
	u32 temperature_tmax;
	u64 busy_ns;
	u64 jobs;
};

/* Mirror of the repr(C) Rust `memacct::Ledger`, in bytes. */
struct asahi_gpu_memory {
	u64 user_committed;
	u64 user_resident;
	u64 kernel_resident;
	u64 refused;
	u64 limit;
	u64 client_limit;
	u64 user_kept;
};

struct asahi_stats_attribute {
	struct device_attribute attr;
	struct device_attribute memory_attr;
	const struct asahi_stats_snapshot *snapshot;
	const struct asahi_gpu_memory *memory;
	bool export_enabled;
	asahi_stats_read_fn read;
	asahi_stats_suspend_fn suspend;
	asahi_stats_frequency_fn frequency;
	asahi_stats_hibernate_fn hibernate;
	struct notifier_block pm_notifier;
	/* Drain at PM_HIBERNATION_PREPARE ahead of the display and PMP notifiers. */
	struct notifier_block hib_notifier;
	/* pm_notifier saw PM_HIBERNATION_PREPARE: it owns the thaw at PM_POST_HIBERNATION. */
	bool hib_late_prepared;
};

#ifdef CONFIG_HIBERNATION
/*
 * Hibernation gate. Native graphics owns firmware DMA (GPU page tables,
 * queues, firmware heap) that a hibernation image cannot recreate: after a
 * resume from disk the GPU is the instance the boot loader left halted, not
 * the one this driver programmed.
 *
 * A backend with a cold restore path (G17: it registers a hibernate
 * callback) drains its GPU at PM_HIBERNATION_PREPARE and re-boots the
 * firmware against the restored host state at PM_POST_HIBERNATION; while
 * only such devices are bound, hibernation is allowed unless
 * hibernate_republish=0. Any other bound GPU still refuses it unless
 * hibernate_cold_init=1.
 *
 * Restoring an image is always refused while a GPU is bound here: the kernel
 * that restores must not own a GPU firmware session of its own, since the
 * image's driver would then find a running firmware it did not program.
 */
static bool asahi_hibernate_cold_init;
module_param_named(hibernate_cold_init, asahi_hibernate_cold_init, bool, 0644);
MODULE_PARM_DESC(hibernate_cold_init,
		 "Allow hibernation while a GPU without a restore path owns DMA (experimental)");

static bool asahi_hibernate_republish = true;
module_param_named(hibernate_republish, asahi_hibernate_republish, bool, 0644);
MODULE_PARM_DESC(hibernate_republish,
		 "Allow hibernation with a G17 GPU bound: drain it before the image, re-boot its firmware after resume (default: on)");

static atomic_t asahi_hibernate_devices = ATOMIC_INIT(0);
/* Bound devices without a hibernate callback. */
static atomic_t asahi_hibernate_unrestorable = ATOMIC_INIT(0);

static int asahi_hibernate_notify(struct notifier_block *nb, unsigned long action,
				  void *unused)
{
	int devices = atomic_read(&asahi_hibernate_devices);

	switch (action) {
	case PM_HIBERNATION_PREPARE:
		if (!devices || asahi_hibernate_cold_init)
			break;
		if (atomic_read(&asahi_hibernate_unrestorable)) {
			pr_warn("asahi: refusing hibernation: a bound GPU has no restore path (hibernate_cold_init=1 to try)\n");
			return notifier_from_errno(-EBUSY);
		}
		if (!asahi_hibernate_republish) {
			pr_warn("asahi: refusing hibernation: hibernate_republish=0\n");
			return notifier_from_errno(-EBUSY);
		}
		break;
	case PM_RESTORE_PREPARE:
		/*
		 * A restorable (G17) device bound here would leave a firmware running
		 * with this kernel's graph under the image's driver, which then takes it
		 * for its own: always refuse. hibernate_cold_init only relaxes the
		 * refusal for backends without a restore path.
		 */
		if (devices > atomic_read(&asahi_hibernate_unrestorable) ||
		    (devices && !asahi_hibernate_cold_init)) {
			pr_warn("asahi: refusing to restore an image: this kernel owns a GPU firmware session\n");
			return notifier_from_errno(-EBUSY);
		}
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block asahi_hibernate_nb = {
	.notifier_call = asahi_hibernate_notify,
	/* Ahead of the G17 drain (20): a refusal costs no drain. */
	.priority = 30,
};

static int asahi_hibernate_device_bound(bool restorable)
{
	int ret = 0;

	if (!restorable)
		atomic_inc(&asahi_hibernate_unrestorable);
	if (atomic_inc_return(&asahi_hibernate_devices) == 1) {
		ret = register_pm_notifier(&asahi_hibernate_nb);
		if (ret) {
			atomic_dec(&asahi_hibernate_devices);
			if (!restorable)
				atomic_dec(&asahi_hibernate_unrestorable);
		}
	}
	return ret;
}

static void asahi_hibernate_device_unbound(bool restorable)
{
	if (atomic_dec_and_test(&asahi_hibernate_devices))
		unregister_pm_notifier(&asahi_hibernate_nb);
	if (!restorable)
		atomic_dec(&asahi_hibernate_unrestorable);
}
#else
static inline int asahi_hibernate_device_bound(bool restorable) { return 0; }
static inline void asahi_hibernate_device_unbound(bool restorable) { }
#endif

/* 0: unavailable, 1: valid, 2: stale, 3: unsupported; matches g17::telemetry. */
static const char *asahi_stats_quality(u32 status)
{
	switch (status) {
	case 1:
		return "valid";
	case 2:
		return "stale";
	case 3:
		return "unsupported";
	default:
		return "unavailable";
	}
}

static ssize_t agx_qualified_stats_show(struct asahi_stats_attribute *stats, char *buf)
{
	struct asahi_qualified_stats snap;
	ssize_t n = 0;

	stats->read(stats->snapshot, &snap);
	n += sysfs_emit_at(buf, n, "utilisation_status %s\n",
			   asahi_stats_quality(snap.util_status));
	if (snap.util_status == 1) {
		n += sysfs_emit_at(buf, n, "util_tiler_percent %u\n", snap.tiler);
		n += sysfs_emit_at(buf, n, "util_renderer_percent %u\n", snap.renderer);
		n += sysfs_emit_at(buf, n, "util_device_percent %u\n", snap.device);
	} else {
		n += sysfs_emit_at(buf, n, "util_tiler_percent unsupported\n"
				   "util_renderer_percent unsupported\n"
				   "util_device_percent unsupported\n");
	}
	/*
	 * unsupported comes from the firmware match data (g17::stats): V25G83
	 * never emits a pstate record, so it is not a transient state.
	 * unavailable keeps meaning "no qualified sample yet".
	 */
	n += sysfs_emit_at(buf, n, "pstate_status %s\n",
			   asahi_stats_quality(snap.pstate_status));
	if (snap.pstate_status == 1)
		n += sysfs_emit_at(buf, n, "pstate %u\n", snap.pstate);
	else
		n += sysfs_emit_at(buf, n, "pstate unsupported\n");
	/* No host frequency request, measured power or temperature. */
	n += sysfs_emit_at(buf, n, "requested_frequency_hz unsupported\n");
	/*
	 * V26A428 reports the state the GPU runs at: its frequency is the
	 * achieved one. State 0 is off (gated cores) and runs at 0 Hz.
	 */
	if (snap.pstate_status == 1 && (snap.achieved_frequency_hz || !snap.pstate))
		n += sysfs_emit_at(buf, n, "achieved_frequency_hz %llu\n",
				   (unsigned long long)snap.achieved_frequency_hz);
	else
		n += sysfs_emit_at(buf, n, "achieved_frequency_hz unsupported\n");
	if (snap.busy_status == 1)
		n += sysfs_emit_at(buf, n, "busy_ns %llu\n",
				   (unsigned long long)snap.busy_ns);
	else
		n += sysfs_emit_at(buf, n, "busy_ns unsupported\n");
	n += sysfs_emit_at(buf, n, "power_mw unsupported\n"
			   "temperature_raw unsupported\n");
	n += sysfs_emit_at(buf, n, "utilisation_source firmware-statistics-tag-0x13\n"
			   "utilisation_samples %llu\n"
			   "utilisation_host_ns %llu\n",
			   (unsigned long long)snap.util_samples,
			   (unsigned long long)snap.util_host_ns);
	return n;
}

static int asahi_stats_pm_notify(struct notifier_block *nb, unsigned long action, void *unused)
{
	struct asahi_stats_attribute *stats =
		container_of(nb, struct asahi_stats_attribute, pm_notifier);

	switch (action) {
	case PM_HIBERNATION_PREPARE:
		/* The drain already ran in asahi_stats_hib_notify(). */
		stats->suspend(stats->snapshot, true);
		WRITE_ONCE(stats->hib_late_prepared, true);
		break;
	case PM_SUSPEND_PREPARE:
	case PM_RESTORE_PREPARE:
		stats->suspend(stats->snapshot, true);
		break;
	case PM_POST_HIBERNATION:
		/*
		 * After a resume from disk the boot loader has cold-restarted the GPU
		 * firmware while the host state was restored from the image; after an
		 * aborted hibernation the firmware kept running. The callback tells
		 * them apart, re-boots a cold firmware against the restored state and
		 * reopens submission. This notifier has a lower priority than the PMP
		 * restore, which has already run.
		 */
		if (stats->hibernate)
			stats->hibernate(stats->snapshot, ASAHI_HIBERNATE_THAW);
		stats->suspend(stats->snapshot, false);
		WRITE_ONCE(stats->hib_late_prepared, false);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_RESTORE:
		stats->suspend(stats->snapshot, false);
		break;
	}
	return NOTIFY_OK;
}

/*
 * G17 drain, at a priority above the display (DCP) and PMP notifiers: a GPU that cannot be
 * drained refuses the hibernation before any other driver has given anything up (the DCP
 * releases its display links at PM_HIBERNATION_PREPARE). A refusal here reaches no later
 * notifier, and the chain sends PM_POST_HIBERNATION only to the notifiers before it.
 *
 * The thaw stays with asahi_stats_pm_notify(), below the PMP restore that a cold firmware
 * needs. Only when the transition failed before that notifier saw PM_HIBERNATION_PREPARE
 * (a later notifier refused it) does it never get PM_POST_HIBERNATION; the firmware then
 * kept running, and the thaw runs here. After a resume from disk the flag is set in the
 * image, so the restored kernel leaves the cold republish to asahi_stats_pm_notify().
 */
static int asahi_stats_hib_notify(struct notifier_block *nb, unsigned long action, void *unused)
{
	struct asahi_stats_attribute *stats =
		container_of(nb, struct asahi_stats_attribute, hib_notifier);
	int ret;

	switch (action) {
	case PM_HIBERNATION_PREPARE:
		WRITE_ONCE(stats->hib_late_prepared, false);
		ret = stats->hibernate(stats->snapshot, ASAHI_HIBERNATE_FREEZE);
		if (ret)
			return notifier_from_errno(ret);
		break;
	case PM_POST_HIBERNATION:
		if (!READ_ONCE(stats->hib_late_prepared))
			stats->hibernate(stats->snapshot, ASAHI_HIBERNATE_THAW);
		break;
	}
	return NOTIFY_OK;
}

static ssize_t agx_stats_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct asahi_stats_attribute *stats =
		container_of(attr, struct asahi_stats_attribute, attr);
	const struct asahi_stats_snapshot *snap = stats->snapshot;
	ssize_t n = 0;
	u32 pstate;
	u64 frequency_hz;

	if (!stats->export_enabled)
		return sysfs_emit(buf, "unsupported\n");
	if (stats->read)
		return agx_qualified_stats_show(stats, buf);
	pstate = READ_ONCE(snap->pstate);
	n += scnprintf(buf + n, PAGE_SIZE - n, "busy_ns %llu\n",
		       (unsigned long long)READ_ONCE(snap->busy_ns));
	n += scnprintf(buf + n, PAGE_SIZE - n, "jobs %llu\n",
		       (unsigned long long)READ_ONCE(snap->jobs));
	n += scnprintf(buf + n, PAGE_SIZE - n, "pstate %u\n",
		       pstate);
	n += scnprintf(buf + n, PAGE_SIZE - n, "power_mw %u\n",
		       (u32)READ_ONCE(snap->avg_power_mw));
	n += scnprintf(buf + n, PAGE_SIZE - n, "util1 %u\n",
		       (u32)READ_ONCE(snap->util1));
	n += scnprintf(buf + n, PAGE_SIZE - n, "util2 %u\n",
		       (u32)READ_ONCE(snap->util2));
	n += scnprintf(buf + n, PAGE_SIZE - n, "util3 %u\n",
		       (u32)READ_ONCE(snap->util3));
	n += scnprintf(buf + n, PAGE_SIZE - n, "util4 %u\n",
		       (u32)READ_ONCE(snap->util4));
	n += scnprintf(buf + n, PAGE_SIZE - n, "temperature_raw %u\n",
		       (u32)READ_ONCE(snap->temperature_raw));
	n += scnprintf(buf + n, PAGE_SIZE - n, "temperature_scale %u\n",
		       (u32)READ_ONCE(snap->temperature_scale));
	if (stats->frequency) {
		if (stats->frequency(snap, pstate, &frequency_hz))
			n += sysfs_emit_at(buf, n, "requested_frequency_hz %llu\n",
					   (unsigned long long)frequency_hz);
		else
			n += sysfs_emit_at(buf, n, "requested_frequency_hz unsupported\n");
	}

	return n;
}

/*
 * GPU memory held by the driver, in bytes: user objects created (committed)
 * and held by the GPU (resident), held kernel objects, the limits that admit
 * user objects (0: none), the number of refused creations, and the size of
 * the user objects that userspace asked to keep resident (user_kept), whether
 * their memory is held at the moment or not. The request is a hint: when
 * reclaim releases idle memory of a VM, its kept objects go after the others
 * (see DRM_IOCTL_ASAHI_GEM_RESIDENCY).
 */
static ssize_t gpu_memory_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct asahi_stats_attribute *stats =
		container_of(attr, struct asahi_stats_attribute, memory_attr);
	const struct asahi_gpu_memory *mem = stats->memory;
	ssize_t n = 0;

	n += scnprintf(buf + n, PAGE_SIZE - n, "user_committed %llu\n",
		       (unsigned long long)READ_ONCE(mem->user_committed));
	n += scnprintf(buf + n, PAGE_SIZE - n, "user_resident %llu\n",
		       (unsigned long long)READ_ONCE(mem->user_resident));
	n += scnprintf(buf + n, PAGE_SIZE - n, "kernel_resident %llu\n",
		       (unsigned long long)READ_ONCE(mem->kernel_resident));
	n += scnprintf(buf + n, PAGE_SIZE - n, "limit %llu\n",
		       (unsigned long long)READ_ONCE(mem->limit));
	n += scnprintf(buf + n, PAGE_SIZE - n, "client_limit %llu\n",
		       (unsigned long long)READ_ONCE(mem->client_limit));
	n += scnprintf(buf + n, PAGE_SIZE - n, "refused %llu\n",
		       (unsigned long long)READ_ONCE(mem->refused));
	n += scnprintf(buf + n, PAGE_SIZE - n, "user_kept %llu\n",
		       (unsigned long long)READ_ONCE(mem->user_kept));

	return n;
}

/*
 * Called from Rust's `AsahiDriver::probe` after the DRM device is
 * registered. Returns 0 on success, or a negative errno.
 */
int asahi_sysfs_register(struct device *dev, const void *snapshot,
			 int export_enabled, const void *memory, void **handle,
			 asahi_stats_read_fn read, asahi_stats_suspend_fn suspend,
			 asahi_stats_frequency_fn frequency,
			 asahi_stats_hibernate_fn hibernate)
{
	struct asahi_stats_attribute *stats;
	int ret;

	if (!dev || !handle || !memory || ((export_enabled || read) && !snapshot) ||
	    (!!read != !!suspend) || (hibernate && !suspend))
		return -ENODEV;

	BUILD_BUG_ON(offsetof(struct asahi_qualified_stats, util_host_ns) != 24);
	BUILD_BUG_ON(offsetof(struct asahi_qualified_stats, busy_ns) != 56);
	BUILD_BUG_ON(sizeof(struct asahi_qualified_stats) != 80);
	BUILD_BUG_ON(offsetof(struct asahi_gpu_memory, refused) != 24);
	BUILD_BUG_ON(offsetof(struct asahi_gpu_memory, client_limit) != 40);
	BUILD_BUG_ON(offsetof(struct asahi_gpu_memory, user_kept) != 48);
	BUILD_BUG_ON(sizeof(struct asahi_gpu_memory) != 56);

	BUILD_BUG_ON(offsetof(struct asahi_stats_snapshot, pstate) != 16);
	BUILD_BUG_ON(offsetof(struct asahi_stats_snapshot, busy_ns) != 40);
	BUILD_BUG_ON(offsetof(struct asahi_stats_snapshot, jobs) != 48);
	BUILD_BUG_ON(sizeof(struct asahi_stats_snapshot) != 56);

	stats = kzalloc_obj(*stats);
	if (!stats)
		return -ENOMEM;

	sysfs_attr_init(&stats->attr.attr);
	stats->attr.attr.name = "agx_stats";
	stats->attr.attr.mode = 0444;
	stats->attr.show = agx_stats_show;
	stats->snapshot = snapshot;
	stats->export_enabled = export_enabled;
	stats->read = read;
	stats->suspend = suspend;
	stats->frequency = frequency;
	stats->hibernate = hibernate;
	if (suspend) {
		stats->pm_notifier.notifier_call = asahi_stats_pm_notify;
		/* Below the PMP restore notifier (priority 10), which must run first. */
		stats->pm_notifier.priority = -10;
		ret = register_pm_notifier(&stats->pm_notifier);
		if (ret) {
			kfree(stats);
			return ret;
		}
		if (hibernate) {
			stats->hib_notifier.notifier_call = asahi_stats_hib_notify;
			/* Above the DCP (10) and PMP (10) notifiers. */
			stats->hib_notifier.priority = 20;
			ret = register_pm_notifier(&stats->hib_notifier);
			if (ret) {
				unregister_pm_notifier(&stats->pm_notifier);
				kfree(stats);
				return ret;
			}
		}
	}

	/*
	 * Counted by the gate (30) only once both notifiers are in the chain: a
	 * hibernation the gate lets through between the two would reach the stats
	 * notifier (-10, which marks the late prepare) without the drain (20), and
	 * the image would hold a GPU that was never drained.
	 */
	ret = asahi_hibernate_device_bound(hibernate != NULL);
	if (ret) {
		if (stats->suspend) {
			if (stats->hibernate)
				unregister_pm_notifier(&stats->hib_notifier);
			unregister_pm_notifier(&stats->pm_notifier);
		}
		kfree(stats);
		return ret;
	}

	sysfs_attr_init(&stats->memory_attr.attr);
	stats->memory_attr.attr.name = "gpu_memory";
	stats->memory_attr.attr.mode = 0444;
	stats->memory_attr.show = gpu_memory_show;
	stats->memory = memory;

	ret = device_create_file(dev, &stats->attr);
	if (ret) {
		asahi_hibernate_device_unbound(hibernate != NULL);
		if (stats->suspend) {
			if (stats->hibernate)
				unregister_pm_notifier(&stats->hib_notifier);
			unregister_pm_notifier(&stats->pm_notifier);
		}
		kfree(stats);
		return ret;
	}

	ret = device_create_file(dev, &stats->memory_attr);
	if (ret) {
		device_remove_file(dev, &stats->attr);
		asahi_hibernate_device_unbound(hibernate != NULL);
		if (stats->suspend) {
			if (stats->hibernate)
				unregister_pm_notifier(&stats->hib_notifier);
			unregister_pm_notifier(&stats->pm_notifier);
		}
		kfree(stats);
		return ret;
	}

	*handle = stats;
	return 0;
}
EXPORT_SYMBOL_GPL(asahi_sysfs_register);

/*
 * G17: GPU idle power-off delay (ms, 2-1000). On T6040/25G83 the firmware powers the GPU cores
 * off after 2 ms idle and reads its own delay only at boot; above 2 the driver holds the cores
 * powered until the GPU has been idle this long (power::IDLE_HOLD_MS). Each wake-up there costs
 * about 1 ms, so userspace may lengthen it on AC power.
 */
static u32 *asahi_idle_word;

static ssize_t idle_off_delay_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(*asahi_idle_word));
}

static ssize_t idle_off_delay_ms_store(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t count)
{
	u32 ms;

	if (kstrtou32(buf, 0, &ms) || ms < 2 || ms > 1000)
		return -EINVAL;
	WRITE_ONCE(*asahi_idle_word, ms);
	return count;
}

static DEVICE_ATTR_RW(idle_off_delay_ms);

int asahi_sysfs_register_idle(struct device *dev, u32 *word)
{
	int ret;

	if (!dev || !word)
		return -ENODEV;
	asahi_idle_word = word;
	ret = device_create_file(dev, &dev_attr_idle_off_delay_ms);
	if (ret)
		asahi_idle_word = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(asahi_sysfs_register_idle);

void asahi_sysfs_unregister(struct device *dev, void *handle)
{
	struct asahi_stats_attribute *stats = handle;

	if (!dev || !stats)
		return;

	/* Out of the gate first: the reverse of asahi_sysfs_register(). */
	asahi_hibernate_device_unbound(stats->hibernate != NULL);
	/* Both notifier removal and file removal join readers of the Rust Arc. */
	if (stats->suspend) {
		if (stats->hibernate)
			unregister_pm_notifier(&stats->hib_notifier);
		unregister_pm_notifier(&stats->pm_notifier);
	}
	/* Removal drains active show callbacks before either allocation dies. */
	device_remove_file(dev, &stats->memory_attr);
	device_remove_file(dev, &stats->attr);
	if (asahi_idle_word) {
		device_remove_file(dev, &dev_attr_idle_off_delay_ms);
		asahi_idle_word = NULL;
	}
	kfree(stats);
}
EXPORT_SYMBOL_GPL(asahi_sysfs_unregister);

MODULE_AUTHOR("AGX driver maintainer");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("AGX firmware stats sysfs shim");

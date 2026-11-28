// SPDX-License-Identifier: GPL-2.0-only OR MIT
//
// Per-device sysfs shim. Rust owns the snapshot through an Arc in the bound
// driver. The attribute wrapper is removed and its readers drained before
// that Arc is dropped. Atomic fields may reflect different sample instants.

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/hwmon.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
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
	struct device_attribute limit_attr;
	const struct asahi_stats_snapshot *snapshot;
	const struct asahi_gpu_memory *memory;
	bool export_enabled;
	asahi_stats_read_fn read;
	asahi_stats_suspend_fn suspend;
	asahi_stats_frequency_fn frequency;
	struct notifier_block pm_notifier;
#if IS_REACHABLE(CONFIG_HWMON)
	struct device *hwmon;
	struct mutex hwmon_lock;
	ktime_t hwmon_at;
	struct asahi_qualified_stats hwmon_snap;
	bool hwmon_has_power;
	bool hwmon_has_temp;
	bool hwmon_has_freq;
#endif
};

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
	 * unsupported comes from the firmware match data (g17::stats): builds
	 * without a pstate telemetry protocol (such as T6040 25G83, left on
	 * Protocol::NONE pending measurement) have no qualified pstate support,
	 * so it is not a transient state.
	 * unavailable keeps meaning "no qualified sample yet".
	 */
	n += sysfs_emit_at(buf, n, "pstate_status %s\n",
			   asahi_stats_quality(snap.pstate_status));
	/*
	 * A pstate that is not valid reads its status word, so stale or
	 * unavailable is not mistaken for a missing feature. Where it is
	 * unsupported, the output is unchanged.
	 */
	if (snap.pstate_status == 1)
		n += sysfs_emit_at(buf, n, "pstate %u\n", snap.pstate);
	else
		n += sysfs_emit_at(buf, n, "pstate %s\n",
				   asahi_stats_quality(snap.pstate_status));
	/* No host frequency request, measured power or temperature. */
	n += sysfs_emit_at(buf, n, "requested_frequency_hz unsupported\n");
	/*
	 * Firmwares with a pstate protocol (V26A428, T6041 25G83) report
	 * the state the GPU runs at: its frequency is the achieved one. State 0
	 * is off (gated cores) and runs at 0 Hz.
	 */
	if (snap.pstate_status == 1 && (snap.achieved_frequency_hz || !snap.pstate))
		n += sysfs_emit_at(buf, n, "achieved_frequency_hz %llu\n",
				   (unsigned long long)snap.achieved_frequency_hz);
	else if (snap.pstate_status == 1)
		/* A valid state with no frequency in the board's table. */
		n += sysfs_emit_at(buf, n, "achieved_frequency_hz unsupported\n");
	else
		n += sysfs_emit_at(buf, n, "achieved_frequency_hz %s\n",
				   asahi_stats_quality(snap.pstate_status));
	if (snap.busy_status == 1)
		n += sysfs_emit_at(buf, n, "busy_ns %llu\n",
				   (unsigned long long)snap.busy_ns);
	else if (snap.busy_status == 2)
		/* The total stopped (a statistics ring was quarantined). */
		n += sysfs_emit_at(buf, n, "busy_ns stale\n");
	else
		n += sysfs_emit_at(buf, n, "busy_ns unsupported\n");
	if (snap.power_status == 1)
		n += sysfs_emit_at(buf, n, "power_mw %u\n", snap.power_mw);
	else
		n += sysfs_emit_at(buf, n, "power_mw unsupported\n");
	n += sysfs_emit_at(buf, n, "temperature_raw unsupported\n");
	if (snap.temp_status == 1)
		n += sysfs_emit_at(buf, n, "hotspot_temperature_mc %d\n", snap.temp_mc);
	else
		n += sysfs_emit_at(buf, n, "hotspot_temperature_mc unsupported\n");
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
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		stats->suspend(stats->snapshot, true);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		stats->suspend(stats->snapshot, false);
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

static DEFINE_MUTEX(asahi_limit_lock);

static ssize_t gpu_mem_limit_pct_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct asahi_stats_attribute *stats =
		container_of(attr, struct asahi_stats_attribute, limit_attr);
	u64 total = (u64)totalram_pages() << PAGE_SHIFT;
	u64 client = READ_ONCE(stats->memory->client_limit);

	return sysfs_emit(buf, "%llu\n",
			  client ? (unsigned long long)div64_u64(client * 100 + total / 2, total) : 0);
}

static ssize_t gpu_mem_limit_pct_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	u32 pct;

	if (kstrtou32(buf, 0, &pct))
		return -EINVAL;

	mutex_lock(&asahi_limit_lock);
	asahi_set_mem_limit_pct(pct);
	mutex_unlock(&asahi_limit_lock);
	return count;
}

#if IS_REACHABLE(CONFIG_HWMON)
#define ASAHI_HWMON_CACHE_NS	250000000LL

static void asahi_hwmon_sample(struct asahi_stats_attribute *stats,
			       struct asahi_qualified_stats *out)
{
	ktime_t now = ktime_get();

	mutex_lock(&stats->hwmon_lock);
	if (!stats->hwmon_at || ktime_sub(now, stats->hwmon_at) >= ASAHI_HWMON_CACHE_NS) {
		stats->read(stats->snapshot, &stats->hwmon_snap);
		stats->hwmon_at = now ? now : 1;
	}
	*out = stats->hwmon_snap;
	mutex_unlock(&stats->hwmon_lock);
}

static umode_t asahi_hwmon_is_visible(const void *data,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	const struct asahi_stats_attribute *stats = data;

	switch (type) {
	case hwmon_temp:
		return stats->hwmon_has_temp ? 0444 : 0;
	case hwmon_power:
		return stats->hwmon_has_power ? 0444 : 0;
	default:
		return 0;
	}
}

static int asahi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct asahi_stats_attribute *stats = dev_get_drvdata(dev);
	struct asahi_qualified_stats snap;

	asahi_hwmon_sample(stats, &snap);

	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;
		if (snap.temp_status != 1)
			return -ENODATA;
		*val = snap.temp_mc;
		return 0;
	case hwmon_power:
		if (attr != hwmon_power_input && attr != hwmon_power_average)
			return -EOPNOTSUPP;
		if (snap.power_status != 1)
			return -ENODATA;
		*val = (long)((u64)snap.power_mw * 1000ULL);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int asahi_hwmon_read_string(struct device *dev,
				   enum hwmon_sensor_types type, u32 attr,
				   int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = "SoC hotspot";
		return 0;
	case hwmon_power:
		*str = "GPU";
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info *const asahi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_AVERAGE | HWMON_P_LABEL),
	NULL
};

static const struct hwmon_ops asahi_hwmon_ops = {
	.is_visible = asahi_hwmon_is_visible,
	.read = asahi_hwmon_read,
	.read_string = asahi_hwmon_read_string,
};

static const struct hwmon_chip_info asahi_hwmon_chip = {
	.ops = &asahi_hwmon_ops,
	.info = asahi_hwmon_info,
};

static ssize_t freq1_input_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct asahi_stats_attribute *stats = dev_get_drvdata(dev);
	struct asahi_qualified_stats snap;

	asahi_hwmon_sample(stats, &snap);
	if (snap.pstate_status == 1 && (snap.achieved_frequency_hz || !snap.pstate))
		return sysfs_emit(buf, "%llu\n",
				  (unsigned long long)snap.achieved_frequency_hz);
	return -ENODATA;
}
static DEVICE_ATTR_RO(freq1_input);

static ssize_t freq1_label_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "sclk\n");
}
static DEVICE_ATTR_RO(freq1_label);

static struct attribute *asahi_hwmon_attrs[] = {
	&dev_attr_freq1_input.attr,
	&dev_attr_freq1_label.attr,
	NULL,
};

static const struct attribute_group asahi_hwmon_group = {
	.attrs = asahi_hwmon_attrs,
};

static const struct attribute_group *asahi_hwmon_groups[] = {
	&asahi_hwmon_group,
	NULL,
};

static void asahi_hwmon_register(struct device *dev,
				 struct asahi_stats_attribute *stats)
{
	struct asahi_qualified_stats probe;
	struct device *hwmon;

	mutex_init(&stats->hwmon_lock);
	stats->read(stats->snapshot, &probe);
	stats->hwmon_has_power = probe.power_status != 3;
	stats->hwmon_has_temp = probe.temp_status != 3;
	stats->hwmon_has_freq = probe.pstate_status != 3;

	if (!stats->hwmon_has_power && !stats->hwmon_has_temp && !stats->hwmon_has_freq)
		return;

	hwmon = hwmon_device_register_with_info(dev, "asahi_gpu", stats,
						&asahi_hwmon_chip,
						stats->hwmon_has_freq ? asahi_hwmon_groups : NULL);
	if (IS_ERR(hwmon)) {
		dev_warn(dev, "GPU sensors not registered (%pe)\n", hwmon);
		return;
	}
	stats->hwmon = hwmon;
}
#endif

/*
 * Called from Rust's `AsahiDriver::probe` after the DRM device is
 * registered. Returns 0 on success, or a negative errno.
 */
int asahi_sysfs_register(struct device *dev, const void *snapshot,
			 int export_enabled, const void *memory, void **handle,
			 asahi_stats_read_fn read, asahi_stats_suspend_fn suspend,
			 asahi_stats_frequency_fn frequency)
{
	struct asahi_stats_attribute *stats;
	int ret;

	if (!dev || !handle || !memory || ((export_enabled || read) && !snapshot) ||
	    (!!read != !!suspend))
		return -ENODEV;

	BUILD_BUG_ON(offsetof(struct asahi_qualified_stats, util_host_ns) != 24);
	BUILD_BUG_ON(offsetof(struct asahi_qualified_stats, busy_ns) != 56);
	BUILD_BUG_ON(offsetof(struct asahi_qualified_stats, power_status) != 80);
	BUILD_BUG_ON(offsetof(struct asahi_qualified_stats, temp_mc) != 92);
	BUILD_BUG_ON(sizeof(struct asahi_qualified_stats) != 96);
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
	if (suspend) {
		stats->pm_notifier.notifier_call = asahi_stats_pm_notify;
		ret = register_pm_notifier(&stats->pm_notifier);
		if (ret) {
			kfree(stats);
			return ret;
		}
	}

	sysfs_attr_init(&stats->memory_attr.attr);
	stats->memory_attr.attr.name = "gpu_memory";
	stats->memory_attr.attr.mode = 0444;
	stats->memory_attr.show = gpu_memory_show;
	/* Rust owns the ledger and changes its limits. */
	stats->memory = memory;

	sysfs_attr_init(&stats->limit_attr.attr);
	stats->limit_attr.attr.name = "gpu_mem_limit_pct";
	stats->limit_attr.attr.mode = 0644;
	stats->limit_attr.show = gpu_mem_limit_pct_show;
	stats->limit_attr.store = gpu_mem_limit_pct_store;

	ret = device_create_file(dev, &stats->attr);
	if (ret) {
		if (stats->suspend)
			unregister_pm_notifier(&stats->pm_notifier);
		kfree(stats);
		return ret;
	}

	ret = device_create_file(dev, &stats->memory_attr);
	if (ret) {
		device_remove_file(dev, &stats->attr);
		if (stats->suspend)
			unregister_pm_notifier(&stats->pm_notifier);
		kfree(stats);
		return ret;
	}

	ret = device_create_file(dev, &stats->limit_attr);
	if (ret) {
		device_remove_file(dev, &stats->memory_attr);
		device_remove_file(dev, &stats->attr);
		if (stats->suspend)
			unregister_pm_notifier(&stats->pm_notifier);
		kfree(stats);
		return ret;
	}

#if IS_REACHABLE(CONFIG_HWMON)
	if (stats->read)
		asahi_hwmon_register(dev, stats);
#endif

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

#if IS_REACHABLE(CONFIG_HWMON)
	if (stats->hwmon)
		hwmon_device_unregister(stats->hwmon);
	if (stats->read)
		mutex_destroy(&stats->hwmon_lock);
#endif

	/* Both notifier removal and file removal join readers of the Rust Arc. */
	if (stats->suspend)
		unregister_pm_notifier(&stats->pm_notifier);
	/* Removal drains active show callbacks before either allocation dies. */
	device_remove_file(dev, &stats->limit_attr);
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

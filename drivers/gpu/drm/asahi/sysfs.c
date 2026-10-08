// SPDX-License-Identifier: GPL-2.0-only OR MIT
//
// Per-device sysfs shim. Rust owns the snapshot through an Arc in the bound
// driver. The attribute wrapper is removed and its readers drained before
// that Arc is dropped. Atomic fields may reflect different sample instants.

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "sysfs.h"

/* Mirror of repr(C) Rust atomics; READ_ONCE accesses the current values. */
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

struct asahi_stats_attribute {
	struct device_attribute attr;
	const struct asahi_stats_snapshot *snapshot;
	bool export_enabled;
};

static ssize_t agx_stats_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct asahi_stats_attribute *stats =
		container_of(attr, struct asahi_stats_attribute, attr);
	const struct asahi_stats_snapshot *snap = stats->snapshot;
	ssize_t n = 0;

	if (!stats->export_enabled)
		return sysfs_emit(buf, "unsupported\n");
	n += scnprintf(buf + n, PAGE_SIZE - n, "busy_ns %llu\n",
		       (unsigned long long)READ_ONCE(snap->busy_ns));
	n += scnprintf(buf + n, PAGE_SIZE - n, "jobs %llu\n",
		       (unsigned long long)READ_ONCE(snap->jobs));
	n += scnprintf(buf + n, PAGE_SIZE - n, "pstate %u\n",
		       (u32)READ_ONCE(snap->pstate));
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

	return n;
}


/*
 * Called from Rust's `AsahiDriver::probe` after the DRM device is
 * registered. Returns 0 on success, or a negative errno.
 */
int asahi_sysfs_register(struct device *dev, const void *snapshot,
			 int export_enabled, void **handle)
{
	struct asahi_stats_attribute *stats;
	int ret;

	if (!dev || !snapshot || !handle)
		return -ENODEV;

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

	ret = device_create_file(dev, &stats->attr);
	if (ret) {
		kfree(stats);
		return ret;
	}

	*handle = stats;
	return 0;
}
EXPORT_SYMBOL_GPL(asahi_sysfs_register);

void asahi_sysfs_unregister(struct device *dev, void *handle)
{
	struct asahi_stats_attribute *stats = handle;

	if (!dev || !stats)
		return;

	/* Removal drains active show callbacks before either allocation dies. */
	device_remove_file(dev, &stats->attr);
	kfree(stats);
}
EXPORT_SYMBOL_GPL(asahi_sysfs_unregister);

MODULE_AUTHOR("AGX driver maintainer");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Asahi AGX firmware stats sysfs shim");

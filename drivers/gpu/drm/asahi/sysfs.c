// SPDX-License-Identifier: GPL-2.0-only OR MIT
//
// Sysfs shim for the AGX firmware stats export.
//
// The Rust side keeps the stats state in `crate::stats::StatsSnapshot` and
// publishes its raw pointer to the static `asahi_stats_snapshot_ptr` below
// (an atomic `u64` so the writer and reader do not need locking). This file
// owns the actual `device_attribute` and the formatted output, because
// `device_create_file` and the `device_attribute` macros are not in the
// Rust bindgen bindings for this kernel tree.
//
// The contract is one read-only file:
//   /sys/class/drm/card*/device/agx_stats
// Format: `key value\n` per line, ASCII integers, owner-readable.

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>

/*
 * Mirror of `crate::stats::StatsSnapshot` field layout. Rust sets
 * `asahi_stats_snapshot_ptr` (an atomic u64) to the heap address of one of
 * these, and clears it on unregister. Field reads are all `Relaxed` atomic
 * u32 / u64 (the C side uses READ_ONCE).
 */
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
	u64 last_busy_ts;
	u64 busy_ns;
	u64 jobs;
};

/*
 * Set by Rust; read via READ_ONCE. A NULL (0) pointer means the device has
 * not yet exposed stats (or has been unregistered). The Rust side owns the
 * lifetime of the pointed-to struct.
 */
unsigned long long asahi_stats_snapshot_ptr;
EXPORT_SYMBOL_GPL(asahi_stats_snapshot_ptr);

static ssize_t asahi_agx_stats_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct asahi_stats_snapshot __rcu *snap;
	ssize_t n = 0;

	(void)attr;

	rcu_read_lock();
	snap = (struct asahi_stats_snapshot __rcu *)
		READ_ONCE(asahi_stats_snapshot_ptr);
	if (!snap) {
		rcu_read_unlock();
		return scnprintf(buf, PAGE_SIZE, "unsupported\n");
	}
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
	rcu_read_unlock();

	return n;
}

static DEVICE_ATTR_RO(agx_stats);

/*
 * Called from Rust's `AsahiDriver::probe` after the DRM device is
 * registered. Returns 0 on success, or a negative errno.
 */
int asahi_sysfs_register(struct device *dev)
{
	int ret;

	if (!dev)
		return -ENODEV;

	ret = device_create_file(dev, &dev_attr_agx_stats);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(asahi_sysfs_register);

void asahi_sysfs_unregister(struct device *dev)
{
	if (!dev)
		return;

	device_remove_file(dev, &dev_attr_agx_stats);
	WRITE_ONCE(asahi_stats_snapshot_ptr, 0);
}
EXPORT_SYMBOL_GPL(asahi_sysfs_unregister);

MODULE_AUTHOR("AGX driver maintainer");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Asahi AGX firmware stats sysfs shim");
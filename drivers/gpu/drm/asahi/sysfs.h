/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _ASAHI_SYSFS_H
#define _ASAHI_SYSFS_H

struct device;

int asahi_sysfs_register(struct device *dev, int export_enabled);
void asahi_sysfs_unregister(struct device *dev);
void asahi_stats_set_snapshot_ptr(unsigned long long p);

#endif /* _ASAHI_SYSFS_H */

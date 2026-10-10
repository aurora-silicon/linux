/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _ASAHI_SYSFS_H
#define _ASAHI_SYSFS_H

struct device;

int asahi_neo_sysfs_register(struct device *dev, const void *snapshot,
			 int export_enabled, void **handle);
void asahi_neo_sysfs_unregister(struct device *dev, void *handle);

#endif /* _ASAHI_SYSFS_H */

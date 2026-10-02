/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _ASAHI_SYSFS_H
#define _ASAHI_SYSFS_H

struct device;

int asahi_sysfs_register(struct device *dev);
void asahi_sysfs_unregister(struct device *dev);

#endif /* _ASAHI_SYSFS_H */

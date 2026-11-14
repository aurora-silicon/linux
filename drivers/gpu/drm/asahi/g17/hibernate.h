/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ASAHI_G17_HIBERNATE_H
#define ASAHI_G17_HIBERNATE_H

#include <linux/types.h>

#include "../sysfs.h"

int asahi_hibernate_gate_init(void);
void asahi_hibernate_gate_exit(void);
void asahi_hibernate_device_bound(bool restorable);
void asahi_hibernate_device_unbound(bool restorable);

/*
 * The system sleep and hibernation notifiers of one G17 GPU. NULL with a NULL @suspend (a
 * backend that has no sleep callbacks); an ERR_PTR on failure. @hibernate may be NULL.
 */
void *asahi_pm_hooks_register(const void *snapshot, asahi_stats_suspend_fn suspend,
			      asahi_stats_hibernate_fn hibernate);
void asahi_pm_hooks_unregister(void *hooks);

#endif

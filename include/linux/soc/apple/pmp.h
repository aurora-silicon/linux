/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_H
#define _LINUX_SOC_APPLE_PMP_H
#include <linux/types.h>
struct device;
/* Optional GPU command API is unsupported by this M3/TB integration. */
int apple_pmp_link_device(struct device *consumer);
int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled);
#endif

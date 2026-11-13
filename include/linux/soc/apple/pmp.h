/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_H
#define _LINUX_SOC_APPLE_PMP_H
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>
struct device;
struct dev_pm_ops;
#if IS_REACHABLE(CONFIG_APPLE_PMP)
int apple_pmp_link_device(struct device *consumer);
int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled);
/* After the acknowledged AGX vote and successful GPU firmware startup. */
int apple_pmp_t6030_gpu_ready(void);
#else
static inline int apple_pmp_link_device(struct device *consumer)
{
	return -ENODEV;
}
static inline int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled)
{
	return -ENODEV;
}
static inline int apple_pmp_t6030_gpu_ready(void)
{
	return -ENODEV;
}
#endif
/* Registration is private to the PMP Rust driver and its C bridge. */
bool apple_pmp_keep_bound(void);
int apple_pmp_register(struct device *dev, const void *data);
void apple_pmp_unregister(const void *data);
int apple_pmp_send_power_command(const void *data, u8 command,
				u16 device_id, u32 enabled);
extern const struct dev_pm_ops apple_pmp_pm_ops;
int apple_pmp_sleep(const void *data);
int apple_pmp_wake(const void *data);

/*
 * What the sleep's last step needs to idle and stop the PMP with interrupts off: its RTKit and
 * CPU control, and where the SRAM it keeps goes (no range: sram NULL). Filled in suspend_late.
 */
struct apple_rtkit;
struct apple_pmp_commit_stop {
	struct apple_rtkit *rtk;
	void __iomem *control;
	void __iomem *sram;
	u32 *words;
	size_t count;
};

int apple_pmp_sleep_prepare(const void *data, struct apple_pmp_commit_stop *stop);
/* Rebuild the firmware session after a resume from disk (PM_POST_HIBERNATION). */
int apple_pmp_restore(const void *data);
#endif

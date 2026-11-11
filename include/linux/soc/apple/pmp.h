/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_H
#define _LINUX_SOC_APPLE_PMP_H
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>
struct device;
struct dev_pm_ops;

/*
 * T8112 SOC-device IDs of the CPU-cluster and ANE constraint targets, and the
 * highest qualified DCS and SoC-fabric levels. The host publishes each
 * target's SOC-DEV-DVFS entry at the SOC-device ID from a base of 0xa5, plus
 * the direct DCS and SoC floors in DVFS-state entries 9 and 10.
 */
#define APPLE_PMP_T8112_EACC0		3
#define APPLE_PMP_T8112_PACC0		4
#define APPLE_PMP_T8112_ANE0		6
#define APPLE_PMP_T8112_DCS_MAX		4
#define APPLE_PMP_T8112_SOC_MAX		2
#define APPLE_PMP_T8112_DVFS_STATE_READY	0x8
#define APPLE_PMP_T8112_DVFS_STATE_DCS		0x9
#define APPLE_PMP_T8112_DVFS_STATE_SOC		0xa
#define APPLE_PMP_T8112_DVFS_BASE	0xa5
#define APPLE_PMP_T8112_DVFS_ENTRY(id)	(APPLE_PMP_T8112_DVFS_BASE + (id))

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
#endif

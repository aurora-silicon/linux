/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_TELEMETRY_H
#define _LINUX_SOC_APPLE_PMP_TELEMETRY_H

#include <linux/types.h>

bool apple_pmp_telemetry_retained_board(void);

struct device;
struct device_node;
struct apple_pmp_telemetry;
struct apple_pmp_gpu;

/* Owned by the PMP parent; consumers cannot restart or map the firmware. */
struct apple_pmp_sensor_data {
	int (*read)(void *cookie, unsigned int channel, long *value);
	void *cookie;
	struct device *dev;
};

int apple_pmp_telemetry_register(struct device *dev, void __iomem *sram,
			       size_t size, void __iomem *asc);
void apple_pmp_telemetry_stop(struct device *dev);

/* The GPU consumer is resolved through its apple,pmp phandle. No consumer
 * retains a pointer into the removable thermal policy or its work items.
 */
struct apple_pmp_gpu *apple_pmp_gpu_get(struct device *dev);
void apple_pmp_gpu_put(struct apple_pmp_gpu *gpu);
unsigned int apple_pmp_gpu_power_state(struct apple_pmp_gpu *gpu);
u64 apple_pmp_gpu_epoch(const struct apple_pmp_sensor_data *data);
void apple_pmp_gpu_publish(const struct apple_pmp_sensor_data *data,
			   u64 epoch, unsigned int level, u64 sample_ns);

/*
 * Read through the bound firmware owner, never through a consumer mapping.
 * This checks provider/crash state and record consistency, not firmware
 * progress: T8132 has no qualified temperature timestamp or heartbeat yet.
 * Any context; returns -EPROBE_DEFER before registration, -ENODEV after stop,
 * or the temperature-record error. The caller must retain @np.
 */
int apple_pmp_telemetry_get_hotspot(const struct device_node *np, long *value);

#endif

/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_MCC_H
#define _LINUX_SOC_APPLE_MCC_H

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/types.h>

enum apple_mcc_stream_id {
	APPLE_MCC_STREAM_GPU_PARAMETER_BUFFER_WRITE = 14,
};

#if IS_REACHABLE(CONFIG_APPLE_MCC)
int apple_mcc_release_stream(struct device *consumer, u32 stream_id);
#else
static inline int apple_mcc_release_stream(struct device *consumer, u32 stream_id)
{
	return -ENODEV;
}
#endif

#endif

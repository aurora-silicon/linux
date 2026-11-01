/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ASAHI_COOLING_H
#define ASAHI_COOLING_H
#include <linux/types.h>
struct device;
struct asahi_gpu_cooling;

/* Caller owns three validated, host-writable WC words until unregister. */
struct asahi_gpu_cooling *asahi_gpu_cooling_register(struct device *dev,
						  u32 * const mirrors[3]);
void asahi_gpu_cooling_unregister(struct asahi_gpu_cooling *cooling);
void asahi_gpu_cooling_rearm(struct asahi_gpu_cooling *cooling);
#endif

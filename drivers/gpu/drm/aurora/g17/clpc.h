/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _AURORA_G17_CLPC_H
#define _AURORA_G17_CLPC_H
struct device;
struct apple_clpc_gpu;
int aurora_clpc_gpu_get(void *dev, struct apple_clpc_gpu **out);
void aurora_clpc_gpu_set_active(struct apple_clpc_gpu *gpu, bool active);
void aurora_clpc_gpu_put(struct apple_clpc_gpu *gpu);
#endif

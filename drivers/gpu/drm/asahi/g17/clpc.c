// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/soc/apple/clpc.h>
#include <linux/soc/apple/pmp-report.h>
#include "clpc.h"

int asahi_clpc_gpu_get(void *dev, struct apple_clpc_gpu **out)
{
	return apple_clpc_gpu_get(dev, out);
}
void asahi_clpc_gpu_set_active(struct apple_clpc_gpu *gpu, bool active)
{
	apple_clpc_gpu_set_active(gpu, active);
}
void asahi_clpc_gpu_put(struct apple_clpc_gpu *gpu)
{
	apple_clpc_gpu_put(gpu);
}

int asahi_pmp_report_wait_handed(unsigned int timeout_ms)
{
	return apple_pmp_report_wait_handed(timeout_ms);
}

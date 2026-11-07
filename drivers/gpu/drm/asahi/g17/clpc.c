// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/soc/apple/clpc.h>
#include <linux/moduleparam.h>
#include "clpc.h"

static unsigned int g17_light_effort = 0x8000;
module_param(g17_light_effort, uint, 0644);
MODULE_PARM_DESC(g17_light_effort, "G17 performance effort for quiet GPU work, of 65536 (0: always full)");

/*
 * Light effort runs the same work about twice as long, so the way back to
 * full effort has to sit at about twice the way in, or work that fits light
 * effort sticks at full. Shares in permille, job lengths in microseconds.
 */
static unsigned int g17_light_enter = 350;
module_param(g17_light_enter, uint, 0644);
static unsigned int g17_light_exit = 750;
module_param(g17_light_exit, uint, 0644);
static unsigned int g17_light_job_enter_us = 3000;
module_param(g17_light_job_enter_us, uint, 0644);
static unsigned int g17_light_job_exit_us = 7000;
module_param(g17_light_job_exit_us, uint, 0644);

unsigned int asahi_g17_light_param(unsigned int which)
{
	switch (which) {
	case 0:
		return READ_ONCE(g17_light_enter);
	case 1:
		return READ_ONCE(g17_light_exit);
	case 2:
		return READ_ONCE(g17_light_job_enter_us);
	default:
		return READ_ONCE(g17_light_job_exit_us);
	}
}

unsigned int asahi_g17_light_effort(void)
{
	return READ_ONCE(g17_light_effort);
}

int asahi_clpc_gpu_get(void *dev, struct apple_clpc_gpu **out)
{
	return apple_clpc_gpu_get(dev, out);
}
void asahi_clpc_gpu_set_active(struct apple_clpc_gpu *gpu, bool active)
{
	apple_clpc_gpu_set_active(gpu, active);
}
void asahi_clpc_gpu_set_busy(struct apple_clpc_gpu *gpu, unsigned int permille)
{
	apple_clpc_gpu_set_busy(gpu, permille);
}
void asahi_clpc_gpu_put(struct apple_clpc_gpu *gpu)
{
	apple_clpc_gpu_put(gpu);
}

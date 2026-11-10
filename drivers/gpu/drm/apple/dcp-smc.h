/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCP_SMC_H__
#define __APPLE_DCP_SMC_H__

#include <linux/kconfig.h>

struct apple_dcp;

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_SMC)
int dcp_smc_afk_probe(struct apple_dcp *dcp);
int dcp_smc_afk_publish(struct apple_dcp *dcp);
#else
static inline int dcp_smc_afk_probe(struct apple_dcp *dcp)
{
	return 0;
}

static inline int dcp_smc_afk_publish(struct apple_dcp *dcp)
{
	return 0;
}
#endif

#endif

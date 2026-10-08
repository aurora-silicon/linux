/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple SoC PMC voter interface
 * Copyright (C) The Asahi Linux Contributors
 */

#ifndef _LINUX_SOC_APPLE_PMC_H_
#define _LINUX_SOC_APPLE_PMC_H_

#include <linux/errno.h>
#include <linux/types.h>

struct device;

/*
 * A PMC perf-state floor word holds one 4-bit performance state per rail,
 * rail N in bits [4N+3:4N].
 */
#define APPLE_PMC_RAIL_SHIFT(rail)	(4 * (rail))
#define APPLE_PMC_RAIL_MASK(rail)	(0xfU << APPLE_PMC_RAIL_SHIFT(rail))

#if IS_ENABLED(CONFIG_APPLE_PMC)
unsigned int apple_pmc_rail_count(struct device *pmc);
bool apple_pmc_voter_enabled(struct device *pmc, unsigned int agent,
			     unsigned int rail);
u32 apple_pmc_floor_read(struct device *pmc, unsigned int agent);
int apple_pmc_floor_update(struct device *pmc, unsigned int agent, u32 mask,
			   u32 value);
#else
static inline unsigned int apple_pmc_rail_count(struct device *pmc)
{
	return 0;
}

static inline bool apple_pmc_voter_enabled(struct device *pmc,
					   unsigned int agent,
					   unsigned int rail)
{
	return false;
}

static inline u32 apple_pmc_floor_read(struct device *pmc, unsigned int agent)
{
	return 0;
}

static inline int apple_pmc_floor_update(struct device *pmc,
					 unsigned int agent, u32 mask,
					 u32 value)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_SOC_APPLE_PMC_H_ */

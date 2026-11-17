/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SOC_CPUFREQ_INTERNAL_H_
#define _APPLE_SOC_CPUFREQ_INTERNAL_H_

#include <linux/bits.h>
#include <linux/types.h>

#define APPLE_DVFS_INTERNAL_CMD_SET	BIT_ULL(25)
#define APPLE_DVFS_INTERNAL_CMD_PS2	GENMASK_ULL(15, 12)

static inline u64 apple_soc_cpufreq_command(u64 reg, u64 ps1_mask,
					    u32 ps1_shift, u32 pstate,
					    bool update_ps2)
{
	reg &= ~ps1_mask;
	reg |= (u64)pstate << ps1_shift;
	if (update_ps2) {
		reg &= ~APPLE_DVFS_INTERNAL_CMD_PS2;
		reg |= (u64)pstate << 12;
	}

	return reg | APPLE_DVFS_INTERNAL_CMD_SET;
}

static inline u32 apple_soc_cpufreq_apsc_row(u32 table, u32 stride,
					     u32 pstate)
{
	return table + stride * pstate;
}

#endif

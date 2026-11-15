/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple SoC CPU cluster DVFS: board thermal policy ownership
 */

#ifndef _LINUX_SOC_APPLE_CPUFREQ_THERMAL_H_
#define _LINUX_SOC_APPLE_CPUFREQ_THERMAL_H_

#include <linux/types.h>

struct cpufreq_policy;

int apple_soc_cpufreq_set_thermal_owner(struct cpufreq_policy *policy, bool owned);

#endif /* _LINUX_SOC_APPLE_CPUFREQ_THERMAL_H_ */

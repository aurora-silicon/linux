/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple SoC CPU idle: cluster performance-state parking
 *
 * The cluster DVFS controller keeps the performance state it was last asked
 * for while every CPU of the cluster is idle, and the cluster's voltage
 * follows it. schedutil does not lower the request of an idle cluster: it
 * only runs from scheduler events on the cluster's CPUs. A short burst of a
 * real-time task, which runs with uclamp_min 1024 by default, therefore
 * leaves an idle cluster at its fastest state until the next wakeup.
 *
 * The idle states that may park call @enter before the CPU idles and @exit
 * when it wakes, with interrupts disabled and RCU watching. The cpufreq
 * driver parks a cluster at its slowest state once all of its online CPUs
 * have entered such a state and restores the last request when the first
 * one leaves it.
 */
#ifndef __LINUX_SOC_APPLE_CPUIDLE_H
#define __LINUX_SOC_APPLE_CPUIDLE_H

#include <linux/errno.h>

struct apple_cpuidle_park_ops {
	void (*enter)(unsigned int cpu);
	void (*exit)(unsigned int cpu);
};

#if IS_ENABLED(CONFIG_ARM_APPLE_CPUIDLE)
int apple_cpuidle_set_park_ops(const struct apple_cpuidle_park_ops *ops);
void apple_cpuidle_clear_park_ops(const struct apple_cpuidle_park_ops *ops);
#else
static inline int apple_cpuidle_set_park_ops(const struct apple_cpuidle_park_ops *ops)
{
	return -ENODEV;
}

static inline void apple_cpuidle_clear_park_ops(const struct apple_cpuidle_park_ops *ops)
{
}
#endif

#endif /* __LINUX_SOC_APPLE_CPUIDLE_H */

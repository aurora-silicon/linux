/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_PMP_CPUFREQ_H
#define APPLE_PMP_CPUFREQ_H

struct cpufreq_policy;
/* Nominal completed-state readback, not measured core-clock telemetry. */
int apple_pmp_cpufreq_readback(unsigned int cpu);
int apple_pmp_thermal_release_bootcap(struct cpufreq_policy *policy);

#endif

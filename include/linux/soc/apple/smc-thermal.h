/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple J700 CPU thermal policy: cpufreq and telemetry provider interface
 *
 * Copyright (C) 2026 Eryk Wieliczko
 */
#ifndef _APPLE_SMC_THERMAL_H
#define _APPLE_SMC_THERMAL_H

#include <linux/err.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct cpufreq_policy;
struct device;
struct apple_smc_thermal_cpu;

/* Maximum host transaction/cache age, NOT a firmware sensor-age guarantee. */
#define APPLE_SMC_THERMAL_MAX_AGE_MS 200

#if IS_REACHABLE(CONFIG_APPLE_SMC_CPU_THERMAL)
struct apple_smc_thermal_cpu *apple_smc_thermal_cpu_add(struct cpufreq_policy *policy);
void apple_smc_thermal_cpu_ready(struct apple_smc_thermal_cpu *cpu);
void apple_smc_thermal_cpu_remove(struct apple_smc_thermal_cpu *cpu);
void apple_smc_thermal_cpu_fault(struct apple_smc_thermal_cpu *cpu);
int devm_apple_smc_thermal_register(struct device *dev, int (*read)(void *, int *), void *ctx);
#else
static inline struct apple_smc_thermal_cpu *
apple_smc_thermal_cpu_add(struct cpufreq_policy *policy) { return ERR_PTR(-ENODEV); }
static inline void apple_smc_thermal_cpu_ready(struct apple_smc_thermal_cpu *cpu) {}
static inline void apple_smc_thermal_cpu_remove(struct apple_smc_thermal_cpu *cpu) {}
static inline void apple_smc_thermal_cpu_fault(struct apple_smc_thermal_cpu *cpu) {}
static inline int devm_apple_smc_thermal_register(struct device *dev,
						  int (*read)(void *, int *), void *ctx)
{ return -ENODEV; }
#endif
#endif

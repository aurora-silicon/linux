/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_SMC_THERMAL_H
#define _APPLE_SMC_THERMAL_H

#include <linux/err.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct cpufreq_policy;
struct device;
struct apple_smc_thermal_cpu;
struct thermal_zone_device_ops;
struct thermal_trip;
struct thermal_cooling_device;

/* Maximum host transaction/cache age, NOT a firmware sensor-age guarantee. */
#define APPLE_SMC_THERMAL_MAX_AGE_MS 200

#if IS_REACHABLE(CONFIG_APPLE_SMC_CPU_THERMAL)
struct apple_smc_thermal_cpu *apple_smc_thermal_cpu_add(struct cpufreq_policy *policy);
void apple_smc_thermal_cpu_ready(struct apple_smc_thermal_cpu *cpu);
void apple_smc_thermal_cpu_remove(struct apple_smc_thermal_cpu *cpu);
void apple_smc_thermal_cpu_fault(struct apple_smc_thermal_cpu *cpu);
int devm_apple_smc_thermal_register(struct device *dev, int (*read)(void *, int *), void *ctx);
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
/* Host-side freshness/health of the SMC provider, not sensor publication age. */
bool apple_smc_thermal_ready(void);
bool apple_smc_thermal_live(void);
bool apple_smc_thermal_healthy(void);
/* SMC retains cooling-device/auxiliary-zone ownership. No borrowed pointers.
 * Updates and teardown serialize under its provider lock; fast guards must
 * not wait on this potentially slow interface.
 */
int apple_smc_thermal_aux_register(const struct thermal_zone_device_ops *ops,
				 const struct thermal_trip *trips, int count);
void apple_smc_thermal_aux_update(void);
void apple_smc_thermal_aux_unregister(void);
int apple_smc_thermal_aux_set_policy(int temperature, int hysteresis,
				     int backup_trip, int backup_release);
bool apple_smc_thermal_aux_matches(struct thermal_cooling_device *cdev);
int apple_smc_thermal_aux_cooling_floor(struct thermal_cooling_device *cdev,
				      unsigned int max_khz, unsigned long *state);
bool apple_smc_thermal_aux_cooling_ready(void);
#endif
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

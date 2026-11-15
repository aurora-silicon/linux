/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_EMERGENCY_POLICY_H
#define _LINUX_SOC_APPLE_PMP_EMERGENCY_POLICY_H

/*
 * Historical J713 development shutdown policy. This is NOT a temperature
 * limit obtained from Apple firmware, ADT or qualified hardware evidence.
 * Keep it separate from ordinary cooling tuning and retain its existing
 * restriction until a T8132 emergency contract is qualified; removing it
 * would silently weaken the currently deployed backstop. A working desktop
 * or an observed macOS temperature does not qualify an emergency threshold.
 */
#define APPLE_PMP_CRITICAL_MC 120000

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_APPLE_AURA_HIBERNATE_H
#define __LINUX_SOC_APPLE_AURA_HIBERNATE_H

#include <linux/types.h>

#ifdef CONFIG_APPLE_AURA_HIBERNATE
bool aura_hibernate_layout_fixed(void);
bool aura_hibernate_pfn_nosave(unsigned long pfn);
#else
static inline bool aura_hibernate_layout_fixed(void) { return false; }
static inline bool aura_hibernate_pfn_nosave(unsigned long pfn) { return false; }
#endif

#endif

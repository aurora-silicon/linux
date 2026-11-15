/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __LINUX_SOC_APPLE_PMGR_MISC_H
#define __LINUX_SOC_APPLE_PMGR_MISC_H

#if IS_ENABLED(CONFIG_APPLE_PMGR_MISC)
int apple_pmgr_fabric_hold(void);
void apple_pmgr_fabric_release(void);
#else
static inline int apple_pmgr_fabric_hold(void) { return 0; }
static inline void apple_pmgr_fabric_release(void) { }
#endif

#endif

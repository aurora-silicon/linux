/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple SoC PMP report region
 * Copyright (C) The Asahi Linux Contributors
 */

#ifndef _LINUX_SOC_APPLE_PMP_REPORT_H_
#define _LINUX_SOC_APPLE_PMP_REPORT_H_

#include <linux/errno.h>
#include <linux/types.h>

struct device_node;
struct device;

#if IS_ENABLED(CONFIG_APPLE_PMP_REPORT)
int apple_pmp_report_ane_available(const struct device_node *np);
int apple_pmp_report_ane_perf(const struct device_node *np,
			      struct device *owner, unsigned int level, bool *held);
int apple_pmp_report_wait_supplier_ready(struct device *supplier, unsigned int timeout_ms);
/* Caller holds the PMP owner's vote lock and keeps the supplier bound. */
int apple_pmp_report_check_t6030_gfx(struct device *supplier);
int apple_pmp_report_set_t6030_gfx(struct device *supplier, bool enable);
int apple_pmp_report_wait_ready(struct device_node *entry, unsigned long timeout);
int apple_pmp_report_fast_die_effort(const struct device_node *np,
				     unsigned int lane, u8 *effort);
void apple_pmp_report_sleep(struct device *pmp, bool at_commit);
void apple_pmp_report_wake(struct device *pmp, bool lost_power);
int apple_pmp_report_entry_request(const struct device_node *entry, bool enable);
int apple_pmp_report_entry_commit_owned(const struct device_node *entry, bool owned);
int apple_pmp_report_entry_withdraw_polled(const struct device_node *entry);
#else
static inline int apple_pmp_report_entry_request(const struct device_node *entry,
						 bool enable)
{
	return 0;
}

static inline int apple_pmp_report_entry_commit_owned(const struct device_node *entry,
						      bool owned)
{
	return -ENODEV;
}

static inline int apple_pmp_report_entry_withdraw_polled(const struct device_node *entry)
{
	return 0;
}

static inline void apple_pmp_report_sleep(struct device *pmp, bool at_commit)
{
}

static inline void apple_pmp_report_wake(struct device *pmp, bool lost_power)
{
}

static inline int apple_pmp_report_ane_available(const struct device_node *np)
{
	return -EOPNOTSUPP;
}

static inline int apple_pmp_report_ane_perf(const struct device_node *np,
					    struct device *owner, unsigned int level, bool *held)
{
	*held = false;
	return -EOPNOTSUPP;
}

static inline int apple_pmp_report_wait_supplier_ready(struct device *supplier,
						unsigned int timeout_ms)
{
	return -ENODEV;
}

static inline int apple_pmp_report_check_t6030_gfx(struct device *supplier)
{
	return -ENODEV;
}

static inline int apple_pmp_report_set_t6030_gfx(struct device *supplier, bool enable)
{
	return -ENODEV;
}

static inline int apple_pmp_report_wait_ready(struct device_node *entry,
					      unsigned long timeout)
{
	return -ENODEV;
}

static inline int apple_pmp_report_fast_die_effort(const struct device_node *np,
						   unsigned int lane, u8 *effort)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_SOC_APPLE_PMP_REPORT_H_ */

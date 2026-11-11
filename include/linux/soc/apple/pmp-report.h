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

enum apple_pmp_t8112_constraint_source {
	APPLE_PMP_T8112_CONSTRAINT_CPU,
	APPLE_PMP_T8112_CONSTRAINT_ANE,
	APPLE_PMP_T8112_CONSTRAINT_SOURCE_COUNT,
};

#if IS_ENABLED(CONFIG_APPLE_PMP_REPORT)
int apple_pmp_report_ane_available(const struct device_node *np);
int apple_pmp_report_ane_perf(const struct device_node *np,
			      struct device *owner, unsigned int level, bool *held);
int apple_pmp_report_wait_supplier_ready(struct device *supplier, unsigned int timeout_ms);
int apple_pmp_report_wait_running(struct device_node *report_np, unsigned int timeout_ms);
/* Caller holds the PMP owner's vote lock and keeps the supplier bound. */
int apple_pmp_report_check_t6030_gfx(struct device *supplier);
int apple_pmp_report_set_t6030_gfx(struct device *supplier, bool enable);
int apple_pmp_report_wait_ready(struct device_node *entry, unsigned long timeout);
int apple_pmp_report_fast_die_effort(const struct device_node *np,
				     unsigned int lane, u8 *effort);
/*
 * Set or clear one bit of the PMP power request word of the report bound to @report_np, under the
 * report's lock, with the acknowledge wait (see pmp-report.c).
 */
int apple_pmp_report_set_request(const struct device_node *report_np, unsigned int id,
				 bool enable, bool no_ack);
int apple_pmp_t8112_set_constraint(const struct device_node *report_np,
				   unsigned int entry,
				   enum apple_pmp_t8112_constraint_source source,
				   unsigned int dcs, unsigned int soc);
/*
 * T6001: the GPU driver reports each operation in flight. With the pmp_report.agx_vote parameter set, the AGX request (PS-REQ bit 9) is
 * set while any operation runs and cleared agx_vote_idle_ms after the last one ends. Parameter off: no effect. Both may be called from
 * any context. release() clears the request at once (GPU driver unbind), process context.
 */
void apple_pmp_agx_activity_begin(void);
void apple_pmp_agx_activity_end(void);
void apple_pmp_agx_activity_release(void);
void apple_pmp_agx_activity_resume(void);
#else
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

static inline void apple_pmp_agx_activity_begin(void)
{
}

static inline void apple_pmp_agx_activity_end(void)
{
}

static inline void apple_pmp_agx_activity_resume(void)
{
}

static inline void apple_pmp_agx_activity_release(void)
{
}

static inline int apple_pmp_report_wait_supplier_ready(struct device *supplier,
						unsigned int timeout_ms)
{
	return -ENODEV;
}

static inline int apple_pmp_report_wait_running(struct device_node *report_np,
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

static inline int apple_pmp_report_set_request(const struct device_node *report_np,
					       unsigned int id, bool enable, bool no_ack)
{
	return -ENODEV;
}

static inline int apple_pmp_t8112_set_constraint(const struct device_node *report_np,
						 unsigned int entry,
						 enum apple_pmp_t8112_constraint_source source,
						 unsigned int dcs, unsigned int soc)
{
	return -ENODEV;
}

#endif

#endif /* _LINUX_SOC_APPLE_PMP_REPORT_H_ */

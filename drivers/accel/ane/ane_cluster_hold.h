/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_CLUSTER_HOLD_H__
#define __ANE_CLUSTER_HOLD_H__

#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct device;

/*
 * The CPU cluster an ANE node's performance-domains names, held up while
 * requests run: see ane_cluster_hold.c. Embedded in a backend; all zero is
 * a valid, inactive hold until ane_cluster_hold_init().
 */
struct ane_cluster_hold {
	struct device *dev;
	/* Attach and the applied state: the work items and fini. */
	struct mutex lock;
	/* want, busy and last_end_ns, also from the request path. */
	spinlock_t state_lock;
	struct work_struct apply;
	struct work_struct kick;
	struct delayed_work release;
	bool ready;
	bool want, busy;
	u64 last_end_ns;
	/* The cluster: absent when the node names none or it has no CPUs. */
	bool absent;
	bool waiting;
	cpumask_var_t cpus;
	struct cpufreq_policy *policy;
	struct freq_qos_request freq;
	struct dev_pm_qos_request *lat;
	bool held;
};

int ane_cluster_hold_init(struct ane_cluster_hold *h, struct device *dev);
void ane_cluster_hold_busy(struct ane_cluster_hold *h, bool busy);
void ane_cluster_hold_drop(struct ane_cluster_hold *h);
void ane_cluster_hold_fini(struct ane_cluster_hold *h);

#endif

// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Engine-busy CPU cluster boost.
 *
 * Bandwidth-bound programs depend on CPU cluster frequency. On our
 * T8103 hardware, holding the highest cluster frequency reduced slow
 * submissions and keeping the boost until completion reduced latency.
 *
 * Hold a minimum-frequency QoS at the top of every cpufreq policy
 * while a submission runs, then release it boost_idle_ms after the last
 * completion. Use cpufreq constraints so the request composes with the
 * governor without direct DVFS register access.
 */
#include <linux/cpufreq.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "ane.h"

static unsigned int boost_idle_ms = 100;
module_param(boost_idle_ms, uint, 0644);
MODULE_PARM_DESC(boost_idle_ms,
		 "hold every CPU cluster at its top p-state while a submit runs and this long after the last one completes (0 = off)");

/* Lock held. */
static void ane_boost_set(struct ane_device *ane, bool on)
{
	struct ane_boost *b = &ane->boost;
	int cpu, n = 0;

	if (on) {
		for_each_possible_cpu(cpu) {
			struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
			int err;

			if (!policy)
				continue;
			if (cpu != cpumask_first(policy->related_cpus) ||
			    n >= b->nlegs) {
				cpufreq_cpu_put(policy);
				continue;
			}
			err = freq_qos_add_request(&policy->constraints,
						   &b->legs[n], FREQ_QOS_MIN,
						   policy->cpuinfo.max_freq);
			cpufreq_cpu_put(policy);
			if (err >= 0)
				n++;
		}
		b->held = n;
	} else {
		for (n = 0; n < b->held; n++)
			freq_qos_remove_request(&b->legs[n]);
		b->held = 0;
	}
	b->on = on;
}

static void ane_boost_off_work(struct work_struct *work)
{
	struct ane_device *ane =
		container_of(work, struct ane_device, boost.off.work);
	struct ane_boost *b = &ane->boost;

	mutex_lock(&b->lock);
	/* A submit that began or ended after this expiry was armed keeps
	 * the boost on; its own completion re-arms the drop.
	 */
	if (b->on && !b->busy &&
	    time_after_eq(jiffies, b->last_end + msecs_to_jiffies(boost_idle_ms)))
		ane_boost_set(ane, false);
	mutex_unlock(&b->lock);
}

/* Engine lock held: submits never overlap, so busy is a flag. */
void ane_boost_begin(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	if (!b->legs || !boost_idle_ms)
		return;
	mutex_lock(&b->lock);
	b->busy = true;
	if (!b->on)
		ane_boost_set(ane, true);
	mutex_unlock(&b->lock);
}

void ane_boost_end(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	if (!b->legs)
		return;
	mutex_lock(&b->lock);
	b->busy = false;
	b->last_end = jiffies;
	if (b->on)
		mod_delayed_work(system_wq, &b->off,
				 msecs_to_jiffies(boost_idle_ms));
	mutex_unlock(&b->lock);
}

int ane_boost_init(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	mutex_init(&b->lock);
	INIT_DELAYED_WORK(&b->off, ane_boost_off_work);
	b->nlegs = num_possible_cpus();
	b->legs = kcalloc(b->nlegs, sizeof(*b->legs), GFP_KERNEL);
	return b->legs ? 0 : -ENOMEM;
}

void ane_boost_exit(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	if (!b->legs)
		return;
	cancel_delayed_work_sync(&b->off);
	mutex_lock(&b->lock);
	if (b->on)
		ane_boost_set(ane, false);
	mutex_unlock(&b->lock);
	kfree(b->legs);
	b->legs = NULL;
}

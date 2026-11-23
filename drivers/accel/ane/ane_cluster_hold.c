// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Hold up the CPU cluster that an ANE node's performance-domains names while
 * requests run.
 *
 * From the first EXECUTE until ANE_CLUSTER_HOLD_MS after the last one ends,
 * the cluster's cpufreq policy gets a minimum-frequency request at its
 * maximum, and each of its CPUs a resume latency of 0, which keeps cpuidle
 * in a shallow state instead of powering the CPUs down. A maximum on the
 * policy, such as a thermal limit, still wins over the minimum.
 *
 * Nodes without performance-domains, or naming a domain with no CPUs, hold
 * nothing. Requests are taken and dropped from work items, never in the
 * request path. The cluster's cpufreq policy may appear after the ANE
 * probes: the hold attaches at the first request that finds it.
 */
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include "ane_cluster_hold.h"

#define ANE_CLUSTER_HOLD_MS 100

/* Queued on a cluster CPU: its wake-up gives the governor a utilization update. */
static void ane_cluster_hold_kick(struct work_struct *work)
{
}

/* Run on the cluster's idle CPUs: they choose their idle state again. */
static void ane_cluster_hold_nop(void *unused)
{
}

/* Under h->lock: find the cluster's CPUs and policy and add inactive requests. */
static int ane_cluster_hold_attach(struct ane_cluster_hold *h)
{
	struct cpufreq_policy *policy = NULL;
	struct of_phandle_args args;
	unsigned int cpu;
	int ret;

	if (h->policy)
		return 0;
	if (h->absent)
		return -ENODEV;
	ret = of_parse_phandle_with_args(h->dev->of_node, "performance-domains",
					 "#performance-domain-cells", 0, &args);
	if (ret) {
		WRITE_ONCE(h->absent, true);
		return -ENODEV;
	}
	cpumask_clear(h->cpus);
	for_each_possible_cpu(cpu) {
		struct device_node *np = of_cpu_device_node_get(cpu);
		struct of_phandle_args cpu_args;

		if (!np)
			continue;
		if (!of_parse_phandle_with_args(np, "performance-domains",
						"#performance-domain-cells", 0, &cpu_args)) {
			if (cpu_args.np == args.np &&
			    cpu_args.args_count == args.args_count &&
			    (!args.args_count ||
			     !memcmp(cpu_args.args, args.args,
				     args.args_count * sizeof(args.args[0]))))
				cpumask_set_cpu(cpu, h->cpus);
			of_node_put(cpu_args.np);
		}
		of_node_put(np);
	}
	of_node_put(args.np);
	if (cpumask_empty(h->cpus)) {
		WRITE_ONCE(h->absent, true);
		dev_info(h->dev, "no CPUs in the performance domain; no CPU cluster held\n");
		return -ENODEV;
	}
	for_each_cpu(cpu, h->cpus) {
		policy = cpufreq_cpu_get(cpu);
		if (policy)
			break;
	}
	if (!policy) {
		if (!h->waiting)
			dev_info(h->dev, "no cpufreq policy for CPUs %*pbl yet; held once there is\n",
				 cpumask_pr_args(h->cpus));
		h->waiting = true;
		return -EAGAIN;
	}
	h->lat = kcalloc(nr_cpu_ids, sizeof(*h->lat), GFP_KERNEL);
	if (!h->lat) {
		cpufreq_cpu_put(policy);
		return -ENOMEM;
	}
	ret = freq_qos_add_request(&policy->constraints, &h->freq, FREQ_QOS_MIN,
				   FREQ_QOS_MIN_DEFAULT_VALUE);
	if (ret < 0) {
		kfree(h->lat);
		h->lat = NULL;
		cpufreq_cpu_put(policy);
		WRITE_ONCE(h->absent, true);
		dev_info(h->dev, "no frequency request on cpufreq policy%u (%d); no CPU cluster held\n",
			 policy->cpu, ret);
		return ret;
	}
	for_each_cpu(cpu, h->cpus) {
		struct device *cpu_dev = get_cpu_device(cpu);

		/* Without its request, a CPU is only not kept out of deep idle. */
		if (cpu_dev)
			dev_pm_qos_add_request(cpu_dev, &h->lat[cpu], DEV_PM_QOS_RESUME_LATENCY,
					       PM_QOS_RESUME_LATENCY_NO_CONSTRAINT);
	}
	h->policy = policy;
	dev_info(h->dev, "CPUs %*pbl (cpufreq policy%u) held up while requests run\n",
		 cpumask_pr_args(h->cpus), policy->cpu);
	return 0;
}

/* Take or drop both requests to match want. */
static void ane_cluster_hold_apply(struct work_struct *work)
{
	struct ane_cluster_hold *h = container_of(work, struct ane_cluster_hold, apply);
	unsigned long flags;
	unsigned int cpu;
	bool want;
	s32 latency;

	guard(mutex)(&h->lock);
	spin_lock_irqsave(&h->state_lock, flags);
	want = h->want;
	spin_unlock_irqrestore(&h->state_lock, flags);
	if (want && ane_cluster_hold_attach(h))
		return;
	if (!h->policy || want == h->held)
		return;

	if (freq_qos_update_request(&h->freq, want ? h->policy->cpuinfo.max_freq :
				    FREQ_QOS_MIN_DEFAULT_VALUE) > 0 && want) {
		/*
		 * A fast-switching governor applies a new limit at the cluster's
		 * next utilization update, which an idle cluster may not make
		 * for a long time: refresh the limits and wake a cluster CPU.
		 */
		cpufreq_update_limits(h->policy->cpu);
		cpu = cpumask_first_and(h->cpus, cpu_online_mask);
		if (cpu < nr_cpu_ids)
			queue_work_on(cpu, system_highpri_wq, &h->kick);
	}
	latency = want ? 0 : PM_QOS_RESUME_LATENCY_NO_CONSTRAINT;
	for_each_cpu(cpu, h->cpus)
		if (dev_pm_qos_request_active(&h->lat[cpu]))
			dev_pm_qos_update_request(&h->lat[cpu], latency);
	/* An idle CPU keeps the state it chose until it wakes. */
	on_each_cpu_mask(h->cpus, ane_cluster_hold_nop, NULL, false);
	WRITE_ONCE(h->held, want);
}

/* ANE_CLUSTER_HOLD_MS after the last EXECUTE ended: drop the hold. */
static void ane_cluster_hold_release(struct work_struct *work)
{
	struct ane_cluster_hold *h = container_of(to_delayed_work(work), struct ane_cluster_hold,
						  release);
	u64 hold = (u64)ANE_CLUSTER_HOLD_MS * NSEC_PER_MSEC, idle;
	unsigned long flags, delay = 0;
	bool drop = false;

	spin_lock_irqsave(&h->state_lock, flags);
	idle = ktime_get_ns() - h->last_end_ns;
	if (h->busy)
		delay = msecs_to_jiffies(ANE_CLUSTER_HOLD_MS) + 1;
	else if (idle < hold)
		delay = nsecs_to_jiffies(hold - idle) + 1;
	else if (h->want)
		drop = true;
	if (drop)
		h->want = false;
	spin_unlock_irqrestore(&h->state_lock, flags);
	if (delay)
		queue_delayed_work(system_power_efficient_wq, &h->release, delay);
	else if (drop)
		queue_work(system_highpri_wq, &h->apply);
}

/**
 * ane_cluster_hold_init - prepare the hold for the ANE device @dev
 * @h: the hold, zeroed
 * @dev: the ANE device, whose node's performance-domains names the cluster
 *
 * Return: 0, or -ENOMEM.
 */
int ane_cluster_hold_init(struct ane_cluster_hold *h, struct device *dev)
{
	if (h->ready)
		return 0;
	if (!zalloc_cpumask_var(&h->cpus, GFP_KERNEL))
		return -ENOMEM;
	h->dev = dev;
	mutex_init(&h->lock);
	spin_lock_init(&h->state_lock);
	INIT_WORK(&h->apply, ane_cluster_hold_apply);
	INIT_WORK(&h->kick, ane_cluster_hold_kick);
	INIT_DELAYED_WORK(&h->release, ane_cluster_hold_release);
	h->ready = true;
	return 0;
}

/**
 * ane_cluster_hold_busy - an EXECUTE starts or ends
 * @h: the hold; does nothing before ane_cluster_hold_init() or without a cluster
 * @busy: true before the EXECUTE, false after it
 */
void ane_cluster_hold_busy(struct ane_cluster_hold *h, bool busy)
{
	unsigned long flags;
	bool apply = false;

	if (!h->ready || READ_ONCE(h->absent))
		return;
	spin_lock_irqsave(&h->state_lock, flags);
	h->busy = busy;
	if (busy) {
		/* A new hold, or one whose cluster had no cpufreq policy yet. */
		apply = !h->want || !READ_ONCE(h->held);
		h->want = true;
	} else {
		h->last_end_ns = ktime_get_ns();
	}
	spin_unlock_irqrestore(&h->state_lock, flags);
	if (apply)
		queue_work(system_highpri_wq, &h->apply);
	if (!busy)
		queue_delayed_work(system_power_efficient_wq, &h->release,
				   msecs_to_jiffies(ANE_CLUSTER_HOLD_MS));
}

/**
 * ane_cluster_hold_drop - drop the hold now, with requests stopped
 * @h: the hold
 *
 * Before system sleep: the CPUs go back to their normal idle states.
 */
void ane_cluster_hold_drop(struct ane_cluster_hold *h)
{
	unsigned long flags;

	if (!h->ready)
		return;
	cancel_delayed_work_sync(&h->release);
	spin_lock_irqsave(&h->state_lock, flags);
	h->want = false;
	spin_unlock_irqrestore(&h->state_lock, flags);
	queue_work(system_highpri_wq, &h->apply);
	flush_work(&h->apply);
	cancel_work_sync(&h->kick);
}

/**
 * ane_cluster_hold_fini - remove the requests, with requests stopped
 * @h: the hold
 */
void ane_cluster_hold_fini(struct ane_cluster_hold *h)
{
	unsigned int cpu;

	if (!h->ready)
		return;
	cancel_delayed_work_sync(&h->release);
	cancel_work_sync(&h->apply);
	cancel_work_sync(&h->kick);
	mutex_lock(&h->lock);
	if (h->policy) {
		for_each_cpu(cpu, h->cpus)
			if (dev_pm_qos_request_active(&h->lat[cpu]))
				dev_pm_qos_remove_request(&h->lat[cpu]);
		freq_qos_remove_request(&h->freq);
		cpufreq_cpu_put(h->policy);
		h->policy = NULL;
	}
	kfree(h->lat);
	h->lat = NULL;
	mutex_unlock(&h->lock);
	free_cpumask_var(h->cpus);
	mutex_destroy(&h->lock);
	h->ready = false;
}

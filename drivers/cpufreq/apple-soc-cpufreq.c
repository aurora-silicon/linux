// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple SoC CPU cluster performance state driver
 *
 * Copyright The Asahi Linux Contributors
 *
 * Based on scpi-cpufreq.c
 */

#include <linux/cpuhotplug.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/pm_qos.h>
#include <linux/soc/apple/pmp-report.h>
#include <linux/spinlock.h>
#include <linux/tick.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

#define APPLE_DVFS_CMD				0x20
#define APPLE_DVFS_CMD_BUSY			BIT(31)
#define APPLE_DVFS_CMD_SET			BIT(25)
#define APPLE_DVFS_CMD_PS1_S5L8960X		GENMASK(24, 22)
#define APPLE_DVFS_CMD_PS1_S5L8960X_SHIFT	22
#define APPLE_DVFS_CMD_PS2			GENMASK(15, 12)
#define APPLE_DVFS_CMD_PS1			GENMASK(4, 0)
#define APPLE_DVFS_CMD_PS1_SHIFT		0

/* Same timebase as CPU counter (24MHz) */
#define APPLE_DVFS_LAST_CHG_TIME	0x38

/*
 * Apple ran out of bits and had to shift this in T8112...
 */
#define APPLE_DVFS_STATUS			0x50
#define APPLE_DVFS_STATUS_CUR_PS_S5L8960X	GENMASK(5, 3)
#define APPLE_DVFS_STATUS_CUR_PS_SHIFT_S5L8960X	3
#define APPLE_DVFS_STATUS_TGT_PS_S5L8960X	GENMASK(2, 0)
#define APPLE_DVFS_STATUS_CUR_PS_T8103		GENMASK(7, 4)
#define APPLE_DVFS_STATUS_CUR_PS_SHIFT_T8103	4
#define APPLE_DVFS_STATUS_TGT_PS_T8103		GENMASK(3, 0)
#define APPLE_DVFS_STATUS_CUR_PS_T8112		GENMASK(9, 5)
#define APPLE_DVFS_STATUS_CUR_PS_SHIFT_T8112	5
#define APPLE_DVFS_STATUS_TGT_PS_T8112		GENMASK(4, 0)

/*
 * Div is +1, base clock is 12MHz on existing SoCs.
 * For documentation purposes. We use the OPP table to
 * get the frequency.
 */
#define APPLE_DVFS_PLL_STATUS		0xc0
#define APPLE_DVFS_PLL_FACTOR		0xc8
#define APPLE_DVFS_PLL_FACTOR_MULT	GENMASK(31, 16)
#define APPLE_DVFS_PLL_FACTOR_DIV	GENMASK(15, 0)

#define APPLE_DVFS_TRANSITION_TIMEOUT 400

/*
 * Fast die-temperature limit
 *
 * The power management processor (PMP) of some SoCs runs a control loop on
 * its own die-temperature sensors and publishes, for each CPU cluster, an
 * 8-bit effort: 0 requests no limit, and the effort rises towards 255 as the
 * die exceeds the firmware's target.  The firmware refreshes the effort every
 * few milliseconds, far faster than the SMC temperature sensors, so a cluster
 * can run at full speed until the die hot spot reaches the firmware's target.
 *
 * The firmware requests the limit and the OS enforces it, as scmi-cpufreq
 * does with SCMI performance limits: the limit is a FREQ_QOS_MAX request on
 * the policy, so the cpufreq core applies it to the governor and reports it
 * to the scheduler as cpufreq pressure.  apple_fast_die_limit() maps an
 * effort to that limit.
 *
 * The limit fails safe.  The effort is only valid while the PMP reports that
 * it is running, and the PMP may start late or never, for instance when its
 * driver is not loaded or the bootloader did not prepare it.  Until it
 * reports that it is running, the cluster is held at a fallback limit: the
 * fastest state whose power is at most a third of the fastest state's.  If
 * it has not reported running 30 s after the limit was set up, a warning
 * says that the fast limit is unavailable; the fallback limit stays until
 * the PMP runs.
 *
 * The effort has to be sampled, since no interrupt is known to signal a new
 * one under Linux.  While the limit is at the cluster's maximum, every CPU
 * of the cluster has a pinned, deferrable timer that samples the effort once
 * a tick, and the first CPU to run in a tick takes the sample.  Deferrable
 * timers do not wake idle CPUs, so only busy CPUs sample, and an idle
 * cluster, which produces no heat, costs nothing.  Once the limit is below
 * the maximum, a per-cluster hrtimer samples every 5 ms whether the cluster
 * is busy or not, until the limit is back at the maximum: an idle cluster
 * would otherwise keep a stale limit, which the scheduler sees as reduced
 * capacity and so keeps work away from the cluster.  While the PMP is not
 * running, the hrtimer only checks every 100 ms, and every second once the
 * warning has been given.  A nohz_full CPU can run a task with its tick
 * stopped, and so without sampling, so a cluster with nohz_full CPUs
 * samples from the hrtimer all the time.
 *
 * A sample reads the effort through the PMP report driver; only when the
 * limit it maps to changes is a work item queued to update the QoS request,
 * which must be done in process context.
 */
#define APPLE_FAST_DIE_EFFORT_MAX	255
#define APPLE_FAST_DIE_POLL_MS		5
#define APPLE_FAST_DIE_WAIT_MS		100
#define APPLE_FAST_DIE_GAVE_UP_MS	1000
#define APPLE_FAST_DIE_WARN_S		30

/**
 * struct apple_fast_die - fast die-temperature limit of one cluster
 * @policy: policy of the cluster
 * @report: PMP report region, or NULL if the limit is misdescribed
 * @lane: lane of the cluster in the fast die-temperature report
 * @lock: serialises samples and protects the fields below it
 * @limit: last limit derived from a sample, in kHz; 0 forces an update
 * @polling: @poll is armed
 * @waiting: the last sample found no running PMP
 * @warned: the fast limit has been reported unavailable
 * @per_cpu: the per-CPU deferrable timers sample while the limit is at @max
 * @stopping: ->exit() is tearing the limit down; do not arm @poll again
 * @max: frequency of the fastest state, in kHz
 * @fallback: limit while the PMP is not running, in kHz
 * @start: jiffies when the limit was set up
 * @last_poll: jiffies of the last sample by a per-CPU timer
 * @poll: non-deferrable sampling timer, armed while the limit is below @max
 * @work: updates @req to @limit
 * @req: FREQ_QOS_MAX request that enforces @limit
 * @cpus: every CPU of the cluster, online or not
 * @nr_states: number of frequency-table entries
 * @power: power of each frequency-table entry, in microwatts from the OPP
 *	table, or the frequency when the OPPs do not describe their power
 */
struct apple_fast_die {
	struct cpufreq_policy *policy;
	struct device_node *report;
	unsigned int lane;
	spinlock_t lock;	/* serialises samples; protects the fields below */
	unsigned int limit;
	bool polling;
	bool waiting;
	bool warned;
	bool per_cpu;
	bool stopping;
	unsigned int max;
	unsigned int fallback;
	unsigned long start;
	unsigned long last_poll;
	struct hrtimer poll;
	struct work_struct work;
	struct freq_qos_request req;
	cpumask_var_t cpus;
	unsigned int nr_states;
	unsigned long power[] __counted_by(nr_states);
};

/**
 * struct apple_fast_die_timer - per-CPU sampling timer
 * @timer: pinned, deferrable timer that samples the effort
 * @fd: limit of the cluster of this CPU, NULL when it has none; written with
 *	the CPU hotplug lock held, like the hotplug callbacks that read it
 */
struct apple_fast_die_timer {
	struct timer_list timer;
	struct apple_fast_die *fd;
};

static DEFINE_PER_CPU(struct apple_fast_die_timer, apple_fast_die_timers);
/* Dynamic CPU hotplug state of the per-CPU timers, 0 if there is none. */
static int apple_fast_die_hp_state;

struct apple_soc_cpufreq_info {
	bool has_ps2;
	bool verify_transition;
	bool needs_thermal_policy;
	bool needs_firmware_thermal_control;
	u32 transition_timeout_us;
	u32 max_unmanaged_pstate;
	u64 min_pstate;
	u64 max_pstate;
	u64 cur_pstate_mask;
	u64 cur_pstate_shift;
	u64 ps1_mask;
	u64 ps1_shift;
};

struct apple_cpu_priv {
	struct apple_fast_die *fast_die;
	struct device *cpu_dev;
	void __iomem *reg_base;
	const struct apple_soc_cpufreq_info *info;
	bool transition_failed;
	unsigned int expected_pstate;
};

static struct cpufreq_driver apple_soc_cpufreq_driver;
static void apple_soc_cpufreq_exit_with_limits(struct cpufreq_policy *policy);

static const struct apple_soc_cpufreq_info soc_s5l8960x_info = {
	.has_ps2 = false,
	.max_pstate = 7,
	.cur_pstate_mask = APPLE_DVFS_STATUS_CUR_PS_S5L8960X,
	.cur_pstate_shift = APPLE_DVFS_STATUS_CUR_PS_SHIFT_S5L8960X,
	.ps1_mask = APPLE_DVFS_CMD_PS1_S5L8960X,
	.ps1_shift = APPLE_DVFS_CMD_PS1_S5L8960X_SHIFT,
};

static const struct apple_soc_cpufreq_info soc_t8103_info = {
	.has_ps2 = true,
	.max_pstate = 15,
	.cur_pstate_mask = APPLE_DVFS_STATUS_CUR_PS_T8103,
	.cur_pstate_shift = APPLE_DVFS_STATUS_CUR_PS_SHIFT_T8103,
	.ps1_mask = APPLE_DVFS_CMD_PS1,
	.ps1_shift = APPLE_DVFS_CMD_PS1_SHIFT,
};

static const struct apple_soc_cpufreq_info soc_t8112_info = {
	.has_ps2 = false,
	.max_pstate = 31,
	.cur_pstate_mask = APPLE_DVFS_STATUS_CUR_PS_T8112,
	.cur_pstate_shift = APPLE_DVFS_STATUS_CUR_PS_SHIFT_T8112,
	.ps1_mask = APPLE_DVFS_CMD_PS1,
	.ps1_shift = APPLE_DVFS_CMD_PS1_SHIFT,
};

/*
 * T8140 uses 5-bit PS1 state IDs starting at 1. The command register reads
 * back the last accepted request, not a measured frequency; every transition
 * is verified, see apple_soc_cpufreq_set_target(). Voltage, PLL and firmware
 * throttling configuration are left as the bootloader set them.
 */
static const struct apple_soc_cpufreq_info soc_t8140_info = {
	.verify_transition = true,
	.needs_thermal_policy = true,
	.max_unmanaged_pstate = 2,
	.min_pstate = 1,
	.max_pstate = 31,
	.ps1_mask = APPLE_DVFS_CMD_PS1,
	.ps1_shift = APPLE_DVFS_CMD_PS1_SHIFT,
};

/* T8142 command-state readback; firmware retains CLPC/PMP ownership. */
static const struct apple_soc_cpufreq_info soc_t8142_info = {
	.verify_transition = true,
	.min_pstate = 1,
	.max_pstate = 31,
	.ps1_mask = APPLE_DVFS_CMD_PS1,
	.ps1_shift = APPLE_DVFS_CMD_PS1_SHIFT,
};

/* T8152 ACC uses state 2 as the first OPP and shares P/M requests. */
static const struct apple_soc_cpufreq_info soc_t8152_info = {
	.verify_transition = true,
	.needs_thermal_policy = true,
	.transition_timeout_us = 2000,
	.max_unmanaged_pstate = 3,
	.min_pstate = 2,
	.max_pstate = 31,
	.ps1_mask = APPLE_DVFS_CMD_PS1,
	.ps1_shift = APPLE_DVFS_CMD_PS1_SHIFT,
};

/* Command-state readback, not a measurement of the clock frequency. */
static const struct apple_soc_cpufreq_info soc_t6050_info = {
	.verify_transition = true,
	.needs_firmware_thermal_control = true,
	.transition_timeout_us = 2000,
	.max_pstate = 31,
	.ps1_mask = APPLE_DVFS_CMD_PS1,
	.ps1_shift = APPLE_DVFS_CMD_PS1_SHIFT,
};

static const struct apple_soc_cpufreq_info soc_default_info = {
	.has_ps2 = false,
	.max_pstate = 15,
	.cur_pstate_mask = 0, /* fallback */
	.ps1_mask = APPLE_DVFS_CMD_PS1,
	.ps1_shift = APPLE_DVFS_CMD_PS1_SHIFT,
};

static const struct of_device_id apple_soc_cpufreq_of_match[] __maybe_unused = {
	{
		.compatible = "apple,s5l8960x-cluster-cpufreq",
		.data = &soc_s5l8960x_info,
	},
	{
		.compatible = "apple,t8103-cluster-cpufreq",
		.data = &soc_t8103_info,
	},
	{
		.compatible = "apple,t8112-cluster-cpufreq",
		.data = &soc_t8112_info,
	},
	{
		.compatible = "apple,cluster-cpufreq",
		.data = &soc_default_info,
	},
	{
		.compatible = "apple,t8140-cluster-cpufreq",
		.data = &soc_t8140_info,
	},
	{
		.compatible = "apple,t6050-cluster-cpufreq",
		.data = &soc_t6050_info,
	},
	{
		.compatible = "apple,t8142-cluster-cpufreq",
		.data = &soc_t8142_info,
	},
	{
		.compatible = "apple,t8152-cluster-cpufreq",
		.data = &soc_t8152_info,
	},
	{}
};

static u32 apple_soc_cpufreq_timeout(const struct apple_soc_cpufreq_info *info)
{
	return info->transition_timeout_us ?: APPLE_DVFS_TRANSITION_TIMEOUT;
}

static unsigned int apple_soc_cpufreq_get_rate(unsigned int cpu)
{
	struct cpufreq_policy *policy;
	struct apple_cpu_priv *priv;
	struct cpufreq_frequency_table *p;
	unsigned int pstate;

	policy = cpufreq_cpu_get_raw(cpu);
	if (unlikely(!policy))
		return 0;

	priv = policy->driver_data;

	if (priv->info->cur_pstate_mask) {
		u32 reg = readl_relaxed(priv->reg_base + APPLE_DVFS_STATUS);

		pstate = (reg & priv->info->cur_pstate_mask) >>  priv->info->cur_pstate_shift;
	} else {
		/*
		 * For the fallback case we might not know the layout of DVFS_STATUS,
		 * so just use the command register value (which ignores boost limitations).
		 */
		u64 reg = readq_relaxed(priv->reg_base + APPLE_DVFS_CMD);

		pstate = FIELD_GET(APPLE_DVFS_CMD_PS1, reg);
	}

	cpufreq_for_each_valid_entry(p, policy->freq_table)
		if (p->driver_data == pstate)
			return p->frequency;

	dev_err(priv->cpu_dev, "could not find frequency for pstate %d\n",
		pstate);
	return 0;
}

static int apple_soc_cpufreq_set_target(struct cpufreq_policy *policy,
					unsigned int index)
{
	struct apple_cpu_priv *priv = policy->driver_data;
	unsigned int pstate = policy->freq_table[index].driver_data;
	u64 reg;

	/* OPP level is a hardware ID, not the frequency-table array index. */
	if (pstate < priv->info->min_pstate || pstate > priv->info->max_pstate)
		return -EINVAL;
	if (priv->transition_failed)
		return -EIO;

	if (readq_poll_timeout_atomic(priv->reg_base + APPLE_DVFS_CMD, reg,
				      !(reg & APPLE_DVFS_CMD_BUSY), 2,
				      apple_soc_cpufreq_timeout(priv->info))) {
		if (priv->info->verify_transition) {
			priv->transition_failed = true;
			dev_err(priv->cpu_dev,
				"DVFS busy timeout, command=%#llx; stopping requests\n", reg);
		}
		return -EIO;
	}

	if (priv->info->verify_transition) {
		unsigned int previous = FIELD_GET(APPLE_DVFS_CMD_PS1, reg);

		/* Do not fight an unexpected retained policy owner. */
		if (previous != priv->expected_pstate) {
			priv->transition_failed = true;
			dev_err(priv->cpu_dev, "unexpected DVFS state %u (expected %u); stopping requests\n",
				previous, priv->expected_pstate);
			return -EIO;
		}
		if (previous == pstate)
			return 0;
	}

	reg &= ~priv->info->ps1_mask;
	reg |= pstate << priv->info->ps1_shift;
	if (priv->info->has_ps2) {
		reg &= ~APPLE_DVFS_CMD_PS2;
		reg |= FIELD_PREP(APPLE_DVFS_CMD_PS2, pstate);
	}
	reg |= APPLE_DVFS_CMD_SET;

	writeq_relaxed(reg, priv->reg_base + APPLE_DVFS_CMD);

	if (priv->info->verify_transition &&
	    readq_poll_timeout_atomic(priv->reg_base + APPLE_DVFS_CMD, reg,
				      !(reg & APPLE_DVFS_CMD_BUSY) &&
				      FIELD_GET(APPLE_DVFS_CMD_PS1, reg) == pstate,
				      2, apple_soc_cpufreq_timeout(priv->info))) {
		/* Do not issue another request or guess a rollback after failure. */
		priv->transition_failed = true;
		dev_err(priv->cpu_dev,
			"DVFS completion timeout, command=%#llx; stopping requests\n", reg);
		return -ETIMEDOUT;
	}
	if (priv->info->verify_transition)
		priv->expected_pstate = pstate;

	return 0;
}

static unsigned int apple_soc_cpufreq_fast_switch(struct cpufreq_policy *policy,
						  unsigned int target_freq)
{
	if (apple_soc_cpufreq_set_target(policy, policy->cached_resolved_idx) < 0)
		return 0;

	return policy->freq_table[policy->cached_resolved_idx].frequency;
}

static int apple_soc_cpufreq_find_cluster(struct cpufreq_policy *policy,
					  void __iomem **reg_base,
					  const struct apple_soc_cpufreq_info **info,
					  struct device_node **cluster)
{
	struct of_phandle_args args;
	const struct of_device_id *match;
	int ret = 0;

	ret = of_perf_domain_get_sharing_cpumask(policy->cpu, "performance-domains",
						 "#performance-domain-cells",
						 policy->cpus, &args);
	if (ret < 0)
		return ret;

	match = of_match_node(apple_soc_cpufreq_of_match, args.np);
	if (!match || !of_device_is_available(args.np)) {
		of_node_put(args.np);
		return -ENODEV;
	}

	*info = match->data;

	*reg_base = of_iomap(args.np, 0);
	if (!*reg_base) {
		of_node_put(args.np);
		return -ENOMEM;
	}
	*cluster = args.np;

	return 0;
}

/* The fastest state whose power is at most @budget, or the slowest. */
static unsigned int apple_fast_die_fit(const struct apple_fast_die *fd,
				       u64 budget)
{
	unsigned int i, idx = 0;

	for (i = 0; i < fd->nr_states; i++)
		if (fd->power[i] <= budget)
			idx = i;

	return fd->policy->freq_table[idx].frequency;
}

/*
 * Map an effort to a frequency limit, in kHz: the fastest state whose power
 * fits a budget of (255 - effort) / 255 of the fastest state's power.  Any
 * mapping that lowers the limit monotonically as the effort rises would do,
 * since the firmware's loop is closed on the die temperature and corrects
 * the error; power is the natural axis for the output of a temperature
 * controller.
 */
static unsigned int apple_fast_die_limit(const struct apple_fast_die *fd,
					 unsigned int effort)
{
	/*
	 * Multiply first: dividing the fastest state's power by 255 first
	 * rounds it down, which would limit an unlimited cluster one state low.
	 */
	return apple_fast_die_fit(fd,
				  div_u64((u64)fd->power[fd->nr_states - 1] *
					  (APPLE_FAST_DIE_EFFORT_MAX - effort),
					  APPLE_FAST_DIE_EFFORT_MAX));
}

static ktime_t apple_fast_die_interval(const struct apple_fast_die *fd)
{
	if (!fd->waiting)
		return ms_to_ktime(APPLE_FAST_DIE_POLL_MS);
	if (!fd->warned)
		return ms_to_ktime(APPLE_FAST_DIE_WAIT_MS);
	return ms_to_ktime(APPLE_FAST_DIE_GAVE_UP_MS);
}

/* Arm the non-deferrable timer if the limit needs it.  Call with @lock held. */
static void apple_fast_die_arm(struct apple_fast_die *fd)
{
	if (fd->stopping || fd->polling ||
	    (fd->per_cpu && fd->limit >= fd->max))
		return;

	fd->polling = true;
	hrtimer_start(&fd->poll, apple_fast_die_interval(fd),
		      HRTIMER_MODE_REL_SOFT);
}

/* Take a sample and queue an update if the limit changes.  Call with @lock held. */
static void apple_fast_die_sample(struct apple_fast_die *fd)
{
	struct apple_cpu_priv *priv = fd->policy->driver_data;
	unsigned int limit;
	u8 effort;
	int ret;

	ret = apple_pmp_report_fast_die_effort(fd->report, fd->lane, &effort);
	if (!ret) {
		limit = apple_fast_die_limit(fd, effort);
		fd->waiting = false;
	} else {
		limit = fd->fallback;
		fd->waiting = true;
		if (!fd->warned &&
		    time_after(jiffies, fd->start + APPLE_FAST_DIE_WARN_S * HZ)) {
			fd->warned = true;
			dev_warn(priv->cpu_dev,
				 "PMP not running after %d s (%d): fast die-temperature limit unavailable, CPUs %*pbl stay limited to %u kHz\n",
				 APPLE_FAST_DIE_WARN_S, ret,
				 cpumask_pr_args(fd->cpus), limit);
		}
	}

	if (limit != fd->limit) {
		WRITE_ONCE(fd->limit, limit);
		schedule_work(&fd->work);
	}
}

static void apple_fast_die_work(struct work_struct *work)
{
	struct apple_fast_die *fd = container_of(work, struct apple_fast_die, work);
	struct apple_cpu_priv *priv = fd->policy->driver_data;
	unsigned int limit = READ_ONCE(fd->limit);
	int ret;

	ret = freq_qos_update_request(&fd->req, limit);
	if (ret >= 0)
		return;

	dev_warn_ratelimited(priv->cpu_dev,
			     "failed to update the fast die-temperature limit: %d\n", ret);
	/* Make the next sample queue the update again, and make sure it comes. */
	spin_lock_bh(&fd->lock);
	if (fd->limit == limit) {
		fd->limit = 0;
		apple_fast_die_arm(fd);
	}
	spin_unlock_bh(&fd->lock);
}

static enum hrtimer_restart apple_fast_die_poll(struct hrtimer *t)
{
	struct apple_fast_die *fd = container_of(t, struct apple_fast_die, poll);
	enum hrtimer_restart restart = HRTIMER_NORESTART;

	spin_lock(&fd->lock);
	apple_fast_die_sample(fd);
	if (!fd->stopping && (!fd->per_cpu || fd->limit < fd->max)) {
		hrtimer_forward_now(t, apple_fast_die_interval(fd));
		restart = HRTIMER_RESTART;
	} else {
		fd->polling = false;
	}
	spin_unlock(&fd->lock);

	return restart;
}

static void apple_fast_die_tick(struct timer_list *t)
{
	struct apple_fast_die_timer *ft = timer_container_of(ft, t, timer);
	struct apple_fast_die *fd = ft->fd;
	unsigned long now = jiffies;

	/*
	 * The first busy CPU of the cluster to run in a tick takes the sample,
	 * unless the non-deferrable timer is sampling.
	 */
	if (xchg(&fd->last_poll, now) != now && spin_trylock(&fd->lock)) {
		if (!fd->polling) {
			apple_fast_die_sample(fd);
			apple_fast_die_arm(fd);
		}
		spin_unlock(&fd->lock);
	}
	mod_timer(t, now + 1);
}

static int apple_fast_die_cpu_online(unsigned int cpu)
{
	struct apple_fast_die_timer *ft = per_cpu_ptr(&apple_fast_die_timers, cpu);

	if (!ft->fd)
		return 0;

	/* Wait for a running sample, which rearms the timer, before arming it. */
	timer_delete_sync(&ft->timer);
	ft->timer.expires = jiffies + 1;
	add_timer_on(&ft->timer, cpu);

	return 0;
}

static int apple_fast_die_cpu_offline(unsigned int cpu)
{
	struct apple_fast_die_timer *ft = per_cpu_ptr(&apple_fast_die_timers, cpu);

	if (ft->fd)
		timer_delete_sync(&ft->timer);

	return 0;
}

/*
 * Called at the end of ->init(), with the CPU hotplug lock held.  The core
 * fills policy->related_cpus only after ->init() returns, and may call ->exit()
 * before it does, so the limit keeps its own copy of the cluster's CPUs.
 */
static int apple_fast_die_init(struct cpufreq_policy *policy,
			       struct device_node *cluster)
{
	struct apple_cpu_priv *priv = policy->driver_data;
	struct of_phandle_args args;
	struct apple_fast_die *fd;
	unsigned int cpu, i, n = 0;
	bool have_power = true;
	int parsed, ret;

	parsed = of_parse_phandle_with_fixed_args(cluster, "apple,fast-die-effort",
						  1, 0, &args);
	/* Required thermal admission must fail closed with an older DT. */
	if (parsed == -ENOENT && !priv->info->needs_thermal_policy)
		return 0;

	while (policy->freq_table[n].frequency != CPUFREQ_TABLE_END)
		n++;

	fd = kzalloc_flex(*fd, power, n);
	if (!fd) {
		ret = -ENOMEM;
		goto err_put;
	}
	fd->nr_states = n;
	if (!zalloc_cpumask_var(&fd->cpus, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto err_free;
	}

	for (i = 0; i < n; i++) {
		unsigned long rate = policy->freq_table[i].frequency * 1000UL + 999;
		struct dev_pm_opp *opp = dev_pm_opp_find_freq_floor(priv->cpu_dev, &rate);

		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			goto err_free;
		}
		fd->power[i] = dev_pm_opp_get_power(opp);
		dev_pm_opp_put(opp);
		if (!fd->power[i])
			have_power = false;
	}
	/* Without an energy model, frequency stands in for power. */
	if (!have_power)
		for (i = 0; i < n; i++)
			fd->power[i] = policy->freq_table[i].frequency;

	fd->policy = policy;
	spin_lock_init(&fd->lock);
	INIT_WORK(&fd->work, apple_fast_die_work);
	hrtimer_setup(&fd->poll, apple_fast_die_poll, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
	cpumask_copy(fd->cpus, policy->cpus);
	fd->per_cpu = apple_fast_die_hp_state > 0;
	for_each_cpu(cpu, fd->cpus)
		if (tick_nohz_full_cpu(cpu))
			fd->per_cpu = false;
	fd->max = policy->freq_table[n - 1].frequency;
	fd->fallback = apple_fast_die_fit(fd, div_u64(fd->power[n - 1], 3));
	fd->start = jiffies;

	if (parsed) {
		/* The limit is meant to be there, so keep the fallback. */
		dev_warn(priv->cpu_dev,
			 "invalid apple,fast-die-effort (%d): CPUs %*pbl stay limited to %u kHz\n",
			 parsed, cpumask_pr_args(fd->cpus), fd->fallback);
		fd->warned = true;
	} else {
		fd->report = args.np;
		fd->lane = args.args[0];
	}

	/* Hold the fallback limit until a sample finds the PMP running. */
	fd->limit = fd->fallback;
	ret = freq_qos_add_request(&policy->constraints, &fd->req, FREQ_QOS_MAX,
				   fd->limit);
	if (ret < 0)
		goto err_free;

	priv->fast_die = fd;

	if (fd->per_cpu) {
		for_each_cpu(cpu, fd->cpus) {
			struct apple_fast_die_timer *ft =
				per_cpu_ptr(&apple_fast_die_timers, cpu);

			timer_setup(&ft->timer, apple_fast_die_tick,
				    TIMER_DEFERRABLE | TIMER_PINNED);
			ft->fd = fd;
			if (cpu_online(cpu))
				apple_fast_die_cpu_online(cpu);
		}
	}

	spin_lock_bh(&fd->lock);
	apple_fast_die_arm(fd);
	spin_unlock_bh(&fd->lock);

	dev_dbg(priv->cpu_dev, "fast die-temperature limit set up, fallback %u kHz\n",
		fd->fallback);

	return 0;

err_free:
	free_cpumask_var(fd->cpus);
	kfree(fd);
err_put:
	if (!parsed)
		of_node_put(args.np);
	return ret;
}

static void apple_fast_die_exit(struct apple_cpu_priv *priv)
{
	struct apple_fast_die *fd = priv->fast_die;
	unsigned int cpu;

	if (!fd)
		return;

	/*
	 * Keep the update work from arming the hrtimer again, stop the per-CPU
	 * timers, which can arm it too, then the hrtimer, and then drain the
	 * update any of them may have queued.
	 */
	spin_lock_bh(&fd->lock);
	fd->stopping = true;
	spin_unlock_bh(&fd->lock);
	if (fd->per_cpu) {
		for_each_cpu(cpu, fd->cpus) {
			struct apple_fast_die_timer *ft =
				per_cpu_ptr(&apple_fast_die_timers, cpu);

			timer_shutdown_sync(&ft->timer);
			ft->fd = NULL;
		}
	}
	hrtimer_cancel(&fd->poll);
	cancel_work_sync(&fd->work);
	freq_qos_remove_request(&fd->req);

	priv->fast_die = NULL;
	of_node_put(fd->report);
	free_cpumask_var(fd->cpus);
	kfree(fd);
}

static int apple_soc_cpufreq_init(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *p;
	int ret, i;
	unsigned int transition_latency;
	void __iomem *reg_base;
	struct device *cpu_dev;
	struct apple_cpu_priv *priv;
	const struct apple_soc_cpufreq_info *info;
	struct cpufreq_frequency_table *freq_table;
	struct device_node *cluster;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_err("failed to get cpu%d device\n", policy->cpu);
		return -ENODEV;
	}

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;

	ret = apple_soc_cpufreq_find_cluster(policy, &reg_base, &info, &cluster);
	if (ret) {
		dev_err(cpu_dev, "%s: failed to get cluster info: %d\n", __func__, ret);
		goto out_free_priv;
	}

	/* The boot producer must retain and qualify firmware thermal control. */
	if (info->needs_firmware_thermal_control &&
	    !of_property_read_bool(cluster, "apple,firmware-managed-thermal")) {
		dev_err(cpu_dev, "firmware thermal ownership is not declared\n");
		ret = -ENODEV;
		goto out_iounmap;
	}

	ret = dev_pm_opp_of_cpumask_add_table(policy->cpus);
	if (ret < 0) {
		dev_err(cpu_dev, "%s: failed to add OPP table: %d\n", __func__, ret);
		goto out_iounmap;
	}

	ret = dev_pm_opp_get_opp_count(cpu_dev);
	if (ret <= 0) {
		dev_dbg(cpu_dev, "OPP table is not ready, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto out_free_table;
	}

	ret = dev_pm_opp_init_cpufreq_table(cpu_dev, &freq_table);
	if (ret) {
		dev_err(cpu_dev, "failed to init cpufreq table: %d\n", ret);
		goto out_free_table;
	}

	/* Get OPP levels (p-state indexes) and stash them in driver_data */
	for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		unsigned long rate = freq_table[i].frequency * 1000UL + 999;
		struct dev_pm_opp *opp = dev_pm_opp_find_freq_floor(cpu_dev, &rate);

		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			goto out_free_cpufreq_table;
		}
		freq_table[i].driver_data = dev_pm_opp_get_level(opp);
		dev_pm_opp_put(opp);
		if (freq_table[i].driver_data < info->min_pstate ||
		    freq_table[i].driver_data > info->max_pstate) {
			ret = -EINVAL;
			goto out_free_cpufreq_table;
		}
	}

	if (info->verify_transition) {
		u64 cmd;
		unsigned int state;

		/* Registration must not silently reset an unknown inherited state. */
		ret = readq_poll_timeout_atomic(reg_base + APPLE_DVFS_CMD, cmd,
						!(cmd & APPLE_DVFS_CMD_BUSY), 2,
						apple_soc_cpufreq_timeout(info));
		if (ret)
			goto out_free_cpufreq_table;
		state = FIELD_GET(APPLE_DVFS_CMD_PS1, cmd);
		for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++)
			if (freq_table[i].driver_data == state)
				break;
		if (freq_table[i].frequency == CPUFREQ_TABLE_END) {
			ret = -ERANGE;
			goto out_free_cpufreq_table;
		}
		policy->cur = freq_table[i].frequency;
		priv->expected_pstate = state;
	}

	/* T8140 has IPA and the PMP limit; other SoCs keep their safe ceiling. */
	if (info->needs_thermal_policy && info != &soc_t8140_info) {
		cpufreq_for_each_valid_entry(p, freq_table) {
			if (p->driver_data <= info->max_unmanaged_pstate)
				continue;
			dev_err(cpu_dev, "higher P-states require a qualified thermal policy\n");
			ret = -ENODEV;
			goto out_free_cpufreq_table;
		}
	}

	priv->cpu_dev = cpu_dev;
	priv->reg_base = reg_base;
	priv->info = info;
	policy->driver_data = priv;
	policy->freq_table = freq_table;

	ret = apple_fast_die_init(policy, cluster);
	if (ret) {
		dev_err(cpu_dev, "failed to set up the fast die-temperature limit: %d\n", ret);
		goto out_clear_policy;
	}
	of_node_put(cluster);

	transition_latency = dev_pm_opp_get_max_transition_latency(cpu_dev);
	if (!transition_latency) {
		/* Conservative transaction bound, not a measured transition time. */
		transition_latency = apple_soc_cpufreq_timeout(info) * NSEC_PER_USEC;
		if (info->verify_transition)
			transition_latency *= 2;
	}

	policy->cpuinfo.transition_latency = transition_latency;
	policy->dvfs_possible_from_any_cpu = true;
	policy->fast_switch_possible = !info->verify_transition;
	policy->suspend_freq = freq_table[0].frequency;

	return 0;

out_clear_policy:
	policy->driver_data = NULL;
	policy->freq_table = NULL;
out_free_cpufreq_table:
	dev_pm_opp_free_cpufreq_table(cpu_dev, &freq_table);
out_free_table:
	dev_pm_opp_of_cpumask_remove_table(policy->cpus);
out_iounmap:
	iounmap(reg_base);
	of_node_put(cluster);
out_free_priv:
	kfree(priv);
	return ret;
}

static void apple_soc_cpufreq_exit(struct cpufreq_policy *policy)
{
	struct apple_cpu_priv *priv = policy->driver_data;

	dev_pm_opp_free_cpufreq_table(priv->cpu_dev, &policy->freq_table);
	/*
	 * The core clears the last CPU from policy->cpus before calling
	 * ->exit(), so remove the tables of every CPU the policy covered.
	 */
	dev_pm_opp_of_cpumask_remove_table(policy->related_cpus);
	iounmap(priv->reg_base);
	kfree(priv);
}

static struct cpufreq_driver apple_soc_cpufreq_driver = {
	.name		= "apple-cpufreq",
	.flags		= CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
			  CPUFREQ_NEED_INITIAL_FREQ_CHECK | CPUFREQ_IS_COOLING_DEV,
	.verify		= cpufreq_generic_frequency_table_verify,
	.get		= apple_soc_cpufreq_get_rate,
	.init		= apple_soc_cpufreq_init,
	.exit		= apple_soc_cpufreq_exit_with_limits,
	.target_index	= apple_soc_cpufreq_set_target,
	.fast_switch	= apple_soc_cpufreq_fast_switch,
	.register_em	= cpufreq_register_em_with_opp,
	.set_boost	= cpufreq_boost_set_sw,
	.suspend	= cpufreq_generic_suspend,
};

/* Drain firmware-limit updates before releasing the policy's OPP table. */
static void apple_soc_cpufreq_exit_with_limits(struct cpufreq_policy *policy)
{
	apple_fast_die_exit(policy->driver_data);
	apple_soc_cpufreq_exit(policy);
}

static int __init apple_soc_cpufreq_module_init(void)
{
	struct device_node *np;
	int ret;

	if (!of_machine_is_compatible("apple,arm-platform"))
		return -ENODEV;

	/*
	 * Only SoCs with a fast die-temperature limit need the per-CPU timers
	 * to follow CPU hotplug.  Either order against the cpufreq core's own
	 * hotplug state is fine: ->init() arms the timers of the CPUs that are
	 * online, and ->exit() stops them all.  Without the state the limit
	 * still works, sampling from the hrtimer all the time.
	 */
	np = of_find_node_with_property(NULL, "apple,fast-die-effort");
	if (np) {
		of_node_put(np);
		ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
						"cpufreq/apple-soc:online",
						apple_fast_die_cpu_online,
						apple_fast_die_cpu_offline);
		if (ret < 0)
			pr_warn("apple-cpufreq: no CPU hotplug state (%d), fast die-temperature limit always polls\n",
				ret);
		else
			apple_fast_die_hp_state = ret;
	}

	ret = cpufreq_register_driver(&apple_soc_cpufreq_driver);
	if (ret && apple_fast_die_hp_state > 0)
		cpuhp_remove_state_nocalls(apple_fast_die_hp_state);

	return ret;
}
module_init(apple_soc_cpufreq_module_init);

static void __exit apple_soc_cpufreq_module_exit(void)
{
	cpufreq_unregister_driver(&apple_soc_cpufreq_driver);
	if (apple_fast_die_hp_state > 0)
		cpuhp_remove_state_nocalls(apple_fast_die_hp_state);
}
module_exit(apple_soc_cpufreq_module_exit);

MODULE_DEVICE_TABLE(of, apple_soc_cpufreq_of_match);
MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_DESCRIPTION("Apple SoC CPU cluster DVFS driver");
MODULE_LICENSE("GPL");

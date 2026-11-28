// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC host performance controller (CLPC) floors
 *
 * On macOS the CLPC raises the floor of the memory controller (DCS) and the
 * SoC fabric while CPU clusters, the GPU or the ANE are busy, through its own
 * voter agent in the PMC. This driver keeps that agent's floor in step with
 * Linux demand:
 *
 *  - every sampling period, each cpufreq policy reports how busy its
 *    busiest CPU was and how fast the policy runs relative to its maximum;
 *    a busy policy requests a DCS and fabric state from a table indexed by
 *    that frequency quartile;
 *  - each rail keeps its last request for a hold time after demand stops,
 *    then drops back to 0, so idle releases the floor.
 *
 * The native mapping from CPU demand to DCS and fabric states is not known.
 * The tables below are measured policy, not a copy of macOS.
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpufreq.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/of_platform.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/soc/apple/clpc.h>
#include <linux/debugfs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mfd/macsmc.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/soc/apple/pmc.h>
#include <linux/tick.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define APPLE_CLPC_MAX_CLUSTERS	8
#define APPLE_CLPC_LEVELS	4

enum apple_clpc_rail {
	APPLE_CLPC_DCS,
	APPLE_CLPC_SOC,
	APPLE_CLPC_RAILS,
};

/* Sampling period */
#define APPLE_CLPC_SAMPLE_MS	4
/* Busy threshold of a CPU cluster, percent of the sampling period */
#define APPLE_CLPC_BUSY_PCT	50
/* Time a rail keeps its last request after demand stops */
#define APPLE_CLPC_HOLD_MS	32
/*
 * Time the GPU floor stays released after the PMP last limited the DCS. Under
 * sustained heavy rendering the PMP limits the DCS again well within this, so
 * the floor stays released; light submissions that follow get the floor back
 * this long after the PMP stops limiting.
 */
#define APPLE_CLPC_GPU_BACKOFF_MS	100
/* Package power readings in the low-power profile: every 100 ms */
#define APPLE_CLPC_POWER_MS	100

/* Why the last vote was cast, for debugfs */
#define APPLE_CLPC_REASON_CPU		BIT(0)
#define APPLE_CLPC_REASON_HOST_GPU	BIT(1)
#define APPLE_CLPC_REASON_STREAMING_GPU	BIT(2)
#define APPLE_CLPC_REASON_DENSE_GPU	BIT(3)
#define APPLE_CLPC_REASON_HOLD		BIT(4)
#define APPLE_CLPC_REASON_ANE		BIT(5)

/* A lower GPU busy level must last this long; a higher one applies at once. */
#define APPLE_CLPC_GPU_DOWN_MS		400

/**
 * struct apple_clpc_hw - per-SoC CLPC parameters
 * @rail: PMC rail of the DCS and of the fabric
 * @sustain: consecutive busy samples before a cluster requests a state;
 *	0 or 1 for the first
 * @map: state of each rail for a busy cluster below 25, 50, 75 and 100
 *	percent of its maximum frequency
 * @gpu_demand: streaming GPU work requests @gpu
 * @gpu: state of each rail for streaming GPU work
 * @gpu_sustain_ms: time graphics stays powered without a break before it
 *	counts as streaming, see apple_clpc_gpu_streaming(); 0 counts it at once
 * @pmp_backoff: release that GPU floor while the PMP, voter agent
 *	@pmp_agent, limits the DCS, see apple_clpc_gpu_limited()
 * @pmp_agent: PMC floor voter agent of the PMP, read for @pmp_backoff
 * @gpu_dense: submissions per sampling period that make light GPU work
 *	dense, see apple_clpc_gpu_dense(); 0 for none
 * @ane: state of each rail while ANE work is in flight; all zero disables input
 * @ane_cluster: /cpus/cpu-map cluster whose CPUs' frequency the ANE engine
 *	speed follows
 * @ane_cluster_khz: minimum frequency of that cluster while ANE work is in
 *	flight; 0 for none
 * @ane_cluster_machine: the machine compatible the cluster floor was
 *	measured on; SoCs sharing this CLPC without it get no cluster floor
 * @gpu_input: the GPU driver reports its work in flight through
 *	apple_clpc_gpu_get(); while there is some, both rails go to the top
 *	of @map (or @gpu where higher), or to @gpu_busy if enabled from the DT
 * @gpu_busy: state of each rail for reported GPU work below @gpu_busy_mid,
 *	@gpu_busy_high and @gpu_busy_top and at or above it, from the node's
 *	apple,gpu-busy-memory-states and apple,gpu-busy-permille properties;
 *	a non-zero top DCS state enables the busy-share ladder, otherwise
 *	reported GPU work falls back to the top of @map (or @gpu where higher)
 * @gpu_busy_mid: GPU busy share in permille (apple_clpc_gpu_set_busy())
 *	from which reported GPU work gets the second column of @gpu_busy
 * @gpu_busy_high: busy share from which it gets the third column
 * @gpu_busy_top: busy share from which it gets the top column, as does a
 *	GPU that reports no share. The share is the busiest engine's time spent
 *	executing at whatever clock the GPU runs, not bandwidth: a scrolling
 *	browser reads a few percent, CS2 at 120 fps ~480-620 permille.
 * @idle_on_time: sample every period even with no vote held, instead of
 *	from a deferrable timer
 * @low_power_khz: CPU frequency ceiling of the low-power platform profile,
 *	taken up to the next table frequency, for a node without
 *	apple,low-power-frequency-max-mhz; 0 for none
 * @low_power_mw: package power (the SMC's PHPC key) the low-power profile
 *	holds the CPU clusters to; 0 for none
 * @high_power_machine: the machine compatible that had High Power mode from
 *	this table before the board DT carried "apple,high-power-mode"; kept so
 *	that a DTB without the property keeps High Power mode. NULL for none
 */
struct apple_clpc_hw {
	unsigned int rail[APPLE_CLPC_RAILS];
	unsigned int sustain;
	u8 map[APPLE_CLPC_RAILS][APPLE_CLPC_LEVELS];
	bool gpu_demand;
	u8 gpu[APPLE_CLPC_RAILS];
	unsigned int gpu_sustain_ms;
	bool pmp_backoff;
	unsigned int pmp_agent;
	unsigned int gpu_dense;
	bool gpu_input;
	u8 gpu_busy[APPLE_CLPC_RAILS][APPLE_CLPC_LEVELS];
	unsigned int gpu_busy_mid;
	unsigned int gpu_busy_high;
	unsigned int gpu_busy_top;
	u8 ane[APPLE_CLPC_RAILS];
	unsigned int ane_cluster;
	unsigned int ane_cluster_khz;
	const char *ane_cluster_machine;
	bool idle_on_time;
	unsigned int low_power_khz;
	unsigned int low_power_mw;
	const char *high_power_machine;
};

struct apple_clpc_cluster {
	unsigned int policy_cpu;
	cpumask_var_t cpus;
	unsigned int busy;
	unsigned int streak;
	unsigned int level;
	unsigned int request[APPLE_CLPC_RAILS];
	/* The platform profile's frequency ceiling (0 for none) and its top */
	struct freq_qos_request max_qos;
	unsigned int max_khz, top_khz;
};

struct apple_clpc_vote {
	unsigned int state;
	unsigned long last_demand;
};

/* Independent lifetime: a late consumer put never dereferences a removed provider. */
struct apple_clpc_gpu {
	struct kref ref;
	spinlock_t lock;
	bool active;
	u64 updated_ns;
	u64 sequence;
	/* Busy share of the last window in permille, if the GPU reports one */
	unsigned int busy;
	bool busy_valid;
	unsigned int level;
	unsigned long level_since;
	struct delayed_work *wake;
};

/* One ANE publisher, with storage surviving provider removal. */
struct apple_clpc_ane {
	struct kref ref;
	/* Protects activity, event metadata and wake across provider removal. */
	spinlock_t lock;
	bool active;
	bool pending;
	u64 updated_ns;
	u64 sequence;
	struct delayed_work *wake;
};

struct apple_clpc;

struct apple_clpc_idle_timer {
	struct timer_list timer;
	struct apple_clpc *clpc;
	u64 last_idle;
	u64 last_wall;
};

struct apple_clpc {
	struct device *dev;
	struct device *pmc;
	const struct apple_clpc_hw *hw;
	/*
	 * The board's states for a CPU cluster busy in the top quarter of its
	 * range (apple,cpu-busy-floor) outside the low-power profile, in place
	 * of the top of @hw->map; GPU and ANE demand still use @hw. Unset:
	 * @hw->map applies.
	 */
	u8 cpu_top[APPLE_CLPC_RAILS];
	bool cpu_top_set;
	unsigned int agent;
	/* Samples while a vote is held; idle_work or per-CPU idle_timers defer to busy CPUs. */
	struct delayed_work work;
	struct delayed_work idle_work;
	struct apple_clpc_idle_timer __percpu *idle_timers;
	struct mutex idle_lock;
	cpumask_t idle_online_cpus;
	struct hlist_node cpuhp_node;
	bool cpuhp_added;
	atomic_t idle_armed;
	struct mutex sample_lock;
	bool stopped;
	struct apple_clpc_cluster clusters[APPLE_CLPC_MAX_CLUSTERS];
	unsigned int nr_clusters;
	cpumask_var_t covered;
	unsigned long next_scan;
	u64 *idle;
	u64 *wall;
	struct apple_clpc_vote vote[APPLE_CLPC_RAILS];
	u32 mask;
	u32 saved_floor;
	unsigned long samples;
	unsigned long update_errors;
	unsigned long gpu_read_errors;
	struct dentry *debugfs;
	struct regmap *gpu_pmgr;
	struct apple_clpc_gpu *gpu;
	struct apple_clpc_ane *ane;
	unsigned int ane_states[APPLE_CLPC_RAILS];
	/* Minimum frequency of hw->ane_cluster while ANE work is in flight. */
	u32 ane_cluster_khz;
	int ane_cpu;
	struct freq_qos_request ane_qos;
	unsigned int ane_qos_khz;
	unsigned long ane_until;
	struct notifier_block policy_nb;
	bool policy_nb_added;
	u32 last_floor;
	u32 last_reason;
	u64 last_vote_ns;
	u32 gpu_offset;
	/* Graphics powered at the last sample, since when, and that sample's time */
	bool gpu_on;
	unsigned long gpu_on_since;
	unsigned long gpu_sampled;
	/* Submissions seen at the last sample, and dense samples in a row */
	u64 gpu_sequence;
	unsigned int gpu_dense_streak;
	/* Whether, and when, the PMP last limited the DCS while GPU work streamed */
	bool gpu_limited;
	unsigned long gpu_limited_at;
	/*
	 * Whether the GPU floor was released at the last sample that saw GPU work
	 * stream, and how often it was released and came back, for debugfs.
	 */
	bool gpu_floor_released;
	unsigned long gpu_floor_releases;
	unsigned long gpu_floor_returns;
	/* Samples that applied the GPU floor for streaming or dense work. */
	unsigned long gpu_streaming_samples;
	unsigned long gpu_dense_samples;
	/* The platform profile and, for its power target, the SMC */
	bool has_pmc;
	enum platform_profile_option profile;
	bool low_power;
	/* the DT ceiling, else hw->low_power_khz, else 0 */
	unsigned int low_power_khz;
	unsigned int low_power_mw;
	struct device_node *smc_np;
	struct apple_smc *smc;
	/* The performance profile is sent to the SMC as High Power mode */
	bool high_power;
	u8 high_power_seq;
	bool high_power_sent;
	unsigned long power_next;
	unsigned long power_reads, power_updates;
	int power_mw;
};

/* Protect publication/removal of provider drvdata, never a submission path. */
static DEFINE_MUTEX(apple_clpc_clients_lock);

static void apple_clpc_gpu_free(struct kref *ref)
{
	kfree(container_of(ref, struct apple_clpc_gpu, ref));
}

void apple_clpc_gpu_set_active(struct apple_clpc_gpu *gpu, bool active)
{
	unsigned long flags;

	if (!gpu)
		return;
	spin_lock_irqsave(&gpu->lock, flags);
	/* A cold GPU publisher must not wait for a deferrable CPU-idle timer.
	 * Removal clears wake under this same lock before draining work.
	 */
	if (active && !gpu->active && gpu->wake)
		mod_delayed_work(system_freezable_power_efficient_wq, gpu->wake, 0);
	gpu->active = active;
	/* A new burst after idle starts from the top, not from an old share. */
	if (!active)
		gpu->busy_valid = false;
	gpu->updated_ns = ktime_get_mono_fast_ns();
	gpu->sequence++;
	spin_unlock_irqrestore(&gpu->lock, flags);
}
EXPORT_SYMBOL_GPL(apple_clpc_gpu_set_active);

/**
 * apple_clpc_gpu_set_busy() - Report the GPU busy share of the last window
 * @gpu: the GPU input, from apple_clpc_gpu_get()
 * @permille: share of the window the GPU spent executing work, 0..1000
 *
 * Scales the floor that reported GPU work gets, see &struct apple_clpc_hw.gpu_busy.
 */
void apple_clpc_gpu_set_busy(struct apple_clpc_gpu *gpu, unsigned int permille)
{
	unsigned long flags;

	if (!gpu)
		return;
	spin_lock_irqsave(&gpu->lock, flags);
	gpu->busy = min(permille, 1000U);
	gpu->busy_valid = true;
	spin_unlock_irqrestore(&gpu->lock, flags);
}
EXPORT_SYMBOL_GPL(apple_clpc_gpu_set_busy);

void apple_clpc_gpu_put(struct apple_clpc_gpu *gpu)
{
	if (gpu) {
		apple_clpc_gpu_set_active(gpu, false);
		kref_put(&gpu->ref, apple_clpc_gpu_free);
	}
}
EXPORT_SYMBOL_GPL(apple_clpc_gpu_put);

int apple_clpc_gpu_get(struct device *dev, struct apple_clpc_gpu **out)
{
	struct device_node *np;
	struct platform_device *provider;
	struct apple_clpc *clpc;
	struct device_link *link;
	bool fallback = false;
	int ret = -EPROBE_DEFER;

	*out = NULL;
	np = of_parse_phandle(dev->of_node, "apple,clpc", 0);
	/*
	 * Device trees that predate the link on the T8140 GPU still have
	 * exactly one T8140 CLPC: take that one, as a hint the GPU can do
	 * without, so that it never waits for a CLPC that does not probe.
	 */
	if (!np && !of_find_property(dev->of_node, "apple,clpc", NULL) &&
	    of_device_is_compatible(dev->of_node, "apple,agx-t8140")) {
		np = of_find_compatible_node(NULL, NULL, "apple,t8140-clpc");
		fallback = true;
		ret = 0;
	}
	if (!np)
		return of_find_property(dev->of_node, "apple,clpc", NULL) ? -EINVAL : 0;
	if (!of_device_is_compatible(np, "apple,t6041-max-clpc") &&
	    !of_device_is_compatible(np, "apple,t6041-clpc") &&
	    !of_device_is_compatible(np, "apple,t6050-clpc") &&
	    !of_device_is_compatible(np, "apple,t8132-clpc") &&
	    !of_device_is_compatible(np, "apple,t8140-clpc") &&
	    !of_device_is_compatible(np, "apple,t8142-clpc") &&
	    !of_device_is_compatible(np, "apple,t8152-clpc")) {
		of_node_put(np);
		return -EINVAL;
	}
	/* A disabled provider never probes: run without its input, do not defer. */
	if (!of_device_is_available(np)) {
		of_node_put(np);
		return 0;
	}
	provider = of_find_device_by_node(np);
	of_node_put(np);
	if (!provider)
		return ret;
	if (fallback && !device_is_bound(&provider->dev)) {
		put_device(&provider->dev);
		return 0;
	}
	link = device_link_add(dev, &provider->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		put_device(&provider->dev);
		return -ENOMEM;
	}
	mutex_lock(&apple_clpc_clients_lock);
	clpc = dev_get_drvdata(&provider->dev);
	if (clpc && clpc->gpu && refcount_read(&clpc->gpu->ref.refcount) == 1) {
		kref_get(&clpc->gpu->ref);
		*out = clpc->gpu;
		ret = 0;
	}
	mutex_unlock(&apple_clpc_clients_lock);
	put_device(&provider->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_clpc_gpu_get);

/*
 * The column of @hw->gpu_busy for reported GPU work, from its busy share.
 * Called once per CLPC sample: a higher column applies at once, a lower one
 * at the first sample at least APPLE_CLPC_GPU_DOWN_MS after the last one
 * that asked for the current column or a higher one.
 */
static unsigned int apple_clpc_gpu_level(const struct apple_clpc_hw *hw,
					 struct apple_clpc_gpu *gpu)
{
	unsigned long flags;
	unsigned int level;

	spin_lock_irqsave(&gpu->lock, flags);
	if (!gpu->busy_valid || gpu->busy >= hw->gpu_busy_top)
		level = APPLE_CLPC_LEVELS - 1;
	else if (gpu->busy >= hw->gpu_busy_high)
		level = 2;
	else
		level = gpu->busy >= hw->gpu_busy_mid ? 1 : 0;
	if (level >= gpu->level ||
	    time_after_eq(jiffies, gpu->level_since +
			  msecs_to_jiffies(APPLE_CLPC_GPU_DOWN_MS))) {
		gpu->level = level;
		gpu->level_since = jiffies;
	}
	level = gpu->level;
	spin_unlock_irqrestore(&gpu->lock, flags);
	return level;
}

static bool apple_clpc_gpu_active(struct apple_clpc_gpu *gpu, u64 *updated, u64 *sequence)
{
	unsigned long flags;
	bool active;

	spin_lock_irqsave(&gpu->lock, flags);
	active = gpu->active;
	*updated = gpu->updated_ns;
	*sequence = gpu->sequence;
	spin_unlock_irqrestore(&gpu->lock, flags);
	return active;
}

static void apple_clpc_gpu_owner_put(void *data)
{
	struct apple_clpc_gpu *gpu = data;

	kref_put(&gpu->ref, apple_clpc_gpu_free);
}

static void apple_clpc_ane_free(struct kref *ref)
{
	kfree(container_of(ref, struct apple_clpc_ane, ref));
}

void apple_clpc_ane_set_active(struct apple_clpc_ane *ane, bool active)
{
	unsigned long flags;

	if (!ane)
		return;
	spin_lock_irqsave(&ane->lock, flags);
	if (active && !ane->active) {
		/* Preserve even an EXECUTE shorter than the first queued sample. */
		ane->pending = true;
		if (ane->wake)
			mod_delayed_work(system_freezable_power_efficient_wq, ane->wake, 0);
	}
	ane->active = active;
	ane->updated_ns = ktime_get_mono_fast_ns();
	ane->sequence++;
	spin_unlock_irqrestore(&ane->lock, flags);
}
EXPORT_SYMBOL_GPL(apple_clpc_ane_set_active);

void apple_clpc_ane_put(struct apple_clpc_ane *ane)
{
	if (ane) {
		apple_clpc_ane_set_active(ane, false);
		kref_put(&ane->ref, apple_clpc_ane_free);
	}
}
EXPORT_SYMBOL_GPL(apple_clpc_ane_put);

/* SoCs whose CLPC takes ANE input; each SoC has exactly one CLPC. */
static const struct of_device_id apple_clpc_ane_providers[] = {
	{ .compatible = "apple,t6050-clpc" },
	{ .compatible = "apple,t6041-max-clpc" },
	{ .compatible = "apple,t6041-clpc" },
	{ .compatible = "apple,t8140-clpc" },
	{ .compatible = "apple,t8142-clpc" },
	{}
};

int apple_clpc_ane_get(struct device *dev, struct apple_clpc_ane **out)
{
	struct device_node *np;
	struct platform_device *provider;
	struct apple_clpc *clpc;

	*out = NULL;
	if (!dev->of_node)
		return 0;
	np = of_parse_phandle(dev->of_node, "apple,clpc", 0);
	if (!np && !of_find_property(dev->of_node, "apple,clpc", NULL))
		np = of_find_matching_node(NULL, apple_clpc_ane_providers);
	if (!np)
		return 0;
	if (!of_match_node(apple_clpc_ane_providers, np)) {
		of_node_put(np);
		return 0;
	}
	provider = of_find_device_by_node(np);
	of_node_put(np);
	if (!provider)
		return 0;
	/*
	 * Optional input: never defer ANE startup for an absent CLPC driver.
	 * No device link is needed: the independent reference protects late
	 * publications and puts after the provider has withdrawn its work.
	 */
	mutex_lock(&apple_clpc_clients_lock);
	clpc = dev_get_drvdata(&provider->dev);
	if (device_is_bound(&provider->dev) && clpc && clpc->ane &&
	    refcount_read(&clpc->ane->ref.refcount) == 1) {
		kref_get(&clpc->ane->ref);
		*out = clpc->ane;
	}
	mutex_unlock(&apple_clpc_clients_lock);
	put_device(&provider->dev);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_clpc_ane_get);

static void apple_clpc_ane_owner_put(void *data)
{
	struct apple_clpc_ane *ane = data;

	kref_put(&ane->ref, apple_clpc_ane_free);
}

static void apple_clpc_ane_detach(struct apple_clpc_ane *ane)
{
	unsigned long flags;

	if (!ane)
		return;
	spin_lock_irqsave(&ane->lock, flags);
	ane->wake = NULL;
	spin_unlock_irqrestore(&ane->lock, flags);
}

/* Called under sample_lock, like the CPU and GPU demand inputs. */
static bool apple_clpc_ane_input_demand(struct apple_clpc *clpc, unsigned int *demand)
{
	struct apple_clpc_ane *ane = clpc->ane;
	unsigned long flags;
	unsigned int rail;
	bool active;

	if (!ane)
		return false;
	spin_lock_irqsave(&ane->lock, flags);
	active = ane->active || ane->pending;
	ane->pending = false;
	spin_unlock_irqrestore(&ane->lock, flags);
	if (active)
		for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
			demand[rail] = max(demand[rail], clpc->ane_states[rail]);
	return active;
}

/* cpufreq policies can appear after probe; pick up new ones as they do. */
static void apple_clpc_find_clusters(struct apple_clpc *clpc)
{
	unsigned int cpu, i;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct apple_clpc_cluster *cl;

		if (cpumask_test_cpu(cpu, clpc->covered))
			continue;
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		for (i = 0; i < clpc->nr_clusters; i++)
			if (clpc->clusters[i].policy_cpu == policy->cpu)
				break;
		if (i == clpc->nr_clusters && i < APPLE_CLPC_MAX_CLUSTERS) {
			cl = &clpc->clusters[clpc->nr_clusters++];
			cl->policy_cpu = policy->cpu;
		}
		if (i < clpc->nr_clusters) {
			cl = &clpc->clusters[i];
			cpumask_set_cpu(cpu, cl->cpus);
			cpumask_set_cpu(cpu, clpc->covered);
			if (clpc->idle && clpc->wall)
				clpc->idle[cpu] = get_cpu_idle_time(cpu, &clpc->wall[cpu], 0);
		}
		cpufreq_cpu_put(policy);
	}
}

/*
 * The state a busy CPU cluster in frequency quarter @q asks @rail for.
 * The board's override applies outside the low-power platform profile,
 * where macOS drops the single-thread busy floor.
 */
static unsigned int apple_clpc_cpu_state(const struct apple_clpc *clpc,
					 unsigned int rail, unsigned int q)
{
	if (clpc->cpu_top_set && q == APPLE_CLPC_LEVELS - 1 &&
	    !READ_ONCE(clpc->low_power))
		return clpc->cpu_top[rail];
	return clpc->hw->map[rail][q];
}

static void apple_clpc_sample_busy(struct apple_clpc *clpc,
				   struct apple_clpc_cluster *cl)
{
	unsigned int cpu, busy = 0;

	for_each_cpu(cpu, cl->cpus) {
		u64 wall, idle, dw, di;
		bool valid;

		/*
		 * An offline CPU accrues no idle time while the wall clock runs
		 * on, so it would look fully busy. Skip it, and take a fresh
		 * baseline once it is back.
		 */
		if (!cpu_online(cpu)) {
			clpc->wall[cpu] = 0;
			continue;
		}

		idle = get_cpu_idle_time(cpu, &wall, 0);
		dw = wall - clpc->wall[cpu];
		di = idle - clpc->idle[cpu];
		valid = clpc->wall[cpu];
		clpc->wall[cpu] = wall;
		clpc->idle[cpu] = idle;
		if (valid && dw && di <= dw)
			busy = max_t(unsigned int, busy,
				     div64_u64((dw - di) * 100, dw));
	}
	cl->busy = busy;
}

static void apple_clpc_sample_cluster(struct apple_clpc *clpc,
				      struct apple_clpc_cluster *cl)
{
	struct cpufreq_policy *policy;
	unsigned int busy, q, rail;

	apple_clpc_sample_busy(clpc, cl);
	busy = cl->busy;

	cl->level = 0;
	policy = cpufreq_cpu_get(cl->policy_cpu);
	if (policy) {
		if (policy->cpuinfo.max_freq)
			cl->level = policy->cur * 100 / policy->cpuinfo.max_freq;
		cpufreq_cpu_put(policy);
	}

	q = min_t(unsigned int, cl->level / (100 / APPLE_CLPC_LEVELS),
		  APPLE_CLPC_LEVELS - 1);
	if (busy < APPLE_CLPC_BUSY_PCT)
		cl->streak = 0;
	else if (cl->streak < clpc->hw->sustain)
		cl->streak++;
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		cl->request[rail] = busy >= APPLE_CLPC_BUSY_PCT &&
				    cl->streak >= clpc->hw->sustain ?
				   apple_clpc_cpu_state(clpc, rail, q) : 0;
}

static unsigned int apple_clpc_vote(struct apple_clpc_vote *vote,
				    unsigned int demand, unsigned long now)
{
	if (demand) {
		vote->last_demand = now;
		vote->state = demand;
	} else if (time_after_eq(now, vote->last_demand +
					      msecs_to_jiffies(APPLE_CLPC_HOLD_MS))) {
		vote->state = 0;
	}

	return vote->state;
}

static u32 apple_clpc_floor(struct apple_clpc *clpc, const unsigned int *state)
{
	u32 floor = 0;
	unsigned int rail;

	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		floor |= (state[rail] & 0xf) <<
			 APPLE_PMC_RAIL_SHIFT(clpc->hw->rail[rail]);

	return floor;
}

static enum cpuhp_state apple_clpc_cpuhp_state;

static unsigned long apple_clpc_idle_step(void)
{
	return max_t(unsigned long, msecs_to_jiffies(APPLE_CLPC_SAMPLE_MS), 2);
}

static void apple_clpc_idle_timer_fn(struct timer_list *timer)
{
	struct apple_clpc_idle_timer *t = timer_container_of(t, timer, timer);
	struct apple_clpc *clpc = t->clpc;
	unsigned int cpu = smp_processor_id();
	unsigned long step = apple_clpc_idle_step();
	u64 wall, idle, dw, di;

	if (READ_ONCE(clpc->stopped) || !atomic_read(&clpc->idle_armed))
		return;

	idle = get_cpu_idle_time(cpu, &wall, 0);
	if (wall < t->last_wall || idle < t->last_idle) {
		t->last_wall = wall;
		t->last_idle = idle;
		mod_timer(&t->timer, jiffies + step);
		return;
	}
	dw = wall - t->last_wall;
	if (dw < APPLE_CLPC_SAMPLE_MS * 1000ULL) {
		mod_timer(&t->timer, jiffies + step);
		return;
	}
	di = idle - t->last_idle;
	t->last_wall = wall;
	t->last_idle = idle;

	/*
	 * A CPU that only woke briefly for an IRQ or short timer stayed mostly
	 * idle (or an out-of-order iowait/idle read yielded di > dw); re-arm
	 * its pinned deferrable timer without waking a kworker.
	 */
	if (di > dw || (dw - di) * 100 < dw * APPLE_CLPC_BUSY_PCT) {
		mod_timer(&t->timer, jiffies + step);
		return;
	}

	if (atomic_cmpxchg(&clpc->idle_armed, 1, 0) == 1)
		mod_delayed_work(system_freezable_power_efficient_wq,
				 &clpc->work, 0);
}

static int apple_clpc_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct apple_clpc *clpc = hlist_entry_safe(node, struct apple_clpc,
						   cpuhp_node);
	struct apple_clpc_idle_timer *t;

	if (!clpc || !clpc->idle_timers)
		return 0;

	guard(mutex)(&clpc->idle_lock);
	cpumask_set_cpu(cpu, &clpc->idle_online_cpus);
	if (READ_ONCE(clpc->stopped) || !atomic_read(&clpc->idle_armed))
		return 0;

	t = per_cpu_ptr(clpc->idle_timers, cpu);
	timer_delete_sync(&t->timer);
	t->last_idle = get_cpu_idle_time(cpu, &t->last_wall, 0);
	t->timer.expires = jiffies + apple_clpc_idle_step();
	add_timer_on(&t->timer, cpu);
	return 0;
}

static int apple_clpc_cpu_down(unsigned int cpu, struct hlist_node *node)
{
	struct apple_clpc *clpc = hlist_entry_safe(node, struct apple_clpc,
						   cpuhp_node);

	if (!clpc || !clpc->idle_timers)
		return 0;

	/* Delete before CPU teardown migrates the pinned timer elsewhere. */
	guard(mutex)(&clpc->idle_lock);
	cpumask_clear_cpu(cpu, &clpc->idle_online_cpus);
	timer_delete_sync(&per_cpu_ptr(clpc->idle_timers, cpu)->timer);
	return 0;
}

/*
 * Arm a pinned deferrable timer on every online CPU. Idle CPUs stay asleep
 * (TIMER_DEFERRABLE never sends an IPI or wakes a NO_HZ CPU), while any busy
 * CPU runs the callback once its tick resumes and kicks active CLPC sampling.
 */
static void apple_clpc_arm_idle(struct apple_clpc *clpc)
{
	unsigned long expires = jiffies + apple_clpc_idle_step();
	unsigned int cpu;

	/*
	 * Serialize with CPU hotplug callbacks and halt via idle_lock (ordered
	 * strictly inside cpuhp and sample_lock) rather than taking
	 * cpus_read_lock() inside sample_lock.
	 */
	guard(mutex)(&clpc->idle_lock);
	atomic_set(&clpc->idle_armed, 0);
	smp_mb();
	for_each_possible_cpu(cpu) {
		struct apple_clpc_idle_timer *t = per_cpu_ptr(clpc->idle_timers, cpu);

		timer_delete_sync(&t->timer);
	}

	for_each_cpu(cpu, &clpc->idle_online_cpus) {
		struct apple_clpc_idle_timer *t = per_cpu_ptr(clpc->idle_timers, cpu);

		t->last_idle = get_cpu_idle_time(cpu, &t->last_wall, 0);
	}
	atomic_set(&clpc->idle_armed, 1);
	smp_mb();
	for_each_cpu(cpu, &clpc->idle_online_cpus) {
		struct apple_clpc_idle_timer *t = per_cpu_ptr(clpc->idle_timers, cpu);

		t->timer.expires = expires;
		add_timer_on(&t->timer, cpu);
	}
}

static void apple_clpc_queue(struct apple_clpc *clpc, bool idle)
{
	if (READ_ONCE(clpc->stopped))
		return;

	if (!clpc->has_pmc) {
		unsigned long delay;

		cancel_delayed_work(&clpc->idle_work);
		if (clpc->low_power_khz && clpc->low_power_mw &&
		    READ_ONCE(clpc->low_power))
			delay = msecs_to_jiffies(APPLE_CLPC_POWER_MS);
		else if (!cpumask_equal(clpc->covered, cpu_possible_mask))
			delay = HZ;
		else
			return;
		mod_delayed_work(system_freezable_power_efficient_wq, &clpc->work, delay);
		return;
	}

	/*
	 * A held vote has to be sampled on time so that it drops once demand
	 * stops; with no vote held, sampling can wait for a CPU that wakes up
	 * anyway rather than waking an idle one every period. On SoCs with
	 * @idle_on_time (and without nohz_full, which can stop ticks on busy
	 * CPUs), arm a per-CPU deferrable timer across online CPUs so an idle
	 * system takes zero CLPC wakeups. Preserve any fresh client demand
	 * already queued on clpc->work.
	 */
	if (idle && clpc->idle_timers && !tick_nohz_full_enabled()) {
		cancel_delayed_work(&clpc->idle_work);
		if (delayed_work_pending(&clpc->work))
			return;
		apple_clpc_arm_idle(clpc);
		return;
	}

	atomic_set(&clpc->idle_armed, 0);
	idle = idle && !clpc->hw->idle_on_time;
	/* A demand edge can queue work while the deferrable pass is pending.
	 * Keep one sampling cadence once the serialized pass settles.
	 */
	cancel_delayed_work(idle ? &clpc->work : &clpc->idle_work);
	queue_delayed_work(system_freezable_power_efficient_wq,
			   idle ? &clpc->idle_work : &clpc->work,
			   msecs_to_jiffies(APPLE_CLPC_SAMPLE_MS));
}

/*
 * Whether the GPU streams work: graphics has stayed powered for the SoC's
 * gpu_sustain_ms, with no sample in between that found it off and no gap in
 * sampling long enough to have missed that. The firmware powers graphics off
 * a few milliseconds after the GPU idles, so a frame-paced load such as video
 * playback powers it up and down with each frame and does not count. A failed
 * read counts as powered, so read errors alone do not end a stream that is
 * already under way.
 */
static bool apple_clpc_gpu_streaming(struct apple_clpc *clpc, unsigned long now)
{
	bool gap = time_after(now, clpc->gpu_sampled +
				   msecs_to_jiffies(2 * APPLE_CLPC_SAMPLE_MS));
	u32 status;
	int err;

	clpc->gpu_sampled = now;
	err = regmap_read(clpc->gpu_pmgr, clpc->gpu_offset, &status);
	if (err) {
		clpc->gpu_read_errors++;
		dev_warn_once(clpc->dev,
			      "graphics power state read failed (%d); holding the GPU floor\n",
			      err);
	}
	if (!err && (status & GENMASK(7, 4)) != GENMASK(7, 4)) {
		clpc->gpu_on = false;
		return false;
	}

	if (!clpc->gpu_on || gap)
		clpc->gpu_on_since = now;
	clpc->gpu_on = true;
	return time_after_eq(now, clpc->gpu_on_since +
				  msecs_to_jiffies(clpc->hw->gpu_sustain_ms));
}

/* Samples in a row that light GPU work must stay dense for: one 60 Hz frame. */
#define APPLE_CLPC_GPU_DENSE_SAMPLES	4

/*
 * Whether light GPU work is dense: at least gpu_dense submissions in each of
 * the last GPU_DENSE_SAMPLES sampling periods. Such work keeps graphics
 * powering up and down between submissions, so it never counts as streaming,
 * yet each of its submissions waits on the fabric as streaming work does.
 * Work paced by frames leaves a quieter period in every frame and stays below.
 * @sequence counts every submission the GPU driver published; a sample late by
 * more than one period ends a streak, as its count no longer covers one period.
 */
static bool apple_clpc_gpu_dense_step(unsigned int *streak, u64 *last,
				      u64 sequence, unsigned int threshold,
				      bool late)
{
	u64 submissions = sequence - *last;

	*last = sequence;
	if (!threshold || late || submissions < threshold)
		*streak = 0;
	else if (*streak < APPLE_CLPC_GPU_DENSE_SAMPLES)
		(*streak)++;
	return *streak >= APPLE_CLPC_GPU_DENSE_SAMPLES;
}

static bool apple_clpc_gpu_dense(struct apple_clpc *clpc, bool late)
{
	u64 updated, sequence;

	if (!clpc->gpu || !clpc->hw->gpu_dense)
		return false;
	apple_clpc_gpu_active(clpc->gpu, &updated, &sequence);
	return apple_clpc_gpu_dense_step(&clpc->gpu_dense_streak,
					 &clpc->gpu_sequence, sequence,
					 clpc->hw->gpu_dense, late);
}

/*
 * Whether the PMP holds the GPU to a power budget: while GPU work streams, it
 * votes the DCS below the top state, which it does under sustained heavy
 * rendering but not for streams of light submissions. The GPU floor would
 * then only take power and thermal headroom from the GPU, so it stays
 * released for GPU_BACKOFF_MS after the PMP last did so.
 */
static bool apple_clpc_gpu_limited(struct apple_clpc *clpc, unsigned long now)
{
	unsigned int rail = clpc->hw->rail[APPLE_CLPC_DCS];
	unsigned int dcs;

	if (!clpc->hw->pmp_backoff)
		return false;

	dcs = (apple_pmc_floor_read(clpc->pmc, clpc->hw->pmp_agent) >>
	       APPLE_PMC_RAIL_SHIFT(rail)) & 0xf;
	if (dcs < clpc->hw->gpu[APPLE_CLPC_DCS]) {
		clpc->gpu_limited = true;
		clpc->gpu_limited_at = now;
	}
	return clpc->gpu_limited &&
	       time_before(now, clpc->gpu_limited_at +
				msecs_to_jiffies(APPLE_CLPC_GPU_BACKOFF_MS));
}

/* Count the GPU floor's releases and returns while GPU work streams. */
static void apple_clpc_gpu_floor_count(struct apple_clpc *clpc, bool released)
{
	if (released == clpc->gpu_floor_released)
		return;
	if (released)
		clpc->gpu_floor_releases++;
	else
		clpc->gpu_floor_returns++;
	clpc->gpu_floor_released = released;
}

/*
 * The GPU driver's input, where it drives demand instead of the GFX power
 * state: while the driver reports work, every rail at @gpu_busy for the
 * reported busy share when @gpu_busy has a non-zero top DCS state, or at the
 * top of @map (or @gpu where that is higher) otherwise.
 */
static bool apple_clpc_gpu_input_demand(const struct apple_clpc_hw *hw,
					struct apple_clpc_gpu *gpu,
					unsigned int *demand)
{
	bool has_busy = hw->gpu_busy[APPLE_CLPC_DCS][APPLE_CLPC_LEVELS - 1];
	u64 updated, sequence;
	unsigned int level = 0, rail;

	if (!gpu || hw->gpu_demand ||
	    !apple_clpc_gpu_active(gpu, &updated, &sequence))
		return false;
	if (has_busy)
		level = apple_clpc_gpu_level(hw, gpu);
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++) {
		unsigned int state = has_busy ? hw->gpu_busy[rail][level] :
				     max(hw->map[rail][APPLE_CLPC_LEVELS - 1],
					 hw->gpu[rail]);

		demand[rail] = max(demand[rail], state);
	}
	return true;
}

/*
 * Hold the ANE's cluster frequency floor from the first sample that sees ANE
 * work until the vote hold after the last one. The request is added on first
 * use: cpufreq may register its policies after the CLPC probes.
 */
static void apple_clpc_ane_cluster(struct apple_clpc *clpc, bool active,
				   unsigned long now)
{
	unsigned int khz = 0;

	if (active)
		clpc->ane_until = now + msecs_to_jiffies(APPLE_CLPC_HOLD_MS);
	if (time_before(now, clpc->ane_until))
		khz = clpc->ane_cluster_khz;
	else
		clpc->ane_until = now;
	if (khz && !freq_qos_request_active(&clpc->ane_qos) && clpc->ane_cpu >= 0) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(clpc->ane_cpu);

		if (policy) {
			if (freq_qos_add_request(&policy->constraints, &clpc->ane_qos,
						 FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE) < 0)
				clpc->update_errors++;
			cpufreq_cpu_put(policy);
		}
	}
	if (khz == clpc->ane_qos_khz || !freq_qos_request_active(&clpc->ane_qos))
		return;
	if (freq_qos_update_request(&clpc->ane_qos, khz ?: FREQ_QOS_MIN_DEFAULT_VALUE) < 0)
		clpc->update_errors++;
	else
		clpc->ane_qos_khz = khz;
}

/* The next frequency in @policy's table above (@up) or below @khz, or @khz. */
static unsigned int apple_clpc_step_khz(struct cpufreq_policy *policy,
					unsigned int khz, bool up)
{
	struct cpufreq_frequency_table *pos;
	unsigned int best = khz;

	if (!policy->freq_table)
		return khz;
	cpufreq_for_each_valid_entry(pos, policy->freq_table) {
		if (up ? pos->frequency > khz && (best == khz || pos->frequency < best) :
			 pos->frequency < khz && (best == khz || pos->frequency > best))
			best = pos->frequency;
	}
	return best;
}

/*
 * The low-power platform-profile frequency ceiling for @policy: the first
 * table frequency at or above @low_power_khz (matching macOS Low Power Mode's
 * round-up), or policy max if the cluster is slower or @low_power_khz is 0.
 */
static unsigned int apple_clpc_low_power_top_khz(struct cpufreq_policy *policy,
						 unsigned int low_power_khz)
{
	unsigned int top;

	if (!low_power_khz)
		return policy->cpuinfo.max_freq;
	top = apple_clpc_step_khz(policy, low_power_khz - 1, true);
	if (top < low_power_khz)
		top = policy->cpuinfo.max_freq;
	return top;
}

/*
 * The SMC of the apple,smc phandle, once its driver is bound; NULL before.
 * Callers test IS_ENABLED(CONFIG_MFD_MACSMC) first, so a kernel without the
 * SMC driver links without the apple_smc_*() calls.
 */
static struct apple_smc *apple_clpc_smc(struct apple_clpc *clpc)
{
	struct platform_device *pdev;

	if (!clpc->smc && clpc->smc_np) {
		pdev = of_find_device_by_node(clpc->smc_np);
		if (pdev && device_is_bound(&pdev->dev) &&
		    device_link_add(clpc->dev, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
			clpc->smc = dev_get_drvdata(&pdev->dev);
		if (pdev)
			put_device(&pdev->dev);
	}
	return clpc->smc;
}

/* The package power the SMC reports (PHPC, a float in W), in mW. */
static int apple_clpc_read_power(struct apple_clpc *clpc)
{
	int exp, ret;
	u64 mw;
	u32 f;

	if (!IS_ENABLED(CONFIG_MFD_MACSMC) || !apple_clpc_smc(clpc))
		return -ENODEV;
	ret = apple_smc_read_u32(clpc->smc, SMC_KEY(PHPC), &f);
	if (ret)
		return ret;
	exp = (f >> 23) & 0xff;
	if ((f >> 31) || exp == 0xff)
		return -ENODATA;
	if (!exp)
		return 0;
	mw = ((u64)(f & GENMASK(22, 0)) | BIT(23)) * 1000;
	exp -= 150;
	return exp >= 0 ? INT_MAX : mw >> -exp;
}

/*
 * Whether cluster a's ceiling takes the next step before cluster b's: down,
 * the higher ceiling (the faster cluster on a tie); up, the lower (the
 * slower on a tie). So the clusters come down and go back up together.
 */
static bool apple_clpc_steps_first(unsigned int a_khz, unsigned int a_max,
				   unsigned int b_khz, unsigned int b_max, bool up)
{
	if (a_khz != b_khz)
		return up ? a_khz < b_khz : a_khz > b_khz;
	return up ? a_max < b_max : a_max > b_max;
}

/*
 * Pick which cluster takes @step (-1 down, +1 up) for the low-power package
 * power loop, and return its next frequency in @out_khz. Stepping down skips
 * clusters below APPLE_CLPC_BUSY_PCT so a GPU or display load holding PHPC
 * above @low_power_mw does not walk idle CPUs down to their minimum OPP;
 * stepping back up restores every capped cluster once PHPC drops.
 */
static int apple_clpc_pick_profile_step(struct apple_clpc *clpc,
					struct cpufreq_policy *const *policy,
					int step, unsigned int *out_khz)
{
	unsigned int i, khz, pick_khz = 0;
	int pick = -1;

	if (!step)
		return -1;
	for (i = 0; i < clpc->nr_clusters; i++) {
		struct apple_clpc_cluster *cl = &clpc->clusters[i];

		if (!policy[i])
			continue;
		if (step < 0 && cl->busy < APPLE_CLPC_BUSY_PCT)
			continue;
		khz = apple_clpc_step_khz(policy[i], cl->max_khz, step > 0);
		if (khz == cl->max_khz || khz > cl->top_khz)
			continue;
		if (pick < 0 ||
		    apple_clpc_steps_first(cl->max_khz, policy[i]->cpuinfo.max_freq,
					   clpc->clusters[pick].max_khz,
					   policy[pick]->cpuinfo.max_freq, step > 0)) {
			pick = i;
			pick_khz = khz;
		}
	}
	if (pick >= 0)
		*out_khz = pick_khz;
	return pick;
}

/*
 * The platform profile's ceilings. In the low-power profile every cluster
 * stays at or below apple_clpc_low_power_top_khz(). When @low_power_mw is set,
 * each new package power reading above it takes one table step off one busy
 * cluster (busy >= APPLE_CLPC_BUSY_PCT), and each one below 90% of it puts one
 * back on any capped cluster, in the order of apple_clpc_steps_first().
 * Elsewhere there is no ceiling.
 */
static void apple_clpc_profile_limit(struct apple_clpc *clpc)
{
	bool low = READ_ONCE(clpc->low_power);
	struct cpufreq_policy *policy[APPLE_CLPC_MAX_CLUSTERS] = {};
	unsigned int i, pick_khz = 0;
	int mw, step = 0, pick;

	if (!clpc->low_power_khz || time_before(jiffies, clpc->power_next))
		return;
	clpc->power_next = jiffies + msecs_to_jiffies(APPLE_CLPC_POWER_MS);
	if (low && clpc->low_power_mw && clpc->smc_np) {
		mw = apple_clpc_read_power(clpc);
		clpc->power_reads++;
		/* The SMC updates the reading less often: act on a new one only. */
		if (mw >= 0 && mw != clpc->power_mw) {
			clpc->power_updates++;
			if (mw > (int)clpc->low_power_mw)
				step = -1;
			else if (mw < (int)clpc->low_power_mw * 9 / 10)
				step = 1;
		}
		clpc->power_mw = mw;
	}
	for (i = 0; i < clpc->nr_clusters; i++) {
		struct apple_clpc_cluster *cl = &clpc->clusters[i];

		if (!clpc->has_pmc && low && clpc->low_power_mw)
			apple_clpc_sample_busy(clpc, cl);
		policy[i] = cpufreq_cpu_get(cl->policy_cpu);
		if (!policy[i])
			continue;
		if (!freq_qos_request_active(&cl->max_qos) &&
		    freq_qos_add_request(&policy[i]->constraints, &cl->max_qos, FREQ_QOS_MAX,
					 FREQ_QOS_MAX_DEFAULT_VALUE) < 0)
			clpc->update_errors++;
		cl->top_khz = apple_clpc_low_power_top_khz(policy[i],
							   clpc->low_power_khz);
		if (!low)
			cl->max_khz = 0;
		else if (!cl->max_khz || !clpc->low_power_mw ||
			 cl->max_khz > cl->top_khz)
			cl->max_khz = cl->top_khz;
	}
	pick = apple_clpc_pick_profile_step(clpc, policy, step, &pick_khz);
	if (pick >= 0)
		clpc->clusters[pick].max_khz = pick_khz;
	for (i = 0; i < clpc->nr_clusters; i++) {
		struct apple_clpc_cluster *cl = &clpc->clusters[i];

		if (policy[i] && freq_qos_request_active(&cl->max_qos) &&
		    freq_qos_update_request(&cl->max_qos, cl->max_khz ?:
					    FREQ_QOS_MAX_DEFAULT_VALUE) < 0)
			clpc->update_errors++;
		if (policy[i])
			cpufreq_cpu_put(policy[i]);
	}
}

static void apple_clpc_sample(struct apple_clpc *clpc)
{
	unsigned int demand[APPLE_CLPC_RAILS] = {};
	unsigned int state[APPLE_CLPC_RAILS] = {};
	unsigned long now = jiffies;
	unsigned int i, rail;
	u32 reason = 0, floor = 0;

	atomic_set(&clpc->idle_armed, 0);
	clpc->samples++;
	if (!cpumask_equal(clpc->covered, cpu_possible_mask) &&
	    (!clpc->has_pmc || time_after_eq(now, clpc->next_scan))) {
		apple_clpc_find_clusters(clpc);
		clpc->next_scan = now + HZ;
	}

	if (clpc->has_pmc) {
		for (i = 0; i < clpc->nr_clusters; i++) {
			struct apple_clpc_cluster *cl = &clpc->clusters[i];

			apple_clpc_sample_cluster(clpc, cl);
			for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
				demand[rail] = max(demand[rail], cl->request[rail]);
		}
		for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
			if (demand[rail])
				reason |= APPLE_CLPC_REASON_CPU;
		if (apple_clpc_gpu_input_demand(clpc->hw, clpc->gpu, demand))
			reason |= APPLE_CLPC_REASON_HOST_GPU;
		if (apple_clpc_ane_input_demand(clpc, demand))
			reason |= APPLE_CLPC_REASON_ANE;
		apple_clpc_ane_cluster(clpc, reason & APPLE_CLPC_REASON_ANE, now);
		if (clpc->gpu_pmgr) {
			/* Both run every sample: each tracks state across samples. */
			bool late = time_after(now, clpc->gpu_sampled +
						    msecs_to_jiffies(2 * APPLE_CLPC_SAMPLE_MS));
			bool dense = apple_clpc_gpu_dense(clpc, late);
			bool streaming = apple_clpc_gpu_streaming(clpc, now);

			if (streaming || dense) {
				bool released = apple_clpc_gpu_limited(clpc, now);

				apple_clpc_gpu_floor_count(clpc, released);
				if (!released) {
					reason |= streaming ? APPLE_CLPC_REASON_STREAMING_GPU :
							      APPLE_CLPC_REASON_DENSE_GPU;
					if (streaming)
						clpc->gpu_streaming_samples++;
					else
						clpc->gpu_dense_samples++;
					for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
						demand[rail] = max(demand[rail],
								   clpc->hw->gpu[rail]);
				}
			}
		}

		for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
			state[rail] = apple_clpc_vote(&clpc->vote[rail], demand[rail],
						      now);

		floor = apple_clpc_floor(clpc, state);
		for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
			if (state[rail] > demand[rail])
				reason |= APPLE_CLPC_REASON_HOLD;
		clpc->last_floor = floor;
		clpc->last_reason = reason;
		clpc->last_vote_ns = ktime_get_mono_fast_ns();
		if (apple_pmc_floor_update(clpc->pmc, clpc->agent, clpc->mask, floor))
			clpc->update_errors++;
	}

	apple_clpc_profile_limit(clpc);

	/*
	 * Sample on time while graphics is powered, to see it power off, and in
	 * low power when an SMC package power target is active. A static
	 * ceiling is applied synchronously on profile change and a High Power
	 * only profile has nothing to poll at idle, so both stay deferrable.
	 */
	apple_clpc_queue(clpc, !floor && !clpc->gpu_on &&
			 !clpc->ane_qos_khz &&
			 !(clpc->low_power_khz && clpc->low_power_mw &&
			   READ_ONCE(clpc->low_power)));
}

static void apple_clpc_run(struct apple_clpc *clpc)
{
	mutex_lock(&clpc->sample_lock);
	if (!READ_ONCE(clpc->stopped))
		apple_clpc_sample(clpc);
	mutex_unlock(&clpc->sample_lock);
}

static void apple_clpc_work(struct work_struct *work)
{
	apple_clpc_run(container_of(to_delayed_work(work), struct apple_clpc, work));
}

static void apple_clpc_idle_work(struct work_struct *work)
{
	apple_clpc_run(container_of(to_delayed_work(work), struct apple_clpc, idle_work));
}

/*
 * apple,cpu-busy-floor: the DCS and fabric states, in that order, for a CPU
 * cluster busy in the top quarter of its frequency range outside the low-power
 * profile, where the board's measurement differs from the SoC's table. A bad
 * value costs the override, not the driver.
 */
static void apple_clpc_cpu_top_init(struct apple_clpc *clpc)
{
	u32 top[APPLE_CLPC_RAILS];
	unsigned int rail;
	int ret;

	if (!of_property_present(clpc->dev->of_node, "apple,cpu-busy-floor"))
		return;
	ret = of_property_read_u32_array(clpc->dev->of_node, "apple,cpu-busy-floor",
					 top, APPLE_CLPC_RAILS);
	for (rail = 0; !ret && rail < APPLE_CLPC_RAILS; rail++)
		if (top[rail] > 0xf)
			ret = -ERANGE;
	if (ret || of_property_count_u32_elems(clpc->dev->of_node,
					       "apple,cpu-busy-floor") != APPLE_CLPC_RAILS) {
		dev_warn(clpc->dev, "invalid apple,cpu-busy-floor; using the SoC's states\n");
		return;
	}
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		clpc->cpu_top[rail] = top[rail];
	clpc->cpu_top_set = true;
	dev_info(clpc->dev, "busy CPU floor: DCS %u, fabric %u\n",
		 clpc->cpu_top[APPLE_CLPC_DCS], clpc->cpu_top[APPLE_CLPC_SOC]);
}

static int apple_clpc_status_show(struct seq_file *s, void *unused)
{
	struct apple_clpc *clpc = s->private;
	unsigned int i, rail;

	if (clpc->has_pmc) {
		seq_printf(s, "agent: %u floor: %#010x (saved %#010x)\n", clpc->agent,
			   apple_pmc_floor_read(clpc->pmc, clpc->agent),
			   clpc->saved_floor);
		for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
			seq_printf(s, "%s: rail %u state %u enabled %u map %u,%u,%u,%u\n",
				   rail == APPLE_CLPC_DCS ? "dcs" : "soc",
				   clpc->hw->rail[rail], clpc->vote[rail].state,
				   apple_pmc_voter_enabled(clpc->pmc, clpc->agent,
							   clpc->hw->rail[rail]),
				   apple_clpc_cpu_state(clpc, rail, 0),
				   apple_clpc_cpu_state(clpc, rail, 1),
				   apple_clpc_cpu_state(clpc, rail, 2),
				   apple_clpc_cpu_state(clpc, rail, 3));
		seq_printf(s, "last_vote: %#x reason_bits %#x sampled_ns %llu (cpu=1 host_gpu=2 streaming_gpu=4 dense_gpu=8 hold=16 ane=32)\n",
			   READ_ONCE(clpc->last_floor), READ_ONCE(clpc->last_reason),
			   READ_ONCE(clpc->last_vote_ns));
	}
	if (clpc->gpu) {
		struct apple_clpc_gpu *gpu = clpc->gpu;
		unsigned int busy, level;
		unsigned long flags;
		u64 updated, sequence;
		bool active = apple_clpc_gpu_active(gpu, &updated, &sequence);
		bool busy_valid;

		/* Show the level the last sample chose; reading must not move it. */
		spin_lock_irqsave(&gpu->lock, flags);
		busy = gpu->busy;
		busy_valid = gpu->busy_valid;
		level = gpu->level;
		spin_unlock_irqrestore(&gpu->lock, flags);
		if (clpc->hw->gpu_busy[APPLE_CLPC_DCS][APPLE_CLPC_LEVELS - 1])
			seq_printf(s, "host_gpu busy: %u permille (%s) level %u\n",
				   busy, busy_valid ? "reported" : "none", level);
		else
			seq_printf(s, "host_gpu busy: %u permille (%s)\n",
				   busy, busy_valid ? "reported" : "none");
		seq_printf(s, "host_gpu: active %u request_ns %llu sequence %llu\n",
			   active, updated, sequence);
	}
	if (clpc->ane) {
		struct apple_clpc_ane *ane = clpc->ane;
		unsigned long flags;
		u64 updated, sequence;
		bool active;

		spin_lock_irqsave(&ane->lock, flags);
		active = ane->active;
		updated = ane->updated_ns;
		sequence = ane->sequence;
		spin_unlock_irqrestore(&ane->lock, flags);
		seq_printf(s, "ane: active %u request_ns %llu sequence %llu\n",
			   active, updated, sequence);
	}
	seq_printf(s, "samples: %lu update errors: %lu\n", clpc->samples,
		   clpc->update_errors);
	if (clpc->low_power_khz) {
		int power_mw = clpc->power_mw;

		if (!clpc->low_power_mw && clpc->smc_np) {
			mutex_lock(&clpc->sample_lock);
			if (!READ_ONCE(clpc->stopped)) {
				int mw = apple_clpc_read_power(clpc);

				if (mw >= 0)
					power_mw = mw;
			}
			mutex_unlock(&clpc->sample_lock);
		}
		seq_printf(s, "profile: %s power %d mW (%lu new of %lu readings) ceilings (kHz):",
			   clpc->profile == PLATFORM_PROFILE_LOW_POWER ? "low-power" :
			   clpc->profile == PLATFORM_PROFILE_PERFORMANCE ? "performance" :
			   "balanced", power_mw, clpc->power_updates, clpc->power_reads);
		for (i = 0; i < clpc->nr_clusters; i++)
			seq_printf(s, " cpu%u %u", clpc->clusters[i].policy_cpu,
				   clpc->clusters[i].max_khz);
		seq_puts(s, "\n");
	}
	if (clpc->high_power)
		seq_printf(s, "smc high power: %s (%u records sent)\n",
			   clpc->high_power_sent ? "on" : "off", clpc->high_power_seq);
	if (clpc->gpu_pmgr) {
		seq_printf(s, "gpu: powered %u for %u ms, pmp limited ", clpc->gpu_on,
			   clpc->gpu_on ? jiffies_to_msecs(jiffies - clpc->gpu_on_since) : 0);
		if (clpc->gpu_limited)
			seq_printf(s, "%u ms ago\n",
				   jiffies_to_msecs(jiffies - clpc->gpu_limited_at));
		else
			seq_puts(s, "never\n");
		seq_printf(s, "gpu floor releases: %lu returns: %lu\n",
			   clpc->gpu_floor_releases, clpc->gpu_floor_returns);
		seq_printf(s, "gpu floor samples: streaming %lu dense %lu\n",
			   clpc->gpu_streaming_samples, clpc->gpu_dense_samples);
		seq_printf(s, "gpu read errors: %lu\n", clpc->gpu_read_errors);
	}
	if (clpc->has_pmc) {
		for (i = 0; i < clpc->nr_clusters; i++)
			seq_printf(s, "cluster cpu%u: busy %u%% level %u%% -> dcs %u soc %u\n",
				   clpc->clusters[i].policy_cpu, clpc->clusters[i].busy,
				   clpc->clusters[i].level,
				   clpc->clusters[i].request[APPLE_CLPC_DCS],
				   clpc->clusters[i].request[APPLE_CLPC_SOC]);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_clpc_status);

static int apple_clpc_ane_states_show(struct seq_file *s, void *unused)
{
	struct apple_clpc *clpc = s->private;

	guard(mutex)(&clpc->sample_lock);
	seq_printf(s, "%u %u\n", clpc->ane_states[APPLE_CLPC_DCS],
		   clpc->ane_states[APPLE_CLPC_SOC]);
	return 0;
}

static int apple_clpc_ane_states_open(struct inode *inode, struct file *file)
{
	return single_open(file, apple_clpc_ane_states_show, inode->i_private);
}

/* The top measured CPU/GPU states bound the rail's supported range. */
static void apple_clpc_ane_states_set(struct apple_clpc *clpc, const int *states)
{
	unsigned int rail, top;

	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++) {
		top = max(clpc->hw->map[rail][APPLE_CLPC_LEVELS - 1], clpc->hw->gpu[rail]);
		clpc->ane_states[rail] = clamp(states[rail], 0, (int)top);
	}
}

static ssize_t apple_clpc_ane_states_write(struct file *file, const char __user *buf,
					   size_t count, loff_t *ppos)
{
	struct apple_clpc *clpc = ((struct seq_file *)file->private_data)->private;
	char text[48], extra;
	int states[APPLE_CLPC_RAILS];

	if (!count || count >= sizeof(text))
		return -EINVAL;
	if (copy_from_user(text, buf, count))
		return -EFAULT;
	text[count] = '\0';
	if (sscanf(text, "%d %d %c", &states[APPLE_CLPC_DCS],
		   &states[APPLE_CLPC_SOC], &extra) != 2)
		return -EINVAL;
	guard(mutex)(&clpc->sample_lock);
	apple_clpc_ane_states_set(clpc, states);
	if (!READ_ONCE(clpc->stopped))
		mod_delayed_work(system_freezable_power_efficient_wq, &clpc->work, 0);
	return count;
}

static const struct file_operations apple_clpc_ane_states_fops = {
	.owner = THIS_MODULE,
	.open = apple_clpc_ane_states_open,
	.read = seq_read,
	.write = apple_clpc_ane_states_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void apple_clpc_free_clusters(struct apple_clpc *clpc)
{
	unsigned int i;

	for (i = 0; i < APPLE_CLPC_MAX_CLUSTERS; i++)
		free_cpumask_var(clpc->clusters[i].cpus);
	free_cpumask_var(clpc->covered);
}

/* Stop sampling and give the floor back, before suspend and on removal. */
static void apple_clpc_halt(struct apple_clpc *clpc)
{
	unsigned int rail, i, cpu;

	/*
	 * Each pass queues one of the two works (or arms per-CPU deferrable
	 * timers) unless stopped. Cancel any in-flight pass first, drain every
	 * per-CPU idle timer so none can re-queue work, then cancel once more.
	 */
	WRITE_ONCE(clpc->stopped, true);
	atomic_set(&clpc->idle_armed, 0);
	cancel_delayed_work_sync(&clpc->work);
	cancel_delayed_work_sync(&clpc->idle_work);
	if (clpc->idle_timers) {
		scoped_guard(mutex, &clpc->idle_lock) {
			for_each_possible_cpu(cpu) {
				struct apple_clpc_idle_timer *t =
					per_cpu_ptr(clpc->idle_timers, cpu);

				timer_delete_sync(&t->timer);
			}
		}
	}
	for (i = 0; i < 2; i++) {
		cancel_delayed_work_sync(&clpc->work);
		cancel_delayed_work_sync(&clpc->idle_work);
	}
	if (clpc->has_pmc)
		apple_pmc_floor_update(clpc->pmc, clpc->agent, clpc->mask,
				       clpc->saved_floor);
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		clpc->vote[rail].state = 0;
	if (clpc->ane_qos_khz &&
	    freq_qos_update_request(&clpc->ane_qos, FREQ_QOS_MIN_DEFAULT_VALUE) >= 0)
		clpc->ane_qos_khz = 0;
	clpc->ane_until = jiffies;
}

static int apple_clpc_send_high_power(struct apple_clpc *clpc, bool on);

static void apple_clpc_stop(void *data)
{
	struct apple_clpc *clpc = data;
	unsigned int i;

	mutex_lock(&apple_clpc_clients_lock);
	dev_set_drvdata(clpc->dev, NULL);
	mutex_unlock(&apple_clpc_clients_lock);
	if (clpc->gpu) {
		unsigned long flags;

		spin_lock_irqsave(&clpc->gpu->lock, flags);
		clpc->gpu->wake = NULL;
		spin_unlock_irqrestore(&clpc->gpu->lock, flags);
	}
	apple_clpc_ane_detach(clpc->ane);
	debugfs_remove_recursive(clpc->debugfs);
	WRITE_ONCE(clpc->stopped, true);
	atomic_set(&clpc->idle_armed, 0);
	if (clpc->cpuhp_added) {
		cpuhp_state_remove_instance_nocalls(apple_clpc_cpuhp_state,
						    &clpc->cpuhp_node);
		clpc->cpuhp_added = false;
	}
	apple_clpc_halt(clpc);
	/* The SMC keeps this mode after the driver's profile state is gone. */
	mutex_lock(&clpc->sample_lock);
	if (clpc->high_power && clpc->high_power_sent)
		apple_clpc_send_high_power(clpc, false);
	mutex_unlock(&clpc->sample_lock);
	if (clpc->policy_nb_added)
		cpufreq_unregister_notifier(&clpc->policy_nb, CPUFREQ_POLICY_NOTIFIER);
	if (freq_qos_request_active(&clpc->ane_qos))
		freq_qos_remove_request(&clpc->ane_qos);
	for (i = 0; i < clpc->nr_clusters; i++)
		if (freq_qos_request_active(&clpc->clusters[i].max_qos))
			freq_qos_remove_request(&clpc->clusters[i].max_qos);
	apple_clpc_free_clusters(clpc);
}

/*
 * The ANE cluster floor and the profile ceilings are requests on cpufreq
 * policies, which go away with the cpufreq driver (a module). Withdraw them
 * then; they are added again once a policy covers the CPUs.
 */
static int apple_clpc_policy_notify(struct notifier_block *nb, unsigned long event,
				    void *data)
{
	struct apple_clpc *clpc = container_of(nb, struct apple_clpc, policy_nb);
	struct cpufreq_policy *policy = data;
	unsigned int i;

	if (event == CPUFREQ_CREATE_POLICY) {
		if (!READ_ONCE(clpc->stopped))
			mod_delayed_work(system_freezable_power_efficient_wq, &clpc->work, 0);
		return NOTIFY_OK;
	}
	if (event != CPUFREQ_REMOVE_POLICY)
		return NOTIFY_DONE;
	guard(mutex)(&clpc->sample_lock);
	for (i = 0; i < clpc->nr_clusters; i++)
		if (cpumask_test_cpu(clpc->clusters[i].policy_cpu, policy->related_cpus)) {
			if (freq_qos_request_active(&clpc->clusters[i].max_qos))
				freq_qos_remove_request(&clpc->clusters[i].max_qos);
			clpc->clusters[i].max_khz = 0;
		}
	if (clpc->ane_cpu < 0 || !cpumask_test_cpu(clpc->ane_cpu, policy->related_cpus))
		return NOTIFY_OK;
	if (freq_qos_request_active(&clpc->ane_qos))
		freq_qos_remove_request(&clpc->ane_qos);
	clpc->ane_qos_khz = 0;
	return NOTIFY_OK;
}

/* The first CPU of /cpus/cpu-map/cluster@index, or -ENODEV. */
static int apple_clpc_cluster_cpu(unsigned int index)
{
	struct device_node *map, *cluster, *core, *cpu;
	char name[16];
	int id = -ENODEV;

	map = of_find_node_by_path("/cpus/cpu-map");
	if (!map)
		return id;
	snprintf(name, sizeof(name), "cluster%u", index);
	cluster = of_get_child_by_name(map, name);
	of_node_put(map);
	if (!cluster)
		return id;
	core = of_get_child_by_name(cluster, "core0");
	of_node_put(cluster);
	if (!core)
		return id;
	cpu = of_parse_phandle(core, "cpu", 0);
	of_node_put(core);
	if (cpu) {
		id = of_cpu_node_to_id(cpu);
		of_node_put(cpu);
	}
	return id;
}

/*
 * macOS tells the SMC its power mode through thermalmonitord's 120-byte zETM
 * record, written at every mode change: a record type (0x1d), a version (1), a
 * count of the writes, zeros, and at byte 112 a float that is 1.0 while High
 * Power mode is on and 0.0 otherwise. With it on, the SMC lets the fans run up
 * to their maximum under a sustained load; in Automatic they level off well
 * below it, whatever the temperature. No other SMC key changes with the mode.
 */
#define APPLE_CLPC_ZETM_SIZE		120
#define APPLE_CLPC_ZETM_TYPE		0x1d
#define APPLE_CLPC_ZETM_VERSION		1
#define APPLE_CLPC_ZETM_HIGH_POWER	112
#define APPLE_CLPC_FLOAT_ONE		0x3f800000

static int apple_clpc_send_high_power(struct apple_clpc *clpc, bool on)
{
	u8 rec[APPLE_CLPC_ZETM_SIZE] = { APPLE_CLPC_ZETM_TYPE, APPLE_CLPC_ZETM_VERSION };
	int ret;

	if (!IS_ENABLED(CONFIG_MFD_MACSMC) || !apple_clpc_smc(clpc))
		return -ENODEV;
	rec[2] = ++clpc->high_power_seq;
	put_unaligned_le32(on ? APPLE_CLPC_FLOAT_ONE : 0, rec + APPLE_CLPC_ZETM_HIGH_POWER);
	ret = apple_smc_write(clpc->smc, SMC_KEY(zETM), rec, sizeof(rec));
	if (ret < 0)
		dev_warn(clpc->dev, "High Power mode %s not sent to the SMC: %d\n",
			 on ? "on" : "off", ret);
	else
		clpc->high_power_sent = on;
	return ret < 0 ? ret : 0;
}

static int apple_clpc_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static int apple_clpc_profile_get(struct device *dev, enum platform_profile_option *profile)
{
	struct apple_clpc *clpc = dev_get_drvdata(dev);

	*profile = READ_ONCE(clpc->profile);
	return 0;
}

static int apple_clpc_profile_set(struct device *dev, enum platform_profile_option profile)
{
	struct apple_clpc *clpc = dev_get_drvdata(dev);
	bool high_power = profile == PLATFORM_PROFILE_PERFORMANCE;
	bool need_work;

	mutex_lock(&clpc->sample_lock);
	/* Performance is macOS's High Power mode; the other two are its Automatic. */
	if (clpc->high_power && high_power != clpc->high_power_sent) {
		int ret = apple_clpc_send_high_power(clpc, high_power);

		if (ret) {
			mutex_unlock(&clpc->sample_lock);
			return ret;
		}
	}
	WRITE_ONCE(clpc->profile, profile);
	WRITE_ONCE(clpc->low_power, profile == PLATFORM_PROFILE_LOW_POWER);
	/* Apply profile ceilings right away so userspace sees the change immediately. */
	clpc->power_next = jiffies;
	if (!READ_ONCE(clpc->stopped) && clpc->low_power_khz) {
		if (!cpumask_equal(clpc->covered, cpu_possible_mask))
			apple_clpc_find_clusters(clpc);
		apple_clpc_profile_limit(clpc);
	}
	need_work = clpc->has_pmc ||
		    (clpc->low_power_khz && clpc->low_power_mw &&
		     profile == PLATFORM_PROFILE_LOW_POWER) ||
		    !cpumask_equal(clpc->covered, cpu_possible_mask);
	mutex_unlock(&clpc->sample_lock);
	if (!READ_ONCE(clpc->stopped) && need_work)
		mod_delayed_work(system_freezable_power_efficient_wq, &clpc->work, 0);
	return 0;
}

static const struct platform_profile_ops apple_clpc_profile_ops = {
	.probe = apple_clpc_profile_probe,
	.profile_get = apple_clpc_profile_get,
	.profile_set = apple_clpc_profile_set,
};

/*
 * Fills @hw->gpu_busy from a DT matrix: APPLE_CLPC_RAILS rows of
 * APPLE_CLPC_LEVELS states, each bounded by the compatible table's rail
 * maximum (and the 4-bit PMC state field).
 */
static int apple_clpc_dt_map(struct apple_clpc_hw *hw, const u32 *cells, int n)
{
	u8 states[APPLE_CLPC_RAILS][APPLE_CLPC_LEVELS];
	unsigned int rail, level;

	if (n != APPLE_CLPC_RAILS * APPLE_CLPC_LEVELS)
		return -EINVAL;
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++) {
		u32 top = max(hw->map[rail][APPLE_CLPC_LEVELS - 1],
			      hw->gpu[rail]);

		for (level = 0; level < APPLE_CLPC_LEVELS; level++) {
			u32 state = cells[rail * APPLE_CLPC_LEVELS + level];

			if (state > top || state > 15)
				return -EINVAL;
			states[rail][level] = state;
		}
	}
	memcpy(hw->gpu_busy, states, sizeof(states));
	return 0;
}

/* The busy shares from which reported GPU work gets the 2nd, 3rd and top column. */
static int apple_clpc_dt_busy(struct apple_clpc_hw *hw, const u32 *cells, int n)
{
	if (n != 3 || cells[0] > cells[1] || cells[1] > cells[2] || cells[2] > 1000)
		return -EINVAL;
	hw->gpu_busy_mid = cells[0];
	hw->gpu_busy_high = cells[1];
	hw->gpu_busy_top = cells[2];
	return 0;
}

/*
 * Applies parsed DT GPU busy-share cells onto a copy of @soc. Any error in
 * either property warns and leaves @soc unchanged so a malformed optional
 * ladder never aborts CLPC probe.
 */
static bool apple_clpc_dt_apply(struct device *dev,
				const struct apple_clpc_hw *soc,
				struct apple_clpc_hw *out,
				const u32 *states, int n_states,
				const u32 *busy, int n_busy)
{
	int err;

	*out = *soc;
	err = n_states < 0 ? n_states : apple_clpc_dt_map(out, states, n_states);
	if (!err)
		err = n_busy < 0 ? n_busy : apple_clpc_dt_busy(out, busy, n_busy);
	if (err) {
		dev_warn(dev,
			 "ignoring invalid GPU busy-share states in the device tree (%d)\n",
			 err);
		*out = *soc;
		return false;
	}
	return true;
}

/* Reads up to @max cells of @name; returns how many, or an error. */
static int apple_clpc_dt_cells(struct device_node *np, const char *name,
			       u32 *cells, int max)
{
	int n = of_property_count_u32_elems(np, name);

	if (n < 0)
		return n;
	if (n > max)
		return -EOVERFLOW;
	return of_property_read_u32_array(np, name, cells, n) ?: n;
}

/*
 * The compatible's table, with the GPU busy-share memory states the node
 * gives (apple,pmc.yaml). Without those properties, or if they are malformed,
 * the compatible's own table applies unchanged.
 */
static const struct apple_clpc_hw *apple_clpc_dt_hw(struct device *dev,
						    const struct apple_clpc_hw *soc)
{
	u32 states[APPLE_CLPC_RAILS * APPLE_CLPC_LEVELS];
	u32 busy[3];
	struct device_node *np = dev->of_node;
	struct apple_clpc_hw copy, *hw;
	int n_states, n_busy;

	if (!of_property_present(np, "apple,gpu-busy-memory-states") &&
	    !of_property_present(np, "apple,gpu-busy-permille"))
		return soc;
	n_states = apple_clpc_dt_cells(np, "apple,gpu-busy-memory-states",
				       states, ARRAY_SIZE(states));
	n_busy = apple_clpc_dt_cells(np, "apple,gpu-busy-permille",
				     busy, ARRAY_SIZE(busy));
	if (!apple_clpc_dt_apply(dev, soc, &copy, states, n_states,
				 busy, n_busy))
		return soc;
	hw = devm_kmemdup(dev, &copy, sizeof(copy), GFP_KERNEL);
	if (!hw)
		return ERR_PTR(-ENOMEM);
	return hw;
}

static void apple_clpc_put_smc_np(void *data)
{
	struct apple_clpc *clpc = data;

	of_node_put(clpc->smc_np);
}

static int apple_clpc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *gpu = NULL;
	struct apple_clpc *clpc;
	unsigned int rail, i;
	bool high_power, has_profile;
	u32 mhz, mw;
	int ret;

	clpc = devm_kzalloc(dev, sizeof(*clpc), GFP_KERNEL);
	if (!clpc)
		return -ENOMEM;

	clpc->dev = dev;
	clpc->pmc = dev->parent;
	clpc->hw = apple_clpc_dt_hw(dev, of_device_get_match_data(dev));
	if (IS_ERR(clpc->hw))
		return PTR_ERR(clpc->hw);

	clpc->has_pmc = clpc->pmc && apple_pmc_rail_count(clpc->pmc) > 0;
	clpc->ane_cpu = -ENODEV;
	clpc->ane_until = jiffies;

	/*
	 * The boot loader copies the ADT's Low Power mode CPU ceiling
	 * (pmgr/clpc cpu-low-power-frequency-max) into the node. Without it,
	 * a SoC's own value applies where it has one; otherwise low-power
	 * ceiling stays unavailable until the machine supplies it.
	 */
	if (!of_property_read_u32(dev->of_node, "apple,low-power-frequency-max-mhz", &mhz))
		clpc->low_power_khz = mhz * 1000;
	else
		clpc->low_power_khz = clpc->hw->low_power_khz;
	if (!of_property_read_u32(dev->of_node, "apple,low-power-mw", &mw))
		clpc->low_power_mw = mw;
	else
		clpc->low_power_mw = clpc->hw->low_power_mw;
	/*
	 * The board DT says whether the machine has High Power mode: then the
	 * performance profile tells the SMC that it is on (the zETM record).
	 * A machine with High Power mode and no low-power ceiling still gets
	 * the platform profile, named "apple-smc" so that userspace keeps its
	 * own low-power CPU budget.
	 */
	high_power = of_property_read_bool(dev->of_node, "apple,high-power-mode");
	/*
	 * A DTB from before the property (the ESP copy can lag the kernel)
	 * keeps High Power mode where this table already gave it.
	 */
	if (!high_power && clpc->hw->high_power_machine)
		high_power = of_machine_is_compatible(clpc->hw->high_power_machine);
	if ((clpc->low_power_khz || high_power) &&
	    IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE)) {
		/*
		 * The SMC is optional: without it (no apple,smc phandle, or a
		 * kernel without the SMC driver) the low-power profile has only
		 * its ceiling and there is no High Power mode.
		 */
		if (IS_ENABLED(CONFIG_MFD_MACSMC)) {
			clpc->smc_np = of_parse_phandle(dev->of_node, "apple,smc", 0);
			if (clpc->smc_np) {
				ret = devm_add_action_or_reset(dev, apple_clpc_put_smc_np, clpc);
				if (ret)
					return ret;
			}
		}
		clpc->high_power = clpc->smc_np && high_power;
	}
	/* High Power mode alone needs the SMC; without it there is no profile. */
	has_profile = (clpc->low_power_khz || clpc->high_power) &&
		      IS_REACHABLE(CONFIG_ACPI_PLATFORM_PROFILE);
	if (!clpc->has_pmc && !has_profile)
		return dev_err_probe(dev, -ENODEV,
				     "parent is not a PMC and no platform profile is available\n");

	if (clpc->has_pmc) {
		apple_clpc_cpu_top_init(clpc);
		/* T6040 firmware keeps GFX powered for its whole lifetime. A separate
		 * host publication/idle input, not that supplier state, drives demand;
		 * the T8132 and M5 (T8142) GPU drivers report through the same input.
		 * On T8140 the same input counts submissions for dense light work.
		 */
		if (clpc->hw->gpu_input || clpc->hw->gpu_dense) {
			clpc->gpu = kzalloc_obj(*clpc->gpu);
			if (!clpc->gpu)
				return -ENOMEM;
			kref_init(&clpc->gpu->ref);
			spin_lock_init(&clpc->gpu->lock);
			ret = devm_add_action_or_reset(dev, apple_clpc_gpu_owner_put, clpc->gpu);
			if (ret)
				return ret;
		}
		if (clpc->hw->ane[APPLE_CLPC_DCS] || clpc->hw->ane[APPLE_CLPC_SOC]) {
			clpc->ane = kzalloc_obj(*clpc->ane);
			if (!clpc->ane)
				return -ENOMEM;
			kref_init(&clpc->ane->ref);
			spin_lock_init(&clpc->ane->lock);
			ret = devm_add_action_or_reset(dev, apple_clpc_ane_owner_put, clpc->ane);
			if (ret)
				return ret;
			for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
				clpc->ane_states[rail] = clpc->hw->ane[rail];
		}
		if (clpc->ane && clpc->hw->ane_cluster_khz &&
		    of_machine_is_compatible(clpc->hw->ane_cluster_machine)) {
			clpc->ane_cpu = apple_clpc_cluster_cpu(clpc->hw->ane_cluster);
			if (clpc->ane_cpu >= 0) {
				clpc->policy_nb.notifier_call = apple_clpc_policy_notify;
				if (!cpufreq_register_notifier(&clpc->policy_nb,
							       CPUFREQ_POLICY_NOTIFIER)) {
					clpc->policy_nb_added = true;
					clpc->ane_cluster_khz = clpc->hw->ane_cluster_khz;
				}
			}
			if (!clpc->ane_cluster_khz)
				dev_dbg(dev, "no ANE cluster floor (cpu-map cluster%u)\n",
					clpc->hw->ane_cluster);
		}
		/* The property is optional: without it, CPU demand alone sets the floors. */
		if (clpc->hw->gpu_demand)
			gpu = of_parse_phandle(dev->of_node, "apple,gpu-power-state", 0);
		if (gpu) {
			ret = of_property_read_u32(gpu, "reg", &clpc->gpu_offset);
			clpc->gpu_pmgr = syscon_node_to_regmap(gpu->parent);
			of_node_put(gpu);
			if (ret)
				return dev_err_probe(dev, ret, "graphics power state has no reg\n");
			if (IS_ERR(clpc->gpu_pmgr))
				return dev_err_probe(dev, PTR_ERR(clpc->gpu_pmgr),
						     "no regmap for the graphics power state\n");
		}

		ret = of_property_read_u32(dev->of_node, "reg", &clpc->agent);
		if (ret)
			return dev_err_probe(dev, ret, "missing voter agent\n");

		for (rail = 0; rail < APPLE_CLPC_RAILS; rail++) {
			if (clpc->hw->rail[rail] >= apple_pmc_rail_count(clpc->pmc))
				return -EINVAL;
			clpc->mask |= APPLE_PMC_RAIL_MASK(clpc->hw->rail[rail]);
		}

		clpc->idle = devm_kcalloc(dev, nr_cpu_ids, sizeof(*clpc->idle),
					  GFP_KERNEL);
		clpc->wall = devm_kcalloc(dev, nr_cpu_ids, sizeof(*clpc->wall),
					  GFP_KERNEL);
		if (!clpc->idle || !clpc->wall)
			return -ENOMEM;

		for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
			if (!apple_pmc_voter_enabled(clpc->pmc, clpc->agent,
						     clpc->hw->rail[rail]))
				dev_warn(dev, "voter agent %u is not enabled on rail %u\n",
					 clpc->agent, clpc->hw->rail[rail]);

		clpc->saved_floor = apple_pmc_floor_read(clpc->pmc, clpc->agent) &
				    clpc->mask;
	}

	for (i = 0; i < APPLE_CLPC_MAX_CLUSTERS; i++) {
		if (!zalloc_cpumask_var(&clpc->clusters[i].cpus, GFP_KERNEL)) {
			apple_clpc_free_clusters(clpc);
			return -ENOMEM;
		}
	}
	if (!zalloc_cpumask_var(&clpc->covered, GFP_KERNEL)) {
		apple_clpc_free_clusters(clpc);
		return -ENOMEM;
	}

	apple_clpc_find_clusters(clpc);
	clpc->next_scan = jiffies + HZ;
	mutex_init(&clpc->sample_lock);
	mutex_init(&clpc->idle_lock);
	cpumask_clear(&clpc->idle_online_cpus);
	INIT_DELAYED_WORK(&clpc->work, apple_clpc_work);
	INIT_DEFERRABLE_WORK(&clpc->idle_work, apple_clpc_idle_work);
	if (clpc->has_pmc && clpc->hw->idle_on_time) {
		unsigned int cpu;

		clpc->idle_timers = devm_alloc_percpu(dev, struct apple_clpc_idle_timer);
		if (!clpc->idle_timers) {
			apple_clpc_free_clusters(clpc);
			return -ENOMEM;
		}
		for_each_possible_cpu(cpu) {
			struct apple_clpc_idle_timer *t =
				per_cpu_ptr(clpc->idle_timers, cpu);

			t->clpc = clpc;
			timer_setup(&t->timer, apple_clpc_idle_timer_fn,
				    TIMER_DEFERRABLE | TIMER_PINNED);
		}
		if (apple_clpc_cpuhp_state > 0) {
			ret = cpuhp_state_add_instance(apple_clpc_cpuhp_state,
						       &clpc->cpuhp_node);
			if (ret) {
				apple_clpc_free_clusters(clpc);
				return ret;
			}
			clpc->cpuhp_added = true;
		}
	}
	/* Sample at a cold publication only where that publication is the demand.
	 * Dense counting needs no extra sample, and one taken as each frame of a
	 * frame-paced client starts finds its CPUs busy and votes their fabric.
	 */
	if (clpc->gpu && !clpc->hw->gpu_demand)
		clpc->gpu->wake = &clpc->work;

	if (clpc->ane)
		clpc->ane->wake = &clpc->work;

	clpc->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("status", 0400, clpc->debugfs, clpc,
			    &apple_clpc_status_fops);

	if (clpc->ane)
		debugfs_create_file("ane_states", 0600, clpc->debugfs, clpc,
				    &apple_clpc_ane_states_fops);
	if (clpc->ane && clpc->ane_cpu >= 0)
		debugfs_create_u32("ane_cluster_khz", 0600, clpc->debugfs,
				   &clpc->ane_cluster_khz);

	ret = devm_add_action_or_reset(dev, apple_clpc_stop, clpc);
	if (ret)
		return ret;

	clpc->profile = PLATFORM_PROFILE_BALANCED;
	clpc->power_next = jiffies;
	if (has_profile) {
		struct device *ppdev;

		if (clpc->low_power_khz && !clpc->policy_nb_added) {
			clpc->policy_nb.notifier_call = apple_clpc_policy_notify;
			clpc->policy_nb_added = !cpufreq_register_notifier(&clpc->policy_nb,
									   CPUFREQ_POLICY_NOTIFIER);
		}
		/*
		 * "apple-clpc" budgets the low-power profile itself; "apple-smc"
		 * only carries High Power mode, and userspace keeps the CPU budget.
		 */
		ppdev = devm_platform_profile_register(dev,
						       clpc->low_power_khz ?
						       "apple-clpc" : "apple-smc",
						       clpc, &apple_clpc_profile_ops);
		if (IS_ERR(ppdev)) {
			if (!clpc->has_pmc)
				return PTR_ERR(ppdev);
			dev_warn(dev, "no platform profile: %pe\n", ppdev);
		}
	}

	mutex_lock(&apple_clpc_clients_lock);
	platform_set_drvdata(pdev, clpc);
	mutex_unlock(&apple_clpc_clients_lock);
	mutex_lock(&clpc->sample_lock);
	apple_clpc_queue(clpc, true);
	mutex_unlock(&clpc->sample_lock);
	return 0;
}

/*
 * Tasks are frozen before devices suspend, and with them the sampling, so
 * the floor would otherwise stay at whatever the last busy sample asked for
 * through the whole sleep.
 */
static int apple_clpc_suspend(struct device *dev)
{
	apple_clpc_halt(dev_get_drvdata(dev));
	return 0;
}

static int apple_clpc_resume(struct device *dev)
{
	struct apple_clpc *clpc = dev_get_drvdata(dev);
	unsigned int cpu;

	mutex_lock(&clpc->sample_lock);
	if (clpc->wall)
		for_each_possible_cpu(cpu)
			clpc->wall[cpu] = 0;
	clpc->power_next = jiffies;
	WRITE_ONCE(clpc->stopped, false);
	apple_clpc_queue(clpc, true);
	mutex_unlock(&clpc->sample_lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(apple_clpc_pm_ops, apple_clpc_suspend,
				apple_clpc_resume);

/* Only the stock M5/F3 busy floor is qualified on T8132. Do not invent
 * intermediate frequency-to-memory tables; idle can still release the vote.
 *
 * The G16 firmware keeps graphics powered while the GPU idles, so its power
 * state would hold the floor up all the time: on J713 that keeps the DCS and
 * the fabric at their top states at an idle desktop, about 0.45 W more than
 * with the floor released. The GPU driver reports its work instead, as on
 * T6040, and gets the M5/F3 floor while it has work in flight. Without that
 * floor, rendering is about 45% slower.
 */
static const struct apple_clpc_hw apple_clpc_hw_t8132 = {
	.rail = { [APPLE_CLPC_DCS] = 0, [APPLE_CLPC_SOC] = 1 },
	.map = {
		[APPLE_CLPC_DCS] = { 5, 5, 5, 5 },
		[APPLE_CLPC_SOC] = { 3, 3, 3, 3 },
	},
	.gpu_demand = false,
	.gpu_input = true,
};

/*
 * T8140: DCS states 0-4 on rail 0, fabric states 0-3 on rail 1. A busy
 * cluster raises the DCS one state per frequency quartile and leaves the
 * fabric alone.
 *
 * GPU work needs both rails at their top states: the GPU firmware takes up
 * and completes every command over the fabric. With the fabric left at
 * state 0, light GPU submissions take 30-50% longer than at state 3, and
 * macOS holds the fabric at its top states while such work streams. The
 * firmware powers graphics off a few milliseconds after the GPU idles, so
 * graphics that stays powered for 64 ms marks such a stream. Loads
 * paced by frames power it up and down with each frame instead: on J700,
 * 1080p video playback and light browsing keep it powered for at most 63 ms
 * at a time, and leave the floor down. The floor is released HOLD_MS after
 * graphics powers off.
 *
 * The PMP (agent 0) votes the DCS to its top state for streams of light GPU
 * submissions, and lowers it under sustained heavy rendering, which needs
 * neither rail's floor; see apple_clpc_gpu_limited().
 *
 * Light submissions that come every few milliseconds let graphics power off
 * between them, so they never stream, yet each still waits on the fabric,
 * which with the panel off nothing else holds up. Two or more submissions in
 * each of four sampling periods in a row (16 ms, one 60 Hz frame) get the
 * floor too; a frame-paced load leaves a quieter period in every frame.
 */
static const struct apple_clpc_hw apple_clpc_hw_t8140 = {
	.rail = { [APPLE_CLPC_DCS] = 0, [APPLE_CLPC_SOC] = 1 },
	.map = {
		[APPLE_CLPC_DCS] = { 1, 2, 3, 4 },
		[APPLE_CLPC_SOC] = { 0, 0, 0, 0 },
	},
	.gpu_demand = true,
	.gpu = { [APPLE_CLPC_DCS] = 4, [APPLE_CLPC_SOC] = 3 },
	.gpu_sustain_ms = 64,
	.pmp_backoff = true,
	.pmp_agent = 0,
	.gpu_dense = 2,
	/*
	 * Measured ANE demand (Neo, MobileNetV1): DCS 4 alone takes an
	 * EXECUTE from 2.3-2.5 to 1.8 ms, DCS 4 with fabric 3 (the GPU's
	 * streaming floor) to 1.25-1.31 ms.
	 */
	.ane = { [APPLE_CLPC_DCS] = 4, [APPLE_CLPC_SOC] = 3 },
};

/*
 * T6040: DCS states 0-5 on rail 0, fabric states 0-2 on rail 1. No cluster
 * runs below a quarter of its maximum (P 1260 of 4512 MHz, E 1020 of
 * 2592 MHz), so the first column only completes the table. A busy cluster
 * in the upper half asks for the top states. With the floor held at DCS 5
 * and fabric 2 the CPUs read 237-242 GB/s at 135 ns DRAM latency, against
 * 41 GB/s and 314 ns at 0 and 0. The second column is provisional until the
 * power of each DCS state is measured.
 *
 * A cluster requests only after three busy samples in a row (12 ms). At idle
 * a single busy sample raised the floor about 60 % of the time, from short
 * bursts of background work; three in a row bring that to about 11 %.
 * GPU demand bypasses the CPU streak and uses the same qualified DCS 5 /
 * fabric 2 states until publication/in-flight demand ends and HOLD_MS expires.
 * GFX supplier power cannot drive this input: firmware keeps it always on.
 */
static const struct apple_clpc_hw apple_clpc_hw_t6040 = {
	.rail = { [APPLE_CLPC_DCS] = 0, [APPLE_CLPC_SOC] = 1 },
	.map = {
		[APPLE_CLPC_DCS] = { 1, 2, 5, 5 },
		[APPLE_CLPC_SOC] = { 0, 1, 2, 2 },
	},
	.sustain = 3,
	.gpu_demand = false,
	.gpu_input = true,
	/*
	 * ANE work needs two things its own floor (PMC SoC-max: fabric 2)
	 * does not give it. ResNet-50 streams about 50 MB of weights per
	 * inference and runs DRAM bound at DCS 0, so hold the DCS 5 / fabric
	 * 2 a busy CPU cluster gets. And the engine runs at a speed set by
	 * the frequency of the second P cluster (cpu-map cluster2), whatever
	 * runs on it: MobileNetV1 takes 65-375 us of engine time (mean 165)
	 * with that cluster at its idle frequency and 88-91 us with it at
	 * 3.3 GHz or above (no gain above 3.756 GHz, measured at 1.8 to 4.5);
	 * the first P cluster and the E cluster make no difference.
	 */
	.ane = { [APPLE_CLPC_DCS] = 5, [APPLE_CLPC_SOC] = 2 },
	.ane_cluster = 2,
	.ane_cluster_khz = 3756000,
	.ane_cluster_machine = "apple,t6040",
};

/*
 * T6041 (M4 Max): DCS states 0-5 on rail 0, fabric states 0-3 on rail 1.
 * Unlike M4 Pro (4 AMCCs, 256-bit LPDDR5X-8533, fabric states 0-2), M4 Max's
 * 6/8-AMCC 384/512-bit LPDDR5X-8533 memory subsystem adds fabric state 3
 * (VOVD; ADT clpc fabric-mem-separation-max = 3) to sustain full 410/546 GB/s
 * bandwidth, and macOS 27.2.1 votes DCS 5 and fabric 3 immediately for active
 * CPU demand without a busy-sample streak (as on T8132), raising 12-thread
 * CPU DRAM reads on J616c from ~256 GB/s at fabric 2 to ~297 GB/s.
 *
 * G17 GPU demand uses the same host publication/idle input as T6040 and
 * holds DCS 5 / fabric 3 while GPU work is in flight (~230 GB/s vs ~43 GB/s
 * at state 0 on J616c). ANE demand preserves T6041's DCS 5 / fabric 2 floor,
 * and the ADT's cpu-low-power-frequency-max = 2808 MHz supplies the Low
 * Power platform profile ceiling (package power target not yet measured).
 *
 * Sampling stays on time with no vote held, as on T6050: with a single busy
 * core and the remaining 15 CPUs idle on M4 Max, a deferrable timer waits on
 * an idle CPU and leaves single-threaded DRAM pointer chasing stuck at DCS
 * state 0 (~284 ns per load on J616c vs ~166-187 ns once sampled). Other SoCs
 * keep their deferrable idle timers.
 */
static const struct apple_clpc_hw apple_clpc_hw_t6041 = {
	.rail = { [APPLE_CLPC_DCS] = 0, [APPLE_CLPC_SOC] = 1 },
	.map = {
		[APPLE_CLPC_DCS] = { 5, 5, 5, 5 },
		[APPLE_CLPC_SOC] = { 3, 3, 3, 3 },
	},
	.gpu_demand = false,
	.gpu_input = true,
	.ane = { [APPLE_CLPC_DCS] = 5, [APPLE_CLPC_SOC] = 2 },
	.idle_on_time = true,
	.low_power_khz = 2808000,
};

/*
 * T8142: DCS states 0-7 on rail 0, fabric states 0-4 on rail 1. The fabric
 * state bounds CPU memory bandwidth: four P cores copy at ~40 GB/s with the
 * fabric at 1 and at ~110 GB/s at 4 (macOS ~124).
 *
 * The M5 GPU driver reports its submissions in flight. Once the PMP runs, its
 * vote (agent 0) drops from iBoot's DCS 7 / fabric 1 to 0 and nothing else
 * raises the floor for the GPU: a 4K RGBA16F fill then takes 5.1 ms instead
 * of 0.9 ms with both rails at the top of the map.
 */
static const struct apple_clpc_hw apple_clpc_hw_t8142 = {
	.rail = { [APPLE_CLPC_DCS] = 0, [APPLE_CLPC_SOC] = 1 },
	.map = {
		[APPLE_CLPC_DCS] = { 2, 4, 6, 7 },
		[APPLE_CLPC_SOC] = { 1, 2, 3, 4 },
	},
	.gpu_input = true,
	/*
	 * Measured ANE demand (J813, H17; J815/J704 share the compatible and
	 * are untested): DCS 7 is the top DCS state; fabric 3 is a measured
	 * policy choice below the top fabric state (4). With the CPU pinned at
	 * minimum frequency so ANE owns the floor, firmware time drops from
	 * (0, 0) -> (7, 2) -> (7, 3) across denoise-640 (16.5 -> 11.7 -> 10.3
	 * ms; cold SLC 20.0 -> 11.7 -> 10.4 ms), unet-320 conv (3.94 -> 2.79
	 * -> 2.46 ms), and mm-512x2048 GEMM (627 -> 493 -> 420 us; cold SLC
	 * 782 -> 560 -> 526 us). Fabric 4 gives no further speedup on any of
	 * them and adds 71-121 mW of system power (4.28 -> 4.41 W on
	 * denoise-640, 3.94 -> 4.04 W on unet-320, 3.95 -> 4.02 W on
	 * mm-512x2048).
	 */
	.ane = { [APPLE_CLPC_DCS] = 7, [APPLE_CLPC_SOC] = 3 },
	/*
	 * With the engine AXI2AF bridge tunables applied, pure ANE execution
	 * time on J813 also tracks the P-cluster frequency (cpu-map cluster1),
	 * independent of which cluster hosts the submitting thread (pinned to
	 * an E-core with DCS 7 / SOC 3, unet-320 firmware report_done drops
	 * from 660.5 us with no P-cluster floor to 350.7 us at 4176 MHz,
	 * 4.49 W, with no further gain at 4464 MHz, 5.31 W).
	 */
	.ane_cluster = 1,
	.ane_cluster_khz = 4176000,
	.ane_cluster_machine = "apple,t8142",
};

/*
 * Standalone CLPC node without a PMC voter parent (for example T8112): no
 * memory/fabric floor votes; platform profiles are registered only when the
 * machine supplies apple,low-power-frequency-max-mhz or apple,high-power-mode.
 */
static const struct apple_clpc_hw apple_clpc_hw_generic = {};

#if IS_BUILTIN(CONFIG_KUNIT) && !defined(MODULE)
#include <kunit/test.h>
#include <linux/completion.h>

static void apple_clpc_gpu_request_lifetime_test(struct kunit *test)
{
	struct apple_clpc_gpu *gpu = kzalloc_obj(*gpu);
	u64 updated, sequence;

	KUNIT_ASSERT_NOT_NULL(test, gpu);
	kref_init(&gpu->ref);
	spin_lock_init(&gpu->lock);
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_active(gpu, &updated, &sequence));
	KUNIT_EXPECT_EQ(test, sequence, 0ull);
	/* Provider and one consumer own independent references. */
	kref_get(&gpu->ref);
	apple_clpc_gpu_set_active(gpu, true);
	KUNIT_EXPECT_TRUE(test, apple_clpc_gpu_active(gpu, &updated, &sequence));
	KUNIT_EXPECT_GT(test, updated, 0ull);
	KUNIT_EXPECT_EQ(test, sequence, 1ull);
	apple_clpc_gpu_put(gpu);
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_active(gpu, &updated, &sequence));
	KUNIT_EXPECT_EQ(test, sequence, 2ull);
	kref_put(&gpu->ref, apple_clpc_gpu_free);
}

static void apple_clpc_gpu_provider_removal_test(struct kunit *test)
{
	struct apple_clpc_gpu *gpu = kzalloc_obj(*gpu);
	u64 updated, sequence;

	KUNIT_ASSERT_NOT_NULL(test, gpu);
	kref_init(&gpu->ref);
	spin_lock_init(&gpu->lock);
	kref_get(&gpu->ref);
	/* Provider removal must not leave a late consumer with freed storage. */
	kref_put(&gpu->ref, apple_clpc_gpu_free);
	apple_clpc_gpu_set_active(gpu, true);
	KUNIT_EXPECT_TRUE(test, apple_clpc_gpu_active(gpu, &updated, &sequence));
	apple_clpc_gpu_set_active(gpu, false);
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_active(gpu, &updated, &sequence));
	apple_clpc_gpu_put(gpu);
}

struct apple_clpc_wake_test {
	struct delayed_work work;
	struct completion done;
};

static void apple_clpc_wake_complete(struct work_struct *work)
{
	struct apple_clpc_wake_test *wake = container_of(to_delayed_work(work),
													struct apple_clpc_wake_test, work);
	complete(&wake->done);
}

static void apple_clpc_gpu_cold_wake_test(struct kunit *test)
{
	struct apple_clpc_gpu *gpu = kzalloc_obj(*gpu);
	struct apple_clpc_wake_test wake;

	KUNIT_ASSERT_NOT_NULL(test, gpu);
	kref_init(&gpu->ref);
	spin_lock_init(&gpu->lock);
	INIT_DELAYED_WORK(&wake.work, apple_clpc_wake_complete);
	init_completion(&wake.done);
	gpu->wake = &wake.work;
	apple_clpc_gpu_set_active(gpu, true);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&wake.done, HZ), 0ul);
	/* Model provider withdrawal before its work storage is destroyed. */
	gpu->wake = NULL;
	cancel_delayed_work_sync(&wake.work);
	apple_clpc_gpu_put(gpu);
}

static void apple_clpc_gpu_dense_test(struct kunit *test)
{
	unsigned int streak = 0, i;
	u64 last = 0, sequence = 0;

	/* Two submissions in each of four periods in a row: dense. */
	for (i = 0; i < APPLE_CLPC_GPU_DENSE_SAMPLES - 1; i++) {
		sequence += 2;
		KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_dense_step(&streak, &last,
							       sequence, 2, false));
	}
	sequence += 2;
	KUNIT_EXPECT_TRUE(test, apple_clpc_gpu_dense_step(&streak, &last,
						      sequence, 2, false));
	/* A quieter period, as every frame of a 60 Hz load has, ends it. */
	sequence += 1;
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_dense_step(&streak, &last,
						       sequence, 2, false));
	KUNIT_EXPECT_EQ(test, streak, 0u);
	/* A late sample counts more than one period: it ends a streak too. */
	for (i = 0; i < APPLE_CLPC_GPU_DENSE_SAMPLES; i++) {
		sequence += 4;
		apple_clpc_gpu_dense_step(&streak, &last, sequence, 2, false);
	}
	KUNIT_EXPECT_EQ(test, streak, APPLE_CLPC_GPU_DENSE_SAMPLES);
	sequence += 40;
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_dense_step(&streak, &last,
						       sequence, 2, true));
	/* No threshold: never dense. */
	streak = APPLE_CLPC_GPU_DENSE_SAMPLES;
	sequence += 10;
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_dense_step(&streak, &last,
						       sequence, 0, false));
}

/* T8132: the GPU driver's reported work, not GFX power, gets the M5/F3 floor. */
static void apple_clpc_t8132_gpu_input_test(struct kunit *test)
{
	struct apple_clpc_hw power_state = apple_clpc_hw_t8132;
	unsigned int demand[APPLE_CLPC_RAILS] = {};
	struct apple_clpc_gpu *gpu = kzalloc_obj(*gpu);

	KUNIT_ASSERT_NOT_NULL(test, gpu);
	kref_init(&gpu->ref);
	spin_lock_init(&gpu->lock);
	KUNIT_EXPECT_FALSE(test, apple_clpc_hw_t8132.gpu_demand);

	/* No input, or no work reported: no GPU floor. */
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_input_demand(&apple_clpc_hw_t8132,
							     NULL, demand));
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_input_demand(&apple_clpc_hw_t8132,
							     gpu, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 0u);
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_SOC], 0u);

	/* Work in flight: DCS 5 and fabric 3. */
	apple_clpc_gpu_set_active(gpu, true);
	KUNIT_EXPECT_TRUE(test, apple_clpc_gpu_input_demand(&apple_clpc_hw_t8132,
							    gpu, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 5u);
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_SOC], 3u);

	/* A table that follows GFX power ignores the input. */
	power_state.gpu_demand = true;
	memset(demand, 0, sizeof(demand));
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_input_demand(&power_state, gpu,
							     demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 0u);

	/* Drained: nothing more is asked; the vote's hold releases it. */
	apple_clpc_gpu_set_active(gpu, false);
	KUNIT_EXPECT_FALSE(test, apple_clpc_gpu_input_demand(&apple_clpc_hw_t8132,
							     gpu, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 0u);
	apple_clpc_gpu_put(gpu);
}

static void apple_clpc_t6041_policy_test(struct kunit *test)
{
	struct apple_clpc *clpc = kunit_kzalloc(test, sizeof(*clpc), GFP_KERNEL);
	struct apple_clpc_gpu *gpu = kzalloc_obj(*gpu);
	unsigned int demand[APPLE_CLPC_RAILS] = {};
	struct apple_clpc_ane ane = {};
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, clpc);
	KUNIT_ASSERT_NOT_NULL(test, gpu);
	kref_init(&gpu->ref);
	spin_lock_init(&gpu->lock);
	spin_lock_init(&ane.lock);

	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6041.sustain, 0u);
	KUNIT_EXPECT_FALSE(test, apple_clpc_hw_t6041.gpu_demand);
	KUNIT_EXPECT_TRUE(test, apple_clpc_hw_t6041.gpu_input);
	KUNIT_EXPECT_TRUE(test, apple_clpc_hw_t6041.idle_on_time);
	KUNIT_EXPECT_FALSE(test, apple_clpc_hw_t6040.idle_on_time);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6041.low_power_khz, 2808000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6040.low_power_khz, 0u);
	for (i = 0; i < APPLE_CLPC_LEVELS; i++) {
		KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6041.map[APPLE_CLPC_DCS][i], 5);
		KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6041.map[APPLE_CLPC_SOC][i], 3);
	}

	clpc->hw = &apple_clpc_hw_t6041;
	clpc->ane = &ane;
	clpc->ane_states[APPLE_CLPC_DCS] = clpc->hw->ane[APPLE_CLPC_DCS];
	clpc->ane_states[APPLE_CLPC_SOC] = clpc->hw->ane[APPLE_CLPC_SOC];

	apple_clpc_ane_set_active(&ane, true);
	KUNIT_EXPECT_TRUE(test, apple_clpc_ane_input_demand(clpc, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 5u);
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_SOC], 2u);

	apple_clpc_gpu_set_active(gpu, true);
	KUNIT_EXPECT_TRUE(test, apple_clpc_gpu_input_demand(&apple_clpc_hw_t6041,
							    gpu, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 5u);
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_SOC], 3u);
	apple_clpc_gpu_put(gpu);
}

static const struct apple_clpc_hw apple_clpc_hw_t6050;

static void apple_clpc_ane_demand_hold_test(struct kunit *test)
{
	struct apple_clpc_ane ane = {};
	struct apple_clpc_vote vote = {};
	struct apple_clpc *clpc = kunit_kzalloc(test, sizeof(*clpc), GFP_KERNEL);
	unsigned int demand[APPLE_CLPC_RAILS] = {};
	unsigned long now = 100, hold = msecs_to_jiffies(APPLE_CLPC_HOLD_MS);

	KUNIT_ASSERT_NOT_NULL(test, clpc);
	spin_lock_init(&ane.lock);
	clpc->ane = &ane;
	clpc->hw = &apple_clpc_hw_t8140;
	clpc->ane_states[APPLE_CLPC_DCS] = clpc->hw->ane[APPLE_CLPC_DCS];
	clpc->ane_states[APPLE_CLPC_SOC] = clpc->hw->ane[APPLE_CLPC_SOC];
	KUNIT_EXPECT_FALSE(test, apple_clpc_ane_input_demand(clpc, demand));
	apple_clpc_ane_set_active(&ane, true);
	KUNIT_EXPECT_TRUE(test, apple_clpc_ane_input_demand(clpc, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 4u);
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_SOC], 3u);
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, demand[APPLE_CLPC_DCS], now), 4u);
	/* Demand continues across samples, without another publication. */
	now += hold;
	KUNIT_EXPECT_TRUE(test, apple_clpc_ane_input_demand(clpc, demand));
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, demand[APPLE_CLPC_DCS], now), 4u);
	apple_clpc_ane_set_active(&ane, false);
	memset(demand, 0, sizeof(demand));
	KUNIT_EXPECT_FALSE(test, apple_clpc_ane_input_demand(clpc, demand));
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, demand[APPLE_CLPC_DCS],
					      now + hold - 1), 4u);
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, demand[APPLE_CLPC_DCS],
					      now + hold), 0u);
	/* A request completing before the worker runs still earns the hold. */
	apple_clpc_ane_set_active(&ane, true);
	apple_clpc_ane_set_active(&ane, false);
	KUNIT_EXPECT_TRUE(test, apple_clpc_ane_input_demand(clpc, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 4u);
	KUNIT_EXPECT_FALSE(test, apple_clpc_ane_input_demand(clpc, demand));
	KUNIT_EXPECT_GT(test, ane.updated_ns, 0ull);
	KUNIT_EXPECT_EQ(test, ane.sequence, 4ull);
	/* T6040 holds its top qualified DCS and fabric states for the ANE. */
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6040.ane[APPLE_CLPC_DCS], 5);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6040.ane[APPLE_CLPC_SOC], 2);
	/* T8142 holds DCS 7 and fabric 3 while ANE work is in flight. */
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t8142.ane[APPLE_CLPC_DCS], 7);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t8142.ane[APPLE_CLPC_SOC], 3);
	clpc->hw = &apple_clpc_hw_t8142;
	clpc->ane_states[APPLE_CLPC_DCS] = clpc->hw->ane[APPLE_CLPC_DCS];
	clpc->ane_states[APPLE_CLPC_SOC] = clpc->hw->ane[APPLE_CLPC_SOC];
	memset(demand, 0, sizeof(demand));
	apple_clpc_ane_set_active(&ane, true);
	KUNIT_EXPECT_TRUE(test, apple_clpc_ane_input_demand(clpc, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 7u);
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_SOC], 3u);
	apple_clpc_ane_set_active(&ane, false);
	/* T6050 holds DCS 6 and fabric 2 while ANE work is in flight. */
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6050.ane[APPLE_CLPC_DCS], 6);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6050.ane[APPLE_CLPC_SOC], 2);
	clpc->hw = &apple_clpc_hw_t6050;
	clpc->ane_states[APPLE_CLPC_DCS] = clpc->hw->ane[APPLE_CLPC_DCS];
	clpc->ane_states[APPLE_CLPC_SOC] = clpc->hw->ane[APPLE_CLPC_SOC];
	memset(demand, 0, sizeof(demand));
	apple_clpc_ane_set_active(&ane, true);
	KUNIT_EXPECT_TRUE(test, apple_clpc_ane_input_demand(clpc, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 6u);
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_SOC], 2u);
	apple_clpc_ane_set_active(&ane, false);
	memset(demand, 0, sizeof(demand));
	KUNIT_EXPECT_FALSE(test, apple_clpc_ane_input_demand(clpc, demand));
}

static void apple_clpc_ane_cluster_test(struct kunit *test)
{
	struct apple_clpc *clpc = kunit_kzalloc(test, sizeof(*clpc), GFP_KERNEL);
	unsigned long now = 1000;

	KUNIT_ASSERT_NOT_NULL(test, clpc);
	/* No CPU for the cluster: the floor is never requested. */
	clpc->ane_cpu = -ENODEV;
	clpc->ane_cluster_khz = apple_clpc_hw_t6040.ane_cluster_khz;
	apple_clpc_ane_cluster(clpc, true, now);
	KUNIT_EXPECT_FALSE(test, freq_qos_request_active(&clpc->ane_qos));
	KUNIT_EXPECT_EQ(test, clpc->ane_qos_khz, 0u);
	/* The hold runs from the last active sample. */
	KUNIT_EXPECT_TRUE(test, time_before(now, clpc->ane_until));
	KUNIT_EXPECT_FALSE(test, time_before(now + msecs_to_jiffies(APPLE_CLPC_HOLD_MS),
					     clpc->ane_until));
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t6040.ane_cluster, 2u);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t8142.ane_cluster, 1u);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t8142.ane_cluster_khz, 4176000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t8140.ane_cluster_khz, 0u);
}

static void apple_clpc_ane_states_test(struct kunit *test)
{
	struct apple_clpc *clpc = kunit_kzalloc(test, sizeof(*clpc), GFP_KERNEL);
	struct apple_clpc_ane ane = {};
	unsigned int demand[APPLE_CLPC_RAILS] = { 5, 2 };
	int states[APPLE_CLPC_RAILS] = { 4, 3 };

	KUNIT_ASSERT_NOT_NULL(test, clpc);
	clpc->hw = &apple_clpc_hw_t8140;
	clpc->ane = &ane;
	spin_lock_init(&ane.lock);
	apple_clpc_ane_states_set(clpc, states);
	apple_clpc_ane_set_active(&ane, true);
	KUNIT_EXPECT_TRUE(test, apple_clpc_ane_input_demand(clpc, demand));
	/* ANE cannot lower a concurrent CPU/GPU request. */
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 5u);
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_SOC], 3u);
	states[APPLE_CLPC_DCS] = -1;
	states[APPLE_CLPC_SOC] = 100;
	apple_clpc_ane_states_set(clpc, states);
	KUNIT_EXPECT_EQ(test, clpc->ane_states[APPLE_CLPC_DCS], 0u);
	KUNIT_EXPECT_EQ(test, clpc->ane_states[APPLE_CLPC_SOC], 3u);
}

static void apple_clpc_ane_no_provider_test(struct kunit *test)
{
	struct apple_clpc *clpc = kunit_kzalloc(test, sizeof(*clpc), GFP_KERNEL);
	struct apple_clpc_ane *ane = NULL;
	unsigned int demand[APPLE_CLPC_RAILS] = {};
	struct device dev = {};

	KUNIT_ASSERT_NOT_NULL(test, clpc);
	KUNIT_EXPECT_EQ(test, apple_clpc_ane_get(&dev, &ane), 0);
	KUNIT_EXPECT_PTR_EQ(test, ane, NULL);
	apple_clpc_ane_set_active(ane, true);
	apple_clpc_ane_set_active(ane, false);
	apple_clpc_ane_put(ane);
	KUNIT_EXPECT_FALSE(test, apple_clpc_ane_input_demand(clpc, demand));
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_DCS], 0u);
	KUNIT_EXPECT_EQ(test, demand[APPLE_CLPC_SOC], 0u);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_t8132.ane[APPLE_CLPC_DCS], 0);
}

static void apple_clpc_ane_wake_removal_test(struct kunit *test)
{
	struct apple_clpc_ane *ane = kzalloc_obj(*ane);
	struct apple_clpc_wake_test wake;

	KUNIT_ASSERT_NOT_NULL(test, ane);
	kref_init(&ane->ref);
	spin_lock_init(&ane->lock);
	INIT_DELAYED_WORK(&wake.work, apple_clpc_wake_complete);
	init_completion(&wake.done);
	ane->wake = &wake.work;
	kref_get(&ane->ref);
	apple_clpc_ane_set_active(ane, true);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&wake.done, HZ), 0ul);
	/* Actual removal ordering: detach, drain, drop the provider reference. */
	apple_clpc_ane_detach(ane);
	cancel_delayed_work_sync(&wake.work);
	apple_clpc_ane_owner_put(ane);
	reinit_completion(&wake.done);
	apple_clpc_ane_set_active(ane, false);
	apple_clpc_ane_set_active(ane, true);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&wake.work));
	KUNIT_EXPECT_FALSE(test, completion_done(&wake.done));
	apple_clpc_ane_put(ane);
}

static void apple_clpc_cpu_busy_floor_test(struct kunit *test)
{
	struct apple_clpc *clpc = kunit_kzalloc(test, sizeof(*clpc), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, clpc);
	clpc->hw = &apple_clpc_hw_t6050;

	/* Without the board override, the top quarter uses the SoC map. */
	KUNIT_EXPECT_EQ(test, apple_clpc_cpu_state(clpc, APPLE_CLPC_DCS, 3), 5u);
	KUNIT_EXPECT_EQ(test, apple_clpc_cpu_state(clpc, APPLE_CLPC_SOC, 3), 2u);

	/* With the override active outside low-power, only the top quarter changes. */
	clpc->cpu_top[APPLE_CLPC_DCS] = 6;
	clpc->cpu_top[APPLE_CLPC_SOC] = 3;
	clpc->cpu_top_set = true;
	KUNIT_EXPECT_EQ(test, apple_clpc_cpu_state(clpc, APPLE_CLPC_DCS, 2), 5u);
	KUNIT_EXPECT_EQ(test, apple_clpc_cpu_state(clpc, APPLE_CLPC_SOC, 2), 2u);
	KUNIT_EXPECT_EQ(test, apple_clpc_cpu_state(clpc, APPLE_CLPC_DCS, 3), 6u);
	KUNIT_EXPECT_EQ(test, apple_clpc_cpu_state(clpc, APPLE_CLPC_SOC, 3), 3u);

	/* Under the low-power platform profile, fall back to the SoC map. */
	WRITE_ONCE(clpc->low_power, true);
	KUNIT_EXPECT_EQ(test, apple_clpc_cpu_state(clpc, APPLE_CLPC_DCS, 3), 5u);
	KUNIT_EXPECT_EQ(test, apple_clpc_cpu_state(clpc, APPLE_CLPC_SOC, 3), 2u);
}

static void apple_clpc_low_power_roundup_test(struct kunit *test)
{
	/* M4 Pro P-cluster OPPs around the 2808 MHz ADT ceiling (J616s). */
	struct cpufreq_frequency_table p_table[] = {
		{ .frequency = 2592000 },
		{ .frequency = 2868000 },
		{ .frequency = 3132000 },
		{ .frequency = 4512000 },
		{ .frequency = CPUFREQ_TABLE_END },
	};
	struct cpufreq_frequency_table e_table[] = {
		{ .frequency = 1020000 },
		{ .frequency = 2592000 },
		{ .frequency = CPUFREQ_TABLE_END },
	};
	/* T8112 (J473/J413) E and P cluster OPPs around the 1960 MHz ADT ceiling. */
	struct cpufreq_frequency_table t8112_e_table[] = {
		{ .frequency = 600000 },
		{ .frequency = 1752000 },
		{ .frequency = 2004000 },
		{ .frequency = 2424000 },
		{ .frequency = CPUFREQ_TABLE_END },
	};
	struct cpufreq_frequency_table t8112_p_table[] = {
		{ .frequency = 660000 },
		{ .frequency = 1704000 },
		{ .frequency = 1968000 },
		{ .frequency = 3204000 },
		{ .frequency = CPUFREQ_TABLE_END },
	};
	struct cpufreq_policy p_pol = {
		.freq_table = p_table,
		.cpuinfo = { .min_freq = 2592000, .max_freq = 4512000 },
	};
	struct cpufreq_policy e_pol = {
		.freq_table = e_table,
		.cpuinfo = { .min_freq = 1020000, .max_freq = 2592000 },
	};
	struct cpufreq_policy t8112_e_pol = {
		.freq_table = t8112_e_table,
		.cpuinfo = { .min_freq = 600000, .max_freq = 2424000 },
	};
	struct cpufreq_policy t8112_p_pol = {
		.freq_table = t8112_p_table,
		.cpuinfo = { .min_freq = 660000, .max_freq = 3204000 },
	};
	struct cpufreq_policy no_table_pol = {
		.freq_table = NULL,
		.cpuinfo = { .min_freq = 660000, .max_freq = 3204000 },
	};

	/* Explicit ceiling rounds UP to the first OPP >= 2808 MHz (2868 MHz). */
	KUNIT_EXPECT_EQ(test,
			apple_clpc_low_power_top_khz(&p_pol, 2808000),
			2868000u);
	/* Slower cluster below 2808 MHz stays at its max (2592 MHz). */
	KUNIT_EXPECT_EQ(test,
			apple_clpc_low_power_top_khz(&e_pol, 2808000),
			2592000u);
	/* T8112 1960 MHz ADT ceiling rounds up to 2004 MHz (E) and 1968 MHz (P). */
	KUNIT_EXPECT_EQ(test,
			apple_clpc_low_power_top_khz(&t8112_e_pol, 1960000),
			2004000u);
	KUNIT_EXPECT_EQ(test,
			apple_clpc_low_power_top_khz(&t8112_p_pol, 1960000),
			1968000u);
	/* Missing machine ceiling (0) must not invent a fallback cap. */
	KUNIT_EXPECT_EQ(test,
			apple_clpc_low_power_top_khz(&t8112_p_pol, 0),
			3204000u);
	/* NULL freq_table must not dereference NULL; stays at policy max. */
	KUNIT_EXPECT_EQ(test,
			apple_clpc_low_power_top_khz(&no_table_pol, 1960000),
			3204000u);
}

static void apple_clpc_profile_ops_test(struct kunit *test)
{
	struct apple_clpc *clpc = kunit_kzalloc(test, sizeof(*clpc), GFP_KERNEL);
	struct device dev = {};
	unsigned long choices = 0;
	enum platform_profile_option prof = PLATFORM_PROFILE_BALANCED;

	KUNIT_ASSERT_NOT_NULL(test, clpc);
	KUNIT_ASSERT_TRUE(test, zalloc_cpumask_var(&clpc->covered, GFP_KERNEL));
	cpumask_copy(clpc->covered, cpu_possible_mask);
	mutex_init(&clpc->sample_lock);
	INIT_DELAYED_WORK(&clpc->work, apple_clpc_work);
	INIT_DEFERRABLE_WORK(&clpc->idle_work, apple_clpc_idle_work);
	clpc->hw = &apple_clpc_hw_generic;
	clpc->low_power_khz = 1960000;
	clpc->has_pmc = false;
	clpc->stopped = true;
	clpc->profile = PLATFORM_PROFILE_BALANCED;
	dev_set_drvdata(&dev, clpc);

	/* Standalone generic table carries no hardcoded ceiling or power cap. */
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_generic.low_power_khz, 0u);
	KUNIT_EXPECT_EQ(test, apple_clpc_hw_generic.low_power_mw, 0u);

	KUNIT_EXPECT_EQ(test, apple_clpc_profile_probe(clpc, &choices), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(PLATFORM_PROFILE_LOW_POWER, &choices));
	KUNIT_EXPECT_TRUE(test, test_bit(PLATFORM_PROFILE_BALANCED, &choices));
	KUNIT_EXPECT_TRUE(test, test_bit(PLATFORM_PROFILE_PERFORMANCE, &choices));

	KUNIT_EXPECT_EQ(test, apple_clpc_profile_set(&dev, PLATFORM_PROFILE_LOW_POWER), 0);
	KUNIT_EXPECT_EQ(test, apple_clpc_profile_get(&dev, &prof), 0);
	KUNIT_EXPECT_EQ(test, prof, PLATFORM_PROFILE_LOW_POWER);
	KUNIT_EXPECT_TRUE(test, clpc->low_power);

	/* Static standalone ceiling (low_power_mw == 0) must not queue periodic work. */
	WRITE_ONCE(clpc->stopped, false);
	apple_clpc_queue(clpc, true);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&clpc->work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&clpc->idle_work));

	/* With a package power target, standalone low-power schedules 100 ms polling. */
	clpc->low_power_mw = 15000;
	apple_clpc_queue(clpc, true);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&clpc->work));
	WRITE_ONCE(clpc->stopped, true);
	cancel_delayed_work_sync(&clpc->work);
	clpc->low_power_mw = 0;

	KUNIT_EXPECT_EQ(test, apple_clpc_profile_set(&dev, PLATFORM_PROFILE_PERFORMANCE), 0);
	KUNIT_EXPECT_EQ(test, apple_clpc_profile_get(&dev, &prof), 0);
	KUNIT_EXPECT_EQ(test, prof, PLATFORM_PROFILE_PERFORMANCE);
	KUNIT_EXPECT_FALSE(test, clpc->low_power);

	KUNIT_EXPECT_EQ(test, apple_clpc_profile_set(&dev, PLATFORM_PROFILE_BALANCED), 0);
	KUNIT_EXPECT_EQ(test, apple_clpc_profile_get(&dev, &prof), 0);
	KUNIT_EXPECT_EQ(test, prof, PLATFORM_PROFILE_BALANCED);
	KUNIT_EXPECT_FALSE(test, clpc->low_power);

	free_cpumask_var(clpc->covered);
}

static struct kunit_case apple_clpc_cases[] = {
	KUNIT_CASE(apple_clpc_cpu_busy_floor_test),
	KUNIT_CASE(apple_clpc_ane_demand_hold_test),
	KUNIT_CASE(apple_clpc_ane_states_test),
	KUNIT_CASE(apple_clpc_ane_cluster_test),
	KUNIT_CASE(apple_clpc_ane_no_provider_test),
	KUNIT_CASE(apple_clpc_ane_wake_removal_test),
	KUNIT_CASE(apple_clpc_t8132_gpu_input_test),
	KUNIT_CASE(apple_clpc_t6041_policy_test),
	KUNIT_CASE(apple_clpc_gpu_dense_test),
	KUNIT_CASE(apple_clpc_gpu_cold_wake_test),
	KUNIT_CASE(apple_clpc_gpu_request_lifetime_test),
	KUNIT_CASE(apple_clpc_gpu_provider_removal_test),
	KUNIT_CASE(apple_clpc_low_power_roundup_test),
	KUNIT_CASE(apple_clpc_profile_ops_test),
	{}
};
static struct kunit_suite apple_clpc_suite = {
	.name = "apple-clpc-gpu",
	.test_cases = apple_clpc_cases,
};
kunit_test_suite(apple_clpc_suite);

/* A cpufreq table out of order, with an invalid entry, as a driver may give it. */
static struct cpufreq_policy *apple_clpc_test_policy(struct kunit *test)
{
	static const unsigned int khz[] = {
		600000, CPUFREQ_ENTRY_INVALID, 1200000, 2000000, 1500000,
	};
	struct cpufreq_frequency_table *table;
	struct cpufreq_policy *policy;
	unsigned int i;

	policy = kunit_kzalloc(test, sizeof(*policy), GFP_KERNEL);
	table = kunit_kcalloc(test, ARRAY_SIZE(khz) + 1, sizeof(*table), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, policy);
	KUNIT_ASSERT_NOT_NULL(test, table);
	for (i = 0; i < ARRAY_SIZE(khz); i++)
		table[i].frequency = khz[i];
	table[i].frequency = CPUFREQ_TABLE_END;
	policy->freq_table = table;
	policy->cpuinfo.max_freq = 2000000;
	return policy;
}

static void apple_clpc_step_khz_test(struct kunit *test)
{
	struct cpufreq_policy *policy = apple_clpc_test_policy(test);

	/* One table step either way, whatever the table order. */
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 1200000, true), 1500000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 1500000, true), 2000000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 1500000, false), 1200000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 1200000, false), 600000u);
	/* Off the table: the nearest entry in that direction. */
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 1300000, true), 1500000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 1300000, false), 1200000u);
	/*
	 * No step past either end: @khz back. The invalid entry sits above
	 * every real one, so these up steps are the ones that would pick it.
	 */
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 2000000, true), 2000000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 600000, false), 600000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 2100000, true), 2100000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 0, false), 0u);

	/*
	 * The lookup apple_clpc_profile_limit() makes for the low-power top: the
	 * first entry at or above low_power_khz, so a value between two entries
	 * rounds up and an exact entry is kept. Its max_freq fallback is not
	 * covered here.
	 */
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 1400000 - 1, true), 1500000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 1500000 - 1, true), 1500000u);
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 600000 - 1, true), 600000u);
	/* Above the table nothing is found and @khz comes back. */
	KUNIT_EXPECT_EQ(test, apple_clpc_step_khz(policy, 2100000 - 1, true), 2100000u - 1);
}

static void apple_clpc_steps_first_test(struct kunit *test)
{
	/* Down: the higher ceiling first; on a tie, the faster cluster. */
	KUNIT_EXPECT_TRUE(test, apple_clpc_steps_first(2000000, 2000000, 1500000, 3000000, false));
	KUNIT_EXPECT_FALSE(test, apple_clpc_steps_first(1500000, 3000000, 2000000, 2000000, false));
	KUNIT_EXPECT_TRUE(test, apple_clpc_steps_first(1500000, 3000000, 1500000, 2000000, false));
	KUNIT_EXPECT_FALSE(test, apple_clpc_steps_first(1500000, 2000000, 1500000, 3000000, false));

	/* Up: the lower ceiling first; on a tie, the slower cluster. */
	KUNIT_EXPECT_TRUE(test, apple_clpc_steps_first(1500000, 3000000, 2000000, 2000000, true));
	KUNIT_EXPECT_FALSE(test, apple_clpc_steps_first(2000000, 2000000, 1500000, 3000000, true));
	KUNIT_EXPECT_TRUE(test, apple_clpc_steps_first(1500000, 2000000, 1500000, 3000000, true));
	KUNIT_EXPECT_FALSE(test, apple_clpc_steps_first(1500000, 3000000, 1500000, 2000000, true));

	/* Identical clusters: the one already picked keeps the step. */
	KUNIT_EXPECT_FALSE(test, apple_clpc_steps_first(1500000, 2000000, 1500000, 2000000, true));
	KUNIT_EXPECT_FALSE(test, apple_clpc_steps_first(1500000, 2000000, 1500000, 2000000, false));
}

static void apple_clpc_vote_test(struct kunit *test)
{
	unsigned long hold = msecs_to_jiffies(APPLE_CLPC_HOLD_MS);
	/*
	 * Start so that the hold deadline wraps to 0 while its last held
	 * sample is still ULONG_MAX: a compare that is not wrap-safe drops
	 * the vote early there.
	 */
	unsigned long t0 = ULONG_MAX - (hold - 1);
	struct apple_clpc_vote vote = {};

	KUNIT_ASSERT_GT(test, hold, 1ul);
	KUNIT_ASSERT_EQ(test, t0 + hold - 1, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, 0, t0), 0u);
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, 5, t0), 5u);
	/* No demand: the state holds for APPLE_CLPC_HOLD_MS, then drops. */
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, 0, t0 + hold - 1), 5u);
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, 0, t0 + hold), 0u);
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, 0, t0 + 2 * hold), 0u);

	/* A new demand replaces the state at once, down as well as up. */
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, 7, t0 + 2 * hold), 7u);
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, 2, t0 + 2 * hold + 1), 2u);
	/* and restarts the hold. */
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, 0, t0 + 3 * hold), 2u);
	KUNIT_EXPECT_EQ(test, apple_clpc_vote(&vote, 0, t0 + 3 * hold + 1), 0u);
}

static void apple_clpc_floor_test(struct kunit *test)
{
	struct apple_clpc *clpc = kunit_kzalloc(test, sizeof(*clpc), GFP_KERNEL);
	struct apple_clpc_hw *hw = kunit_kzalloc(test, sizeof(*hw), GFP_KERNEL);
	unsigned int state[APPLE_CLPC_RAILS] = {};

	KUNIT_ASSERT_NOT_NULL(test, clpc);
	KUNIT_ASSERT_NOT_NULL(test, hw);
	clpc->hw = hw;

	/* Each rail's state in its own nibble of the PMC floor word. */
	hw->rail[APPLE_CLPC_DCS] = 0;
	hw->rail[APPLE_CLPC_SOC] = 1;
	KUNIT_EXPECT_EQ(test, apple_clpc_floor(clpc, state), 0u);
	state[APPLE_CLPC_DCS] = 5;
	state[APPLE_CLPC_SOC] = 3;
	KUNIT_EXPECT_EQ(test, apple_clpc_floor(clpc, state), 0x35u);

	/* The rail numbers come from the table, not the rail order. */
	hw->rail[APPLE_CLPC_DCS] = 2;
	hw->rail[APPLE_CLPC_SOC] = 0;
	KUNIT_EXPECT_EQ(test, apple_clpc_floor(clpc, state), 0x503u);

	/* A state wider than a nibble cannot spill into another rail. */
	state[APPLE_CLPC_DCS] = 0x1f;
	state[APPLE_CLPC_SOC] = 0x10;
	KUNIT_EXPECT_EQ(test, apple_clpc_floor(clpc, state), 0xf00u);
}

static void apple_clpc_profile_step_busy_test(struct kunit *test)
{
	struct apple_clpc *clpc = kunit_kzalloc(test, sizeof(*clpc), GFP_KERNEL);
	struct cpufreq_policy *slow = apple_clpc_test_policy(test);
	struct cpufreq_policy *fast = apple_clpc_test_policy(test);
	struct cpufreq_policy *policies[APPLE_CLPC_MAX_CLUSTERS] = { slow, fast };
	unsigned int next_khz = 0;

	KUNIT_ASSERT_NOT_NULL(test, clpc);
	slow->cpuinfo.max_freq = 1500000;
	fast->cpuinfo.max_freq = 2000000;
	clpc->nr_clusters = 2;
	clpc->clusters[0].top_khz = 1500000;
	clpc->clusters[0].max_khz = 1500000;
	clpc->clusters[1].top_khz = 2000000;
	clpc->clusters[1].max_khz = 2000000;

	/*
	 * GPU/display holding PHPC above low_power_mw while all CPU clusters
	 * are below APPLE_CLPC_BUSY_PCT must not step any cluster down.
	 */
	clpc->clusters[0].busy = 10;
	clpc->clusters[1].busy = APPLE_CLPC_BUSY_PCT - 1;
	KUNIT_EXPECT_EQ(test, apple_clpc_pick_profile_step(clpc, policies, -1, &next_khz), -1);

	/*
	 * Only the busy cluster is eligible to step down, even when an idle
	 * cluster sits at a higher ceiling.
	 */
	clpc->clusters[0].busy = APPLE_CLPC_BUSY_PCT;
	clpc->clusters[1].busy = 20;
	KUNIT_EXPECT_EQ(test, apple_clpc_pick_profile_step(clpc, policies, -1, &next_khz), 0);
	KUNIT_EXPECT_EQ(test, next_khz, 1200000u);

	/* When both clusters are busy, apple_clpc_steps_first() ordering applies. */
	clpc->clusters[0].busy = 80;
	clpc->clusters[1].busy = 60;
	KUNIT_EXPECT_EQ(test, apple_clpc_pick_profile_step(clpc, policies, -1, &next_khz), 1);
	KUNIT_EXPECT_EQ(test, next_khz, 1500000u);

	/*
	 * Stepping back up when PHPC falls below 90% restores idle clusters too,
	 * lowest ceiling first (slower cluster on a tie).
	 */
	clpc->clusters[0].busy = 0;
	clpc->clusters[1].busy = 0;
	clpc->clusters[0].max_khz = 1200000;
	clpc->clusters[1].max_khz = 1200000;
	KUNIT_EXPECT_EQ(test, apple_clpc_pick_profile_step(clpc, policies, 1, &next_khz), 0);
	KUNIT_EXPECT_EQ(test, next_khz, 1500000u);

	/* Zero step does nothing regardless of busy state. */
	clpc->clusters[0].busy = 100;
	KUNIT_EXPECT_EQ(test, apple_clpc_pick_profile_step(clpc, policies, 0, &next_khz), -1);
}

static struct kunit_case apple_clpc_policy_cases[] = {
	KUNIT_CASE(apple_clpc_step_khz_test),
	KUNIT_CASE(apple_clpc_steps_first_test),
	KUNIT_CASE(apple_clpc_vote_test),
	KUNIT_CASE(apple_clpc_floor_test),
	KUNIT_CASE(apple_clpc_profile_step_busy_test),
	{}
};
static struct kunit_suite apple_clpc_policy_suite = {
	.name = "apple-clpc-policy",
	.test_cases = apple_clpc_policy_cases,
};
kunit_test_suite(apple_clpc_policy_suite);
#endif

/*
 * T8152: DCS states 0-7 and fabric states 0-4. The G18 GPU driver reports its
 * work in flight, as on T8142, and gets the top of the map while it has some.
 * Without that input only busy CPU clusters raised the floor: on the J873g a
 * GPU-bound render ran 34% faster with it (783 against 585 frames/s), and
 * before, the GPU ran faster the more CPU time its own job waits used.
 */
static const struct apple_clpc_hw apple_clpc_hw_t8152 = {
	.rail = { [APPLE_CLPC_DCS] = 0, [APPLE_CLPC_SOC] = 1 },
	.map = {
		[APPLE_CLPC_DCS] = { 2, 4, 6, 7 },
		[APPLE_CLPC_SOC] = { 1, 2, 3, 4 },
	},
	.gpu_input = true,
};

/*
 * T6050 (M5 Pro/Max): DCS states 0-6 on rail 0, fabric states 0-3 on rail 1.
 * The boot loader enables no agent on either rail, so without this floor
 * the DRAM controller and the fabric stay at state 0: CPU memcpy caps at
 * about 34 GB/s on the M5 Pro. A busy cluster in the upper half of its range
 * asks for DCS 5 and fabric 2; three busy samples in a row (12 ms) are
 * needed, as on T6040. macOS's CLPC votes from memory stalls instead, which
 * this driver doesn't see (J714sAP, 26A428: no vote for one CPU-bound thread,
 * DCS 6 and fabric 2 for memcpy); DCS 5 and fabric 2 keep pointer chasing at
 * 110 ns against 300 ns at state 0. The GPU driver reports its work in
 * flight and gets DCS 6 and fabric 2 while it has some, the floor macOS's
 * PMP holds for GPU compute: bandwidth-bound GPU work runs about 11% slower
 * at DCS 5, and none runs faster at fabric 3 (the PMP asks for it for fill).
 *
 * Sampling stays on time with no vote held. From a deferrable timer, a CPU
 * that went busy after an idle spell waited for the timer's own CPU to wake,
 * and pointer chasing ran at 300 ns for 60 ms and more; on time the floor is
 * in place within 24-32 ms, for no measurable idle power (6.44 against 6.40 W).
 *
 * Platform profiles follow macOS's power modes (J714sAP, 26A428, traced
 * under the same loads in each). Automatic and High Power set no CPU limit:
 * an 18-thread FMA loop starts at the top clocks in both and the P cluster
 * slows as the package heats in both, and Linux runs it within 2% of either
 * unlimited. Low Power runs one thread at 2844 MHz, the 2880 MHz state
 * above the ADT's clpc cpu-low-power-frequency-max (2808 MHz), and holds the
 * package at 14.3-14.7 W (XNU's power report to the PMP) under the FMA loop.
 */
static const struct apple_clpc_hw apple_clpc_hw_t6050 = {
	.rail = { [APPLE_CLPC_DCS] = 0, [APPLE_CLPC_SOC] = 1 },
	.map = {
		[APPLE_CLPC_DCS] = { 2, 4, 5, 5 },
		[APPLE_CLPC_SOC] = { 1, 2, 2, 2 },
	},
	.gpu = { [APPLE_CLPC_DCS] = 6, [APPLE_CLPC_SOC] = 2 },
	/*
	 * ANE demand, measured on J775c (M5 Max, H17): DCS 6 and fabric 2, the
	 * top states this table already uses. unet-320 (8 tasks, 200 runs)
	 * host p50 is 5504 us with no ANE floor and 2551 us at (6, 2); (6, 3)
	 * measured 2536 us, the same within run-to-run spread, so fabric stays
	 * at 2.
	 */
	.ane = { [APPLE_CLPC_DCS] = 6, [APPLE_CLPC_SOC] = 2 },
	.sustain = 3,
	.gpu_input = true,
	.idle_on_time = true,
	.low_power_khz = 2808000,
	.low_power_mw = 15000,
	/*
	 * Only for DTBs without apple,high-power-mode: the 14-inch M5 Pro had
	 * High Power mode here first. The record is as macOS 27.0 writes it there.
	 */
	.high_power_machine = "apple,j714s",
};

static const struct of_device_id apple_clpc_of_match[] = {
	{ .compatible = "apple,t6050-clpc", .data = &apple_clpc_hw_t6050 },
	{ .compatible = "apple,t8132-clpc", .data = &apple_clpc_hw_t8132 },
	{ .compatible = "apple,t8140-clpc", .data = &apple_clpc_hw_t8140 },
	{ .compatible = "apple,t8142-clpc", .data = &apple_clpc_hw_t8142 },
	{ .compatible = "apple,t8152-clpc", .data = &apple_clpc_hw_t8152 },
	{ .compatible = "apple,t6041-max-clpc", .data = &apple_clpc_hw_t6041 },
	{ .compatible = "apple,t6041-clpc", .data = &apple_clpc_hw_t6040 },
	{ .compatible = "apple,clpc", .data = &apple_clpc_hw_generic },
	{}
};
MODULE_DEVICE_TABLE(of, apple_clpc_of_match);

static struct platform_driver apple_clpc_driver = {
	.probe = apple_clpc_probe,
	.driver = {
		.name = "apple-clpc",
		.of_match_table = apple_clpc_of_match,
		.pm = pm_sleep_ptr(&apple_clpc_pm_ops),
	},
};
static int __init apple_clpc_driver_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
				      "soc/apple/clpc:online",
				      apple_clpc_cpu_online,
				      apple_clpc_cpu_down);
	if (ret < 0)
		return ret;
	apple_clpc_cpuhp_state = ret;

	ret = platform_driver_register(&apple_clpc_driver);
	if (ret) {
		cpuhp_remove_multi_state(apple_clpc_cpuhp_state);
		apple_clpc_cpuhp_state = 0;
	}
	return ret;
}
module_init(apple_clpc_driver_init);

static void __exit apple_clpc_driver_exit(void)
{
	platform_driver_unregister(&apple_clpc_driver);
	if (apple_clpc_cpuhp_state > 0) {
		cpuhp_remove_multi_state(apple_clpc_cpuhp_state);
		apple_clpc_cpuhp_state = 0;
	}
}
module_exit(apple_clpc_driver_exit);

MODULE_DESCRIPTION("Apple SoC host performance controller floors");
MODULE_LICENSE("Dual MIT/GPL");

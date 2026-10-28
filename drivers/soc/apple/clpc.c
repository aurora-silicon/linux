// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC host performance controller (CLPC) floors
 *
 * On macOS the CLPC raises the floor of the memory controller (DCS) and the
 * SoC fabric while the CPU clusters or the GPU are busy, through its own
 * voter agent in the PMC. This driver keeps that agent's floor in step with
 * Linux CPU load:
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

#include <linux/cpufreq.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/soc/apple/clpc.h>
#include <linux/debugfs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/soc/apple/pmc.h>
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
};

struct apple_clpc_cluster {
	unsigned int policy_cpu;
	cpumask_var_t cpus;
	unsigned int busy;
	unsigned int streak;
	unsigned int level;
	unsigned int request[APPLE_CLPC_RAILS];
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
	struct delayed_work *wake;
};

struct apple_clpc {
	struct device *dev;
	struct device *pmc;
	const struct apple_clpc_hw *hw;
	unsigned int agent;
	/* Samples while a vote is held; idle_work defers to busy CPUs. */
	struct delayed_work work;
	struct delayed_work idle_work;
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
	struct dentry *debugfs;
	struct regmap *gpu_pmgr;
	struct apple_clpc_gpu *gpu;
	u32 last_floor;
	u32 last_reason;
	u64 last_vote_ns;
	u32 gpu_offset;
	/* Graphics powered at the last sample, since when, and that sample's time */
	bool gpu_on;
	unsigned long gpu_on_since;
	unsigned long gpu_sampled;
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
	gpu->updated_ns = ktime_get_mono_fast_ns();
	gpu->sequence++;
	spin_unlock_irqrestore(&gpu->lock, flags);
}
EXPORT_SYMBOL_GPL(apple_clpc_gpu_set_active);

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
	int ret = -EPROBE_DEFER;

	*out = NULL;
	np = of_parse_phandle(dev->of_node, "apple,clpc", 0);
	if (!np)
		return of_find_property(dev->of_node, "apple,clpc", NULL) ? -EINVAL : 0;
	if (!of_device_is_compatible(np, "apple,t6041-clpc")) {
		of_node_put(np);
		return -EINVAL;
	}
	provider = of_find_device_by_node(np);
	of_node_put(np);
	if (!provider)
		return ret;
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
			clpc->idle[cpu] = get_cpu_idle_time(cpu, &clpc->wall[cpu], 0);
		}
		cpufreq_cpu_put(policy);
	}
}

static void apple_clpc_sample_cluster(struct apple_clpc *clpc,
				      struct apple_clpc_cluster *cl)
{
	struct cpufreq_policy *policy;
	unsigned int cpu, busy = 0, q, rail;

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
				   clpc->hw->map[rail][q] : 0;
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

static void apple_clpc_queue(struct apple_clpc *clpc, bool idle)
{
	if (READ_ONCE(clpc->stopped))
		return;

	/*
	 * A held vote has to be sampled on time so that it drops once demand
	 * stops; with no vote held, sampling can wait for a CPU that wakes up
	 * anyway rather than waking an idle one every period.
	 */
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

static void apple_clpc_sample(struct apple_clpc *clpc)
{
	unsigned int demand[APPLE_CLPC_RAILS] = {};
	unsigned int state[APPLE_CLPC_RAILS];
	unsigned long now = jiffies;
	unsigned int i, rail;
	u32 reason = 0, floor;

	clpc->samples++;
	if (!cpumask_equal(clpc->covered, cpu_possible_mask) &&
	    time_after_eq(now, clpc->next_scan)) {
		apple_clpc_find_clusters(clpc);
		clpc->next_scan = now + HZ;
	}

	for (i = 0; i < clpc->nr_clusters; i++) {
		struct apple_clpc_cluster *cl = &clpc->clusters[i];

		apple_clpc_sample_cluster(clpc, cl);
		for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
			demand[rail] = max(demand[rail], cl->request[rail]);
	}
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		if (demand[rail])
			reason |= 1;
	if (clpc->gpu && !clpc->hw->gpu_demand) {
		u64 updated, sequence;

		if (apple_clpc_gpu_active(clpc->gpu, &updated, &sequence)) {
			reason |= 2;
			for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
				demand[rail] = max(demand[rail],
						clpc->hw->map[rail][APPLE_CLPC_LEVELS - 1]);
		}
	}
	if (clpc->gpu_pmgr && apple_clpc_gpu_streaming(clpc, now)) {
		bool released = apple_clpc_gpu_limited(clpc, now);

		apple_clpc_gpu_floor_count(clpc, released);
		if (!released) {
			reason |= 4;
			for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
				demand[rail] = max(demand[rail],
						   clpc->hw->gpu[rail]);
		}
	}

	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		state[rail] = apple_clpc_vote(&clpc->vote[rail], demand[rail],
					      now);

	floor = apple_clpc_floor(clpc, state);
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		if (state[rail] > demand[rail])
			reason |= 16;
	clpc->last_floor = floor;
	clpc->last_reason = reason;
	clpc->last_vote_ns = ktime_get_mono_fast_ns();
	if (apple_pmc_floor_update(clpc->pmc, clpc->agent, clpc->mask, floor))
		clpc->update_errors++;

	/* Sample on time while graphics is powered, to see it power off. */
	apple_clpc_queue(clpc, !apple_clpc_floor(clpc, state) && !clpc->gpu_on);
}

static void apple_clpc_work(struct work_struct *work)
{
	struct apple_clpc *clpc = container_of(to_delayed_work(work), struct apple_clpc, work);

	mutex_lock(&clpc->sample_lock);
	if (!READ_ONCE(clpc->stopped))
		apple_clpc_sample(clpc);
	mutex_unlock(&clpc->sample_lock);
}

static void apple_clpc_idle_work(struct work_struct *work)
{
	struct apple_clpc *clpc = container_of(to_delayed_work(work), struct apple_clpc, idle_work);

	mutex_lock(&clpc->sample_lock);
	if (!READ_ONCE(clpc->stopped))
		apple_clpc_sample(clpc);
	mutex_unlock(&clpc->sample_lock);
}

static int apple_clpc_status_show(struct seq_file *s, void *unused)
{
	struct apple_clpc *clpc = s->private;
	unsigned int i, rail;

	seq_printf(s, "agent: %u floor: %#010x (saved %#010x)\n", clpc->agent,
		   apple_pmc_floor_read(clpc->pmc, clpc->agent),
		   clpc->saved_floor);
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		seq_printf(s, "%s: rail %u state %u enabled %u map %u,%u,%u,%u\n",
			   rail == APPLE_CLPC_DCS ? "dcs" : "soc",
			   clpc->hw->rail[rail], clpc->vote[rail].state,
			   apple_pmc_voter_enabled(clpc->pmc, clpc->agent,
						   clpc->hw->rail[rail]),
			   clpc->hw->map[rail][0], clpc->hw->map[rail][1],
			   clpc->hw->map[rail][2], clpc->hw->map[rail][3]);
	seq_printf(s, "last_vote: %#x reason_bits %#x sampled_ns %llu (cpu=1 host_gpu=2 streaming_gpu=4 hold=16)\n",
		   READ_ONCE(clpc->last_floor), READ_ONCE(clpc->last_reason),
		   READ_ONCE(clpc->last_vote_ns));
	if (clpc->gpu) {
		u64 updated, sequence;
		bool active = apple_clpc_gpu_active(clpc->gpu, &updated, &sequence);

		seq_printf(s, "host_gpu: active %u request_ns %llu sequence %llu\n",
			   active, updated, sequence);
	}
	seq_printf(s, "samples: %lu update errors: %lu\n", clpc->samples,
		   clpc->update_errors);
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
	}
	for (i = 0; i < clpc->nr_clusters; i++)
		seq_printf(s, "cluster cpu%u: busy %u%% level %u%% -> dcs %u soc %u\n",
			   clpc->clusters[i].policy_cpu, clpc->clusters[i].busy,
			   clpc->clusters[i].level,
			   clpc->clusters[i].request[APPLE_CLPC_DCS],
			   clpc->clusters[i].request[APPLE_CLPC_SOC]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_clpc_status);

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
	unsigned int rail, i;

	/*
	 * Each pass queues one of the two works unless stopped. A pass that
	 * began before the flag was set can still queue the other work once,
	 * which the second round cancels.
	 */
	WRITE_ONCE(clpc->stopped, true);
	for (i = 0; i < 2; i++) {
		cancel_delayed_work_sync(&clpc->work);
		cancel_delayed_work_sync(&clpc->idle_work);
	}
	apple_pmc_floor_update(clpc->pmc, clpc->agent, clpc->mask,
			       clpc->saved_floor);
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		clpc->vote[rail].state = 0;
}

static void apple_clpc_stop(void *data)
{
	struct apple_clpc *clpc = data;

	mutex_lock(&apple_clpc_clients_lock);
	dev_set_drvdata(clpc->dev, NULL);
	mutex_unlock(&apple_clpc_clients_lock);
	if (clpc->gpu) {
		unsigned long flags;

		spin_lock_irqsave(&clpc->gpu->lock, flags);
		clpc->gpu->wake = NULL;
		spin_unlock_irqrestore(&clpc->gpu->lock, flags);
	}
	apple_clpc_halt(clpc);
	debugfs_remove_recursive(clpc->debugfs);
	apple_clpc_free_clusters(clpc);
}

static int apple_clpc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_clpc *clpc;
	unsigned int rail, i;
	int ret;

	clpc = devm_kzalloc(dev, sizeof(*clpc), GFP_KERNEL);
	if (!clpc)
		return -ENOMEM;

	clpc->dev = dev;
	clpc->pmc = dev->parent;
	clpc->hw = of_device_get_match_data(dev);
	/* T6040 firmware keeps GFX powered for its whole lifetime. A separate
	 * host publication/idle input, not that supplier state, drives demand.
	 */
	if (of_device_is_compatible(dev->of_node, "apple,t6041-clpc")) {
		clpc->gpu = kzalloc_obj(*clpc->gpu);
		if (!clpc->gpu)
			return -ENOMEM;
		kref_init(&clpc->gpu->ref);
		spin_lock_init(&clpc->gpu->lock);
		ret = devm_add_action_or_reset(dev, apple_clpc_gpu_owner_put, clpc->gpu);
		if (ret)
			return ret;
	}
	if (clpc->hw->gpu_demand) {
		struct device_node *gpu = of_parse_phandle(dev->of_node,
							 "apple,gpu-power-state", 0);

		if (!gpu)
			return dev_err_probe(dev, -EINVAL, "missing graphics power state\n");
		ret = of_property_read_u32(gpu, "reg", &clpc->gpu_offset);
		clpc->gpu_pmgr = syscon_node_to_regmap(gpu->parent);
		of_node_put(gpu);
		if (ret)
			return ret;
		if (IS_ERR(clpc->gpu_pmgr))
			return PTR_ERR(clpc->gpu_pmgr);
	}
	if (!apple_pmc_rail_count(clpc->pmc))
		return dev_err_probe(dev, -ENODEV, "parent is not a PMC\n");

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

	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		if (!apple_pmc_voter_enabled(clpc->pmc, clpc->agent,
					     clpc->hw->rail[rail]))
			dev_warn(dev, "voter agent %u is not enabled on rail %u\n",
				 clpc->agent, clpc->hw->rail[rail]);

	clpc->saved_floor = apple_pmc_floor_read(clpc->pmc, clpc->agent) &
			    clpc->mask;
	apple_clpc_find_clusters(clpc);
	clpc->next_scan = jiffies + HZ;
	mutex_init(&clpc->sample_lock);
	INIT_DELAYED_WORK(&clpc->work, apple_clpc_work);
	INIT_DEFERRABLE_WORK(&clpc->idle_work, apple_clpc_idle_work);
	if (clpc->gpu)
		clpc->gpu->wake = &clpc->work;

	clpc->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("status", 0400, clpc->debugfs, clpc,
			    &apple_clpc_status_fops);

	ret = devm_add_action_or_reset(dev, apple_clpc_stop, clpc);
	if (ret)
		return ret;

	mutex_lock(&apple_clpc_clients_lock);
	platform_set_drvdata(pdev, clpc);
	mutex_unlock(&apple_clpc_clients_lock);
	apple_clpc_queue(clpc, true);
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

	for_each_possible_cpu(cpu)
		clpc->wall[cpu] = 0;
	WRITE_ONCE(clpc->stopped, false);
	apple_clpc_queue(clpc, true);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(apple_clpc_pm_ops, apple_clpc_suspend,
				apple_clpc_resume);

/* Only the stock M5/F3 busy floor is qualified on T8132. Do not invent
 * intermediate frequency-to-memory tables; idle can still release the vote.
 */
static const struct apple_clpc_hw apple_clpc_hw_t8132 = {
	.rail = { [APPLE_CLPC_DCS] = 0, [APPLE_CLPC_SOC] = 1 },
	.map = {
		[APPLE_CLPC_DCS] = { 5, 5, 5, 5 },
		[APPLE_CLPC_SOC] = { 3, 3, 3, 3 },
	},
	.gpu_demand = true,
	.gpu = { [APPLE_CLPC_DCS] = 5, [APPLE_CLPC_SOC] = 3 },
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
};

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

static struct kunit_case apple_clpc_cases[] = {
	KUNIT_CASE(apple_clpc_gpu_cold_wake_test),
	KUNIT_CASE(apple_clpc_gpu_request_lifetime_test),
	KUNIT_CASE(apple_clpc_gpu_provider_removal_test),
	{}
};
static struct kunit_suite apple_clpc_suite = {
	.name = "apple-clpc-gpu",
	.test_cases = apple_clpc_cases,
};
kunit_test_suite(apple_clpc_suite);
#endif

static const struct of_device_id apple_clpc_of_match[] = {
	{ .compatible = "apple,t8132-clpc", .data = &apple_clpc_hw_t8132 },
	{ .compatible = "apple,t8140-clpc", .data = &apple_clpc_hw_t8140 },
	{ .compatible = "apple,t6041-clpc", .data = &apple_clpc_hw_t6040 },
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
module_platform_driver(apple_clpc_driver);

MODULE_DESCRIPTION("Apple SoC host performance controller floors");
MODULE_LICENSE("Dual MIT/GPL");

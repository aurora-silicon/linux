// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC host performance controller (CLPC) floors
 *
 * On macOS the CLPC raises the floor of the memory controller (DCS) and the
 * SoC fabric while the CPU clusters or the GPU are busy, through its own
 * voter agent in the PMC. This driver keeps that agent's floor in step with
 * Linux CPU load and, optionally, GPU activity:
 *
 *  - every sampling period, each cpufreq policy reports how busy its
 *    busiest CPU was and how fast the policy runs relative to its maximum;
 *    a busy policy requests a DCS and fabric state from a table indexed by
 *    that frequency quartile;
 *  - while the GPU's PMGR power state reports the GPU as actually powered,
 *    a fixed GPU floor is requested;
 *  - each rail keeps its last request for a hold time after demand stops,
 *    then drops back to 0, so idle releases the floor.
 *
 * The native mapping from CPU and GPU demand to DCS and fabric states has
 * not been decoded. The default tables are measured policy, not a copy of
 * macOS, and can be changed with module parameters.
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/debugfs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/soc/apple/pmc.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define APPLE_CLPC_MAX_CLUSTERS	8
#define APPLE_CLPC_LEVELS	4

/* PMGR power state: ACTUAL is bits [7:4]; 0xf means powered on. */
#define APPLE_PMGR_PS_ACTUAL	GENMASK(7, 4)
#define APPLE_PMGR_PS_ACTIVE	0xf

enum apple_clpc_rail {
	APPLE_CLPC_DCS,
	APPLE_CLPC_SOC,
	APPLE_CLPC_RAILS,
};

static unsigned int sample_ms = 4;
module_param(sample_ms, uint, 0444);
MODULE_PARM_DESC(sample_ms, "Sampling period in ms");

static unsigned int busy_pct = 50;
module_param(busy_pct, uint, 0444);
MODULE_PARM_DESC(busy_pct, "Busy threshold of a CPU cluster, percent of the sampling period");

static unsigned int hold_ms = 32;
module_param(hold_ms, uint, 0444);
MODULE_PARM_DESC(hold_ms, "Time a rail keeps its last request after demand stops, ms");

static char *dcs_map = "1,2,3,4";
module_param(dcs_map, charp, 0444);
MODULE_PARM_DESC(dcs_map, "DCS state for a busy cluster below 25, 50, 75 and 100 percent of its maximum frequency");

static char *soc_map = "0,0,0,0";
module_param(soc_map, charp, 0444);
MODULE_PARM_DESC(soc_map, "Fabric state for a busy cluster below 25, 50, 75 and 100 percent of its maximum frequency");

static unsigned int gpu_dcs;
module_param(gpu_dcs, uint, 0444);
MODULE_PARM_DESC(gpu_dcs, "DCS state while the GPU is powered (0: none)");

static unsigned int gpu_soc;
module_param(gpu_soc, uint, 0444);
MODULE_PARM_DESC(gpu_soc, "Fabric state while the GPU is powered (0: none)");

/**
 * struct apple_clpc_hw - per-SoC CLPC parameters
 * @rail: PMC rail of the DCS and of the fabric
 * @max_state: highest state of the DCS and of the fabric
 */
struct apple_clpc_hw {
	unsigned int rail[APPLE_CLPC_RAILS];
	unsigned int max_state[APPLE_CLPC_RAILS];
};

struct apple_clpc_cluster {
	unsigned int policy_cpu;
	cpumask_var_t cpus;
	unsigned int busy;
	unsigned int level;
	unsigned int request[APPLE_CLPC_RAILS];
};

struct apple_clpc_vote {
	unsigned int state;
	unsigned long last_demand;
};

struct apple_clpc {
	struct device *dev;
	struct device *pmc;
	const struct apple_clpc_hw *hw;
	unsigned int agent;
	struct regmap *gpu_regmap;
	u32 gpu_offset;
	/* Samples while a vote is held; idle_work defers to busy CPUs. */
	struct delayed_work work;
	struct delayed_work idle_work;
	bool stopped;
	struct apple_clpc_cluster clusters[APPLE_CLPC_MAX_CLUSTERS];
	unsigned int nr_clusters;
	cpumask_var_t covered;
	unsigned long next_scan;
	u64 *idle;
	u64 *wall;
	u8 map[APPLE_CLPC_RAILS][APPLE_CLPC_LEVELS];
	unsigned int gpu_state[APPLE_CLPC_RAILS];
	struct apple_clpc_vote vote[APPLE_CLPC_RAILS];
	int force[APPLE_CLPC_RAILS];
	u32 mask;
	u32 saved_floor;
	bool gpu_on;
	unsigned long samples;
	unsigned long update_errors;
	struct dentry *debugfs;
};

static int apple_clpc_parse_map(struct apple_clpc *clpc, const char *s,
				enum apple_clpc_rail rail)
{
	unsigned int v[APPLE_CLPC_LEVELS];
	unsigned int i;

	if (sscanf(s, "%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3]) != 4)
		return -EINVAL;
	for (i = 0; i < APPLE_CLPC_LEVELS; i++) {
		if (v[i] > clpc->hw->max_state[rail])
			return -EINVAL;
		clpc->map[rail][i] = v[i];
	}

	return 0;
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
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		cl->request[rail] = busy >= busy_pct ? clpc->map[rail][q] : 0;
}

static bool apple_clpc_gpu_powered(struct apple_clpc *clpc)
{
	u32 ps;

	/*
	 * Hardware auto-gating powers the GPU down while the requested state
	 * stays on, so only the actual state says whether the GPU runs.
	 */
	if (regmap_read(clpc->gpu_regmap, clpc->gpu_offset, &ps))
		return false;

	return FIELD_GET(APPLE_PMGR_PS_ACTUAL, ps) == APPLE_PMGR_PS_ACTIVE;
}

static unsigned int apple_clpc_vote(struct apple_clpc_vote *vote,
				    unsigned int demand, unsigned long now)
{
	if (demand) {
		vote->last_demand = now;
		vote->state = demand;
	} else if (time_after_eq(now, vote->last_demand +
					      msecs_to_jiffies(hold_ms))) {
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
	queue_delayed_work(system_freezable_power_efficient_wq,
			   idle ? &clpc->idle_work : &clpc->work,
			   msecs_to_jiffies(sample_ms));
}

static void apple_clpc_sample(struct apple_clpc *clpc)
{
	unsigned int demand[APPLE_CLPC_RAILS] = {};
	unsigned int state[APPLE_CLPC_RAILS];
	unsigned long now = jiffies;
	unsigned int i, rail;
	int force;

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

	clpc->gpu_on = clpc->gpu_regmap &&
		       (clpc->gpu_state[APPLE_CLPC_DCS] ||
			clpc->gpu_state[APPLE_CLPC_SOC]) &&
		       apple_clpc_gpu_powered(clpc);
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++) {
		if (clpc->gpu_on)
			demand[rail] = max(demand[rail], clpc->gpu_state[rail]);
		state[rail] = apple_clpc_vote(&clpc->vote[rail], demand[rail],
					      now);
		force = READ_ONCE(clpc->force[rail]);
		if (force >= 0)
			state[rail] = force;
	}

	if (apple_pmc_floor_update(clpc->pmc, clpc->agent, clpc->mask,
				   apple_clpc_floor(clpc, state)))
		clpc->update_errors++;

	apple_clpc_queue(clpc, !apple_clpc_floor(clpc, state));
}

static void apple_clpc_work(struct work_struct *work)
{
	apple_clpc_sample(container_of(to_delayed_work(work), struct apple_clpc,
				       work));
}

static void apple_clpc_idle_work(struct work_struct *work)
{
	apple_clpc_sample(container_of(to_delayed_work(work), struct apple_clpc,
				       idle_work));
}

static int apple_clpc_status_show(struct seq_file *s, void *unused)
{
	struct apple_clpc *clpc = s->private;
	unsigned int i, rail;

	seq_printf(s, "agent: %u floor: %#010x (saved %#010x)\n", clpc->agent,
		   apple_pmc_floor_read(clpc->pmc, clpc->agent),
		   clpc->saved_floor);
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		seq_printf(s, "%s: rail %u state %u enabled %u force %d map %u,%u,%u,%u gpu %u\n",
			   rail == APPLE_CLPC_DCS ? "dcs" : "soc",
			   clpc->hw->rail[rail], clpc->vote[rail].state,
			   apple_pmc_voter_enabled(clpc->pmc, clpc->agent,
						   clpc->hw->rail[rail]),
			   READ_ONCE(clpc->force[rail]), clpc->map[rail][0],
			   clpc->map[rail][1], clpc->map[rail][2],
			   clpc->map[rail][3], clpc->gpu_state[rail]);
	seq_printf(s, "gpu: %s\n", !clpc->gpu_regmap ? "no input" :
		   !clpc->gpu_state[APPLE_CLPC_DCS] &&
		   !clpc->gpu_state[APPLE_CLPC_SOC] ? "no floor" :
		   clpc->gpu_on ? "powered" : "off");
	seq_printf(s, "samples: %lu update errors: %lu\n", clpc->samples,
		   clpc->update_errors);
	for (i = 0; i < clpc->nr_clusters; i++)
		seq_printf(s, "cluster cpu%u: busy %u%% level %u%% -> dcs %u soc %u\n",
			   clpc->clusters[i].policy_cpu, clpc->clusters[i].busy,
			   clpc->clusters[i].level,
			   clpc->clusters[i].request[APPLE_CLPC_DCS],
			   clpc->clusters[i].request[APPLE_CLPC_SOC]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_clpc_status);

/*
 * Diagnostic override: "DCS SOC" holds each rail at a fixed state, -1
 * returns that rail to demand control; "-1" alone clears both.
 */
static ssize_t apple_clpc_force_write(struct file *file,
				      const char __user *ubuf, size_t len,
				      loff_t *pos)
{
	struct apple_clpc *clpc = file->private_data;
	int v[APPLE_CLPC_RAILS], n;
	unsigned int rail;
	char buf[32];

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	n = sscanf(buf, "%d %d", &v[APPLE_CLPC_DCS], &v[APPLE_CLPC_SOC]);
	if (n == 1 && v[APPLE_CLPC_DCS] == -1)
		v[APPLE_CLPC_SOC] = -1;
	else if (n != 2)
		return -EINVAL;

	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		if (v[rail] < -1 || v[rail] > (int)clpc->hw->max_state[rail])
			return -EINVAL;
	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++)
		WRITE_ONCE(clpc->force[rail], v[rail]);

	dev_info(clpc->dev, "diagnostic override: dcs %d soc %d\n",
		 v[APPLE_CLPC_DCS], v[APPLE_CLPC_SOC]);
	return len;
}

static const struct file_operations apple_clpc_force_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = apple_clpc_force_write,
};

static int apple_clpc_gpu_input(struct apple_clpc *clpc)
{
	struct device_node *ps, *pmgr;
	int ret;

	ps = of_parse_phandle(clpc->dev->of_node, "apple,gpu-power-state", 0);
	if (!ps)
		return 0;

	ret = of_property_read_u32(ps, "reg", &clpc->gpu_offset);
	pmgr = of_get_parent(ps);
	of_node_put(ps);
	if (ret) {
		of_node_put(pmgr);
		return ret;
	}

	clpc->gpu_regmap = syscon_node_to_regmap(pmgr);
	of_node_put(pmgr);
	if (IS_ERR(clpc->gpu_regmap)) {
		ret = PTR_ERR(clpc->gpu_regmap);
		clpc->gpu_regmap = NULL;
		return ret;
	}

	return 0;
}

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
	if (!apple_pmc_rail_count(clpc->pmc))
		return dev_err_probe(dev, -ENODEV, "parent is not a PMC\n");

	ret = of_property_read_u32(dev->of_node, "reg", &clpc->agent);
	if (ret)
		return dev_err_probe(dev, ret, "missing voter agent\n");

	for (rail = 0; rail < APPLE_CLPC_RAILS; rail++) {
		if (clpc->hw->rail[rail] >= apple_pmc_rail_count(clpc->pmc))
			return -EINVAL;
		clpc->mask |= APPLE_PMC_RAIL_MASK(clpc->hw->rail[rail]);
		clpc->force[rail] = -1;
	}

	ret = apple_clpc_parse_map(clpc, dcs_map, APPLE_CLPC_DCS) ?:
	      apple_clpc_parse_map(clpc, soc_map, APPLE_CLPC_SOC);
	if (ret)
		return dev_err_probe(dev, ret, "invalid state map\n");
	if (gpu_dcs > clpc->hw->max_state[APPLE_CLPC_DCS] ||
	    gpu_soc > clpc->hw->max_state[APPLE_CLPC_SOC])
		return dev_err_probe(dev, -EINVAL, "invalid GPU state\n");
	clpc->gpu_state[APPLE_CLPC_DCS] = gpu_dcs;
	clpc->gpu_state[APPLE_CLPC_SOC] = gpu_soc;

	ret = apple_clpc_gpu_input(clpc);
	if (ret)
		return dev_err_probe(dev, ret, "invalid GPU power state\n");

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
	INIT_DELAYED_WORK(&clpc->work, apple_clpc_work);
	INIT_DEFERRABLE_WORK(&clpc->idle_work, apple_clpc_idle_work);

	clpc->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("status", 0400, clpc->debugfs, clpc,
			    &apple_clpc_status_fops);
	debugfs_create_file("force", 0200, clpc->debugfs, clpc,
			    &apple_clpc_force_fops);

	ret = devm_add_action_or_reset(dev, apple_clpc_stop, clpc);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, clpc);
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

/* T8140: DCS states 0-4 on rail 0, fabric states 0-3 on rail 1. */
static const struct apple_clpc_hw apple_clpc_hw_t8140 = {
	.rail = { [APPLE_CLPC_DCS] = 0, [APPLE_CLPC_SOC] = 1 },
	.max_state = { [APPLE_CLPC_DCS] = 4, [APPLE_CLPC_SOC] = 3 },
};

static const struct of_device_id apple_clpc_of_match[] = {
	{ .compatible = "apple,t8140-clpc", .data = &apple_clpc_hw_t8140 },
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

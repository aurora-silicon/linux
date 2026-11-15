// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC PMGR device power state driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/interconnect-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/apple/pmgr-misc.h>
#include <linux/sort.h>
#include <linux/workqueue.h>
#if IS_ENABLED(CONFIG_APPLE_PMGR_DCS_KUNIT_TEST)
#include <kunit/static_stub.h>
#endif

#define APPLE_CLKGEN_PSTATE 0
#define APPLE_CLKGEN_PSTATE_DESIRED GENMASK(3, 0)
#define APPLE_CLKGEN_PSTATE_ACTUAL GENMASK(7, 4)

#define DCS_DEV_PSTATE_MIN_T600X 7
#define DCS_VALID_STATES_T600X (BIT(5) | BIT(6) | BIT(7) | BIT(9))
#define SYS_DEV_PSTATE_SUSPEND 1

#define APPLE_PMGR_IDLE_POLL_MS 250
#define APPLE_PMGR_RAISE_TIMEOUT_US 1000
#define APPLE_PMGR_PS_ACTUAL GENMASK(7, 4)
#define APPLE_PMGR_PS_ACTIVE 0xf

/*
 * Bandwidth users that keep the fabric up whenever their power domain is
 * active. External display and ANE system domains stay on even when idle,
 * so external displays vote through apple_pmgr_fabric_hold() instead, and
 * the ANE is not covered.
 */
static const char * const apple_pmgr_fabric_voters[] = {
	"gfx", "avd_sys", "isp_sys",
};

enum sys_device {
	DEV_FABRIC,
	DEV_DCS,
	DEV_MAX,
};

struct apple_pmgr_sys_device {
	void __iomem *base;
	u32 active_state;
	u32 suspend_state;
};

struct apple_pmgr_misc_hw {
	u32 dev_min_ps[DEV_MAX];
	u32 dcs_valid_states;
};

struct apple_pmgr_dcs_opp {
	u32 state;
	u32 bw;
};

struct apple_pmgr_misc {
	struct device *dev;
	struct apple_pmgr_sys_device devices[DEV_MAX];

	/* Idle fabric governor; lock serializes fabric writes while awake. */
	struct mutex lock;
	struct delayed_work idle_work;		/* deferrable: fabric high */
	struct delayed_work idle_work_prompt;	/* bounded raise: fabric low */
	u32 fabric_now;				/* protected by lock */
	unsigned int holds;			/* protected by lock */
	bool sleeping;				/* protected by lock */
	bool voters_ok;
	bool pmp_owned; /* PMP is described: no governor, holds don't raise */
	bool pmp_sleep; /* PMP runs: this sleep leaves DCS to it (see prepare) */
	u32 pmp_fabric; /* PMP's desired fabric p-state, saved across suspend */
	struct icc_provider dcs_provider;
	struct icc_onecell_data *dcs_data;
	struct apple_pmgr_dcs_opp dcs_opps[16];
	unsigned int dcs_num_opps;
	u32 dcs_valid_states;
	bool dcs_failed;
	bool dcs_bw_active;
	struct {
		struct regmap *regmap;
		u32 reg;
	} votes[ARRAY_SIZE(apple_pmgr_fabric_voters)];
};

static struct apple_pmgr_misc *apple_pmgr_misc_inst;
static unsigned int idle_fabric_pstate;

static int apple_pmgr_raise_fabric(struct apple_pmgr_misc *misc);

/* Decimal kB/s throughout, as in the OPP and interconnect bindings. */
static int apple_pmgr_dcs_aggregate(struct icc_node *node, u32 tag, u32 avg,
				    u32 peak, u32 *agg_avg, u32 *agg_peak)
{
	/* Saturate rather than wrap a large group of consumers to a low vote. */
	*agg_avg = min_t(u64, (u64)*agg_avg + avg, U32_MAX);
	*agg_peak = max(*agg_peak, peak);
	return 0;
}

static int apple_pmgr_dcs_select(struct apple_pmgr_misc *misc, u32 bw, u32 *state)
{
	unsigned int i;

	for (i = 0; i < misc->dcs_num_opps; i++) {
		if (bw <= misc->dcs_opps[i].bw) {
			*state = misc->dcs_opps[i].state;
			return 0;
		}
	}
	return -ERANGE;
}

static bool apple_pmgr_dcs_state_valid(struct apple_pmgr_misc *misc, u32 state)
{
	return state <= FIELD_MAX(APPLE_CLKGEN_PSTATE_DESIRED) &&
	       (misc->dcs_valid_states & BIT(state));
}

static void apple_pmgr_dcs_command(u32 word, void __iomem *reg)
{
#if IS_ENABLED(CONFIG_APPLE_PMGR_DCS_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_pmgr_dcs_command, word, reg);
#endif
	writel(word, reg);
}

static int apple_pmgr_dcs_write(struct apple_pmgr_misc *misc, u32 state)
{
	void __iomem *reg = misc->devices[DEV_DCS].base + APPLE_CLKGEN_PSTATE;
	u32 val;

	if (!apple_pmgr_dcs_state_valid(misc, state))
		return -EINVAL;
	/* DCS takes a complete command word, not a replay of readback bits. */
	apple_pmgr_dcs_command(FIELD_PREP(APPLE_CLKGEN_PSTATE_DESIRED, state), reg);
	return readl_poll_timeout(reg, val,
		FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) == state,
		10, APPLE_PMGR_RAISE_TIMEOUT_US);
}

static int apple_pmgr_dcs_set(struct icc_node *src, struct icc_node *dst)
{
	struct apple_pmgr_misc *misc = src->data;
	u32 state, old, val, bw = 0;
	unsigned int i;
	int ret;

	guard(mutex)(&misc->lock);
	if (misc->pmp_owned || misc->sleeping)
		return -EBUSY;
	if (misc->dcs_failed)
		return -EIO;
	/* sync_state invokes set(node, node); keep both endpoints' constraints. */
	for (i = 0; i < misc->dcs_data->num_nodes; i++) {
		struct icc_node *node = misc->dcs_data->nodes[i];

		if (!node)
			continue;
		bw = max(bw, max(node->avg_bw, node->peak_bw));
		bw = max(bw, max(node->init_avg, node->init_peak));
	}
	ret = apple_pmgr_dcs_select(misc, bw, &state);
	if (ret)
		return ret;
	if (!apple_pmgr_dcs_state_valid(misc, state))
		return -EINVAL;
	old = misc->devices[DEV_DCS].active_state;
	val = readl(misc->devices[DEV_DCS].base + APPLE_CLKGEN_PSTATE);
	if (FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED, val) != old ||
	    FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) != old)
		return -EBUSY;
	/* Untracked DMA clients need the fabric as well as DCS bandwidth. */
	if (bw) {
		ret = apple_pmgr_raise_fabric(misc);
		if (ret)
			return ret;
	}
	ret = apple_pmgr_dcs_write(misc, state);
	if (ret) {
		/* A failed request must not replace the last confirmed vote. */
		if (apple_pmgr_dcs_write(misc, old)) {
			misc->dcs_failed = true;
			misc->dcs_bw_active = true;
			dev_err(misc->dev, "DCS vote and rollback timed out; refusing further votes\n");
		}
		return ret;
	}
	misc->devices[DEV_DCS].active_state = state;
	misc->dcs_bw_active = bw != 0;
	return 0;
}

static int apple_pmgr_dcs_get_bw(struct icc_node *node, u32 *avg, u32 *peak)
{
	struct apple_pmgr_misc *misc = node->data;

	/* Keep the boot state until all described consumers have probed. */
	*avg = 0;
	*peak = misc->dcs_opps[misc->dcs_num_opps - 1].bw;
	return 0;
}

static int apple_pmgr_dcs_opp_cmp(const void *a, const void *b)
{
	const struct apple_pmgr_dcs_opp *oa = a, *ob = b;

	return (oa->state > ob->state) - (oa->state < ob->state);
}

static int apple_pmgr_dcs_validate(struct apple_pmgr_misc *misc)
{
	unsigned int i;

	if (misc->dcs_num_opps < 2 || misc->dcs_num_opps > ARRAY_SIZE(misc->dcs_opps))
		return -EINVAL;
	sort(misc->dcs_opps, misc->dcs_num_opps, sizeof(misc->dcs_opps[0]),
	     apple_pmgr_dcs_opp_cmp, NULL);
	for (i = 0; i < misc->dcs_num_opps; i++) {
		const struct apple_pmgr_dcs_opp *opp = &misc->dcs_opps[i];

		if (!opp->bw || !apple_pmgr_dcs_state_valid(misc, opp->state))
			return -EINVAL;
		if (i && (opp->state <= misc->dcs_opps[i - 1].state ||
			  opp->bw <= misc->dcs_opps[i - 1].bw))
			return -EINVAL;
	}
	/* Never request more than the boot firmware's known working state. */
	if (misc->dcs_opps[misc->dcs_num_opps - 1].state !=
	    misc->devices[DEV_DCS].active_state)
		return -EINVAL;
	return 0;
}

static void apple_pmgr_dcs_cleanup(void *data)
{
	struct apple_pmgr_misc *misc = data;

	icc_provider_deregister(&misc->dcs_provider);
	icc_nodes_remove(&misc->dcs_provider);
}

static void apple_pmgr_misc_sync_state(struct device *dev)
{
	struct apple_pmgr_misc *misc = dev_get_drvdata(dev);

	if (misc->dcs_data)
		icc_sync_state(dev);
}

static int apple_pmgr_dcs_init(struct apple_pmgr_misc *misc)
{
	struct device *dev = misc->dev;
	struct device_node *table __free(device_node) = NULL;
	struct icc_provider *provider = &misc->dcs_provider;
	struct icc_node *node;
	u32 cells;
	unsigned int i;
	int ret;

	if (!of_property_present(dev->of_node, "operating-points-v2"))
		return of_property_present(dev->of_node, "#interconnect-cells") ? -EINVAL : 0;
	/* This interface is for the direct host owner, never a PMP fallback. */
	if (misc->pmp_owned || !of_device_is_compatible(dev->of_node, "apple,t6000-pmgr-misc"))
		return -EBUSY;
	if (of_property_count_u32_elems(dev->of_node, "operating-points-v2") != 1 ||
	    of_property_read_u32(dev->of_node, "#interconnect-cells", &cells) || cells != 1)
		return -EINVAL;
	table = of_parse_phandle(dev->of_node, "operating-points-v2", 0);
	if (!table || !of_device_is_available(table) ||
	    !of_device_is_compatible(table, "operating-points-v2-apple-dcs"))
		return -EINVAL;
	for_each_available_child_of_node_scoped(table, child) {
		struct apple_pmgr_dcs_opp *opp;

		if (misc->dcs_num_opps == ARRAY_SIZE(misc->dcs_opps))
			return -E2BIG;
		opp = &misc->dcs_opps[misc->dcs_num_opps++];
		if (of_property_present(child, "opp-supported-hw") ||
		    of_property_present(child, "turbo-mode") ||
		    of_property_count_u32_elems(child, "opp-level") != 1 ||
		    of_property_count_u32_elems(child, "opp-peak-kBps") != 1 ||
		    of_property_read_u32(child, "opp-level", &opp->state) ||
		    of_property_read_u32(child, "opp-peak-kBps", &opp->bw))
			return -EINVAL;
	}
	ret = apple_pmgr_dcs_validate(misc);
	if (ret)
		return dev_err_probe(dev, ret, "invalid measured DCS OPP table\n");
	misc->dcs_data = devm_kzalloc(dev, struct_size(misc->dcs_data, nodes, 2), GFP_KERNEL);
	if (!misc->dcs_data)
		return -ENOMEM;
	misc->dcs_data->num_nodes = 2;
	provider->dev = dev;
	provider->set = apple_pmgr_dcs_set;
	provider->aggregate = apple_pmgr_dcs_aggregate;
	provider->get_bw = apple_pmgr_dcs_get_bw;
	provider->xlate = of_icc_xlate_onecell;
	provider->data = misc->dcs_data;
	icc_provider_init(provider);
	for (i = 0; i < 2; i++) {
		node = icc_node_create_dyn();
		if (IS_ERR(node)) {
			ret = PTR_ERR(node);
			goto remove_nodes;
		}
		node->name = i ? "t600x-dcs-memory" : "t600x-dcs-clients";
		node->data = misc;
		misc->dcs_data->nodes[i] = node;
		icc_node_add(node, provider);
	}
	ret = icc_link_create(misc->dcs_data->nodes[0], misc->dcs_data->nodes[1]->id);
	if (ret)
		goto remove_nodes;
	ret = icc_provider_register(provider);
	if (ret)
		goto remove_nodes;
	return devm_add_action_or_reset(dev, apple_pmgr_dcs_cleanup, misc);

remove_nodes:
	icc_nodes_remove(provider);
	return ret;
}

static void apple_pmgr_sys_dev_set_pstate(struct apple_pmgr_misc *misc,
					  enum sys_device dev, bool active)
{
	u32 pstate;
	u32 val;

	if (!misc->devices[dev].base)
		return;

	if (active)
		pstate = misc->devices[dev].active_state;
	else
		pstate = misc->devices[dev].suspend_state;

	dev_dbg(misc->dev, "set %d ps to pstate %d\n", dev, pstate);

	if (dev == DEV_DCS && misc->dcs_valid_states) {
		if (WARN_ON_ONCE(!apple_pmgr_dcs_state_valid(misc, pstate)))
			return;
		apple_pmgr_dcs_command(FIELD_PREP(APPLE_CLKGEN_PSTATE_DESIRED, pstate),
				       misc->devices[dev].base + APPLE_CLKGEN_PSTATE);
		return;
	}

	val = readl_relaxed(misc->devices[dev].base + APPLE_CLKGEN_PSTATE);
	FIELD_MODIFY(APPLE_CLKGEN_PSTATE_DESIRED, &val, pstate);
	writel_relaxed(val, misc->devices[dev].base + APPLE_CLKGEN_PSTATE);
}

static void apple_pmgr_write_fabric(struct apple_pmgr_misc *misc, u32 pstate)
{
	void __iomem *reg = misc->devices[DEV_FABRIC].base + APPLE_CLKGEN_PSTATE;
	u32 val;

	lockdep_assert_held(&misc->lock);
	val = readl_relaxed(reg);
	val &= ~APPLE_CLKGEN_PSTATE_DESIRED;
	val |= FIELD_PREP(APPLE_CLKGEN_PSTATE_DESIRED, pstate);
	writel_relaxed(val, reg);
	misc->fabric_now = pstate;
}

/* Raise to the boot p-state and wait until the hardware reports it. */
static int apple_pmgr_raise_fabric(struct apple_pmgr_misc *misc)
{
	u32 active = misc->devices[DEV_FABRIC].active_state;
	u32 val;
	int ret;

	lockdep_assert_held(&misc->lock);
	/* PMP firmware owns the fabric; holds delegate to it. */
	if (misc->pmp_owned || misc->fabric_now == active)
		return 0;
	apple_pmgr_write_fabric(misc, active);
	ret = readl_poll_timeout(misc->devices[DEV_FABRIC].base + APPLE_CLKGEN_PSTATE,
				 val, FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) == active,
				 10, APPLE_PMGR_RAISE_TIMEOUT_US);
	/* Not confirmed: make the next caller try again. */
	if (ret)
		misc->fabric_now = U32_MAX;
	return ret;
}

/**
 * apple_pmgr_fabric_hold() - keep the fabric at its boot p-state
 *
 * For bandwidth users the idle governor cannot see, such as an external
 * display. Returns once the fabric is confirmed up, or an error if it did
 * not get there in time; the hold stands either way and must be released.
 * With PMP owning the fabric no vote is sent: the firmware decides, and
 * this returns success.
 */
int apple_pmgr_fabric_hold(void)
{
	/* Pairs with the release in probe: the instance is set up before it is seen. */
	struct apple_pmgr_misc *misc = smp_load_acquire(&apple_pmgr_misc_inst);

	if (!misc)
		return 0;
	guard(mutex)(&misc->lock);
	misc->holds++;
	return apple_pmgr_raise_fabric(misc);
}
EXPORT_SYMBOL_GPL(apple_pmgr_fabric_hold);

void apple_pmgr_fabric_release(void)
{
	/* Pairs with the release in probe: the instance is set up before it is seen. */
	struct apple_pmgr_misc *misc = smp_load_acquire(&apple_pmgr_misc_inst);

	if (!misc)
		return;
	guard(mutex)(&misc->lock);
	if (!WARN_ON_ONCE(!misc->holds))
		misc->holds--;
}
EXPORT_SYMBOL_GPL(apple_pmgr_fabric_release);

static bool apple_pmgr_voters_active(struct apple_pmgr_misc *misc)
{
	unsigned int i;
	u32 val;

	for (i = 0; i < ARRAY_SIZE(misc->votes); i++) {
		/*
		 * Only the actual state shows activity: the GPU domain keeps its
		 * target on with auto power gating and the hardware gates it
		 * when idle. Unreadable counts as busy.
		 */
		if (regmap_read(misc->votes[i].regmap, misc->votes[i].reg, &val) ||
		    FIELD_GET(APPLE_PMGR_PS_ACTUAL, val) == APPLE_PMGR_PS_ACTIVE)
			return true;
	}
	return false;
}

/* Every expected voter must be found, or the governor never lowers. */
static void apple_pmgr_find_voters(struct apple_pmgr_misc *misc)
{
	unsigned int found = 0, i;
	struct device_node *np;
	const char *label;
	u32 reg;

	for_each_compatible_node(np, NULL, "apple,pmgr-pwrstate") {
		if (!of_device_is_available(np) ||
		    of_property_read_string(np, "label", &label) ||
		    of_property_read_u32(np, "reg", &reg))
			continue;
		for (i = 0; i < ARRAY_SIZE(apple_pmgr_fabric_voters); i++) {
			struct regmap *map;

			if (strcmp(label, apple_pmgr_fabric_voters[i]) ||
			    misc->votes[i].regmap)
				continue;
			map = syscon_node_to_regmap(np->parent);
			if (IS_ERR(map))
				break;
			misc->votes[i].regmap = map;
			misc->votes[i].reg = reg;
			found++;
			break;
		}
	}
	misc->voters_ok = found == ARRAY_SIZE(apple_pmgr_fabric_voters);
	/* Desktops have no ISP: say so only where the governor is wanted. */
	if (!misc->voters_ok && READ_ONCE(idle_fabric_pstate))
		dev_warn(misc->dev, "idle fabric: found %u of %zu bandwidth voters, staying high\n",
			 found, ARRAY_SIZE(apple_pmgr_fabric_voters));
	else if (!misc->voters_ok)
		dev_dbg(misc->dev, "idle fabric: found %u of %zu bandwidth voters\n",
			found, ARRAY_SIZE(apple_pmgr_fabric_voters));
}

/* True while every cpufreq policy runs at its lowest frequency. */
static bool apple_pmgr_cpus_at_min(void)
{
	struct cpufreq_policy *policy;
	unsigned int cpu;
	bool at_min = true;

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			return false;
		if (READ_ONCE(policy->cur) > policy->cpuinfo.min_freq)
			at_min = false;
		cpufreq_cpu_put(policy);
		if (!at_min)
			break;
	}
	return at_min;
}

static void apple_pmgr_idle_eval(struct apple_pmgr_misc *misc)
{
	unsigned int idle = READ_ONCE(idle_fabric_pstate);
	u32 active = misc->devices[DEV_FABRIC].active_state;
	bool low = false;

	scoped_guard(mutex, &misc->lock) {
		if (misc->sleeping)
			return;
		low = idle && idle < active && misc->voters_ok && !misc->pmp_owned &&
		      !misc->holds && !misc->dcs_bw_active && !apple_pmgr_voters_active(misc) &&
		      apple_pmgr_cpus_at_min();
		if (low && misc->fabric_now != idle)
			apple_pmgr_write_fabric(misc, idle);
		else if (!low)
			apple_pmgr_raise_fabric(misc);
		if (!idle)
			return;
	}

	/*
	 * Freezable, so it stops before devices suspend. While the fabric is
	 * high a deferrable timer never wakes an idle CPU; while it is low a
	 * normal timer limits how long new load runs at reduced bandwidth.
	 */
	queue_delayed_work(system_freezable_power_efficient_wq,
			   low ? &misc->idle_work_prompt : &misc->idle_work,
			   msecs_to_jiffies(APPLE_PMGR_IDLE_POLL_MS));
}

static void apple_pmgr_idle_work(struct work_struct *work)
{
	apple_pmgr_idle_eval(container_of(to_delayed_work(work),
					  struct apple_pmgr_misc, idle_work));
}

static void apple_pmgr_idle_work_prompt(struct work_struct *work)
{
	apple_pmgr_idle_eval(container_of(to_delayed_work(work),
					  struct apple_pmgr_misc, idle_work_prompt));
}

static void apple_pmgr_idle_stop(struct apple_pmgr_misc *misc)
{
	cancel_delayed_work_sync(&misc->idle_work);
	cancel_delayed_work_sync(&misc->idle_work_prompt);
	guard(mutex)(&misc->lock);
	apple_pmgr_raise_fabric(misc);
}

static int idle_fabric_pstate_set(const char *val, const struct kernel_param *kp)
{
	/* Pairs with the release in probe: the instance is set up before it is seen. */
	struct apple_pmgr_misc *misc = smp_load_acquire(&apple_pmgr_misc_inst);
	unsigned int ps;
	int ret;

	ret = kstrtouint(val, 0, &ps);
	if (ret)
		return ret;
	if (ps > FIELD_MAX(APPLE_CLKGEN_PSTATE_DESIRED))
		return -EINVAL;
	if (ps && misc && misc->pmp_owned)
		return -EBUSY;
	if (ps && misc && !misc->voters_ok)
		dev_warn(misc->dev, "idle fabric: not every bandwidth voter was found, staying high\n");
	WRITE_ONCE(idle_fabric_pstate, ps);
	if (!misc)
		return 0;
	if (!ps)
		apple_pmgr_idle_stop(misc);
	else
		mod_delayed_work(system_freezable_power_efficient_wq, &misc->idle_work, 0);
	return 0;
}

static const struct kernel_param_ops idle_fabric_pstate_ops = {
	.set = idle_fabric_pstate_set,
	.get = param_get_uint,
};

/*
 * Boot leaves the fabric at its high p-state and nothing lowers it while the
 * system is awake; on t6000 a fabric p-state of 1 saves about 0.5 W at idle.
 */
module_param_cb(idle_fabric_pstate, &idle_fabric_pstate_ops, &idle_fabric_pstate, 0644);
MODULE_PARM_DESC(idle_fabric_pstate,
		 "Fabric p-state while all CPUs run at their lowest frequency and no bandwidth user is active (0 = keep the boot state)");

/*
 * Whether PMP runs: its node is available and its driver is bound. With the
 * node described but no driver (not loaded, or no values from the boot
 * loader), nothing manages DCS, and sleep takes the path without PMP.
 */
static bool apple_pmgr_pmp_running(void)
{
	struct device_node *np __free(device_node) =
		of_find_compatible_node(NULL, NULL, "apple,t6000-pmp-v2");
	struct platform_device *pdev;
	bool bound;

	if (!np || !of_device_is_available(np))
		return false;
	pdev = of_find_device_by_node(np);
	if (!pdev)
		return false;
	bound = device_is_bound(&pdev->dev);
	put_device(&pdev->dev);
	return bound;
}

/* Raise before any device suspends; noirq then applies the suspend p-state. */
static int __maybe_unused apple_pmgr_misc_prepare(struct device *dev)
{
	struct apple_pmgr_misc *misc = dev_get_drvdata(dev);

	/* Serialize the failure check and sleep gate with in-flight votes. */
	scoped_guard(mutex, &misc->lock) {
		/* Failed rollback leaves no confirmed DCS state to restore. */
		if (misc->dcs_failed)
			return -EIO;
		if (misc->devices[DEV_FABRIC].base)
			misc->sleeping = true;
	}
	misc->pmp_sleep = misc->pmp_owned && apple_pmgr_pmp_running();
	if (misc->pmp_owned && !misc->pmp_sleep)
		dev_warn_once(dev, "PMP is described but not running: sleeping without it\n");
	if (!misc->devices[DEV_FABRIC].base)
		return 0;
	apple_pmgr_idle_stop(misc);
	return 0;
}

static void __maybe_unused apple_pmgr_misc_complete(struct device *dev)
{
	struct apple_pmgr_misc *misc = dev_get_drvdata(dev);

	if (!misc->devices[DEV_FABRIC].base)
		return;
	scoped_guard(mutex, &misc->lock)
		misc->sleeping = false;
	if (READ_ONCE(idle_fabric_pstate))
		mod_delayed_work(system_freezable_power_efficient_wq, &misc->idle_work, 0);
}

static int __maybe_unused apple_pmgr_misc_suspend_noirq(struct device *dev)
{
	struct apple_pmgr_misc *misc = dev_get_drvdata(dev);
	int i;

	if (misc->pmp_sleep) {
		/*
		 * Lowering DCS under PMP leaves s2idle unable to resume; the
		 * fabric alone is safe and saves ~0.35 W in s2idle on t6000.
		 * Keep PMP's choice to hand back on resume.
		 */
		void __iomem *base = misc->devices[DEV_FABRIC].base;

		if (base) {
			misc->pmp_fabric = FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED,
						     readl_relaxed(base + APPLE_CLKGEN_PSTATE));
			apple_pmgr_sys_dev_set_pstate(misc, DEV_FABRIC, false);
		}
		return 0;
	}
	for (i = 0; i < DEV_MAX; i++)
		apple_pmgr_sys_dev_set_pstate(misc, i, false);

	return 0;
}

static int __maybe_unused apple_pmgr_misc_resume_noirq(struct device *dev)
{
	struct apple_pmgr_misc *misc = dev_get_drvdata(dev);
	int i;

	misc->fabric_now = misc->devices[DEV_FABRIC].active_state;
	if (misc->pmp_sleep) {
		if (misc->devices[DEV_FABRIC].base) {
			void __iomem *reg = misc->devices[DEV_FABRIC].base + APPLE_CLKGEN_PSTATE;
			u32 val = readl_relaxed(reg);

			/* PMP keeps running: never overwrite a choice it made since. */
			if (FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED, val) ==
			    misc->devices[DEV_FABRIC].suspend_state) {
				val &= ~APPLE_CLKGEN_PSTATE_DESIRED;
				val |= FIELD_PREP(APPLE_CLKGEN_PSTATE_DESIRED, misc->pmp_fabric);
				writel_relaxed(val, reg);
			}
		}
		return 0;
	}
	for (i = 0; i < DEV_MAX; i++)
		apple_pmgr_sys_dev_set_pstate(misc, i, true);

	return 0;
}

static int apple_pmgr_init_device(struct apple_pmgr_misc *misc,
				  const struct apple_pmgr_misc_hw *hw,
				  enum sys_device dev,
				  const char *device_name)
{
	void __iomem *base;
	char name[32];
	u32 val;

	snprintf(name, sizeof(name), "%s-ps", device_name);

	base = devm_platform_ioremap_resource_byname(
		to_platform_device(misc->dev), name);
	if (IS_ERR(base))
		return PTR_ERR(base);

	val = readl_relaxed(base + APPLE_CLKGEN_PSTATE);

	misc->devices[dev].base = base;
	misc->devices[dev].active_state =
		FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED, val);
	misc->devices[dev].suspend_state = hw->dev_min_ps[dev];

	return 0;
}

static int apple_pmgr_misc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct apple_pmgr_misc_hw *hw;
	struct apple_pmgr_misc *misc;
	struct device_node *np;
	int ret;

	misc = devm_kzalloc(dev, sizeof(*misc), GFP_KERNEL);
	if (!misc)
		return -ENOMEM;

	misc->dev = dev;
	hw = of_device_get_match_data(dev);
	if (!hw)
		return -EINVAL;
	misc->dcs_valid_states = hw->dcs_valid_states;

	ret = apple_pmgr_init_device(misc, hw, DEV_FABRIC, "fabric");
	if (ret)
		return ret;

	ret = apple_pmgr_init_device(misc, hw, DEV_DCS, "dcs");
	if (ret)
		return ret;

	platform_set_drvdata(pdev, misc);

	/*
	 * With PMP enabled its firmware owns the fabric and DCS p-states.
	 * Writing them for suspend as well leaves the system unable to resume
	 * from s2idle on t6000, and the idle governor would fight PMP.
	 */
	np = of_find_compatible_node(NULL, NULL, "apple,t6000-pmp-v2");
	misc->pmp_owned = np && of_device_is_available(np);
	of_node_put(np);
	if (misc->pmp_owned)
		dev_info(dev, "PMP manages fabric and DCS p-states\n");

	if (misc->devices[DEV_FABRIC].base) {
		mutex_init(&misc->lock);
		misc->fabric_now = misc->devices[DEV_FABRIC].active_state;
		INIT_DEFERRABLE_WORK(&misc->idle_work, apple_pmgr_idle_work);
		INIT_DELAYED_WORK(&misc->idle_work_prompt, apple_pmgr_idle_work_prompt);
		apple_pmgr_find_voters(misc);
		ret = apple_pmgr_dcs_init(misc);
		if (ret)
			return ret;
		/* Publish only a set-up instance; pairs with the readers' acquire. */
		smp_store_release(&apple_pmgr_misc_inst, misc);
		if (READ_ONCE(idle_fabric_pstate) && misc->pmp_owned) {
			dev_warn(dev, "idle fabric governor disabled: PMP manages the fabric\n");
			WRITE_ONCE(idle_fabric_pstate, 0);
		} else if (READ_ONCE(idle_fabric_pstate)) {
			queue_delayed_work(system_freezable_power_efficient_wq,
					   &misc->idle_work, 0);
		}
	}

	return 0;
}

static const struct apple_pmgr_misc_hw apple_pmgr_misc_hw_t600x = {
	.dcs_valid_states = DCS_VALID_STATES_T600X,
	.dev_min_ps = {
		[DEV_FABRIC] = SYS_DEV_PSTATE_SUSPEND,
		[DEV_DCS] = DCS_DEV_PSTATE_MIN_T600X,
	},
};

static const struct apple_pmgr_misc_hw apple_pmgr_misc_hw_t602x = {
	.dev_min_ps = {
		[DEV_FABRIC] = SYS_DEV_PSTATE_SUSPEND,
		[DEV_DCS] = SYS_DEV_PSTATE_SUSPEND,
	},
};

static const struct of_device_id apple_pmgr_misc_of_match[] = {
	{ .compatible = "apple,t6000-pmgr-misc", .data = &apple_pmgr_misc_hw_t600x },
	{ .compatible = "apple,t6020-pmgr-misc", .data = &apple_pmgr_misc_hw_t602x },
	{}
};

MODULE_DEVICE_TABLE(of, apple_pmgr_misc_of_match);

static const struct dev_pm_ops apple_pmgr_misc_pm_ops = {
	.prepare = apple_pmgr_misc_prepare,
	.complete = apple_pmgr_misc_complete,
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(apple_pmgr_misc_suspend_noirq,
				      apple_pmgr_misc_resume_noirq)
};

static struct platform_driver apple_pmgr_misc_driver = {
	.probe = apple_pmgr_misc_probe,
	.driver = {
		.name = "apple-pmgr-misc",
		.of_match_table = apple_pmgr_misc_of_match,
		/* The governor's workers and holders would outlive an unbind. */
		.suppress_bind_attrs = true,
		.sync_state = apple_pmgr_misc_sync_state,
		.pm = pm_ptr(&apple_pmgr_misc_pm_ops),
	},
};

MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_DESCRIPTION("PMGR misc driver for Apple SoCs");
MODULE_LICENSE("GPL");

module_platform_driver(apple_pmgr_misc_driver);

#if IS_ENABLED(CONFIG_APPLE_PMGR_DCS_KUNIT_TEST)
#include "apple-pmgr-dcs-test.c"
#endif

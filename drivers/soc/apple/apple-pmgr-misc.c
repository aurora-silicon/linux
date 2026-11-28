// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC PMGR device power state driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cpufreq.h>
#include <linux/debugfs.h>
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
#include <linux/rwsem.h>
#include <linux/seq_file.h>
#include <linux/soc/apple/pmgr-misc.h>
#include <linux/sort.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>
#if IS_ENABLED(CONFIG_APPLE_PMGR_DCS_KUNIT_TEST) || IS_ENABLED(CONFIG_APPLE_PMGR_FABRIC_KUNIT_TEST)
#include <kunit/static_stub.h>
#endif

#define APPLE_CLKGEN_PSTATE 0
#define APPLE_CLKGEN_PSTATE_DESIRED GENMASK(3, 0)
#define APPLE_CLKGEN_PSTATE_ACTUAL GENMASK(7, 4)
#define APPLE_CLKGEN_PSTATE_BUSY BIT(31)

#define DCS_DEV_PSTATE_MIN_T600X 7
#define DCS_VALID_STATES_T600X (BIT(5) | BIT(6) | BIT(7) | BIT(9))
/*
 * T8103 live-DRAM raw DCS states: 2->1 resets the SoC while DRAM is live.
 * Whether a board's display pipeline tolerates stepping down to 3 or 4 depends
 * on its scanout bandwidth, and the T8103 ADT does not supply a display DCS
 * floor; boards therefore opt into a verified floor via apple,dcs-min-pstate.
 */
#define DCS_VALID_STATES_T8103 (BIT(3) | BIT(4) | BIT(5) | BIT(6))
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
	DEV_DISP,
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
	bool single_step;
};

struct apple_pmgr_cpu_snap {
	u64 idle_us;
	u64 wall_us;
	bool valid;
	bool was_idle;
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
	u32 dcs_now;				/* protected by lock */
	u32 disp_now;				/* protected by lock */
	u32 idle_fabric_default;		/* validated DT fabric floor, 0 = disabled */
	u32 idle_dcs_pstate;			/* validated DT DCS floor, 0 = disabled */
	u32 idle_disp_pstate;			/* validated DT disp floor, 0 = disabled */
	unsigned int holds;			/* protected by lock */
	bool sleeping;				/* protected by lock */
	bool single_step;
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
	struct apple_pmgr_cpu_snap *cpu_snap;
};

static struct apple_pmgr_misc *apple_pmgr_misc_inst;
static DECLARE_RWSEM(apple_pmgr_inst_sem);
static unsigned int idle_fabric_pstate;
static bool idle_fabric_pstate_explicit;
static atomic_t apple_pmgr_gpu_tracked = ATOMIC_INIT(0);
static atomic_t apple_pmgr_gpu_busy = ATOMIC_INIT(0);

static int apple_pmgr_raise_fabric(struct apple_pmgr_misc *misc);
static int apple_pmgr_raise_dcs(struct apple_pmgr_misc *misc);
static bool apple_pmgr_voters_active(struct apple_pmgr_misc *misc);

#ifdef CONFIG_ARM64
static noinline u32 apple_pmgr_perf_step_hw(void __iomem *reg, u32 state)
{
	u32 val;
	unsigned long cnt = 1UL << 22;

	/*
	 * Execute the target write and busy-bit wait inside one 64-byte
	 * cache line with no memory accesses in between so a memory-clock
	 * transition cannot stall its own instruction fetch.
	 */
	asm volatile(".balign 64\n\t"
		     "ldr	%w0, [%2]\n\t"
		     "bfi	%w0, %w3, #0, #4\n\t"
		     "dsb	sy\n\t"
		     "isb\n\t"
		     "str	%w0, [%2]\n\t"
		     "1:\n\t"
		     "ldr	%w0, [%2]\n\t"
		     "tbz	%w0, #31, 2f\n\t"
		     "subs	%1, %1, #1\n\t"
		     "b.ne	1b\n\t"
		     "2:\n\t"
		     "dsb	sy\n\t"
		     : "=&r"(val), "+r"(cnt)
		     : "r"(reg), "r"(state)
		     : "memory", "cc");
	return val;
}
#else
static u32 apple_pmgr_perf_step_hw(void __iomem *reg, u32 state)
{
	u32 val = readl_relaxed(reg);

	FIELD_MODIFY(APPLE_CLKGEN_PSTATE_DESIRED, &val, state);
	writel(val, reg);
	readl_poll_timeout_atomic(reg, val, !(val & APPLE_CLKGEN_PSTATE_BUSY),
				  1, APPLE_PMGR_RAISE_TIMEOUT_US);
	return val;
}
#endif

static u32 apple_pmgr_perf_step(void __iomem *reg, u32 state)
{
#if IS_ENABLED(CONFIG_APPLE_PMGR_FABRIC_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_pmgr_perf_step, reg, state);
#endif
	return apple_pmgr_perf_step_hw(reg, state);
}

static bool apple_pmgr_step_confirmed(u32 val, u32 expected)
{
	return !(val & APPLE_CLKGEN_PSTATE_BUSY) &&
	       FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) == expected &&
	       FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED, val) == expected;
}

static bool apple_pmgr_rail_confirmed(const struct apple_pmgr_misc *misc,
				      u32 val, u32 expected)
{
	if (misc->single_step && (val & APPLE_CLKGEN_PSTATE_BUSY))
		return false;
	return FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) == expected &&
	       FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED, val) == expected;
}

static int apple_pmgr_step_to(void __iomem *reg, u32 target, u32 valid_mask, u32 *last)
{
	unsigned long flags;
	u32 cur, val;
	int ret;

	if (valid_mask && !(valid_mask & BIT(target)))
		return -EINVAL;

	ret = readl_poll_timeout_atomic(reg, val,
					!(val & APPLE_CLKGEN_PSTATE_BUSY) &&
					FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) ==
					FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED, val),
					1, APPLE_PMGR_RAISE_TIMEOUT_US);
	if (ret) {
		if (last)
			*last = val;
		return -EBUSY;
	}

	cur = FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val);
	/* Never step from an off-range state when a valid-state mask is set. */
	if (valid_mask && !(valid_mask & BIT(cur))) {
		if (last)
			*last = val;
		return -EINVAL;
	}
	if (cur == target) {
		if (last)
			*last = val;
		return 0;
	}

	/* Validate the entire single-step path before writing anything. */
	for (u32 s = cur; s != target; ) {
		s = s < target ? s + 1 : s - 1;
		if (valid_mask && !(valid_mask & BIT(s)))
			return -EINVAL;
	}

	while (cur != target) {
		cur = cur < target ? cur + 1 : cur - 1;
		local_irq_save(flags);
		val = apple_pmgr_perf_step(reg, cur);
		local_irq_restore(flags);
		if (!apple_pmgr_step_confirmed(val, cur)) {
			if (last)
				*last = val;
			return -ETIMEDOUT;
		}
	}
	if (last)
		*last = val;
	return 0;
}

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

static int apple_pmgr_fabric_floor_init(struct apple_pmgr_misc *misc)
{
	struct device *dev = misc->dev;
	u32 floor, active;

	if (!dev->of_node || !of_property_present(dev->of_node, "apple,fabric-min-pstate"))
		return 0;
	if (!misc->devices[DEV_FABRIC].base)
		return -EINVAL;
	if (of_property_count_u32_elems(dev->of_node, "apple,fabric-min-pstate") != 1 ||
	    of_property_read_u32(dev->of_node, "apple,fabric-min-pstate", &floor))
		return dev_err_probe(dev, -EINVAL, "invalid apple,fabric-min-pstate\n");
	if (!floor || floor > FIELD_MAX(APPLE_CLKGEN_PSTATE_DESIRED))
		return dev_err_probe(dev, -EINVAL, "apple,fabric-min-pstate out of range: %u\n",
				     floor);
	if (misc->pmp_owned)
		return 0;

	active = misc->devices[DEV_FABRIC].active_state;
	if (floor > active)
		return dev_err_probe(dev, -EINVAL,
				     "apple,fabric-min-pstate %u exceeds boot state %u\n",
				     floor, active);

	misc->devices[DEV_FABRIC].suspend_state = floor;
	if (floor < active)
		misc->idle_fabric_default = floor;
	return 0;
}

static int apple_pmgr_dcs_floor_init(struct apple_pmgr_misc *misc)
{
	struct device *dev = misc->dev;
	u32 floor, active;

	if (!dev->of_node || !of_property_present(dev->of_node, "apple,dcs-min-pstate"))
		return 0;
	if (misc->dcs_data)
		return dev_err_probe(dev, -EINVAL,
				     "apple,dcs-min-pstate conflicts with operating-points-v2\n");
	if (!misc->devices[DEV_DCS].base)
		return -EINVAL;
	if (of_property_count_u32_elems(dev->of_node, "apple,dcs-min-pstate") != 1 ||
	    of_property_read_u32(dev->of_node, "apple,dcs-min-pstate", &floor))
		return dev_err_probe(dev, -EINVAL, "invalid apple,dcs-min-pstate\n");
	if (!floor || floor > FIELD_MAX(APPLE_CLKGEN_PSTATE_DESIRED))
		return dev_err_probe(dev, -EINVAL, "apple,dcs-min-pstate out of range: %u\n",
				     floor);
	if (misc->pmp_owned)
		return 0;

	active = misc->devices[DEV_DCS].active_state;
	if (floor > active)
		return dev_err_probe(dev, -EINVAL,
				     "apple,dcs-min-pstate %u exceeds boot state %u\n",
				     floor, active);
	if (misc->dcs_valid_states) {
		if (!apple_pmgr_dcs_state_valid(misc, active) ||
		    !apple_pmgr_dcs_state_valid(misc, floor))
			return dev_err_probe(dev, -EINVAL,
					     "invalid DCS state range %u..%u\n",
					     floor, active);
		if (misc->single_step) {
			for (u32 s = floor; s <= active; s++) {
				if (!apple_pmgr_dcs_state_valid(misc, s))
					return dev_err_probe(dev, -EINVAL,
							     "invalid intermediate DCS state %u\n",
							     s);
			}
		}
		misc->dcs_valid_states &= ~GENMASK(floor - 1, 0);
	}

	misc->devices[DEV_DCS].suspend_state = floor;
	if (floor < active)
		misc->idle_dcs_pstate = floor;
	return 0;
}

static int apple_pmgr_disp_floor_init(struct apple_pmgr_misc *misc)
{
	struct device *dev = misc->dev;
	u32 floor, active;

	if (!dev->of_node || !of_property_present(dev->of_node, "apple,disp-min-pstate"))
		return 0;
	if (!misc->devices[DEV_DISP].base)
		return dev_err_probe(dev, -EINVAL,
				     "apple,disp-min-pstate requires disp-ps reg\n");
	if (of_property_count_u32_elems(dev->of_node, "apple,disp-min-pstate") != 1 ||
	    of_property_read_u32(dev->of_node, "apple,disp-min-pstate", &floor))
		return dev_err_probe(dev, -EINVAL, "invalid apple,disp-min-pstate\n");
	if (!floor || floor > FIELD_MAX(APPLE_CLKGEN_PSTATE_DESIRED))
		return dev_err_probe(dev, -EINVAL, "apple,disp-min-pstate out of range: %u\n",
				     floor);
	if (misc->pmp_owned)
		return 0;

	active = misc->devices[DEV_DISP].active_state;
	if (floor > active)
		return dev_err_probe(dev, -EINVAL,
				     "apple,disp-min-pstate %u exceeds boot state %u\n",
				     floor, active);

	misc->devices[DEV_DISP].suspend_state = floor;
	if (floor < active)
		misc->idle_disp_pstate = floor;
	return 0;
}

static int apple_pmgr_sys_dev_set_pstate(struct apple_pmgr_misc *misc,
					 enum sys_device dev, bool active)
{
	u32 pstate;
	u32 val = 0;
	int ret;

	if (!misc->devices[dev].base)
		return 0;

	if (active)
		pstate = misc->devices[dev].active_state;
	else
		pstate = misc->devices[dev].suspend_state;

	dev_dbg(misc->dev, "set %d ps to pstate %d\n", dev, pstate);

	if (misc->single_step) {
		u32 valid_mask = dev == DEV_DCS ? misc->dcs_valid_states : 0;
		u32 active_ps = misc->devices[dev].active_state;

		if (misc->devices[dev].suspend_state == active_ps)
			return 0;
		if (valid_mask &&
		    (WARN_ON_ONCE(!apple_pmgr_dcs_state_valid(misc, pstate)) ||
		     (!active && WARN_ON_ONCE(!apple_pmgr_dcs_state_valid(misc, active_ps)))))
			return -EINVAL;
		ret = apple_pmgr_step_to(misc->devices[dev].base + APPLE_CLKGEN_PSTATE,
					 pstate, valid_mask, &val);
		if (ret)
			dev_err(misc->dev, "set %d ps to %u failed: %d (%#x)\n",
				dev, pstate, ret, val);
		return ret;
	}

	if (dev == DEV_DCS && misc->dcs_valid_states) {
		if (WARN_ON_ONCE(!apple_pmgr_dcs_state_valid(misc, pstate)))
			return -EINVAL;
		apple_pmgr_dcs_command(FIELD_PREP(APPLE_CLKGEN_PSTATE_DESIRED, pstate),
				       misc->devices[dev].base + APPLE_CLKGEN_PSTATE);
		return 0;
	}

	val = readl_relaxed(misc->devices[dev].base + APPLE_CLKGEN_PSTATE);
	FIELD_MODIFY(APPLE_CLKGEN_PSTATE_DESIRED, &val, pstate);
	writel_relaxed(val, misc->devices[dev].base + APPLE_CLKGEN_PSTATE);
	return 0;
}

static int apple_pmgr_write_fabric(struct apple_pmgr_misc *misc, u32 pstate)
{
	void __iomem *reg = misc->devices[DEV_FABRIC].base + APPLE_CLKGEN_PSTATE;
	u32 val;
	int ret;

	lockdep_assert_held(&misc->lock);
	if (misc->single_step) {
		ret = apple_pmgr_step_to(reg, pstate, 0, &val);
		if (ret) {
			misc->fabric_now = U32_MAX;
			return ret;
		}
		misc->fabric_now = pstate;
		return 0;
	}
	val = readl_relaxed(reg);
	val &= ~APPLE_CLKGEN_PSTATE_DESIRED;
	val |= FIELD_PREP(APPLE_CLKGEN_PSTATE_DESIRED, pstate);
	writel_relaxed(val, reg);
	misc->fabric_now = pstate;
	return 0;
}

static int apple_pmgr_write_dcs(struct apple_pmgr_misc *misc, u32 pstate)
{
	void __iomem *reg = misc->devices[DEV_DCS].base + APPLE_CLKGEN_PSTATE;
	u32 val;
	int ret;

	lockdep_assert_held(&misc->lock);
	if (!misc->devices[DEV_DCS].base)
		return 0;
	if (misc->dcs_valid_states) {
		if (!apple_pmgr_dcs_state_valid(misc, pstate) ||
		    !apple_pmgr_dcs_state_valid(misc, misc->devices[DEV_DCS].active_state))
			return -EINVAL;
	}
	if (misc->single_step) {
		ret = apple_pmgr_step_to(reg, pstate, misc->dcs_valid_states, &val);
		if (ret) {
			misc->dcs_now = U32_MAX;
			return ret;
		}
		misc->dcs_now = pstate;
		return 0;
	}
	if (misc->dcs_valid_states) {
		apple_pmgr_dcs_command(FIELD_PREP(APPLE_CLKGEN_PSTATE_DESIRED, pstate), reg);
		misc->dcs_now = pstate;
		return 0;
	}
	val = readl_relaxed(reg);
	FIELD_MODIFY(APPLE_CLKGEN_PSTATE_DESIRED, &val, pstate);
	writel_relaxed(val, reg);
	misc->dcs_now = pstate;
	return 0;
}

static int apple_pmgr_write_disp(struct apple_pmgr_misc *misc, u32 pstate)
{
	void __iomem *reg = misc->devices[DEV_DISP].base + APPLE_CLKGEN_PSTATE;
	u32 val;
	int ret;

	lockdep_assert_held(&misc->lock);
	if (!misc->devices[DEV_DISP].base)
		return 0;
	if (misc->single_step) {
		ret = apple_pmgr_step_to(reg, pstate, 0, &val);
		if (ret) {
			misc->disp_now = U32_MAX;
			return ret;
		}
		misc->disp_now = pstate;
		return 0;
	}
	val = readl_relaxed(reg);
	FIELD_MODIFY(APPLE_CLKGEN_PSTATE_DESIRED, &val, pstate);
	writel_relaxed(val, reg);
	misc->disp_now = pstate;
	return 0;
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
	if (misc->single_step && misc->devices[DEV_FABRIC].suspend_state == active) {
		ret = readl_poll_timeout(misc->devices[DEV_FABRIC].base + APPLE_CLKGEN_PSTATE,
					 val, apple_pmgr_rail_confirmed(misc, val, active),
					 10, APPLE_PMGR_RAISE_TIMEOUT_US);
		if (!ret)
			misc->fabric_now = active;
		return ret;
	}
	ret = apple_pmgr_write_fabric(misc, active);
	if (ret)
		return ret;
	ret = readl_poll_timeout(misc->devices[DEV_FABRIC].base + APPLE_CLKGEN_PSTATE,
				 val, apple_pmgr_rail_confirmed(misc, val, active),
				 10, APPLE_PMGR_RAISE_TIMEOUT_US);
	/* Not confirmed: make the next caller try again. */
	if (ret)
		misc->fabric_now = U32_MAX;
	return ret;
}

static int apple_pmgr_raise_dcs(struct apple_pmgr_misc *misc)
{
	u32 active = misc->devices[DEV_DCS].active_state;
	u32 val;
	int ret;

	lockdep_assert_held(&misc->lock);
	if (misc->pmp_owned || !misc->devices[DEV_DCS].base ||
	    misc->dcs_data || misc->dcs_now == active)
		return 0;
	if (!misc->idle_dcs_pstate) {
		ret = readl_poll_timeout(misc->devices[DEV_DCS].base + APPLE_CLKGEN_PSTATE,
					 val, apple_pmgr_rail_confirmed(misc, val, active),
					 10, APPLE_PMGR_RAISE_TIMEOUT_US);
		if (!ret)
			misc->dcs_now = active;
		return ret;
	}
	ret = apple_pmgr_write_dcs(misc, active);
	if (ret)
		return ret;
	ret = readl_poll_timeout(misc->devices[DEV_DCS].base + APPLE_CLKGEN_PSTATE,
				 val, apple_pmgr_rail_confirmed(misc, val, active),
				 10, APPLE_PMGR_RAISE_TIMEOUT_US);
	if (ret)
		misc->dcs_now = U32_MAX;
	return ret;
}

static int apple_pmgr_raise_disp(struct apple_pmgr_misc *misc)
{
	u32 active = misc->devices[DEV_DISP].active_state;
	u32 val;
	int ret;

	lockdep_assert_held(&misc->lock);
	if (misc->pmp_owned || !misc->devices[DEV_DISP].base ||
	    misc->disp_now == active)
		return 0;
	if (!misc->idle_disp_pstate) {
		ret = readl_poll_timeout(misc->devices[DEV_DISP].base + APPLE_CLKGEN_PSTATE,
					 val, apple_pmgr_rail_confirmed(misc, val, active),
					 10, APPLE_PMGR_RAISE_TIMEOUT_US);
		if (!ret)
			misc->disp_now = active;
		return ret;
	}
	ret = apple_pmgr_write_disp(misc, active);
	if (ret)
		return ret;
	ret = readl_poll_timeout(misc->devices[DEV_DISP].base + APPLE_CLKGEN_PSTATE,
				 val, apple_pmgr_rail_confirmed(misc, val, active),
				 10, APPLE_PMGR_RAISE_TIMEOUT_US);
	if (ret)
		misc->disp_now = U32_MAX;
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
	guard(rwsem_read)(&apple_pmgr_inst_sem);
	struct apple_pmgr_misc *misc = smp_load_acquire(&apple_pmgr_misc_inst);
	int ret;

	if (!misc)
		return 0;
	guard(mutex)(&misc->lock);
	misc->holds++;
	ret = apple_pmgr_raise_fabric(misc);
	ret = apple_pmgr_raise_dcs(misc) ?: ret;
	ret = apple_pmgr_raise_disp(misc) ?: ret;
	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmgr_fabric_hold);

void apple_pmgr_fabric_release(void)
{
	guard(rwsem_read)(&apple_pmgr_inst_sem);
	struct apple_pmgr_misc *misc = smp_load_acquire(&apple_pmgr_misc_inst);

	if (!misc)
		return;
	guard(mutex)(&misc->lock);
	if (!WARN_ON_ONCE(!misc->holds))
		misc->holds--;
}
EXPORT_SYMBOL_GPL(apple_pmgr_fabric_release);

void apple_pmgr_gpu_tracking_start(void)
{
	atomic_inc(&apple_pmgr_gpu_tracked);
}
EXPORT_SYMBOL_GPL(apple_pmgr_gpu_tracking_start);

void apple_pmgr_gpu_tracking_stop(void)
{
	guard(rwsem_read)(&apple_pmgr_inst_sem);
	/* Pairs with the release in probe/teardown. */
	struct apple_pmgr_misc *misc = smp_load_acquire(&apple_pmgr_misc_inst);

	WARN_ON_ONCE(atomic_dec_return(&apple_pmgr_gpu_tracked) < 0);
	if (!misc)
		return;
	guard(mutex)(&misc->lock);
	if (!misc->sleeping && apple_pmgr_voters_active(misc)) {
		apple_pmgr_raise_fabric(misc);
		apple_pmgr_raise_dcs(misc);
		apple_pmgr_raise_disp(misc);
	}
}
EXPORT_SYMBOL_GPL(apple_pmgr_gpu_tracking_stop);

int apple_pmgr_gpu_busy_get(void)
{
	guard(rwsem_read)(&apple_pmgr_inst_sem);
	/* Pairs with the release in probe/teardown. */
	struct apple_pmgr_misc *misc = smp_load_acquire(&apple_pmgr_misc_inst);
	int ret;

	if (!misc) {
		atomic_inc(&apple_pmgr_gpu_busy);
		return 0;
	}
	guard(mutex)(&misc->lock);
	if (misc->sleeping)
		return -EAGAIN;
	atomic_inc(&apple_pmgr_gpu_busy);
	ret = apple_pmgr_raise_fabric(misc);
	ret = apple_pmgr_raise_dcs(misc) ?: ret;
	ret = apple_pmgr_raise_disp(misc) ?: ret;
	if (ret)
		atomic_dec(&apple_pmgr_gpu_busy);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmgr_gpu_busy_get);

void apple_pmgr_gpu_busy_put(void)
{
	WARN_ON_ONCE(atomic_dec_return(&apple_pmgr_gpu_busy) < 0);
}
EXPORT_SYMBOL_GPL(apple_pmgr_gpu_busy_put);

static bool apple_pmgr_voters_active(struct apple_pmgr_misc *misc)
{
	unsigned int i;
	u32 val;

	for (i = 0; i < ARRAY_SIZE(misc->votes); i++) {
		if (i == 0 && atomic_read(&apple_pmgr_gpu_tracked) > 0) {
			if (atomic_read(&apple_pmgr_gpu_busy) > 0)
				return true;
			continue;
		}
		/*
		 * Only the actual state shows activity: the GPU domain keeps its
		 * target on with auto power gating and the hardware gates it
		 * when idle. Missing or unreadable voter counts as busy.
		 */
		if (!misc->votes[i].regmap ||
		    regmap_read(misc->votes[i].regmap, misc->votes[i].reg, &val) ||
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

/* True while every cpufreq policy runs at its lowest frequency or is idle. */
static bool apple_pmgr_cpus_at_min(void)
{
	/* Pairs with the release in probe/teardown. */
	struct apple_pmgr_misc *misc = smp_load_acquire(&apple_pmgr_misc_inst);
	struct cpufreq_policy *policy;
	unsigned int cpu;
	bool at_min = true;

#if IS_ENABLED(CONFIG_APPLE_PMGR_FABRIC_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_pmgr_cpus_at_min);
#endif
	for_each_online_cpu(cpu) {
		bool cpu_idle = false;

		if (misc && misc->cpu_snap && cpu < nr_cpu_ids) {
#ifdef CONFIG_CPU_FREQ
			struct apple_pmgr_cpu_snap *s = &misc->cpu_snap[cpu];
			u64 idle_us, wall_us = 0;

			idle_us = get_cpu_idle_time(cpu, &wall_us, 1);
			if (s->valid && wall_us > s->wall_us) {
				u64 d_wall = wall_us - s->wall_us;

				if (d_wall >= 50000) {
					u64 d_idle = idle_us >= s->idle_us ?
						     idle_us - s->idle_us : 0;
					u64 d_busy = d_wall > d_idle ? d_wall - d_idle : 0;

					s->idle_us = idle_us;
					s->wall_us = wall_us;
					/* <= 5% busy over >= 50 ms window counts as parked-idle. */
					s->was_idle = d_busy * 20 <= d_wall;
				}
				cpu_idle = s->was_idle;
			} else {
				s->idle_us = idle_us;
				s->wall_us = wall_us;
				s->valid = true;
			}
#endif
		}
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			return false;
		if (READ_ONCE(policy->cur) > policy->cpuinfo.min_freq && !cpu_idle)
			at_min = false;
		cpufreq_cpu_put(policy);
	}
	return at_min;
}

static void apple_pmgr_idle_eval(struct apple_pmgr_misc *misc)
{
	unsigned int idle_fab = READ_ONCE(idle_fabric_pstate);
	unsigned int idle_dcs = misc->idle_dcs_pstate;
	unsigned int idle_disp = misc->idle_disp_pstate;
	u32 active_fab = misc->devices[DEV_FABRIC].active_state;
	u32 active_dcs = misc->devices[DEV_DCS].active_state;
	u32 active_disp = misc->devices[DEV_DISP].active_state;
	bool idle_ok = false;
	bool low_fab = false;
	bool low_dcs = false;
	bool low_disp = false;

	scoped_guard(mutex, &misc->lock) {
		if (misc->sleeping)
			return;
		idle_ok = misc->voters_ok && !misc->pmp_owned &&
			  !misc->holds && !misc->dcs_bw_active &&
			  !apple_pmgr_voters_active(misc) &&
			  apple_pmgr_cpus_at_min();
		low_fab = idle_fab && idle_fab >= misc->devices[DEV_FABRIC].suspend_state &&
			  idle_fab < active_fab && idle_ok;
		low_dcs = idle_dcs && idle_dcs >= misc->devices[DEV_DCS].suspend_state &&
			  idle_dcs < active_dcs && misc->devices[DEV_DCS].base &&
			  !misc->dcs_data && idle_ok;
		low_disp = idle_disp && idle_disp >= misc->devices[DEV_DISP].suspend_state &&
			   idle_disp < active_disp && misc->devices[DEV_DISP].base && idle_ok;
		if (low_fab && misc->fabric_now != idle_fab)
			apple_pmgr_write_fabric(misc, idle_fab);
		else if (!low_fab)
			apple_pmgr_raise_fabric(misc);
		if (low_dcs && misc->dcs_now != idle_dcs)
			apple_pmgr_write_dcs(misc, idle_dcs);
		else if (!low_dcs)
			apple_pmgr_raise_dcs(misc);
		if (low_disp && misc->disp_now != idle_disp)
			apple_pmgr_write_disp(misc, idle_disp);
		else if (!low_disp)
			apple_pmgr_raise_disp(misc);
		if (!idle_fab && !idle_dcs && !idle_disp)
			return;
		/*
		 * Freezable, so it stops before devices suspend. Queued under
		 * misc->lock so teardown setting sleeping=true prevents any
		 * cross-requeue between idle_work and idle_work_prompt.
		 */
		queue_delayed_work(system_freezable_power_efficient_wq,
				   (low_fab || low_dcs || low_disp) ?
					&misc->idle_work_prompt :
					&misc->idle_work,
				   msecs_to_jiffies(APPLE_PMGR_IDLE_POLL_MS));
	}
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

static int apple_pmgr_idle_stop(struct apple_pmgr_misc *misc)
{
	int ret;

	cancel_delayed_work_sync(&misc->idle_work);
	cancel_delayed_work_sync(&misc->idle_work_prompt);
	guard(mutex)(&misc->lock);
	ret = apple_pmgr_raise_fabric(misc);
	ret = apple_pmgr_raise_dcs(misc) ?: ret;
	return apple_pmgr_raise_disp(misc) ?: ret;
}

static int idle_fabric_pstate_set(const char *val, const struct kernel_param *kp)
{
	guard(rwsem_read)(&apple_pmgr_inst_sem);
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
	if (ps && misc && ps < misc->devices[DEV_FABRIC].suspend_state)
		return -EINVAL;
	if (ps && misc && !misc->voters_ok)
		dev_warn(misc->dev, "idle fabric: not every bandwidth voter was found, staying high\n");
	WRITE_ONCE(idle_fabric_pstate, ps);
	WRITE_ONCE(idle_fabric_pstate_explicit, true);
	if (!misc)
		return 0;
	if (!ps && !misc->idle_dcs_pstate && !misc->idle_disp_pstate)
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
static struct device_node *apple_pmgr_find_pmp_node(void)
{
	static const char * const pmp_compats[] = {
		"apple,t6000-pmp-v2",
		"apple,t8103-pmp",
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pmp_compats); i++) {
		struct device_node *np = of_find_compatible_node(NULL, NULL, pmp_compats[i]);

		if (np) {
			if (of_device_is_available(np))
				return np;
			of_node_put(np);
		}
	}
	return NULL;
}

static bool apple_pmgr_pmp_running(void)
{
	struct device_node *np __free(device_node) = apple_pmgr_find_pmp_node();
	struct platform_device *pdev;
	bool bound;

	if (!np)
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
	int ret;

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
	ret = apple_pmgr_idle_stop(misc);
	if (ret) {
		scoped_guard(mutex, &misc->lock)
			misc->sleeping = false;
		if (READ_ONCE(idle_fabric_pstate) || misc->idle_dcs_pstate ||
		    misc->idle_disp_pstate)
			mod_delayed_work(system_freezable_power_efficient_wq, &misc->idle_work, 0);
		return ret;
	}
	return 0;
}

static void __maybe_unused apple_pmgr_misc_complete(struct device *dev)
{
	struct apple_pmgr_misc *misc = dev_get_drvdata(dev);

	if (!misc->devices[DEV_FABRIC].base)
		return;
	scoped_guard(mutex, &misc->lock)
		misc->sleeping = false;
	if (READ_ONCE(idle_fabric_pstate) || misc->idle_dcs_pstate ||
	    misc->idle_disp_pstate)
		mod_delayed_work(system_freezable_power_efficient_wq, &misc->idle_work, 0);
}

static u32 apple_pmgr_verify_active(struct apple_pmgr_misc *misc, enum sys_device dev)
{
	u32 active = misc->devices[dev].active_state;
	u32 val;

	if (!misc->devices[dev].base)
		return active;
	val = readl_relaxed(misc->devices[dev].base + APPLE_CLKGEN_PSTATE);
	if (apple_pmgr_rail_confirmed(misc, val, active))
		return active;
	return U32_MAX;
}

static int apple_pmgr_misc_suspend_noirq_target(struct apple_pmgr_misc *misc,
						suspend_state_t target)
{
	int i;

	/*
	 * On SoCs whose deep-sleep commit steps DCS itself (T8103), lowering
	 * fabric/DCS in suspend_noirq runs before the sleep engine stops the
	 * coprocessors and breaks deep sleep. Lower only for s2idle.
	 */
	if (misc->single_step && target != PM_SUSPEND_TO_IDLE)
		return 0;

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
			return apple_pmgr_sys_dev_set_pstate(misc, DEV_FABRIC, false);
		}
		return 0;
	}
	for (i = 0; i < DEV_MAX; i++) {
		int ret = apple_pmgr_sys_dev_set_pstate(misc, i, false);

		if (ret) {
			/* PM core skips resume_noirq on the failing device; roll back now. */
			for (int j = i; j >= 0; j--) {
				int r = apple_pmgr_sys_dev_set_pstate(misc, j, true);

				if (j == DEV_FABRIC)
					misc->fabric_now = r ? U32_MAX :
							   apple_pmgr_verify_active(misc,
										    DEV_FABRIC);
				else if (j == DEV_DCS)
					misc->dcs_now = r ? U32_MAX :
							apple_pmgr_verify_active(misc, DEV_DCS);
				else if (j == DEV_DISP)
					misc->disp_now = r ? U32_MAX :
							 apple_pmgr_verify_active(misc, DEV_DISP);
			}
			return ret;
		}
	}

	return 0;
}

static int __maybe_unused apple_pmgr_misc_suspend_noirq(struct device *dev)
{
	return apple_pmgr_misc_suspend_noirq_target(dev_get_drvdata(dev),
						    pm_suspend_target_state);
}

static int apple_pmgr_misc_resume_noirq_target(struct apple_pmgr_misc *misc,
					       suspend_state_t target)
{
	int i, err = 0;

	if (misc->single_step && target != PM_SUSPEND_TO_IDLE) {
		misc->fabric_now = apple_pmgr_verify_active(misc, DEV_FABRIC);
		misc->dcs_now = apple_pmgr_verify_active(misc, DEV_DCS);
		misc->disp_now = apple_pmgr_verify_active(misc, DEV_DISP);
		return 0;
	}
	if (misc->pmp_sleep) {
		misc->fabric_now = misc->devices[DEV_FABRIC].active_state;
		misc->dcs_now = misc->devices[DEV_DCS].active_state;
		misc->disp_now = misc->devices[DEV_DISP].active_state;
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
	for (i = 0; i < DEV_MAX; i++) {
		int ret = apple_pmgr_sys_dev_set_pstate(misc, i, true);

		if (i == DEV_FABRIC)
			misc->fabric_now = ret ? U32_MAX :
					   apple_pmgr_verify_active(misc, DEV_FABRIC);
		else if (i == DEV_DCS)
			misc->dcs_now = ret ? U32_MAX :
					apple_pmgr_verify_active(misc, DEV_DCS);
		else if (i == DEV_DISP)
			misc->disp_now = ret ? U32_MAX :
					 apple_pmgr_verify_active(misc, DEV_DISP);
		if (ret && !err)
			err = ret;
	}

	return err;
}

static int __maybe_unused apple_pmgr_misc_resume_noirq(struct device *dev)
{
	return apple_pmgr_misc_resume_noirq_target(dev_get_drvdata(dev),
						   pm_suspend_target_state);
}

static int apple_pmgr_init_device(struct apple_pmgr_misc *misc,
				  const struct apple_pmgr_misc_hw *hw,
				  enum sys_device dev,
				  const char *device_name,
				  bool optional)
{
	struct platform_device *pdev = to_platform_device(misc->dev);
	void __iomem *base;
	char name[32];
	u32 val;

	snprintf(name, sizeof(name), "%s-ps", device_name);

	if (optional && !platform_get_resource_byname(pdev, IORESOURCE_MEM, name))
		return 0;

	base = devm_platform_ioremap_resource_byname(pdev, name);
	if (IS_ERR(base))
		return PTR_ERR(base);

	val = readl_relaxed(base + APPLE_CLKGEN_PSTATE);

	misc->devices[dev].base = base;
	misc->devices[dev].active_state =
		FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED, val);
	misc->devices[dev].suspend_state =
		hw->dev_min_ps[dev] ?: misc->devices[dev].active_state;

	return 0;
}

/* Read only: the whole clock-generator word of one system device (the desired state is bits 3:0, the actual state bits 7:4), no side effect. */
static int apple_pmgr_misc_ps_show(struct seq_file *s, void *unused)
{
	const struct apple_pmgr_sys_device *d = s->private;

	seq_printf(s, "%#010x\n", readl_relaxed(d->base + APPLE_CLKGEN_PSTATE));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_pmgr_misc_ps);

static bool apple_pmgr_cpus_at_min_peek(const struct apple_pmgr_misc *misc)
{
	struct cpufreq_policy *policy;
	unsigned int cpu;
	bool at_min = true;

	for_each_online_cpu(cpu) {
		bool cpu_idle = misc && misc->cpu_snap && cpu < nr_cpu_ids &&
				misc->cpu_snap[cpu].valid && misc->cpu_snap[cpu].was_idle;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			return false;
		if (READ_ONCE(policy->cur) > policy->cpuinfo.min_freq && !cpu_idle)
			at_min = false;
		cpufreq_cpu_put(policy);
	}
	return at_min;
}

static int apple_pmgr_misc_status_show(struct seq_file *s, void *unused)
{
	struct apple_pmgr_misc *misc = s->private;
	unsigned int i;
	u32 val;

	guard(mutex)(&misc->lock);
	seq_printf(s, "pmp_owned=%d voters_ok=%d holds=%u dcs_bw_active=%d cpus_at_min=%d gpu_tracked=%d gpu_busy=%d\n",
		   misc->pmp_owned, misc->voters_ok, misc->holds,
		   misc->dcs_bw_active, apple_pmgr_cpus_at_min_peek(misc),
		   atomic_read(&apple_pmgr_gpu_tracked),
		   atomic_read(&apple_pmgr_gpu_busy));
	seq_printf(s, "fabric_active=%u fabric_now=%u dcs_active=%u dcs_now=%u dcs_idle=%u\n",
		   misc->devices[DEV_FABRIC].active_state, misc->fabric_now,
		   misc->devices[DEV_DCS].active_state, misc->dcs_now,
		   misc->idle_dcs_pstate);
	if (misc->devices[DEV_DISP].base)
		seq_printf(s, "disp_active=%u disp_now=%u disp_idle=%u\n",
			   misc->devices[DEV_DISP].active_state, misc->disp_now,
			   misc->idle_disp_pstate);
	for (i = 0; i < ARRAY_SIZE(misc->votes); i++) {
		if (!misc->votes[i].regmap) {
			seq_printf(s, "voter[%s]=missing\n", apple_pmgr_fabric_voters[i]);
		} else if (regmap_read(misc->votes[i].regmap, misc->votes[i].reg, &val)) {
			seq_printf(s, "voter[%s]=err\n", apple_pmgr_fabric_voters[i]);
		} else {
			seq_printf(s, "voter[%s]=%#010x actual=%lu\n",
				   apple_pmgr_fabric_voters[i], val,
				   FIELD_GET(APPLE_PMGR_PS_ACTUAL, val));
		}
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_pmgr_misc_status);

static void apple_pmgr_misc_debugfs_remove(void *dir)
{
	debugfs_remove(dir);
}

static void apple_pmgr_misc_debugfs_init(struct apple_pmgr_misc *misc)
{
	static const char * const names[DEV_MAX] = { "fabric_ps", "dcs_ps", "disp_ps" };
	char *dirname = devm_kasprintf(misc->dev, GFP_KERNEL, "apple-pmgr-misc-%s", dev_name(misc->dev));
	struct dentry *dir;
	int i;

	if (!dirname)
		return;
	dir = debugfs_create_dir(dirname, NULL);
	for (i = 0; i < DEV_MAX; i++)
		if (misc->devices[i].base)
			debugfs_create_file(names[i], 0400, dir, &misc->devices[i], &apple_pmgr_misc_ps_fops);
	debugfs_create_file("status", 0400, dir, misc, &apple_pmgr_misc_status_fops);
	devm_add_action_or_reset(misc->dev, apple_pmgr_misc_debugfs_remove, dir);
}

static void apple_pmgr_misc_teardown(void *data)
{
	struct apple_pmgr_misc *misc = data;
	bool published;

	scoped_guard(rwsem_write, &apple_pmgr_inst_sem) {
		published = apple_pmgr_misc_inst == misc;
		if (published) {
			/* Pairs with the acquire in readers before freeing misc. */
			smp_store_release(&apple_pmgr_misc_inst, NULL);
		}
	}
	if (!published)
		return;
	scoped_guard(mutex, &misc->lock)
		misc->sleeping = true;
	cancel_delayed_work_sync(&misc->idle_work);
	cancel_delayed_work_sync(&misc->idle_work_prompt);
	guard(mutex)(&misc->lock);
	apple_pmgr_raise_fabric(misc);
	apple_pmgr_raise_dcs(misc);
	apple_pmgr_raise_disp(misc);
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
	misc->single_step = hw->single_step;

	ret = apple_pmgr_init_device(misc, hw, DEV_FABRIC, "fabric", false);
	if (ret)
		return ret;

	ret = apple_pmgr_init_device(misc, hw, DEV_DCS, "dcs", false);
	if (ret)
		return ret;

	ret = apple_pmgr_init_device(misc, hw, DEV_DISP, "disp", true);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, misc);

	/*
	 * With PMP enabled its firmware owns the fabric and DCS p-states.
	 * Writing them for suspend as well leaves the system unable to resume
	 * from s2idle on t6000, and the idle governor would fight PMP.
	 */
	np = apple_pmgr_find_pmp_node();
	misc->pmp_owned = !!np;
	of_node_put(np);
	if (misc->pmp_owned)
		dev_info(dev, "PMP manages fabric and DCS p-states\n");

	if (misc->devices[DEV_FABRIC].base) {
		bool start_idle;

		mutex_init(&misc->lock);
		misc->fabric_now = apple_pmgr_verify_active(misc, DEV_FABRIC);
		misc->dcs_now = apple_pmgr_verify_active(misc, DEV_DCS);
		misc->disp_now = apple_pmgr_verify_active(misc, DEV_DISP);
		misc->cpu_snap = devm_kcalloc(dev, nr_cpu_ids, sizeof(*misc->cpu_snap), GFP_KERNEL);
		if (!misc->cpu_snap)
			return -ENOMEM;
		INIT_DEFERRABLE_WORK(&misc->idle_work, apple_pmgr_idle_work);
		INIT_DELAYED_WORK(&misc->idle_work_prompt, apple_pmgr_idle_work_prompt);
		apple_pmgr_find_voters(misc);
		ret = apple_pmgr_dcs_init(misc);
		if (ret)
			return ret;
		ret = apple_pmgr_fabric_floor_init(misc);
		if (ret)
			return ret;
		ret = apple_pmgr_dcs_floor_init(misc);
		if (ret)
			return ret;
		ret = apple_pmgr_disp_floor_init(misc);
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(dev, apple_pmgr_misc_teardown, misc);
		if (ret)
			return ret;
		scoped_guard(rwsem_write, &apple_pmgr_inst_sem) {
			/* Publish only a set-up instance; pairs with the readers' acquire. */
			smp_store_release(&apple_pmgr_misc_inst, misc);
			if (!misc->pmp_owned && !READ_ONCE(idle_fabric_pstate_explicit) &&
			    !READ_ONCE(idle_fabric_pstate) && misc->idle_fabric_default)
				WRITE_ONCE(idle_fabric_pstate, misc->idle_fabric_default);
			if (READ_ONCE(idle_fabric_pstate) && misc->pmp_owned) {
				dev_warn(dev,
					 "idle fabric governor disabled: PMP manages the fabric\n");
				WRITE_ONCE(idle_fabric_pstate, 0);
			} else if (READ_ONCE(idle_fabric_pstate) &&
				   READ_ONCE(idle_fabric_pstate) <
				   misc->devices[DEV_FABRIC].suspend_state) {
				dev_warn(dev, "idle fabric p-state %u below floor %u; disabling\n",
					 READ_ONCE(idle_fabric_pstate),
					 misc->devices[DEV_FABRIC].suspend_state);
				WRITE_ONCE(idle_fabric_pstate, 0);
			}
			start_idle = READ_ONCE(idle_fabric_pstate) || misc->idle_dcs_pstate ||
				     misc->idle_disp_pstate;
		}
		if (start_idle) {
			queue_delayed_work(system_freezable_power_efficient_wq,
					   &misc->idle_work, 0);
		}
	}

	apple_pmgr_misc_debugfs_init(misc);

	return 0;
}

static const struct apple_pmgr_misc_hw apple_pmgr_misc_hw_t8103 = {
	.dcs_valid_states = DCS_VALID_STATES_T8103,
	.single_step = true,
	.dev_min_ps = {
		[DEV_FABRIC] = 0,
		[DEV_DCS] = 0,
	},
};

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
	{ .compatible = "apple,t8103-pmgr-misc", .data = &apple_pmgr_misc_hw_t8103 },
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

#if IS_ENABLED(CONFIG_APPLE_PMGR_FABRIC_KUNIT_TEST)
#include "apple-pmgr-fabric-test.c"
#endif

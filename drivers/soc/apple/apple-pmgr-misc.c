// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC PMGR device power state driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/arch_topology.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/interconnect-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel_stat.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rwsem.h>
#include <linux/sched/topology.h>
#include <linux/seq_file.h>
#include <linux/soc/apple/pmgr-misc.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
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
#define DCS_DEV_PSTATE_MIN_T8103 4
#define DCS_VALID_STATES_T600X (BIT(5) | BIT(6) | BIT(7) | BIT(9))
#define DCS_VALID_STATES_T8103 (BIT(4) | BIT(5) | BIT(6))
#define SYS_DEV_PSTATE_SUSPEND 1

#define APPLE_PMGR_IDLE_POLL_MS 250
#define APPLE_PMGR_TIER_SAMPLE_MS 40
#define APPLE_PMGR_RAISE_TIMEOUT_US 1000
#define APPLE_PMGR_PS_ACTUAL GENMASK(7, 4)
#define APPLE_PMGR_PS_ACTIVE 0xf

#define APPLE_PMGR_GOV_IDLE_CPU_PCT 5
#define APPLE_PMGR_GOV_IDLE_GPU_PCT 5
#define APPLE_PMGR_GOV_P_MAX_FULL_PCT 65
#define APPLE_PMGR_GOV_P_SUM_FULL_PCT 85
#define APPLE_PMGR_GOV_E_SUM_FULL_PCT 250
#define APPLE_PMGR_GOV_GPU_FULL_PCT 35
#define APPLE_PMGR_GOV_GPU_INFLIGHT_MS 10
#define APPLE_PMGR_GOV_RAISE_FULL_SAMPLES 2
#define APPLE_PMGR_GOV_HOLD_FULL_MS 500
#define APPLE_PMGR_GOV_HOLD_LIGHT_MS 500
#define APPLE_PMGR_GOV_MIN_INTERVAL_MS 100

struct apple_pmgr_voter_spec {
	const char *label;
	bool optional;
};

/*
 * Bandwidth users that keep the fabric up whenever their power domain is
 * active. External display and ANE system domains stay on even when idle,
 * so external displays vote through apple_pmgr_fabric_hold() instead, and
 * the ANE is not covered. venc_dma (the encoder's own domain, rather than
 * parent venc_sys which stays on for attached AVE DARTs when unbound) is
 * optional so platforms whose DT omits it keep voters_ok intact.
 */
static const struct apple_pmgr_voter_spec apple_pmgr_fabric_voters[] = {
	{ .label = "gfx" },
	{ .label = "avd_sys" },
	{ .label = "isp_sys" },
	{ .label = "venc_dma", .optional = true },
};

enum sys_device {
	DEV_FABRIC,
	DEV_DCS,
	DEV_DISP,
	DEV_MAX,
};

enum apple_pmgr_tier {
	APPLE_PMGR_TIER_IDLE = 0,
	APPLE_PMGR_TIER_LIGHT = 1,
	APPLE_PMGR_TIER_FULL = 2,
	APPLE_PMGR_TIER_MAX,
};

static const char * const apple_pmgr_tier_names[APPLE_PMGR_TIER_MAX] = {
	[APPLE_PMGR_TIER_IDLE] = "idle",
	[APPLE_PMGR_TIER_LIGHT] = "light",
	[APPLE_PMGR_TIER_FULL] = "full",
};

struct apple_pmgr_gov_cfg {
	unsigned int idle_cpu_pct;
	unsigned int idle_gpu_pct;
	unsigned int p_max_full_pct;
	unsigned int p_sum_full_pct;
	unsigned int e_sum_full_pct;
	unsigned int gpu_full_pct;
	unsigned int gpu_inflight_full_ms;
	unsigned int raise_full_samples;
	unsigned int hold_full_ms;
	unsigned int hold_light_ms;
	unsigned int min_interval_ms;
};

static const struct apple_pmgr_gov_cfg apple_pmgr_gov_cfg_default = {
	.idle_cpu_pct = APPLE_PMGR_GOV_IDLE_CPU_PCT,
	.idle_gpu_pct = APPLE_PMGR_GOV_IDLE_GPU_PCT,
	.p_max_full_pct = APPLE_PMGR_GOV_P_MAX_FULL_PCT,
	.p_sum_full_pct = APPLE_PMGR_GOV_P_SUM_FULL_PCT,
	.e_sum_full_pct = APPLE_PMGR_GOV_E_SUM_FULL_PCT,
	.gpu_full_pct = APPLE_PMGR_GOV_GPU_FULL_PCT,
	.gpu_inflight_full_ms = APPLE_PMGR_GOV_GPU_INFLIGHT_MS,
	.raise_full_samples = APPLE_PMGR_GOV_RAISE_FULL_SAMPLES,
	.hold_full_ms = APPLE_PMGR_GOV_HOLD_FULL_MS,
	.hold_light_ms = APPLE_PMGR_GOV_HOLD_LIGHT_MS,
	.min_interval_ms = APPLE_PMGR_GOV_MIN_INTERVAL_MS,
};

struct apple_pmgr_gov_sample {
	unsigned int e_max;
	unsigned int e_sum;
	unsigned int p_max;
	unsigned int p_sum;
	unsigned int gpu_pct;
	unsigned int gpu_inflight_ms;
	bool gpu_inflight;
	bool hard_override;
};

struct apple_pmgr_gov {
	enum apple_pmgr_tier tier;
	bool initialized;
	bool transitioned;
	unsigned int full_streak;
	u64 last_full_ns;
	u64 last_light_ns;
	u64 last_transition_ns;
	u64 tier_since_ns;
	u64 residency_ns[APPLE_PMGR_TIER_MAX];
	u64 transitions;
	u64 rate_limited;
};

struct apple_pmgr_sys_device {
	void __iomem *base;
	u32 active_state;
	u32 suspend_state;
};

struct apple_pmgr_misc_hw {
	u32 dev_min_ps[DEV_MAX];
	u32 dcs_valid_states;
	u32 default_idle_fabric;
	u32 default_idle_dcs;
	u32 light_fabric;
	u32 light_dcs;
	bool single_step;
	bool tiered_scaling;
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

	/* Fabric/DCS governor; lock serializes writes and governor state. */
	struct mutex lock;
	struct delayed_work idle_work;		/* deferrable: fabric high / held */
	struct delayed_work idle_work_prompt;	/* bounded raise: fabric/DCS lowered */
	u32 fabric_now;				/* protected by lock */
	u32 dcs_now;				/* protected by lock */
	u32 disp_now;				/* protected by lock */
	u32 idle_disp_pstate;			/* validated DT disp floor, 0 = disabled */
	unsigned int holds;			/* protected by lock */
	bool sleeping;				/* protected by lock */
	bool single_step;
	bool tiered_scaling;
	u32 light_fabric;
	u32 light_dcs;
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
	struct apple_pmgr_gov gov;
	struct apple_pmgr_gov_cfg gov_cfg;
	struct apple_pmgr_gov_sample last_sample;
	bool gov_primed;
	u64 gov_gpu_prev_ns;
	u64 gov_last_ns;
};

static struct apple_pmgr_misc *apple_pmgr_misc_inst;
static DECLARE_RWSEM(apple_pmgr_inst_sem);
static unsigned int idle_fabric_pstate;
static unsigned int idle_dcs_pstate;
static bool idle_fabric_pstate_explicit;
static bool idle_dcs_pstate_explicit;
static atomic_t apple_pmgr_gpu_tracked = ATOMIC_INIT(0);

/* IRQ/atomic-safe GPU duty-cycle state pushed on busy/idle edges. */
static struct {
	spinlock_t lock;	/* protects all fields below */
	unsigned int busy_count;
	u64 busy_since_ns;
	u64 busy_ns;
	bool wake_pending;
	struct apple_pmgr_misc *wake;
} apple_pmgr_gpu = {
	.lock = __SPIN_LOCK_UNLOCKED(apple_pmgr_gpu.lock),
};

static int apple_pmgr_raise_fabric(struct apple_pmgr_misc *misc);
static int apple_pmgr_raise_dcs(struct apple_pmgr_misc *misc);
static bool apple_pmgr_voters_active(struct apple_pmgr_misc *misc);
static void apple_pmgr_queue_work(struct apple_pmgr_misc *misc,
				  bool deferrable, unsigned int delay_ms);

static unsigned int apple_pmgr_busy_pct(u64 busy, u64 wall)
{
	if (!wall)
		return 0;
	if (busy >= wall)
		return 100;
	if (busy > U64_MAX / 100)
		return div64_u64(busy, div64_u64(wall, 100));
	return div64_u64(busy * 100, wall);
}

static unsigned int apple_pmgr_cpu_busy_pct(u64 idle0, u64 wall0, u64 idle1, u64 wall1)
{
	u64 dw, di;

	if (wall1 <= wall0 || idle1 < idle0)
		return 0;
	dw = wall1 - wall0;
	di = idle1 - idle0;
	return di >= dw ? 0 : apple_pmgr_busy_pct(dw - di, dw);
}

static void apple_pmgr_gov_force_tier(struct apple_pmgr_gov *gov,
				      enum apple_pmgr_tier tier, u64 now_ns)
{
	if (!gov->initialized) {
		gov->tier = tier;
		gov->tier_since_ns = now_ns;
		gov->last_full_ns = now_ns;
		gov->last_light_ns = now_ns;
		gov->initialized = true;
		return;
	}
	if (gov->tier != tier) {
		if (now_ns > gov->tier_since_ns)
			gov->residency_ns[gov->tier] += now_ns - gov->tier_since_ns;
		gov->tier = tier;
		gov->tier_since_ns = now_ns;
		gov->last_transition_ns = now_ns;
		gov->transitioned = true;
		gov->transitions++;
	}
	if (tier == APPLE_PMGR_TIER_FULL) {
		gov->last_full_ns = now_ns;
		gov->last_light_ns = now_ns;
	} else if (tier == APPLE_PMGR_TIER_LIGHT) {
		gov->last_light_ns = now_ns;
	}
	gov->full_streak = 0;
}

/*
 * Pure tiered governor step: no MMIO, locking or clock source.
 *
 * Hard overrides promote to FULL immediately and bypass rate limiting.
 * Otherwise: IDLE -> LIGHT promotes in 1 sample; LIGHT -> FULL promotes after
 * raise_full_samples consecutive FULL-demand samples (or immediately on a
 * continuous GPU job at or above gpu_inflight_full_ms); demotions hold for
 * hold_full_ms (FULL -> LIGHT) and hold_light_ms (LIGHT -> IDLE), with
 * min_interval_ms rate limiting non-override transitions.
 */
static enum apple_pmgr_tier
apple_pmgr_gov_step(struct apple_pmgr_gov *gov,
		    const struct apple_pmgr_gov_cfg *cfg,
		    u64 now_ns, const struct apple_pmgr_gov_sample *sample)
{
	unsigned int raise_samples = cfg->raise_full_samples ?: 1;
	u64 hold_full_ns = (u64)cfg->hold_full_ms * NSEC_PER_MSEC;
	u64 hold_light_ns = (u64)cfg->hold_light_ms * NSEC_PER_MSEC;
	u64 min_interval_ns = (u64)cfg->min_interval_ms * NSEC_PER_MSEC;
	bool gpu_long, full_demand, light_demand;
	enum apple_pmgr_tier want = gov->tier;

	if (!gov->initialized) {
		gov->tier_since_ns = now_ns;
		gov->last_full_ns = now_ns;
		gov->last_light_ns = now_ns;
		gov->initialized = true;
	}

	gpu_long = sample->gpu_inflight &&
		   sample->gpu_inflight_ms >= cfg->gpu_inflight_full_ms;
	full_demand = sample->hard_override ||
		      sample->p_max >= cfg->p_max_full_pct ||
		      sample->p_sum >= cfg->p_sum_full_pct ||
		      sample->e_sum >= cfg->e_sum_full_pct ||
		      sample->gpu_pct >= cfg->gpu_full_pct ||
		      gpu_long;
	light_demand = full_demand ||
		       sample->e_max > cfg->idle_cpu_pct ||
		       sample->p_max > cfg->idle_cpu_pct ||
		       sample->gpu_pct >= cfg->idle_gpu_pct ||
		       sample->gpu_inflight;

	if (full_demand) {
		if (gov->full_streak < UINT_MAX)
			gov->full_streak++;
		gov->last_full_ns = now_ns;
		gov->last_light_ns = now_ns;
	} else {
		gov->full_streak = 0;
		if (light_demand)
			gov->last_light_ns = now_ns;
	}

	if (sample->hard_override) {
		want = APPLE_PMGR_TIER_FULL;
	} else {
		switch (gov->tier) {
		case APPLE_PMGR_TIER_IDLE:
			if (full_demand && (gpu_long || gov->full_streak >= raise_samples))
				want = APPLE_PMGR_TIER_FULL;
			else if (light_demand)
				want = APPLE_PMGR_TIER_LIGHT;
			break;
		case APPLE_PMGR_TIER_LIGHT:
			if (full_demand && (gpu_long || gov->full_streak >= raise_samples))
				want = APPLE_PMGR_TIER_FULL;
			else if (!light_demand && now_ns >= gov->last_light_ns &&
				 now_ns - gov->last_light_ns >= hold_light_ns)
				want = APPLE_PMGR_TIER_IDLE;
			break;
		case APPLE_PMGR_TIER_FULL:
			if (!full_demand && now_ns >= gov->last_full_ns &&
			    now_ns - gov->last_full_ns >= hold_full_ns)
				want = APPLE_PMGR_TIER_LIGHT;
			break;
		default:
			want = APPLE_PMGR_TIER_FULL;
			break;
		}
	}

	if (want == gov->tier)
		return gov->tier;

	if (!sample->hard_override && gov->transitioned &&
	    now_ns >= gov->last_transition_ns &&
	    now_ns - gov->last_transition_ns < min_interval_ns) {
		gov->rate_limited++;
		return gov->tier;
	}

	if (now_ns > gov->tier_since_ns)
		gov->residency_ns[gov->tier] += now_ns - gov->tier_since_ns;
	if (gov->tier == APPLE_PMGR_TIER_FULL && want == APPLE_PMGR_TIER_LIGHT)
		gov->last_light_ns = now_ns;

	gov->tier = want;
	gov->tier_since_ns = now_ns;
	gov->last_transition_ns = now_ns;
	gov->transitioned = true;
	gov->transitions++;
	return gov->tier;
}

#ifdef CONFIG_ARM64
static noinline u32 apple_pmgr_perf_step(void __iomem *reg, u32 state)
{
	u32 val;
	unsigned long cnt = 1UL << 22;

#if IS_ENABLED(CONFIG_APPLE_PMGR_FABRIC_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_pmgr_perf_step, reg, state);
#endif
	/*
	 * Execute the target write and busy-bit wait inside one 64-byte
	 * cache line with no memory accesses in between so a memory-clock
	 * transition cannot stall its own instruction fetch.
	 */
	asm volatile(
		".balign 64\n\t"
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
static u32 apple_pmgr_perf_step(void __iomem *reg, u32 state)
{
	u32 val = readl_relaxed(reg);

	FIELD_MODIFY(APPLE_CLKGEN_PSTATE_DESIRED, &val, state);
	writel(val, reg);
	readl_poll_timeout_atomic(reg, val, !(val & APPLE_CLKGEN_PSTATE_BUSY),
				  1, APPLE_PMGR_RAISE_TIMEOUT_US);
	return val;
}
#endif

static int apple_pmgr_step_to(void __iomem *reg, u32 target, u32 valid_mask, u32 *last)
{
	unsigned long flags;
	u32 cur, des, val;
	int ret;

	if (valid_mask && !(valid_mask & BIT(target)))
		return -EINVAL;

	ret = readl_poll_timeout_atomic(reg, val, !(val & APPLE_CLKGEN_PSTATE_BUSY),
					1, APPLE_PMGR_RAISE_TIMEOUT_US);
	if (ret) {
		if (last)
			*last = val;
		return -EBUSY;
	}

	cur = FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val);
	des = FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED, val);
	if (cur == target && des == target) {
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

	if (cur == target) {
		local_irq_save(flags);
		val = apple_pmgr_perf_step(reg, target);
		local_irq_restore(flags);
		if ((val & APPLE_CLKGEN_PSTATE_BUSY) ||
		    FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) != target) {
			if (last)
				*last = val;
			return -ETIMEDOUT;
		}
	}

	while (cur != target) {
		cur = cur < target ? cur + 1 : cur - 1;
		local_irq_save(flags);
		val = apple_pmgr_perf_step(reg, cur);
		local_irq_restore(flags);
		if ((val & APPLE_CLKGEN_PSTATE_BUSY) ||
		    FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) != cur) {
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

		if (valid_mask &&
		    (WARN_ON_ONCE(!apple_pmgr_dcs_state_valid(misc, pstate)) ||
		     (!active && WARN_ON_ONCE(!apple_pmgr_dcs_state_valid(misc, misc->devices[dev].active_state)))))
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
	ret = apple_pmgr_write_fabric(misc, active);
	if (ret)
		return ret;
	ret = readl_poll_timeout(misc->devices[DEV_FABRIC].base + APPLE_CLKGEN_PSTATE,
				 val, FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) == active,
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
	ret = apple_pmgr_write_dcs(misc, active);
	if (ret)
		return ret;
	ret = readl_poll_timeout(misc->devices[DEV_DCS].base + APPLE_CLKGEN_PSTATE,
				 val, FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) == active,
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
	ret = apple_pmgr_write_disp(misc, active);
	if (ret)
		return ret;
	ret = readl_poll_timeout(misc->devices[DEV_DISP].base + APPLE_CLKGEN_PSTATE,
				 val, FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) == active,
				 10, APPLE_PMGR_RAISE_TIMEOUT_US);
	if (ret)
		misc->disp_now = U32_MAX;
	return ret;
}

static int apple_pmgr_apply_targets(struct apple_pmgr_misc *misc,
				    u32 target_fab, u32 target_dcs)
{
	u32 active_fab = misc->devices[DEV_FABRIC].active_state;
	u32 active_dcs = misc->devices[DEV_DCS].active_state;
	bool has_dcs = misc->devices[DEV_DCS].base && !misc->dcs_data;
	bool fab_raising, fab_lowering, dcs_raising, dcs_lowering;
	int ret = 0;

	lockdep_assert_held(&misc->lock);
	if (misc->pmp_owned)
		return 0;

	fab_raising = (misc->fabric_now == U32_MAX && target_fab == active_fab) ||
		      (misc->fabric_now != U32_MAX && target_fab > misc->fabric_now);
	fab_lowering = (misc->fabric_now == U32_MAX && target_fab < active_fab) ||
		       (misc->fabric_now != U32_MAX && target_fab < misc->fabric_now);
	dcs_raising = has_dcs &&
		      ((misc->dcs_now == U32_MAX && target_dcs == active_dcs) ||
		       (misc->dcs_now != U32_MAX && target_dcs > misc->dcs_now));
	dcs_lowering = has_dcs &&
		       ((misc->dcs_now == U32_MAX && target_dcs < active_dcs) ||
			(misc->dcs_now != U32_MAX && target_dcs < misc->dcs_now));

	/* Promote fabric first, then DCS. */
	if (fab_raising) {
		if (target_fab == active_fab)
			ret = apple_pmgr_raise_fabric(misc);
		else
			ret = apple_pmgr_write_fabric(misc, target_fab);
		if (ret)
			return ret;
	}

	if (dcs_raising) {
		if (target_dcs == active_dcs)
			ret = apple_pmgr_raise_dcs(misc);
		else
			ret = apple_pmgr_write_dcs(misc, target_dcs);
		if (ret)
			return ret;
	} else if (dcs_lowering) {
		/* Demote DCS first, then fabric. */
		ret = apple_pmgr_write_dcs(misc, target_dcs);
		if (ret)
			return ret;
	}

	if (fab_lowering)
		ret = apple_pmgr_write_fabric(misc, target_fab);

	return ret;
}

static void apple_pmgr_tier_targets(struct apple_pmgr_misc *misc,
				    enum apple_pmgr_tier tier,
				    unsigned int idle_fab,
				    unsigned int idle_dcs,
				    u32 *target_fab, u32 *target_dcs)
{
	u32 active_fab = misc->devices[DEV_FABRIC].active_state;
	u32 active_dcs = misc->devices[DEV_DCS].active_state;
	bool fab_ok = idle_fab && idle_fab < active_fab;
	bool dcs_ok = idle_dcs && idle_dcs < active_dcs &&
		      misc->devices[DEV_DCS].base && !misc->dcs_data &&
		      (!misc->dcs_valid_states ||
		       apple_pmgr_dcs_state_valid(misc, idle_dcs));
	u32 light_fab, light_dcs;

	switch (tier) {
	case APPLE_PMGR_TIER_IDLE:
		*target_fab = fab_ok ? idle_fab : active_fab;
		*target_dcs = dcs_ok ? idle_dcs : active_dcs;
		break;
	case APPLE_PMGR_TIER_LIGHT:
		if (fab_ok) {
			light_fab = misc->light_fabric ?: idle_fab;
			*target_fab = clamp(light_fab, (u32)idle_fab, active_fab);
		} else {
			*target_fab = active_fab;
		}
		if (dcs_ok) {
			light_dcs = misc->light_dcs ?: idle_dcs;
			light_dcs = clamp(light_dcs, (u32)idle_dcs, active_dcs);
			if (misc->dcs_valid_states &&
			    !apple_pmgr_dcs_state_valid(misc, light_dcs))
				light_dcs = active_dcs;
			*target_dcs = light_dcs;
		} else {
			*target_dcs = active_dcs;
		}
		break;
	case APPLE_PMGR_TIER_FULL:
	default:
		*target_fab = active_fab;
		*target_dcs = active_dcs;
		break;
	}
}

static void apple_pmgr_gpu_wake_set(struct apple_pmgr_misc *misc)
{
	unsigned long flags;

	spin_lock_irqsave(&apple_pmgr_gpu.lock, flags);
	apple_pmgr_gpu.wake = misc;
	if (!misc)
		apple_pmgr_gpu.wake_pending = false;
	spin_unlock_irqrestore(&apple_pmgr_gpu.lock, flags);
}

static void apple_pmgr_gpu_wake_clear(void)
{
	unsigned long flags;

	spin_lock_irqsave(&apple_pmgr_gpu.lock, flags);
	apple_pmgr_gpu.wake_pending = false;
	spin_unlock_irqrestore(&apple_pmgr_gpu.lock, flags);
}

static unsigned int apple_pmgr_gpu_busy_count(void)
{
	unsigned long flags;
	unsigned int count;

	spin_lock_irqsave(&apple_pmgr_gpu.lock, flags);
	count = apple_pmgr_gpu.busy_count;
	spin_unlock_irqrestore(&apple_pmgr_gpu.lock, flags);
	return count;
}

static void apple_pmgr_gpu_snapshot(u64 now, u64 *busy_ns, bool *inflight,
				    unsigned int *inflight_ms)
{
	unsigned long flags;
	u64 ns, dur_ns = 0;
	bool active;

	spin_lock_irqsave(&apple_pmgr_gpu.lock, flags);
	ns = apple_pmgr_gpu.busy_ns;
	active = apple_pmgr_gpu.busy_count > 0;
	if (active && now > apple_pmgr_gpu.busy_since_ns) {
		dur_ns = now - apple_pmgr_gpu.busy_since_ns;
		ns += dur_ns;
	}
	spin_unlock_irqrestore(&apple_pmgr_gpu.lock, flags);

	*busy_ns = ns;
	*inflight = active;
	*inflight_ms = min_t(u64, div_u64(dur_ns, NSEC_PER_MSEC), UINT_MAX);
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
	if (misc->tiered_scaling && !misc->pmp_owned)
		apple_pmgr_gov_force_tier(&misc->gov, APPLE_PMGR_TIER_FULL,
					  ktime_get_ns());
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
	if (!WARN_ON_ONCE(!misc->holds)) {
		misc->holds--;
		if (misc->tiered_scaling && !misc->holds &&
		    !misc->sleeping && !misc->pmp_owned &&
		    (READ_ONCE(idle_fabric_pstate) || READ_ONCE(idle_dcs_pstate))) {
			u64 now = ktime_get_ns();

			misc->gov.last_full_ns = now;
			misc->gov.last_light_ns = now;
			apple_pmgr_queue_work(misc, false, APPLE_PMGR_TIER_SAMPLE_MS);
		}
	}
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
	struct apple_pmgr_misc *misc = smp_load_acquire(&apple_pmgr_misc_inst);

	WARN_ON_ONCE(atomic_dec_return(&apple_pmgr_gpu_tracked) < 0);
	if (!misc)
		return;
	guard(mutex)(&misc->lock);
	if (!misc->sleeping && apple_pmgr_voters_active(misc)) {
		apple_pmgr_raise_fabric(misc);
		apple_pmgr_raise_dcs(misc);
		apple_pmgr_raise_disp(misc);
		if (misc->tiered_scaling && !misc->pmp_owned)
			apple_pmgr_gov_force_tier(&misc->gov, APPLE_PMGR_TIER_FULL,
						  ktime_get_ns());
	}
}
EXPORT_SYMBOL_GPL(apple_pmgr_gpu_tracking_stop);

int apple_pmgr_gpu_busy_get(void)
{
	unsigned long flags;
	u64 now = ktime_get_ns();

	spin_lock_irqsave(&apple_pmgr_gpu.lock, flags);
	if (apple_pmgr_gpu.busy_count++ == 0) {
		apple_pmgr_gpu.busy_since_ns = now;
		if (apple_pmgr_gpu.wake &&
		    (READ_ONCE(idle_fabric_pstate) || READ_ONCE(idle_dcs_pstate))) {
			struct apple_pmgr_misc *misc = apple_pmgr_gpu.wake;

			/*
			 * Not cancel_delayed_work_sync: callers may hold
			 * locks or run in atomic context. Pull in both a
			 * deferrable wait and a longer IDLE-tier poll.
			 */
			apple_pmgr_gpu.wake_pending = true;
			cancel_delayed_work(&misc->idle_work);
			mod_delayed_work(system_freezable_power_efficient_wq,
					 &misc->idle_work_prompt, 0);
		}
	}
	spin_unlock_irqrestore(&apple_pmgr_gpu.lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_pmgr_gpu_busy_get);

void apple_pmgr_gpu_busy_put(void)
{
	unsigned long flags;
	u64 now = ktime_get_ns();

	spin_lock_irqsave(&apple_pmgr_gpu.lock, flags);
	if (WARN_ON_ONCE(!apple_pmgr_gpu.busy_count)) {
		spin_unlock_irqrestore(&apple_pmgr_gpu.lock, flags);
		return;
	}
	if (--apple_pmgr_gpu.busy_count == 0) {
		apple_pmgr_gpu.wake_pending = false;
		if (now > apple_pmgr_gpu.busy_since_ns)
			apple_pmgr_gpu.busy_ns += now - apple_pmgr_gpu.busy_since_ns;
	}
	spin_unlock_irqrestore(&apple_pmgr_gpu.lock, flags);
}
EXPORT_SYMBOL_GPL(apple_pmgr_gpu_busy_put);

static bool apple_pmgr_hard_voters_active(struct apple_pmgr_misc *misc)
{
	unsigned int i;
	u32 val;

	for (i = 0; i < ARRAY_SIZE(misc->votes); i++) {
		if (i == 0 && atomic_read(&apple_pmgr_gpu_tracked) > 0)
			continue;
		if (!misc->votes[i].regmap) {
			if (apple_pmgr_fabric_voters[i].optional)
				continue;
			return true;
		}
		/*
		 * Only the actual state shows activity: the GPU domain keeps its
		 * target on with auto power gating and the hardware gates it
		 * when idle. Unreadable voters count as busy.
		 */
		if (regmap_read(misc->votes[i].regmap, misc->votes[i].reg, &val) ||
		    FIELD_GET(APPLE_PMGR_PS_ACTUAL, val) == APPLE_PMGR_PS_ACTIVE)
			return true;
	}
	return false;
}

static bool apple_pmgr_voters_active(struct apple_pmgr_misc *misc)
{
	if (atomic_read(&apple_pmgr_gpu_tracked) > 0 &&
	    apple_pmgr_gpu_busy_count() > 0)
		return true;
	return apple_pmgr_hard_voters_active(misc);
}

/* Every required voter must be found, or the governor never lowers. */
static void apple_pmgr_find_voters(struct apple_pmgr_misc *misc)
{
	unsigned int required = 0, found_req = 0, i;
	struct device_node *np;
	const char *label;
	u32 reg;

	for (i = 0; i < ARRAY_SIZE(apple_pmgr_fabric_voters); i++)
		if (!apple_pmgr_fabric_voters[i].optional)
			required++;

	for_each_compatible_node(np, NULL, "apple,pmgr-pwrstate") {
		if (!of_device_is_available(np) ||
		    of_property_read_string(np, "label", &label) ||
		    of_property_read_u32(np, "reg", &reg))
			continue;
		for (i = 0; i < ARRAY_SIZE(apple_pmgr_fabric_voters); i++) {
			struct regmap *map;

			if (strcmp(label, apple_pmgr_fabric_voters[i].label) ||
			    misc->votes[i].regmap)
				continue;
			map = syscon_node_to_regmap(np->parent);
			if (IS_ERR(map))
				break;
			misc->votes[i].regmap = map;
			misc->votes[i].reg = reg;
			if (!apple_pmgr_fabric_voters[i].optional)
				found_req++;
			break;
		}
	}
	misc->voters_ok = found_req == required;
	/* Desktops have no ISP: say so only where the governor is wanted. */
	if (!misc->voters_ok && READ_ONCE(idle_fabric_pstate))
		dev_warn(misc->dev, "idle fabric: found %u of %u required bandwidth voters, staying high\n",
			 found_req, required);
	else if (!misc->voters_ok)
		dev_dbg(misc->dev, "idle fabric: found %u of %u required bandwidth voters\n",
			found_req, required);
}

static bool apple_pmgr_cpu_is_little(unsigned int cpu)
{
	return arch_scale_cpu_capacity(cpu) < SCHED_CAPACITY_SCALE;
}

static bool apple_pmgr_sample_load(struct apple_pmgr_misc *misc, u64 now,
				   struct apple_pmgr_gov_sample *sample)
{
	u64 period = (u64)APPLE_PMGR_TIER_SAMPLE_MS * NSEC_PER_MSEC;
	unsigned int gpu_inflight_ms = 0;
	bool gpu_inflight = false;
	u64 gpu_ns = 0;
	unsigned int cpu;

#if IS_ENABLED(CONFIG_APPLE_PMGR_FABRIC_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_pmgr_sample_load, misc, now, sample);
#endif
	if (misc->cpu_snap) {
		cpus_read_lock();
		for_each_possible_cpu(cpu) {
			if (cpu < nr_cpu_ids && !cpu_online(cpu))
				misc->cpu_snap[cpu].valid = false;
		}
		for_each_online_cpu(cpu) {
			struct apple_pmgr_cpu_snap *s;
			u64 wall = 0, idle, iowait;

			if (cpu >= nr_cpu_ids)
				continue;
			s = &misc->cpu_snap[cpu];
			idle = get_cpu_idle_time_us(cpu, NULL);
			iowait = get_cpu_iowait_time_us(cpu, &wall);
			if (idle == (u64)-1 || iowait == (u64)-1)
				continue;
			idle += iowait;
			if (misc->gov_primed && s->valid && wall > s->wall_us &&
			    (wall - s->wall_us) * NSEC_PER_USEC <= 10 * period) {
				unsigned int pct = apple_pmgr_cpu_busy_pct(s->idle_us, s->wall_us,
									   idle, wall);

				s->was_idle = pct <= APPLE_PMGR_GOV_IDLE_CPU_PCT;
				if (apple_pmgr_cpu_is_little(cpu)) {
					sample->e_max = max(sample->e_max, pct);
					sample->e_sum += pct;
				} else {
					sample->p_max = max(sample->p_max, pct);
					sample->p_sum += pct;
				}
			}
			s->idle_us = idle;
			s->wall_us = wall;
			s->valid = true;
		}
		cpus_read_unlock();
	}

	apple_pmgr_gpu_snapshot(now, &gpu_ns, &gpu_inflight, &gpu_inflight_ms);
	if (misc->gov_primed && now > misc->gov_last_ns) {
		u64 dgpu = gpu_ns >= misc->gov_gpu_prev_ns ?
			   gpu_ns - misc->gov_gpu_prev_ns : 0;

		sample->gpu_pct = apple_pmgr_busy_pct(dgpu, now - misc->gov_last_ns);
	}
	misc->gov_gpu_prev_ns = gpu_ns;
	misc->gov_last_ns = now;
	sample->gpu_inflight = gpu_inflight;
	sample->gpu_inflight_ms = gpu_inflight_ms;

	if (!misc->gov_primed) {
		misc->gov_primed = true;
		return false;
	}
	return true;
}

/* True while every cpufreq policy runs at its lowest frequency or is idle. */
static bool apple_pmgr_cpus_at_min(void)
{
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

static void apple_pmgr_queue_work(struct apple_pmgr_misc *misc,
				  bool deferrable, unsigned int delay_ms)
{
	unsigned long delay;
	unsigned long flags;
	bool gpu_busy, wake_pending;

	lockdep_assert_held(&misc->lock);
	/*
	 * Freezable, so it stops before devices suspend. Queued under
	 * misc->lock so teardown setting sleeping=true prevents any
	 * cross-requeue between idle_work and idle_work_prompt.
	 *
	 * Hold apple_pmgr_gpu.lock across the cancel/requeue decision so a
	 * 0 -> 1 GPU busy edge in apple_pmgr_gpu_busy_get() cannot race
	 * between reading busy_count and updating the workqueues.
	 * Lock order: misc->lock -> apple_pmgr_gpu.lock -> workqueue/timer locks.
	 */
	spin_lock_irqsave(&apple_pmgr_gpu.lock, flags);
	gpu_busy = atomic_read(&apple_pmgr_gpu_tracked) > 0 &&
		   apple_pmgr_gpu.busy_count > 0;
	wake_pending = gpu_busy && apple_pmgr_gpu.wake_pending;
	apple_pmgr_gpu.wake_pending = false;

	if (gpu_busy) {
		cancel_delayed_work(&misc->idle_work);
		delay = wake_pending ? 0 :
			msecs_to_jiffies(min_t(unsigned int, delay_ms,
					       APPLE_PMGR_TIER_SAMPLE_MS));
		mod_delayed_work(system_freezable_power_efficient_wq,
				 &misc->idle_work_prompt, delay);
	} else if (deferrable) {
		cancel_delayed_work(&misc->idle_work_prompt);
		queue_delayed_work(system_freezable_power_efficient_wq,
				   &misc->idle_work, msecs_to_jiffies(delay_ms));
	} else {
		cancel_delayed_work(&misc->idle_work);
		mod_delayed_work(system_freezable_power_efficient_wq,
				 &misc->idle_work_prompt,
				 msecs_to_jiffies(delay_ms));
	}
	spin_unlock_irqrestore(&apple_pmgr_gpu.lock, flags);
}

static void apple_pmgr_tiered_eval_locked(struct apple_pmgr_misc *misc, u64 now)
{
	unsigned int idle_fab = READ_ONCE(idle_fabric_pstate);
	unsigned int idle_dcs = READ_ONCE(idle_dcs_pstate);
	u32 active_fab = misc->devices[DEV_FABRIC].active_state;
	u32 active_dcs = misc->devices[DEV_DCS].active_state;
	struct apple_pmgr_gov_sample sample;
	enum apple_pmgr_tier tier;
	u32 target_fab, target_dcs;
	bool hard_override, primed;
	int ret;

	lockdep_assert_held(&misc->lock);
	if (misc->sleeping)
		return;
	apple_pmgr_gpu_wake_clear();
	if (!idle_fab && !idle_dcs && !misc->idle_disp_pstate) {
		apple_pmgr_apply_targets(misc, active_fab, active_dcs);
		apple_pmgr_raise_disp(misc);
		if (!misc->pmp_owned)
			apple_pmgr_gov_force_tier(&misc->gov, APPLE_PMGR_TIER_FULL, now);
		return;
	}
	if (misc->pmp_owned) {
		apple_pmgr_queue_work(misc, true, APPLE_PMGR_TIER_SAMPLE_MS);
		return;
	}
	if (!misc->voters_ok) {
		apple_pmgr_apply_targets(misc, active_fab, active_dcs);
		apple_pmgr_raise_disp(misc);
		apple_pmgr_gov_force_tier(&misc->gov, APPLE_PMGR_TIER_FULL, now);
		apple_pmgr_queue_work(misc, true, APPLE_PMGR_TIER_SAMPLE_MS);
		return;
	}

	hard_override = misc->holds > 0 || misc->dcs_bw_active ||
			apple_pmgr_hard_voters_active(misc);
	memset(&sample, 0, sizeof(sample));
	primed = apple_pmgr_sample_load(misc, now, &sample);
	sample.hard_override = hard_override;
	misc->last_sample = sample;

	if (!primed) {
		if (!misc->gov.initialized || hard_override)
			apple_pmgr_gov_force_tier(&misc->gov, APPLE_PMGR_TIER_FULL, now);
		if (hard_override) {
			apple_pmgr_apply_targets(misc, active_fab, active_dcs);
			apple_pmgr_raise_disp(misc);
		}
		apple_pmgr_queue_work(misc, false, APPLE_PMGR_TIER_SAMPLE_MS);
		return;
	}

	tier = apple_pmgr_gov_step(&misc->gov, &misc->gov_cfg, now, &sample);
	apple_pmgr_tier_targets(misc, tier, idle_fab, idle_dcs, &target_fab, &target_dcs);
	ret = apple_pmgr_apply_targets(misc, target_fab, target_dcs);
	if (misc->devices[DEV_DISP].base && misc->idle_disp_pstate) {
		u32 target_disp = (tier == APPLE_PMGR_TIER_FULL || hard_override) ?
				  misc->devices[DEV_DISP].active_state :
				  misc->idle_disp_pstate;

		if (target_disp == misc->devices[DEV_DISP].active_state)
			ret = apple_pmgr_raise_disp(misc) ?: ret;
		else if (misc->disp_now != target_disp)
			ret = apple_pmgr_write_disp(misc, target_disp) ?: ret;
	}
	/*
	 * Keep a non-deferrable poll whenever fabric or DCS is lowered so hard
	 * voters (avd_sys, isp_sys, venc_dma) and cross-core CPU load are
	 * raised within a bounded window; FULL under a hard override stays
	 * deferrable since fabric/DCS are already at max and cannot demote.
	 */
	if (tier == APPLE_PMGR_TIER_IDLE && !ret)
		apple_pmgr_queue_work(misc, false, APPLE_PMGR_IDLE_POLL_MS);
	else if (tier == APPLE_PMGR_TIER_FULL && hard_override && !ret)
		apple_pmgr_queue_work(misc, true, APPLE_PMGR_IDLE_POLL_MS);
	else
		apple_pmgr_queue_work(misc, false, APPLE_PMGR_TIER_SAMPLE_MS);
}

static void apple_pmgr_tiered_eval(struct apple_pmgr_misc *misc, u64 now)
{
	guard(mutex)(&misc->lock);
	apple_pmgr_tiered_eval_locked(misc, now);
}

static void apple_pmgr_idle_eval(struct apple_pmgr_misc *misc)
{
	unsigned int idle_fab = READ_ONCE(idle_fabric_pstate);
	unsigned int idle_dcs = READ_ONCE(idle_dcs_pstate);
	u32 active_fab = misc->devices[DEV_FABRIC].active_state;
	u32 active_dcs = misc->devices[DEV_DCS].active_state;
	bool can_scale_fab, can_scale_dcs;
	bool idle_ok = false;
	bool low_fab = false;
	bool low_dcs = false;
	bool lowered;
	u32 target_fab, target_dcs;

	if (misc->tiered_scaling) {
		apple_pmgr_tiered_eval(misc, ktime_get_ns());
		return;
	}

	scoped_guard(mutex, &misc->lock) {
		if (misc->sleeping)
			return;
		apple_pmgr_gpu_wake_clear();
		can_scale_fab = idle_fab && idle_fab < active_fab;
		can_scale_dcs = idle_dcs && idle_dcs < active_dcs &&
				misc->devices[DEV_DCS].base && !misc->dcs_data &&
				(!misc->dcs_valid_states ||
				 apple_pmgr_dcs_state_valid(misc, idle_dcs));
		idle_ok = misc->voters_ok && !misc->pmp_owned &&
			  !misc->holds && !misc->dcs_bw_active &&
			  !apple_pmgr_voters_active(misc) &&
			  apple_pmgr_cpus_at_min();
		low_fab = can_scale_fab && idle_ok;
		low_dcs = can_scale_dcs && idle_ok;
		target_fab = low_fab ? idle_fab : active_fab;
		target_dcs = low_dcs ? idle_dcs : active_dcs;
		apple_pmgr_apply_targets(misc, target_fab, target_dcs);
		if (!idle_fab && !idle_dcs)
			return;
		/*
		 * Keep a non-deferrable poll while fabric or DCS is lowered so
		 * hard voters and cross-core CPU load are raised within a
		 * bounded window; stay deferrable when at active p-states.
		 */
		lowered = !misc->pmp_owned &&
			  ((misc->fabric_now != U32_MAX &&
			    misc->fabric_now < active_fab) ||
			   (misc->dcs_now != U32_MAX &&
			    misc->dcs_now < active_dcs));
		apple_pmgr_queue_work(misc, !lowered, APPLE_PMGR_IDLE_POLL_MS);
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

static void apple_pmgr_idle_stop(struct apple_pmgr_misc *misc)
{
	cancel_delayed_work_sync(&misc->idle_work);
	cancel_delayed_work_sync(&misc->idle_work_prompt);
	guard(mutex)(&misc->lock);
	apple_pmgr_raise_fabric(misc);
	apple_pmgr_raise_dcs(misc);
	apple_pmgr_raise_disp(misc);
	if (misc->tiered_scaling && !misc->pmp_owned)
		apple_pmgr_gov_force_tier(&misc->gov, APPLE_PMGR_TIER_FULL,
					  ktime_get_ns());
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
	if (ps && misc && !misc->voters_ok)
		dev_warn(misc->dev, "idle fabric: not every bandwidth voter was found, staying high\n");
	WRITE_ONCE(idle_fabric_pstate, ps);
	WRITE_ONCE(idle_fabric_pstate_explicit, true);
	if (!misc)
		return 0;
	if (!ps && !READ_ONCE(idle_dcs_pstate))
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

static int idle_dcs_pstate_set(const char *val, const struct kernel_param *kp)
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
	if (ps && misc && misc->dcs_valid_states &&
	    (!apple_pmgr_dcs_state_valid(misc, ps) ||
	     !apple_pmgr_dcs_state_valid(misc, misc->devices[DEV_DCS].active_state)))
		return -EINVAL;
	if (ps && misc && (misc->pmp_owned || misc->dcs_data))
		return -EBUSY;
	WRITE_ONCE(idle_dcs_pstate, ps);
	WRITE_ONCE(idle_dcs_pstate_explicit, true);
	if (!misc)
		return 0;
	if (!ps && !READ_ONCE(idle_fabric_pstate))
		apple_pmgr_idle_stop(misc);
	else
		mod_delayed_work(system_freezable_power_efficient_wq, &misc->idle_work, 0);
	return 0;
}

static const struct kernel_param_ops idle_dcs_pstate_ops = {
	.set = idle_dcs_pstate_set,
	.get = param_get_uint,
};

module_param_cb(idle_dcs_pstate, &idle_dcs_pstate_ops, &idle_dcs_pstate, 0644);
MODULE_PARM_DESC(idle_dcs_pstate,
		 "DCS p-state while all CPUs run at their lowest frequency and no bandwidth user is active (0 = keep the boot state)");

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
	apple_pmgr_gpu_wake_set(NULL);
	apple_pmgr_idle_stop(misc);
	return 0;
}

static void __maybe_unused apple_pmgr_misc_complete(struct device *dev)
{
	struct apple_pmgr_misc *misc = dev_get_drvdata(dev);

	if (!misc->devices[DEV_FABRIC].base)
		return;
	scoped_guard(mutex, &misc->lock) {
		misc->sleeping = false;
		misc->gov_primed = false;
		if (misc->tiered_scaling && !misc->pmp_owned)
			apple_pmgr_gov_force_tier(&misc->gov, APPLE_PMGR_TIER_FULL,
						  ktime_get_ns());
		if (!misc->pmp_owned)
			apple_pmgr_gpu_wake_set(misc);
	}
	if (READ_ONCE(idle_fabric_pstate) || READ_ONCE(idle_dcs_pstate))
		mod_delayed_work(system_freezable_power_efficient_wq, &misc->idle_work, 0);
}

static int __maybe_unused apple_pmgr_misc_suspend_noirq(struct device *dev)
{
	struct apple_pmgr_misc *misc = dev_get_drvdata(dev);
	int i;

	/*
	 * On SoCs whose deep-sleep commit steps DCS itself (T8103), lowering
	 * fabric/DCS in suspend_noirq runs before the sleep engine stops the
	 * coprocessors and breaks deep sleep. Lower only for s2idle.
	 */
	if (misc->single_step && pm_suspend_target_state != PM_SUSPEND_TO_IDLE)
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
							   misc->devices[DEV_FABRIC].active_state;
				else if (j == DEV_DCS)
					misc->dcs_now = r ? U32_MAX :
							misc->devices[DEV_DCS].active_state;
				else if (j == DEV_DISP)
					misc->disp_now = r ? U32_MAX :
							 misc->devices[DEV_DISP].active_state;
			}
			return ret;
		}
	}

	return 0;
}

static u32 apple_pmgr_verify_active(struct apple_pmgr_misc *misc, enum sys_device dev)
{
	u32 active = misc->devices[dev].active_state;
	u32 val;

	if (!misc->devices[dev].base)
		return active;
	val = readl_relaxed(misc->devices[dev].base + APPLE_CLKGEN_PSTATE);
	if (!(val & APPLE_CLKGEN_PSTATE_BUSY) &&
	    FIELD_GET(APPLE_CLKGEN_PSTATE_ACTUAL, val) == active &&
	    FIELD_GET(APPLE_CLKGEN_PSTATE_DESIRED, val) == active)
		return active;
	return U32_MAX;
}

static int __maybe_unused apple_pmgr_misc_resume_noirq(struct device *dev)
{
	struct apple_pmgr_misc *misc = dev_get_drvdata(dev);
	int i;

	if (misc->single_step && pm_suspend_target_state != PM_SUSPEND_TO_IDLE) {
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
			misc->fabric_now = ret ? U32_MAX : misc->devices[DEV_FABRIC].active_state;
		else if (i == DEV_DCS)
			misc->dcs_now = ret ? U32_MAX : misc->devices[DEV_DCS].active_state;
		else if (i == DEV_DISP)
			misc->disp_now = ret ? U32_MAX : misc->devices[DEV_DISP].active_state;
	}

	return 0;
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
	seq_printf(s, "pmp_owned=%d voters_ok=%d holds=%u dcs_bw_active=%d cpus_at_min=%d gpu_tracked=%d gpu_busy=%u\n",
		   misc->pmp_owned, misc->voters_ok, misc->holds,
		   misc->dcs_bw_active, apple_pmgr_cpus_at_min_peek(misc),
		   atomic_read(&apple_pmgr_gpu_tracked),
		   apple_pmgr_gpu_busy_count());
	seq_printf(s, "fabric_active=%u fabric_now=%u dcs_active=%u dcs_now=%u\n",
		   misc->devices[DEV_FABRIC].active_state, misc->fabric_now,
		   misc->devices[DEV_DCS].active_state, misc->dcs_now);
	if (misc->devices[DEV_DISP].base)
		seq_printf(s, "disp_active=%u disp_now=%u disp_idle=%u\n",
			   misc->devices[DEV_DISP].active_state, misc->disp_now,
			   misc->idle_disp_pstate);
	if (misc->tiered_scaling) {
		u64 res_ms[APPLE_PMGR_TIER_MAX];
		u64 now = ktime_get_ns();

		for (i = 0; i < APPLE_PMGR_TIER_MAX; i++) {
			u64 ns = misc->gov.residency_ns[i];

			if (misc->gov.initialized && i == misc->gov.tier &&
			    now > misc->gov.tier_since_ns)
				ns += now - misc->gov.tier_since_ns;
			res_ms[i] = div_u64(ns, NSEC_PER_MSEC);
		}
		seq_printf(s, "tier=%s transitions=%llu rate_limited=%llu\n",
			   apple_pmgr_tier_names[misc->gov.tier],
			   misc->gov.transitions, misc->gov.rate_limited);
		seq_printf(s, "load e_max=%u e_sum=%u p_max=%u p_sum=%u gpu_pct=%u gpu_inflight=%d gpu_inflight_ms=%u\n",
			   misc->last_sample.e_max, misc->last_sample.e_sum,
			   misc->last_sample.p_max, misc->last_sample.p_sum,
			   misc->last_sample.gpu_pct,
			   misc->last_sample.gpu_inflight,
			   misc->last_sample.gpu_inflight_ms);
		seq_printf(s, "residency_ms idle=%llu light=%llu full=%llu\n",
			   res_ms[APPLE_PMGR_TIER_IDLE],
			   res_ms[APPLE_PMGR_TIER_LIGHT],
			   res_ms[APPLE_PMGR_TIER_FULL]);
	}
	for (i = 0; i < ARRAY_SIZE(misc->votes); i++) {
		if (!misc->votes[i].regmap) {
			seq_printf(s, "voter[%s]=%s\n", apple_pmgr_fabric_voters[i].label,
				   apple_pmgr_fabric_voters[i].optional ? "absent" : "missing");
		} else if (regmap_read(misc->votes[i].regmap, misc->votes[i].reg, &val)) {
			seq_printf(s, "voter[%s]=err\n", apple_pmgr_fabric_voters[i].label);
		} else {
			seq_printf(s, "voter[%s]=%#010x actual=%lu\n",
				   apple_pmgr_fabric_voters[i].label, val,
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

	scoped_guard(rwsem_write, &apple_pmgr_inst_sem)
		smp_store_release(&apple_pmgr_misc_inst, NULL);
	scoped_guard(mutex, &misc->lock) {
		misc->sleeping = true;
		apple_pmgr_gpu_wake_set(NULL);
	}
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
	misc->tiered_scaling = hw->tiered_scaling;
	misc->light_fabric = hw->light_fabric;
	misc->light_dcs = hw->light_dcs;
	misc->gov_cfg = apple_pmgr_gov_cfg_default;
	misc->gov.tier = APPLE_PMGR_TIER_FULL;

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
	misc->pmp_owned = np != NULL;
	of_node_put(np);
	if (misc->pmp_owned)
		dev_info(dev, "PMP manages fabric and DCS p-states\n");

	if (misc->devices[DEV_FABRIC].base) {
		mutex_init(&misc->lock);
		misc->fabric_now = misc->devices[DEV_FABRIC].active_state;
		misc->dcs_now = misc->devices[DEV_DCS].active_state;
		misc->disp_now = misc->devices[DEV_DISP].active_state;
		misc->cpu_snap = devm_kcalloc(dev, nr_cpu_ids, sizeof(*misc->cpu_snap), GFP_KERNEL);
		INIT_DEFERRABLE_WORK(&misc->idle_work, apple_pmgr_idle_work);
		INIT_DELAYED_WORK(&misc->idle_work_prompt, apple_pmgr_idle_work_prompt);
		apple_pmgr_find_voters(misc);
		ret = apple_pmgr_dcs_init(misc);
		if (ret)
			return ret;
		ret = apple_pmgr_disp_floor_init(misc);
		if (ret)
			return ret;
		if (!misc->pmp_owned) {
			if (READ_ONCE(idle_dcs_pstate) && misc->dcs_valid_states &&
			    !apple_pmgr_dcs_state_valid(misc, READ_ONCE(idle_dcs_pstate))) {
				dev_warn(dev, "idle_dcs_pstate=%u is invalid on this SoC; disabling\n",
					 READ_ONCE(idle_dcs_pstate));
				WRITE_ONCE(idle_dcs_pstate, 0);
			}
			if (!READ_ONCE(idle_fabric_pstate_explicit) &&
			    !READ_ONCE(idle_fabric_pstate) && hw->default_idle_fabric)
				WRITE_ONCE(idle_fabric_pstate, hw->default_idle_fabric);
			if (!READ_ONCE(idle_dcs_pstate_explicit) &&
			    !READ_ONCE(idle_dcs_pstate) && hw->default_idle_dcs &&
			    apple_pmgr_dcs_state_valid(misc, misc->devices[DEV_DCS].active_state) &&
			    apple_pmgr_dcs_state_valid(misc, hw->default_idle_dcs))
				WRITE_ONCE(idle_dcs_pstate, hw->default_idle_dcs);
			apple_pmgr_gpu_wake_set(misc);
		}
		/* Publish only a set-up instance; pairs with the readers' acquire. */
		smp_store_release(&apple_pmgr_misc_inst, misc);
		ret = devm_add_action_or_reset(dev, apple_pmgr_misc_teardown, misc);
		if (ret)
			return ret;
		if ((READ_ONCE(idle_fabric_pstate) || READ_ONCE(idle_dcs_pstate)) &&
		    misc->pmp_owned) {
			dev_warn(dev, "idle fabric governor disabled: PMP manages the fabric\n");
			WRITE_ONCE(idle_fabric_pstate, 0);
			WRITE_ONCE(idle_dcs_pstate, 0);
		} else if (READ_ONCE(idle_fabric_pstate) || READ_ONCE(idle_dcs_pstate)) {
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
	.tiered_scaling = true,
	.default_idle_fabric = SYS_DEV_PSTATE_SUSPEND,
	.default_idle_dcs = DCS_DEV_PSTATE_MIN_T8103,
	.light_fabric = 2,
	.light_dcs = DCS_DEV_PSTATE_MIN_T8103,
	.dev_min_ps = {
		[DEV_FABRIC] = SYS_DEV_PSTATE_SUSPEND,
		[DEV_DCS] = DCS_DEV_PSTATE_MIN_T8103,
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

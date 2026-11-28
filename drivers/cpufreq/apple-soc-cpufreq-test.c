// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple SoC CPU cluster performance state driver: the
 * per-SoC command and status register layouts, the waited-for and the lazily
 * verified request paths, the fast die-temperature limit arithmetic and its
 * devicetree parsing, and the board thermal ownership cap.
 *
 * The cluster registers are a block of ordinary memory: the driver only uses
 * plain MMIO accessors on its base pointer, and no test maps a device. A
 * written request reads back as accepted, as on an idle cluster; a test sets
 * the busy bit or another state itself to play the hardware.
 *
 * Most tests run on a cpufreq policy that is not known to the cpufreq core.
 * The ->get() and thermal ownership tests need a policy of the core, so their
 * suite registers a cpufreq driver of its own, whose requests do nothing, and
 * is skipped where a cpufreq driver is already registered.
 *
 * Included by apple-soc-cpufreq.c.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/delay.h>
#include <linux/irqflags.h>
#include <linux/kernfs.h>
#include <trace/events/power.h>

#define CFT_REGS_SIZE	0x100
#define CFT_NR_STATES	5

/* Frequencies in kHz and the P-state (OPP level) of each; made-up values. */
static const struct cpufreq_frequency_table cft_table[CFT_NR_STATES + 1] = {
	{ .driver_data = 1, .frequency = 600000 },
	{ .driver_data = 4, .frequency = 1200000 },
	{ .driver_data = 7, .frequency = 1800000 },
	{ .driver_data = 10, .frequency = 2400000 },
	{ .driver_data = 15, .frequency = 3000000 },
	{ .frequency = CPUFREQ_TABLE_END },
};

struct cft_ctx {
	struct cpufreq_policy *policy;
	struct apple_cpu_priv *priv;
	struct cpufreq_frequency_table *table;
	u8 *regs;
};

static u64 cft_cmd(u8 *regs)
{
	return le64_to_cpup((__le64 *)(regs + APPLE_DVFS_CMD));
}

static void cft_set_cmd(u8 *regs, u64 val)
{
	*(__le64 *)(regs + APPLE_DVFS_CMD) = cpu_to_le64(val);
}

static void cft_set_status(u8 *regs, u32 val)
{
	*(__le32 *)(regs + APPLE_DVFS_STATUS) = cpu_to_le32(val);
}

static void cft_priv_init(struct apple_cpu_priv *priv, u8 *regs,
			  const struct apple_soc_cpufreq_info *info,
			  unsigned int pstate)
{
	/* As apple_soc_cpufreq_init() leaves it. */
	priv->reg_base = (void __iomem __force *)regs;
	priv->info = info;
	raw_spin_lock_init(&priv->lock);
	hrtimer_setup(&priv->verify_timer, apple_soc_cpufreq_verify,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	priv->pending_pstate = -1;
	priv->cap_pstate = info->max_pstate;
	priv->waited = true;
	priv->expected_pstate = pstate;
	cft_set_cmd(regs, pstate);
}

/* A cluster of @info that idles in @pstate, with the cft_table states. */
static struct cft_ctx *cft_setup(struct kunit *test,
				 const struct apple_soc_cpufreq_info *info,
				 unsigned int pstate)
{
	struct cft_ctx *prev = test->priv;
	struct cft_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->regs = kunit_kzalloc(test, CFT_REGS_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->regs);
	ctx->table = kunit_kzalloc(test, sizeof(cft_table), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->table);
	memcpy(ctx->table, cft_table, sizeof(cft_table));
	ctx->policy = kunit_kzalloc(test, sizeof(*ctx->policy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->policy);
	ctx->priv = kunit_kzalloc(test, sizeof(*ctx->priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->priv);

	/* A test may set up one cluster after another, on the same device. */
	if (prev) {
		hrtimer_cancel(&prev->priv->verify_timer);
		ctx->priv->cpu_dev = prev->priv->cpu_dev;
	} else {
		ctx->priv->cpu_dev = kunit_device_register(test, "apple-cpufreq-test");
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->priv->cpu_dev);
	cft_priv_init(ctx->priv, ctx->regs, info, pstate);

	ctx->policy->freq_table = ctx->table;
	ctx->policy->freq_table_sorted = CPUFREQ_TABLE_SORTED_ASCENDING;
	ctx->policy->min = ctx->table[0].frequency;
	ctx->policy->max = ctx->table[CFT_NR_STATES - 1].frequency;
	ctx->policy->cur = ctx->table[0].frequency;
	ctx->policy->fast_switch_enabled = info->lazy_verify;
	ctx->policy->driver_data = ctx->priv;
	ctx->priv->policy = ctx->policy;

	test->priv = ctx;
	return ctx;
}

static void cft_exit(struct kunit *test)
{
	struct cft_ctx *ctx = test->priv;

	if (ctx && ctx->priv)
		hrtimer_cancel(&ctx->priv->verify_timer);
}

/*
 * The lazily verified paths arm the verify timer, a hard interrupt timer due
 * within a millisecond or less. A test calls them with interrupts off, so
 * that the timer cannot run, and ends the section here: this returns whether
 * the timer was armed, and cancels it. The test then runs the timer callback
 * itself, in the state it has prepared.
 */
static bool cft_thaw(struct cft_ctx *ctx, unsigned long flags)
{
	bool queued = hrtimer_is_queued(&ctx->priv->verify_timer);

	hrtimer_cancel(&ctx->priv->verify_timer);
	local_irq_restore(flags);
	return queued;
}

/* Runs the verify timer callback once, as cft_thaw() describes. */
static bool cft_verify(struct cft_ctx *ctx)
{
	unsigned long flags;

	local_irq_save(flags);
	apple_soc_cpufreq_verify(&ctx->priv->verify_timer);
	return cft_thaw(ctx, flags);
}

static bool cft_lazy_request(struct cft_ctx *ctx, unsigned int index,
			     unsigned int *freq)
{
	unsigned long flags;

	local_irq_save(flags);
	*freq = apple_soc_cpufreq_lazy_request(ctx->policy, index);
	return cft_thaw(ctx, flags);
}

static bool cft_lazy_cap(struct cft_ctx *ctx, unsigned int cap,
			 unsigned int freq, int *moved)
{
	unsigned long flags;

	local_irq_save(flags);
	*moved = apple_soc_cpufreq_lazy_cap(ctx->priv, cap, freq);
	return cft_thaw(ctx, flags);
}

/*
 * Register layouts
 */

struct cft_layout {
	const char *compatible;
	u64 ps1_mask;
	u64 cur_pstate_mask;
	u64 min_pstate;
	u64 max_pstate;
	bool has_ps2;
};

static const struct cft_layout cft_layouts[] = {
	{ "apple,s5l8960x-cluster-cpufreq", GENMASK(24, 22), GENMASK(5, 3), 0, 7, false },
	{ "apple,t8103-cluster-cpufreq", GENMASK(4, 0), GENMASK(7, 4), 0, 15, true },
	{ "apple,t8112-cluster-cpufreq", GENMASK(4, 0), GENMASK(9, 5), 0, 31, false },
	{ "apple,t8132-cluster-cpufreq", GENMASK(4, 0), 0, 0, 31, false },
	{ "apple,cluster-cpufreq", GENMASK(4, 0), 0, 0, 15, false },
	{ "apple,t8140-cluster-cpufreq", GENMASK(4, 0), 0, 1, 31, false },
	{ "apple,t6050-cluster-cpufreq", GENMASK(4, 0), 0, 0, 31, false },
	{ "apple,t8142-cluster-cpufreq", GENMASK(4, 0), 0, 1, 31, false },
	{ "apple,t8152-cluster-cpufreq", GENMASK(4, 0), 0, 2, 31, false },
};

static void cft_layout_desc(const struct cft_layout *l, char *desc)
{
	strscpy(desc, l->compatible, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(cft_layout, cft_layouts, cft_layout_desc);

static const struct apple_soc_cpufreq_info *cft_match(const char *compatible)
{
	const struct of_device_id *id;

	for (id = apple_soc_cpufreq_of_match; id->compatible[0]; id++)
		if (!strcmp(id->compatible, compatible))
			return id->data;
	return NULL;
}

/* Each compatible selects the layout of its SoC. */
static void cft_test_layout(struct kunit *test)
{
	const struct cft_layout *l = test->param_value;
	const struct apple_soc_cpufreq_info *info = cft_match(l->compatible);

	KUNIT_ASSERT_NOT_NULL(test, info);
	KUNIT_EXPECT_EQ(test, info->ps1_mask, l->ps1_mask);
	KUNIT_EXPECT_EQ(test, info->ps1_shift, __ffs64(l->ps1_mask));
	KUNIT_EXPECT_EQ(test, info->cur_pstate_mask, l->cur_pstate_mask);
	if (l->cur_pstate_mask)
		KUNIT_EXPECT_EQ(test, info->cur_pstate_shift,
				__ffs64(l->cur_pstate_mask));
	KUNIT_EXPECT_EQ(test, info->min_pstate, l->min_pstate);
	KUNIT_EXPECT_EQ(test, info->max_pstate, l->max_pstate);
	KUNIT_EXPECT_EQ(test, info->has_ps2, l->has_ps2);
}

/* What every layout must satisfy for the request and readback code to work. */
static void cft_test_layout_invariants(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = apple_soc_cpufreq_of_match; id->compatible[0]; id++) {
		const struct apple_soc_cpufreq_info *info = id->data;

		KUNIT_ASSERT_NOT_NULL_MSG(test, info, "%s", id->compatible);
		KUNIT_ASSERT_NE_MSG(test, info->ps1_mask, 0, "%s", id->compatible);
		/* The request field holds every P-state. */
		KUNIT_EXPECT_EQ_MSG(test, info->ps1_shift, __ffs64(info->ps1_mask),
				    "%s", id->compatible);
		KUNIT_EXPECT_LE_MSG(test, info->min_pstate, info->max_pstate,
				    "%s", id->compatible);
		KUNIT_EXPECT_LE_MSG(test, info->max_pstate,
				    info->ps1_mask >> info->ps1_shift,
				    "%s", id->compatible);
		KUNIT_EXPECT_LE_MSG(test, info->max_unmanaged_pstate, info->max_pstate,
				    "%s", id->compatible);
		if (info->has_ps2)
			KUNIT_EXPECT_LE_MSG(test, info->max_pstate,
					    FIELD_MAX(APPLE_DVFS_CMD_PS2),
					    "%s", id->compatible);
		/* So does the status field, where the SoC has one. */
		if (info->cur_pstate_mask) {
			KUNIT_EXPECT_EQ_MSG(test, info->cur_pstate_shift,
					    __ffs64(info->cur_pstate_mask),
					    "%s", id->compatible);
			KUNIT_EXPECT_LE_MSG(test, info->max_pstate,
					    info->cur_pstate_mask >> info->cur_pstate_shift,
					    "%s", id->compatible);
		}
		/* Verification reads the request back as APPLE_DVFS_CMD_PS1. */
		if (info->verify_transition)
			KUNIT_EXPECT_EQ_MSG(test, info->ps1_mask, APPLE_DVFS_CMD_PS1,
					    "%s", id->compatible);
	}
}

struct cft_write {
	const char *name;
	const struct apple_soc_cpufreq_info *info;
	u64 before;
	unsigned int pstate;
	u64 after;
};

static const struct cft_write cft_writes[] = {
	{ "t8103 sets PS1 and PS2", &soc_t8103_info, 0, 9,
	  BIT(25) | (9 << 12) | 9 },
	{ "t8103 replaces PS1 and PS2 only", &soc_t8103_info, ~0ULL, 9,
	  (~0ULL & ~(GENMASK_ULL(15, 12) | GENMASK_ULL(4, 0))) | (9 << 12) | 9 },
	{ "t8112 sets PS1", &soc_t8112_info, 0, 21, BIT(25) | 21 },
	{ "t8112 leaves the PS2 bits", &soc_t8112_info, ~0ULL, 21,
	  (~0ULL & ~GENMASK_ULL(4, 0)) | 21 },
	{ "s5l8960x sets bits 24:22", &soc_s5l8960x_info, 0, 5,
	  BIT(25) | (5 << 22) },
	{ "s5l8960x replaces bits 24:22 only", &soc_s5l8960x_info, ~0ULL, 5,
	  (~0ULL & ~GENMASK_ULL(24, 22)) | (5 << 22) },
	{ "t8140 highest state", &soc_t8140_info, 0, 31, BIT(25) | 31 },
	{ "t8132 lowest state", &soc_t8132_info, GENMASK_ULL(4, 0), 0, BIT(25) },
	{ "generic", &soc_default_info, 3, 12, BIT(25) | 12 },
};

static void cft_write_desc(const struct cft_write *w, char *desc)
{
	strscpy(desc, w->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(cft_write, cft_writes, cft_write_desc);

/* A request replaces the state fields of its SoC and sets the SET bit. */
static void cft_test_write(struct kunit *test)
{
	const struct cft_write *w = test->param_value;
	struct cft_ctx *ctx = cft_setup(test, w->info, 0);

	apple_soc_cpufreq_write(ctx->priv, w->before, w->pstate);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), w->after);
}

static void cft_test_timeout(struct kunit *test)
{
	static const struct apple_soc_cpufreq_info own = {
		.transition_timeout_us = 1234,
	};
	static const struct apple_soc_cpufreq_info none = {};

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_timeout(&own), 1234);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_timeout(&none),
			APPLE_DVFS_TRANSITION_TIMEOUT);
}

/*
 * Waited-for requests
 */

/* The frequency-table index selects the P-state, not the other way round. */
static void cft_test_request(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8103_info, 4);

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_request(ctx->policy, 3), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | (10 << 12) | 10);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_request(ctx->policy, 0), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | (1 << 12) | 1);
}

/* A P-state outside the range of the SoC is refused before any write. */
static void cft_test_request_range(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8103_info, 4);

	ctx->table[4].driver_data = 16;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_request(ctx->policy, 4), -EINVAL);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);
	ctx->table[4].driver_data = 15;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_request(ctx->policy, 4), 0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 15);

	/* The states of T8152 start at 2, those of T8140 at 1. */
	ctx = cft_setup(test, &soc_t8152_info, 4);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_request(ctx->policy, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);
	KUNIT_EXPECT_FALSE(test, ctx->priv->transition_failed);
	ctx->table[0].driver_data = 2;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_request(ctx->policy, 0), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | 2);

	ctx = cft_setup(test, &soc_t8140_info, 4);
	ctx->table[0].driver_data = 0;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_request(ctx->policy, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);
	ctx->table[0].driver_data = 1;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_request(ctx->policy, 0), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | 1);
}

/* A SoC that does not verify drops a request to a busy cluster, no more. */
static void cft_test_set_pstate_busy(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8112_info, 4);

	cft_set_cmd(ctx->regs, APPLE_DVFS_CMD_BUSY | 4);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_pstate(ctx->priv, 10), -EIO);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), APPLE_DVFS_CMD_BUSY | 4);
	KUNIT_EXPECT_FALSE(test, ctx->priv->transition_failed);

	cft_set_cmd(ctx->regs, 4);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_pstate(ctx->priv, 10), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | 10);
}

static void cft_test_set_pstate_verified(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t6050_info, 4);

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_pstate(ctx->priv, 10), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | 10);
	KUNIT_EXPECT_EQ(test, ctx->priv->expected_pstate, 10);

	/* The accepted state is not requested again. */
	cft_set_cmd(ctx->regs, 10);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_pstate(ctx->priv, 10), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 10);
	KUNIT_EXPECT_FALSE(test, ctx->priv->transition_failed);
}

/* A state nobody asked for stops all requests: someone else owns the cluster. */
static void cft_test_set_pstate_unexpected(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t6050_info, 4);

	cft_set_cmd(ctx->regs, 7);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_pstate(ctx->priv, 10), -EIO);
	KUNIT_EXPECT_TRUE(test, ctx->priv->transition_failed);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 7);
	KUNIT_EXPECT_EQ(test, ctx->priv->expected_pstate, 4);

	/* Not even one that would now match. */
	cft_set_cmd(ctx->regs, 4);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_pstate(ctx->priv, 10), -EIO);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);
}

static void cft_test_set_pstate_verified_busy(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t6050_info, 4);

	cft_set_cmd(ctx->regs, APPLE_DVFS_CMD_BUSY | 4);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_pstate(ctx->priv, 10), -EIO);
	KUNIT_EXPECT_TRUE(test, ctx->priv->transition_failed);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), APPLE_DVFS_CMD_BUSY | 4);
	KUNIT_EXPECT_EQ(test, ctx->priv->expected_pstate, 4);
}

/* ->fast_switch() of a SoC that does not verify: lowest state at or above. */
static void cft_test_fast_switch(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8103_info, 4);

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_fast_switch(ctx->policy, 1300000),
			1800000);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | (7 << 12) | 7);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_fast_switch(ctx->policy, 600000),
			600000);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | (1 << 12) | 1);
	/* The policy limits clamp the target. */
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_fast_switch(ctx->policy, UINT_MAX),
			3000000);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | (15 << 12) | 15);
	ctx->policy->max = 2400000;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_fast_switch(ctx->policy, 3000000),
			2400000);

	/* 0 tells the scheduler that nothing was requested. */
	cft_set_cmd(ctx->regs, APPLE_DVFS_CMD_BUSY | 10);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_fast_switch(ctx->policy, 600000), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), APPLE_DVFS_CMD_BUSY | 10);
}

static void cft_test_pstate_freq(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8112_info, 4);

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_pstate_freq(ctx->policy, 1), 600000);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_pstate_freq(ctx->policy, 15), 3000000);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_pstate_freq(ctx->policy, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_pstate_freq(ctx->policy, 5), 0);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_pstate_freq(ctx->policy, 31), 0);
	/* An entry the core has marked invalid has no frequency. */
	ctx->table[2].frequency = CPUFREQ_ENTRY_INVALID;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_pstate_freq(ctx->policy, 7), 0);
}

/*
 * Lazily verified requests
 */

/* An idle cluster takes the request at once; the timer verifies it. */
static void cft_test_lazy_idle(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;
	unsigned int freq;
	bool queued;

	queued = cft_lazy_request(ctx, 3, &freq);
	KUNIT_EXPECT_EQ(test, freq, 2400000);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | 10);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 10);
	KUNIT_EXPECT_EQ(test, priv->asked_pstate, 10);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_FALSE(test, priv->waited);
	KUNIT_EXPECT_EQ(test, ktime_us_delta(priv->verify_deadline, priv->write_time),
			APPLE_DVFS_TRANSITION_TIMEOUT);

	/* The cluster accepted it: the timer has nothing to say or do. */
	KUNIT_EXPECT_FALSE(test, cft_verify(ctx));
	KUNIT_EXPECT_FALSE(test, priv->transition_failed);
	KUNIT_EXPECT_FALSE(test, priv->failure_unreported);

	/* The accepted state is not written again. */
	cft_set_cmd(ctx->regs, 10);
	queued = cft_lazy_request(ctx, 3, &freq);
	KUNIT_EXPECT_EQ(test, freq, 2400000);
	KUNIT_EXPECT_FALSE(test, queued);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 10);
}

/* A busy cluster keeps the newest request pending; the timer writes it. */
static void cft_test_lazy_busy(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;
	unsigned int freq;
	bool queued;

	cft_set_cmd(ctx->regs, APPLE_DVFS_CMD_BUSY | 4);
	queued = cft_lazy_request(ctx, 3, &freq);
	/* Committed all the same: schedutil does not ask twice. */
	KUNIT_EXPECT_EQ(test, freq, 2400000);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), APPLE_DVFS_CMD_BUSY | 4);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 10);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4);
	KUNIT_EXPECT_NE(test, ktime_to_ns(priv->verify_deadline), 0);

	queued = cft_lazy_request(ctx, 2, &freq);
	KUNIT_EXPECT_EQ(test, freq, 1800000);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 7);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), APPLE_DVFS_CMD_BUSY | 4);

	/* Still busy, within the timeout: the timer looks again. */
	priv->verify_deadline = ktime_add_ms(ktime_get(), 60 * MSEC_PER_SEC);
	KUNIT_EXPECT_TRUE(test, cft_verify(ctx));
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 7);
	KUNIT_EXPECT_FALSE(test, priv->transition_failed);

	/* The transition finished as expected: the pending request goes out. */
	cft_set_cmd(ctx->regs, 4);
	KUNIT_EXPECT_TRUE(test, cft_verify(ctx));
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | 7);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 7);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);

	KUNIT_EXPECT_FALSE(test, cft_verify(ctx));
	KUNIT_EXPECT_FALSE(test, priv->transition_failed);
}

/* A pending request for the state the cluster settled in is not written. */
static void cft_test_lazy_pending_same(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);

	ctx->priv->pending_pstate = 4;
	KUNIT_EXPECT_FALSE(test, cft_verify(ctx));
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);
	KUNIT_EXPECT_EQ(test, ctx->priv->pending_pstate, -1);
	KUNIT_EXPECT_FALSE(test, ctx->priv->transition_failed);
}

/* An unexpected state fails the request; the timer reports it, once. */
static void cft_test_lazy_unexpected(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;
	unsigned int freq;
	bool queued;

	cft_set_cmd(ctx->regs, 9);
	queued = cft_lazy_request(ctx, 3, &freq);
	KUNIT_EXPECT_EQ(test, freq, 0);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);
	KUNIT_EXPECT_TRUE(test, priv->failure_unreported);
	KUNIT_EXPECT_EQ(test, priv->failure_command, 9);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 9);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4);

	KUNIT_EXPECT_FALSE(test, cft_verify(ctx));
	KUNIT_EXPECT_FALSE(test, priv->failure_unreported);

	/* No request after a failure, even one that would now match. */
	cft_set_cmd(ctx->regs, 4);
	queued = cft_lazy_request(ctx, 3, &freq);
	KUNIT_EXPECT_EQ(test, freq, 0);
	KUNIT_EXPECT_FALSE(test, queued);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);
	priv->pending_pstate = 10;
	KUNIT_EXPECT_FALSE(test, cft_verify(ctx));
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);
}

/* The timer finds what a request did not: the last request of a burst. */
static void cft_test_lazy_verify_fails(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;

	/* The state changed under us. */
	cft_set_cmd(ctx->regs, 9);
	KUNIT_EXPECT_FALSE(test, cft_verify(ctx));
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);
	KUNIT_EXPECT_FALSE(test, priv->failure_unreported);
	KUNIT_EXPECT_EQ(test, priv->failure_command, 9);

	/* Busy for longer than the timeout; the pending request is dropped. */
	ctx = cft_setup(test, &soc_t8140_info, 4);
	priv = ctx->priv;
	cft_set_cmd(ctx->regs, APPLE_DVFS_CMD_BUSY | 4);
	priv->pending_pstate = 7;
	priv->verify_deadline = ktime_sub_us(ktime_get(), 1);
	KUNIT_EXPECT_FALSE(test, cft_verify(ctx));
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, priv->failure_command, APPLE_DVFS_CMD_BUSY | 4);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), APPLE_DVFS_CMD_BUSY | 4);
}

static void cft_test_lazy_range(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	unsigned int freq;
	bool queued;

	ctx->table[3].driver_data = 0;
	queued = cft_lazy_request(ctx, 3, &freq);
	KUNIT_EXPECT_FALSE(test, queued);
	KUNIT_EXPECT_EQ(test, freq, 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);
	/* Not a fast-switch request at all: waited-for requests keep the cluster. */
	KUNIT_EXPECT_TRUE(test, ctx->priv->waited);
}

/* ->fast_switch() of a lazily verified SoC, with and without a limit. */
static void cft_test_lazy_fast_switch(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;
	unsigned long flags;
	unsigned int freq;
	bool queued;

	local_irq_save(flags);
	freq = apple_soc_cpufreq_fast_switch(ctx->policy, 1300000);
	queued = cft_thaw(ctx, flags);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, freq, 1800000);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | 7);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 7);

	/* Above the limit: the limit is requested, and returned. */
	priv->cap_pstate = 4;
	priv->cap_freq = 1200000;
	local_irq_save(flags);
	freq = apple_soc_cpufreq_fast_switch(ctx->policy, 3000000);
	queued = cft_thaw(ctx, flags);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, freq, 1200000);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 4);
	KUNIT_EXPECT_EQ(test, priv->asked_pstate, 15);

	/* At the limit: as asked. */
	local_irq_save(flags);
	freq = apple_soc_cpufreq_fast_switch(ctx->policy, 1200000);
	cft_thaw(ctx, flags);
	KUNIT_EXPECT_EQ(test, freq, 1200000);
	KUNIT_EXPECT_EQ(test, priv->asked_pstate, 4);
}

/* A new limit moves the cluster at once, down and back up. */
static void cft_test_lazy_cap(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;
	unsigned int freq;
	bool queued;
	int moved;

	cft_lazy_request(ctx, 4, &freq);
	KUNIT_ASSERT_EQ(test, freq, 3000000);
	KUNIT_ASSERT_EQ(test, priv->expected_pstate, 15);

	queued = cft_lazy_cap(ctx, 7, 1800000, &moved);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, moved, 7);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 7);
	KUNIT_EXPECT_EQ(test, priv->cap_pstate, 7);
	KUNIT_EXPECT_EQ(test, priv->cap_freq, 1800000);
	/* What the scheduler asked for is remembered. */
	KUNIT_EXPECT_EQ(test, priv->asked_pstate, 15);

	/* Nothing to move when the limit is already applied. */
	cft_lazy_cap(ctx, 7, 1800000, &moved);
	KUNIT_EXPECT_EQ(test, moved, -1);

	/* A lower state than the limit stays where the scheduler put it. */
	cft_lazy_request(ctx, 1, &freq);
	KUNIT_EXPECT_EQ(test, freq, 1200000);
	cft_lazy_cap(ctx, 10, 2400000, &moved);
	KUNIT_EXPECT_EQ(test, moved, -1);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 4);

	/* The limit rises before schedutil saw it fall: the request is restored. */
	cft_lazy_request(ctx, 4, &freq);
	KUNIT_EXPECT_EQ(test, freq, 2400000);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 10);
	cft_lazy_cap(ctx, 31, 3000000, &moved);
	KUNIT_EXPECT_EQ(test, moved, 15);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 15);

	/* A busy cluster gets the limit as its pending request. */
	cft_set_cmd(ctx->regs, APPLE_DVFS_CMD_BUSY | 15);
	queued = cft_lazy_cap(ctx, 4, 1200000, &moved);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, moved, 4);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 4);
	/* And is not asked again while that is pending. */
	cft_lazy_cap(ctx, 4, 1200000, &moved);
	KUNIT_EXPECT_EQ(test, moved, -1);
}

/* Waited-for requests own the cluster: the limit is recorded, no more. */
static void cft_test_lazy_cap_waited(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;
	unsigned int freq;
	bool queued;
	int moved;

	queued = cft_lazy_cap(ctx, 1, 600000, &moved);
	KUNIT_EXPECT_FALSE(test, queued);
	KUNIT_EXPECT_EQ(test, moved, -1);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);
	KUNIT_EXPECT_EQ(test, priv->cap_pstate, 1);
	KUNIT_EXPECT_EQ(test, priv->cap_freq, 600000);

	/* Nor is a cluster moved whose requests have stopped. */
	cft_lazy_request(ctx, 0, &freq);
	KUNIT_ASSERT_EQ(test, freq, 600000);
	priv->asked_pstate = 15;
	priv->transition_failed = true;
	cft_lazy_cap(ctx, 7, 1800000, &moved);
	KUNIT_EXPECT_EQ(test, moved, -1);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 1);
}

static void cft_test_drain(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;

	priv->waited = false;
	priv->pending_pstate = 7;
	priv->failure_unreported = true;
	priv->failure_command = APPLE_DVFS_CMD_BUSY | 4;
	hrtimer_start(&priv->verify_timer, ms_to_ktime(60 * MSEC_PER_SEC),
		      HRTIMER_MODE_REL_HARD);

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_drain(priv), 7);
	KUNIT_EXPECT_FALSE(test, hrtimer_is_queued(&priv->verify_timer));
	KUNIT_EXPECT_TRUE(test, priv->waited);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_FALSE(test, priv->failure_unreported);

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_drain(priv), -1);
}

/* ->target_index() replaces a pending request and waits for its own. */
static void cft_test_set_target_lazy(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;

	priv->waited = false;
	priv->pending_pstate = 10;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_target(ctx->policy, 2), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | 7);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 7);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_TRUE(test, priv->waited);
}

/* Suspend settles the cluster and records where as policy->cur. */
static void cft_test_suspend_lazy(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;

	/* No suspend frequency: only the driver's own part runs. */
	ctx->policy->suspend_freq = 0;

	priv->waited = false;
	priv->pending_pstate = 10;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_suspend(ctx->policy), 0);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), BIT(25) | 10);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 10);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 2400000);

	/* Nothing pending: policy->cur follows the accepted state. */
	ctx->policy->cur = 600000;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_suspend(ctx->policy), 0);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 2400000);

	/* A cluster that does not settle fails the suspend. */
	ctx->policy->cur = 600000;
	cft_set_cmd(ctx->regs, APPLE_DVFS_CMD_BUSY | 10);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_suspend(ctx->policy), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 600000);
}

#ifdef CONFIG_SUSPEND
static void cft_restore_pm_flags(void *flags)
{
	pm_suspend_global_flags = *(unsigned int *)flags;
}
#endif

/* After a firmware-driven sleep the state the cluster woke in is adopted. */
static void cft_test_resume(struct kunit *test)
{
#ifdef CONFIG_SUSPEND
	struct cft_ctx *ctx = cft_setup(test, &soc_t6050_info, 4);
	struct apple_cpu_priv *priv = ctx->priv;
	unsigned int *flags;

	/* Put the global flags back even if an assertion ends the case. */
	flags = kunit_kmalloc(test, sizeof(*flags), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, flags);
	*flags = pm_suspend_global_flags;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, cft_restore_pm_flags, flags), 0);

	pm_suspend_clear_flags();

	priv->transition_failed = true;
	cft_set_cmd(ctx->regs, 9);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_resume(ctx->policy), 0);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4);
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);

	pm_set_resume_via_firmware();
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_resume(ctx->policy), 0);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 9);
	KUNIT_EXPECT_FALSE(test, priv->transition_failed);

	/* A busy cluster has no state to adopt yet. */
	priv->transition_failed = true;
	cft_set_cmd(ctx->regs, APPLE_DVFS_CMD_BUSY | 12);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_resume(ctx->policy), 0);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 9);
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);

	/* A SoC that does not verify expects nothing. */
	ctx = cft_setup(test, &soc_t8112_info, 4);
	cft_set_cmd(ctx->regs, 9);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_resume(ctx->policy), 0);
	KUNIT_EXPECT_EQ(test, ctx->priv->expected_pstate, 4);
#else
	kunit_skip(test, "needs CONFIG_SUSPEND");
#endif
}

/*
 * Board thermal ownership cap
 */

static void cft_test_unmanaged_max(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8132_info, 4);
	unsigned int i;

	/* Highest state allowed without a thermal policy: 4. */
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_unmanaged_max(ctx->policy), 1200000);

	mutex_lock(&apple_soc_thermal_owner_lock);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_owner_cap(ctx->policy), 1200000);
	ctx->priv->thermal_owned = true;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_owner_cap(ctx->policy),
			FREQ_QOS_MAX_DEFAULT_VALUE);
	mutex_unlock(&apple_soc_thermal_owner_lock);

	ctx->table[1].frequency = CPUFREQ_ENTRY_INVALID;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_unmanaged_max(ctx->policy), 600000);

	/* No state is allowed: the slowest one has to do. */
	for (i = 0; i < CFT_NR_STATES; i++)
		ctx->table[i].driver_data = 5 + i;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_unmanaged_max(ctx->policy), 600000);

	ctx = cft_setup(test, &soc_t8152_info, 4);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_unmanaged_max(ctx->policy), 600000);
}

/*
 * Fast die-temperature limit
 */

static const unsigned long cft_power[CFT_NR_STATES] = { 100, 300, 500, 800, 1000 };

/* A limit that only knows its policy and the power of each state. */
static struct apple_fast_die *cft_fast_die(struct kunit *test, struct cft_ctx *ctx)
{
	struct apple_fast_die *fd;
	unsigned int i;

	fd = kunit_kzalloc(test, struct_size(fd, power, CFT_NR_STATES), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fd);
	fd->nr_states = CFT_NR_STATES;
	for (i = 0; i < CFT_NR_STATES; i++)
		fd->power[i] = cft_power[i];
	fd->policy = ctx->policy;
	fd->max = ctx->table[CFT_NR_STATES - 1].frequency;
	spin_lock_init(&fd->lock);
	return fd;
}

static bool cft_is_state(unsigned int freq)
{
	unsigned int i;

	for (i = 0; i < CFT_NR_STATES; i++)
		if (cft_table[i].frequency == freq)
			return true;
	return false;
}

static void cft_test_fast_die_fit(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8112_info, 4);
	struct apple_fast_die *fd = cft_fast_die(test, ctx);

	/* The slowest state when nothing fits. */
	KUNIT_EXPECT_EQ(test, apple_fast_die_fit(fd, 0), 600000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_fit(fd, 99), 600000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_fit(fd, 100), 600000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_fit(fd, 299), 600000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_fit(fd, 300), 1200000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_fit(fd, 999), 2400000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_fit(fd, 1000), 3000000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_fit(fd, U64_MAX), 3000000);
}

static void cft_test_fast_die_limit(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8112_info, 4);
	struct apple_fast_die *fd = cft_fast_die(test, ctx);
	unsigned int effort, limit, last = UINT_MAX;

	/* No effort is no limit, whatever 1000 / 255 rounds to. */
	KUNIT_EXPECT_EQ(test, apple_fast_die_limit(fd, 0), 3000000);
	/* 204 / 255 of 1000 is 800, 203 / 255 is 796. */
	KUNIT_EXPECT_EQ(test, apple_fast_die_limit(fd, 1), 2400000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_limit(fd, 51), 2400000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_limit(fd, 52), 1800000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_limit(fd, 128), 1200000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_limit(fd, 255), 600000);

	for (effort = 0; effort <= APPLE_FAST_DIE_EFFORT_MAX; effort++) {
		limit = apple_fast_die_limit(fd, effort);
		KUNIT_EXPECT_LE_MSG(test, limit, last, "effort %u", effort);
		KUNIT_EXPECT_TRUE_MSG(test, cft_is_state(limit), "effort %u: %u",
				      effort, limit);
		last = limit;
	}

	/* Tens of watts, in microwatts: the budget needs more than 32 bits. */
	fd->power[CFT_NR_STATES - 1] = 50000000;
	KUNIT_EXPECT_EQ(test, apple_fast_die_limit(fd, 0), 3000000);
	KUNIT_EXPECT_EQ(test, apple_fast_die_limit(fd, 255), 600000);
}

static void cft_test_fast_die_interval(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8112_info, 4);
	struct apple_fast_die *fd = cft_fast_die(test, ctx);

	KUNIT_EXPECT_EQ(test, ktime_to_ms(apple_fast_die_interval(fd)), 5);
	/* Warned, but the PMP runs: back to the fast interval. */
	fd->warned = true;
	KUNIT_EXPECT_EQ(test, ktime_to_ms(apple_fast_die_interval(fd)), 5);
	fd->waiting = true;
	KUNIT_EXPECT_EQ(test, ktime_to_ms(apple_fast_die_interval(fd)), 1000);
	fd->warned = false;
	KUNIT_EXPECT_EQ(test, ktime_to_ms(apple_fast_die_interval(fd)), 100);
}

/* The hrtimer samples only when the per-CPU timers do not. */
static void cft_test_fast_die_arm(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8112_info, 4);
	struct apple_fast_die *fd = cft_fast_die(test, ctx);

	hrtimer_setup(&fd->poll, apple_fast_die_poll, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
	/* The slowest interval: the timer is cancelled long before it runs. */
	fd->waiting = true;
	fd->warned = true;

	spin_lock_bh(&fd->lock);
	fd->per_cpu = true;
	fd->limit = fd->max;
	apple_fast_die_arm(fd);
	spin_unlock_bh(&fd->lock);
	KUNIT_EXPECT_FALSE(test, fd->polling);
	KUNIT_EXPECT_FALSE(test, hrtimer_is_queued(&fd->poll));

	spin_lock_bh(&fd->lock);
	fd->limit = fd->max - 1;
	fd->stopping = true;
	apple_fast_die_arm(fd);
	spin_unlock_bh(&fd->lock);
	KUNIT_EXPECT_FALSE(test, fd->polling);
	KUNIT_EXPECT_FALSE(test, hrtimer_is_queued(&fd->poll));

	spin_lock_bh(&fd->lock);
	fd->stopping = false;
	apple_fast_die_arm(fd);
	spin_unlock_bh(&fd->lock);
	KUNIT_EXPECT_TRUE(test, fd->polling);
	KUNIT_EXPECT_TRUE(test, hrtimer_is_queued(&fd->poll));
	hrtimer_cancel(&fd->poll);

	/* Already polling: not armed twice. */
	spin_lock_bh(&fd->lock);
	apple_fast_die_arm(fd);
	spin_unlock_bh(&fd->lock);
	KUNIT_EXPECT_FALSE(test, hrtimer_is_queued(&fd->poll));

	/* Without per-CPU timers the hrtimer samples at the maximum too. */
	spin_lock_bh(&fd->lock);
	fd->polling = false;
	fd->per_cpu = false;
	fd->limit = fd->max;
	apple_fast_die_arm(fd);
	spin_unlock_bh(&fd->lock);
	KUNIT_EXPECT_TRUE(test, fd->polling);
	KUNIT_EXPECT_TRUE(test, hrtimer_is_queued(&fd->poll));
	hrtimer_cancel(&fd->poll);
}

/* A limit moves a lazily verified cluster and scaling_cur_freq with it. */
static void cft_test_fast_die_cap(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8140_info, 4);
	struct apple_fast_die *fd = cft_fast_die(test, ctx);
	struct apple_cpu_priv *priv = ctx->priv;
	unsigned long flags;
	unsigned int freq;
	bool queued;

	cft_lazy_request(ctx, 4, &freq);
	KUNIT_ASSERT_EQ(test, freq, 3000000);
	ctx->policy->cur = freq;

	/* The fastest state at or below the limit. */
	local_irq_save(flags);
	apple_fast_die_cap(fd, 2000000);
	queued = cft_thaw(ctx, flags);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, priv->cap_pstate, 7);
	KUNIT_EXPECT_EQ(test, priv->cap_freq, 1800000);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 7);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 1800000);

	/* A limit below every state changes nothing. */
	local_irq_save(flags);
	apple_fast_die_cap(fd, 599999);
	queued = cft_thaw(ctx, flags);
	KUNIT_EXPECT_FALSE(test, queued);
	KUNIT_EXPECT_EQ(test, priv->cap_pstate, 7);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 7);

	/* A limit that is a state is that state. */
	local_irq_save(flags);
	apple_fast_die_cap(fd, 3000000);
	queued = cft_thaw(ctx, flags);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, priv->cap_pstate, 15);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 15);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 3000000);
}

/* A SoC whose requests the core limits is left to the core. */
static void cft_test_fast_die_cap_unverified(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t8112_info, 15);
	struct apple_fast_die *fd = cft_fast_die(test, ctx);

	ctx->policy->cur = 3000000;
	apple_fast_die_cap(fd, 1800000);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 15);
	KUNIT_EXPECT_EQ(test, ctx->priv->cap_pstate, soc_t8112_info.max_pstate);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 3000000);
}

static void cft_fast_die_exit(void *data)
{
	apple_fast_die_exit(data);
}

static void cft_free_cpus(void *data)
{
	struct cpufreq_policy *policy = data;

	free_cpumask_var(policy->cpus);
}

static void cft_remove_opps(void *data)
{
	dev_pm_opp_remove_all_dynamic(data);
}

/*
 * A cluster ready for apple_fast_die_init(): its CPU device has an OPP for
 * every state, without power, and its policy has QoS constraints and no CPU,
 * so that no per-CPU timer of the machine is touched.
 */
static struct cft_ctx *cft_fast_die_setup(struct kunit *test,
					  const struct apple_soc_cpufreq_info *info)
{
	struct cft_ctx *ctx = cft_setup(test, info, 4);
	unsigned int i;

	KUNIT_ASSERT_TRUE(test, zalloc_cpumask_var(&ctx->policy->cpus, GFP_KERNEL));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, cft_free_cpus,
							ctx->policy), 0);
	freq_constraints_init(&ctx->policy->constraints);

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, cft_remove_opps,
							ctx->priv->cpu_dev), 0);
	for (i = 0; i < CFT_NR_STATES; i++) {
		struct dev_pm_opp_data opp = {
			.level = ctx->table[i].driver_data,
			.freq = ctx->table[i].frequency * 1000UL,
			.u_volt = 800000,
		};

		KUNIT_ASSERT_EQ(test, dev_pm_opp_add_dynamic(ctx->priv->cpu_dev, &opp), 0);
	}

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, cft_fast_die_exit,
							ctx->priv), 0);
	return ctx;
}

/* A cluster node with @value as its apple,fast-die-effort, or without one. */
static struct device_node *cft_cluster_node(struct kunit *test, __be32 *value,
					    unsigned int cells)
{
	struct device_node *np;
	struct property *prop;

	np = kunit_kzalloc(test, sizeof(*np), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, np);
	np->name = "cpufreq";
	np->full_name = "cpufreq-test";
	if (!value)
		return np;

	prop = kunit_kzalloc(test, sizeof(*prop), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, prop);
	prop->name = "apple,fast-die-effort";
	prop->value = value;
	prop->length = cells * sizeof(*value);
	np->properties = prop;
	return np;
}

static s32 cft_qos_max(struct cft_ctx *ctx)
{
	return freq_qos_read_value(&ctx->policy->constraints, FREQ_QOS_MAX);
}

/* No limit described, none needed: nothing is set up. */
static void cft_test_fast_die_init_none(struct kunit *test)
{
	struct cft_ctx *ctx = cft_fast_die_setup(test, &soc_t8112_info);
	struct device_node *np = cft_cluster_node(test, NULL, 0);

	KUNIT_EXPECT_EQ(test, apple_fast_die_init(ctx->policy, np), 0);
	KUNIT_EXPECT_NULL(test, ctx->priv->fast_die);
	KUNIT_EXPECT_EQ(test, cft_qos_max(ctx), FREQ_QOS_MAX_DEFAULT_VALUE);
}

/* A SoC that needs the limit is held at the fallback by an older devicetree. */
static void cft_test_fast_die_init_required(struct kunit *test)
{
	struct cft_ctx *ctx = cft_fast_die_setup(test, &soc_t8140_info);
	struct device_node *np = cft_cluster_node(test, NULL, 0);
	struct apple_fast_die *fd;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, apple_fast_die_init(ctx->policy, np), 0);
	fd = ctx->priv->fast_die;
	KUNIT_ASSERT_NOT_NULL(test, fd);

	spin_lock_bh(&fd->lock);
	KUNIT_EXPECT_NULL(test, fd->report);
	KUNIT_EXPECT_TRUE(test, fd->warned);
	KUNIT_EXPECT_EQ(test, fd->nr_states, CFT_NR_STATES);
	/* The OPPs describe no power: the frequency stands in. */
	for (i = 0; i < CFT_NR_STATES; i++)
		KUNIT_EXPECT_EQ(test, fd->power[i], ctx->table[i].frequency);
	KUNIT_EXPECT_EQ(test, fd->max, 3000000);
	/* The fastest state with at most a third of the fastest state's power. */
	KUNIT_EXPECT_EQ(test, fd->fallback, 600000);
	KUNIT_EXPECT_EQ(test, fd->limit, 600000);
	spin_unlock_bh(&fd->lock);

	KUNIT_EXPECT_EQ(test, cft_qos_max(ctx), 600000);
	/* Fast-switch requests are clamped from the start. */
	KUNIT_EXPECT_EQ(test, ctx->priv->cap_pstate, 1);
	KUNIT_EXPECT_EQ(test, ctx->priv->cap_freq, 600000);
	KUNIT_EXPECT_EQ(test, cft_cmd(ctx->regs), 4);

	apple_fast_die_exit(ctx->priv);
	KUNIT_EXPECT_NULL(test, ctx->priv->fast_die);
	KUNIT_EXPECT_EQ(test, cft_qos_max(ctx), FREQ_QOS_MAX_DEFAULT_VALUE);
}

KUNIT_DEFINE_ACTION_WRAPPER(cft_put_report, of_node_put, struct device_node *);

/* The property names the PMP report node and the lane of the cluster. */
static void cft_test_fast_die_init_described(struct kunit *test)
{
	struct cft_ctx *ctx = cft_fast_die_setup(test, &soc_t8140_info);
	struct device_node *np, *report;
	struct apple_fast_die *fd;
	__be32 *value;
	int ret;

	/* Hold a reference to the first node that can stand in for the report. */
	for (report = of_find_all_nodes(NULL); report;
	     report = of_find_all_nodes(report)) {
		if (report->phandle)
			break;
	}
	if (!report)
		kunit_skip(test, "no devicetree node with a phandle");
	ret = kunit_add_action_or_reset(test, cft_put_report, report);
	KUNIT_ASSERT_EQ(test, ret, 0);

	value = kunit_kzalloc(test, 2 * sizeof(*value), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, value);
	value[0] = cpu_to_be32(report->phandle);
	value[1] = cpu_to_be32(3);
	np = cft_cluster_node(test, value, 2);

	KUNIT_ASSERT_EQ(test, apple_fast_die_init(ctx->policy, np), 0);
	fd = ctx->priv->fast_die;
	KUNIT_ASSERT_NOT_NULL(test, fd);

	spin_lock_bh(&fd->lock);
	KUNIT_EXPECT_PTR_EQ(test, fd->report, report);
	KUNIT_EXPECT_EQ(test, fd->lane, 3);
	KUNIT_EXPECT_FALSE(test, fd->warned);
	/* Held at the fallback until the PMP is found running. */
	KUNIT_EXPECT_EQ(test, fd->limit, 600000);
	spin_unlock_bh(&fd->lock);
	KUNIT_EXPECT_EQ(test, cft_qos_max(ctx), 600000);

	apple_fast_die_exit(ctx->priv);
	KUNIT_EXPECT_NULL(test, ctx->priv->fast_die);
}

/* A misdescribed limit fails closed, also where none is required. */
static void cft_test_fast_die_init_invalid(struct kunit *test)
{
	struct cft_ctx *ctx = cft_fast_die_setup(test, &soc_t8112_info);
	struct apple_fast_die *fd;
	struct device_node *np;
	__be32 *value;

	/* A phandle without the lane cell. */
	value = kunit_kzalloc(test, sizeof(*value), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, value);
	value[0] = cpu_to_be32(1);
	np = cft_cluster_node(test, value, 1);

	KUNIT_ASSERT_EQ(test, apple_fast_die_init(ctx->policy, np), 0);
	fd = ctx->priv->fast_die;
	KUNIT_ASSERT_NOT_NULL(test, fd);

	spin_lock_bh(&fd->lock);
	KUNIT_EXPECT_NULL(test, fd->report);
	KUNIT_EXPECT_TRUE(test, fd->warned);
	KUNIT_EXPECT_EQ(test, fd->limit, 600000);
	spin_unlock_bh(&fd->lock);
	KUNIT_EXPECT_EQ(test, cft_qos_max(ctx), 600000);

	apple_fast_die_exit(ctx->priv);
	KUNIT_EXPECT_EQ(test, cft_qos_max(ctx), FREQ_QOS_MAX_DEFAULT_VALUE);
}

/*
 * A deferred pending request and a lifted thermal cap must both respect a
 * newer policy->max, and losing fast-switch ownership must drop any pending
 * request instead of writing it under a non-fast-switch governor.
 */
static void cft_test_lazy_policy_limits_and_handoff(struct kunit *test)
{
	struct cft_ctx *ctx = cft_setup(test, &soc_t6050_info, 1);
	unsigned int freq;
	int moved;

	/* Queue a high pending state while BUSY, then lower policy->max. */
	cft_set_cmd(ctx->regs, 1 | APPLE_DVFS_CMD_BUSY);
	KUNIT_EXPECT_TRUE(test, cft_lazy_request(ctx, 4, &freq));
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, ctx->priv->pending_pstate, 15);

	ctx->policy->max = 1800000;
	cft_set_cmd(ctx->regs, 1);
	KUNIT_EXPECT_TRUE(test, cft_verify(ctx));
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 7ULL);
	KUNIT_EXPECT_EQ(test, ctx->priv->expected_pstate, 7U);

	/* Thermal cap restore must also stay within the lowered policy->max. */
	KUNIT_EXPECT_TRUE(test, cft_lazy_cap(ctx, 4, 1200000, &moved));
	KUNIT_EXPECT_EQ(test, moved, 4);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 4ULL);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 1200000U);
	KUNIT_EXPECT_TRUE(test, cft_lazy_cap(ctx, 15, 3000000, &moved));
	KUNIT_EXPECT_EQ(test, moved, 7);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 7ULL);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 1800000U);

	/*
	 * Once QoS publishes a higher policy->max after a cap lift, re-running
	 * lazy_cap restores asked_pstate even without a new schedutil request.
	 */
	ctx->policy->max = 3000000;
	KUNIT_EXPECT_TRUE(test, cft_lazy_cap(ctx, 15, 3000000, &moved));
	KUNIT_EXPECT_EQ(test, moved, 15);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 15ULL);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 3000000U);

	/*
	 * Switching away from a fast-switch governor clears
	 * fast_switch_enabled: the timer must drop the pending state and
	 * restore policy->cur to expected_pstate, and thermal samples must
	 * leave hardware and policy->cur untouched.
	 */
	cft_set_cmd(ctx->regs, 7 | APPLE_DVFS_CMD_BUSY);
	ctx->priv->expected_pstate = 7;
	ctx->policy->cur = 1800000;
	KUNIT_EXPECT_TRUE(test, cft_lazy_request(ctx, 4, &freq));
	KUNIT_EXPECT_EQ(test, ctx->priv->pending_pstate, 15);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 3000000U);

	ctx->policy->fast_switch_enabled = false;
	cft_set_cmd(ctx->regs, 7);
	KUNIT_EXPECT_FALSE(test, cft_verify(ctx));
	KUNIT_EXPECT_EQ(test, ctx->priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(ctx->regs)), 7ULL);
	KUNIT_EXPECT_FALSE(test, ctx->priv->transition_failed);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 1800000U);

	KUNIT_EXPECT_FALSE(test, cft_lazy_cap(ctx, 1, 600000, &moved));
	KUNIT_EXPECT_EQ(test, moved, -1);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 1800000U);

	/* Dropping a pending request while BUSY under !fast_switch_enabled also restores cur. */
	ctx->priv->cap_pstate = soc_t6050_info.max_pstate;
	ctx->policy->fast_switch_enabled = true;
	cft_set_cmd(ctx->regs, 7 | APPLE_DVFS_CMD_BUSY);
	KUNIT_EXPECT_TRUE(test, cft_lazy_request(ctx, 4, &freq));
	KUNIT_EXPECT_EQ(test, ctx->priv->pending_pstate, 15);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 3000000U);

	ctx->policy->fast_switch_enabled = false;
	ctx->priv->verify_deadline = ktime_add_ms(ktime_get(), 60 * MSEC_PER_SEC);
	KUNIT_EXPECT_TRUE(test, cft_verify(ctx));
	KUNIT_EXPECT_EQ(test, ctx->priv->pending_pstate, -1);
	KUNIT_EXPECT_FALSE(test, ctx->priv->transition_failed);
	KUNIT_EXPECT_EQ(test, ctx->policy->cur, 1800000U);
}

static struct kunit_case apple_soc_cpufreq_test_cases[] = {
	KUNIT_CASE(cft_test_lazy_policy_limits_and_handoff),
	KUNIT_CASE_PARAM(cft_test_layout, cft_layout_gen_params),
	KUNIT_CASE(cft_test_layout_invariants),
	KUNIT_CASE_PARAM(cft_test_write, cft_write_gen_params),
	KUNIT_CASE(cft_test_timeout),
	KUNIT_CASE(cft_test_request),
	KUNIT_CASE(cft_test_request_range),
	KUNIT_CASE(cft_test_set_pstate_busy),
	KUNIT_CASE(cft_test_set_pstate_verified),
	KUNIT_CASE(cft_test_set_pstate_unexpected),
	KUNIT_CASE(cft_test_set_pstate_verified_busy),
	KUNIT_CASE(cft_test_fast_switch),
	KUNIT_CASE(cft_test_pstate_freq),
	KUNIT_CASE(cft_test_lazy_idle),
	KUNIT_CASE(cft_test_lazy_busy),
	KUNIT_CASE(cft_test_lazy_pending_same),
	KUNIT_CASE(cft_test_lazy_unexpected),
	KUNIT_CASE(cft_test_lazy_verify_fails),
	KUNIT_CASE(cft_test_lazy_range),
	KUNIT_CASE(cft_test_lazy_fast_switch),
	KUNIT_CASE(cft_test_lazy_cap),
	KUNIT_CASE(cft_test_lazy_cap_waited),
	KUNIT_CASE(cft_test_drain),
	KUNIT_CASE(cft_test_set_target_lazy),
	KUNIT_CASE(cft_test_suspend_lazy),
	KUNIT_CASE(cft_test_resume),
	KUNIT_CASE(cft_test_unmanaged_max),
	KUNIT_CASE(cft_test_fast_die_fit),
	KUNIT_CASE(cft_test_fast_die_limit),
	KUNIT_CASE(cft_test_fast_die_interval),
	KUNIT_CASE(cft_test_fast_die_arm),
	KUNIT_CASE(cft_test_fast_die_cap),
	KUNIT_CASE(cft_test_fast_die_cap_unverified),
	KUNIT_CASE(cft_test_fast_die_init_none),
	KUNIT_CASE(cft_test_fast_die_init_required),
	KUNIT_CASE(cft_test_fast_die_init_described),
	KUNIT_CASE(cft_test_fast_die_init_invalid),
	{}
};

static struct kunit_suite apple_soc_cpufreq_test_suite = {
	.name = "apple_soc_cpufreq",
	.exit = cft_exit,
	.test_cases = apple_soc_cpufreq_test_cases,
};

/*
 * Tests on a policy of the cpufreq core
 *
 * ->get() finds its policy through the core, and the thermal ownership call
 * takes the policy lock and checks which driver is registered. The suite
 * registers a driver under this driver's name whose policies carry a cluster
 * as above; its ->target_index() does nothing, so that the governor of the
 * policy never touches the cluster the tests work on.
 */

static u8 *cft_live_regs;
static struct apple_cpu_priv *cft_live_priv;
static struct cpufreq_frequency_table cft_live_table[CFT_NR_STATES + 1];
static int cft_live_ret = -ENODEV;

static int cft_live_init(struct cpufreq_policy *policy)
{
	policy->driver_data = cft_live_priv;
	policy->freq_table = cft_live_table;
	policy->cur = cft_live_table[0].frequency;
	return 0;
}

static void cft_live_exit(struct cpufreq_policy *policy)
{
	policy->driver_data = NULL;
}

static int cft_live_target(struct cpufreq_policy *policy, unsigned int index)
{
	return 0;
}

static void (*cft_live_after_fast_switch)(struct cpufreq_policy *policy);

static unsigned int cft_live_fast_switch(struct cpufreq_policy *policy,
					 unsigned int target_freq)
{
	unsigned int freq = apple_soc_cpufreq_fast_switch(policy, target_freq);

	if (cft_live_after_fast_switch) {
		void (*hook)(struct cpufreq_policy *policy) = cft_live_after_fast_switch;

		cft_live_after_fast_switch = NULL;
		hook(policy);
	}
	return freq;
}

static struct cpufreq_driver cft_live_driver = {
	.name		= "apple-cpufreq",
	.flags		= CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
			  CPUFREQ_NEED_UPDATE_LIMITS |
			  CPUFREQ_DRIVER_UPDATES_CUR,
	.verify		= apple_soc_cpufreq_verify_policy,
	.init		= cft_live_init,
	.exit		= cft_live_exit,
	.target_index	= cft_live_target,
	.fast_switch	= cft_live_fast_switch,
};

static int cft_live_suite_init(struct kunit_suite *suite)
{
	cft_live_regs = kzalloc(CFT_REGS_SIZE, GFP_KERNEL);
	cft_live_priv = kzalloc_obj(*cft_live_priv);
	if (!cft_live_regs || !cft_live_priv) {
		cft_live_ret = -ENOMEM;
		return 0;
	}

	memcpy(cft_live_table, cft_table, sizeof(cft_table));
	cft_live_priv->cpu_dev = get_cpu_device(cpumask_first(cpu_online_mask));
	cft_priv_init(cft_live_priv, cft_live_regs, &soc_default_info, 4);

	cft_live_ret = cpufreq_register_driver(&cft_live_driver);
	return 0;
}

static void cft_live_suite_exit(struct kunit_suite *suite)
{
	if (!cft_live_ret)
		cpufreq_unregister_driver(&cft_live_driver);
	cft_live_ret = -ENODEV;
	kfree(cft_live_priv);
	kfree(cft_live_regs);
	cft_live_priv = NULL;
	cft_live_regs = NULL;
}

static int cft_live_test_init(struct kunit *test)
{
	if (cft_live_ret == -EEXIST)
		kunit_skip(test, "a cpufreq driver is already registered");
	KUNIT_ASSERT_EQ(test, cft_live_ret, 0);
	KUNIT_ASSERT_STREQ(test, cft_live_driver.name, apple_soc_cpufreq_driver.name);
	return 0;
}

static void cft_live_test_exit(struct kunit *test)
{
	if (cft_live_ret)
		return;
	cft_live_after_fast_switch = NULL;
	cft_live_priv->info = &soc_default_info;
	cft_live_priv->transition_failed = false;
	cft_live_priv->failure_unreported = false;
	cft_set_cmd(cft_live_regs, 4);
	cft_set_status(cft_live_regs, 0);
}

struct cft_get {
	const char *name;
	const struct apple_soc_cpufreq_info *info;
	u32 status;
	u64 cmd;
	unsigned int freq;
};

/*
 * The current state sits in the status register at a place that moved from
 * SoC to SoC; every other bit of the register is set, the target state next
 * to it included. Without a known status layout the last request stands in.
 */
static const struct cft_get cft_gets[] = {
	{ "s5l8960x status bits 5:3", &soc_s5l8960x_info,
	  ~(u32)GENMASK(5, 3) | (4 << 3), ~0ULL, 1200000 },
	{ "t8103 status bits 7:4", &soc_t8103_info,
	  ~(u32)GENMASK(7, 4) | (10 << 4), ~0ULL, 2400000 },
	{ "t8112 status bits 9:5", &soc_t8112_info,
	  ~(u32)GENMASK(9, 5) | (15 << 5), ~0ULL, 3000000 },
	{ "t8112 lowest state", &soc_t8112_info,
	  ~(u32)GENMASK(9, 5) | (1 << 5), ~0ULL, 600000 },
	{ "generic command state", &soc_default_info,
	  ~0U, ~GENMASK_ULL(4, 0) | 7, 1800000 },
	{ "t8132 command state", &soc_t8132_info,
	  0, ~GENMASK_ULL(4, 0) | 10, 2400000 },
	{ "t8140 command state", &soc_t8140_info,
	  ~0U, 15, 3000000 },
	/* States without a frequency-table entry have no frequency. */
	{ "t8103 unknown state", &soc_t8103_info,
	  ~(u32)GENMASK(7, 4) | (5 << 4), 4, 0 },
	{ "t8112 state beyond the table", &soc_t8112_info, ~0U, 4, 0 },
	{ "t8112 state 0", &soc_t8112_info, 0, 4, 0 },
	{ "generic state beyond the table", &soc_default_info, 4 << 4, ~0ULL, 0 },
};

static void cft_get_desc(const struct cft_get *g, char *desc)
{
	strscpy(desc, g->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(cft_get, cft_gets, cft_get_desc);

static void cft_test_get(struct kunit *test)
{
	const struct cft_get *g = test->param_value;

	cft_live_priv->info = g->info;
	cft_set_status(cft_live_regs, g->status);
	cft_set_cmd(cft_live_regs, g->cmd);
	KUNIT_EXPECT_EQ(test,
			apple_soc_cpufreq_get_rate(cpumask_first(cpu_online_mask)),
			g->freq);
}

/* The thermal driver takes the higher states and gives them back. */
static void cft_test_thermal_owner(struct kunit *test)
{
	struct apple_cpu_priv *priv = cft_live_priv;
	struct cpufreq_policy *policy;
	int ret;

	policy = cpufreq_cpu_get(cpumask_first(cpu_online_mask));
	KUNIT_ASSERT_NOT_NULL(test, policy);
	priv->info = &soc_t8132_info;

	/* A cluster that is not held back has nothing to hand over. */
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_thermal_owner(policy, true),
			-ENODEV);
	KUNIT_EXPECT_FALSE(test, priv->thermal_owned);

	/* As apple_soc_cpufreq_init() holds it back. */
	mutex_lock(&apple_soc_thermal_owner_lock);
	ret = freq_qos_add_request(&policy->constraints, &priv->unmanaged_cap,
				   FREQ_QOS_MAX, apple_soc_cpufreq_owner_cap(policy));
	mutex_unlock(&apple_soc_thermal_owner_lock);
	KUNIT_EXPECT_GE(test, ret, 0);
	if (ret < 0)
		goto out;
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&policy->constraints, FREQ_QOS_MAX),
			1200000);

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_thermal_owner(policy, true), 0);
	KUNIT_EXPECT_TRUE(test, priv->thermal_owned);
	KUNIT_EXPECT_GE(test, freq_qos_read_value(&policy->constraints, FREQ_QOS_MAX),
			3000000);

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_thermal_owner(policy, true), 0);
	KUNIT_EXPECT_TRUE(test, priv->thermal_owned);

	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_set_thermal_owner(policy, false), 0);
	KUNIT_EXPECT_FALSE(test, priv->thermal_owned);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&policy->constraints, FREQ_QOS_MAX),
			1200000);

	freq_qos_remove_request(&priv->unmanaged_cap);
out:
	priv->thermal_owned = false;
	cpufreq_cpu_put(policy);
}

struct cft_cap_lift_ctx {
	struct cpufreq_policy *policy;
	struct apple_cpu_priv *priv;
	bool gov_stopped;
};

static void cft_cap_lift_cleanup(void *data)
{
	struct cft_cap_lift_ctx *ctx = data;
	struct cpufreq_policy *policy = ctx->policy;
	struct apple_cpu_priv *priv = ctx->priv;

	hrtimer_cancel(&priv->verify_timer);
	scoped_guard(cpufreq_policy_write, policy) {
		policy->suspend_freq = 0;
		policy->fast_switch_enabled = false;
		priv->policy = NULL;
		priv->policy_bounds_valid = false;
		priv->waited = true;
		if (ctx->gov_stopped)
			cpufreq_start_governor(policy);
	}
	cpufreq_cpu_put(policy);
}

static void cft_remove_qos_req(void *data)
{
	struct freq_qos_request *req = data;

	if (freq_qos_request_active(req))
		freq_qos_remove_request(req);
}

static struct cpufreq_policy *cft_live_policy_setup(struct kunit *test)
{
	unsigned int cpu = cpumask_first(cpu_online_mask);
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	struct apple_cpu_priv *priv = cft_live_priv;
	struct cft_cap_lift_ctx *ctx;

	KUNIT_ASSERT_NOT_NULL(test, policy);
	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		cpufreq_cpu_put(policy);
		KUNIT_FAIL_AND_ABORT(test, "failed to allocate test context");
	}
	ctx->policy = policy;
	ctx->priv = priv;
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, cft_cap_lift_cleanup,
						  ctx),
			0);
	/*
	 * Stop the live governor before enabling fast_switch_enabled on the
	 * shared policy so scheduler utilization callbacks cannot race this
	 * test over cft_live_regs or priv state.
	 */
	scoped_guard(cpufreq_policy_write, policy) {
		cpufreq_stop_governor(policy);
		ctx->gov_stopped = true;
	}
	return policy;
}

static void cft_live_add_qos(struct kunit *test, struct cpufreq_policy *policy,
			     struct freq_qos_request **req_out,
			     enum freq_qos_req_type type, s32 value)
{
	struct freq_qos_request *req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, req);
	KUNIT_ASSERT_GE(test, freq_qos_add_request(&policy->constraints, req,
						   type, value),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, cft_remove_qos_req, req),
			0);
	*req_out = req;
}

static void cft_live_reset_t6050(struct cpufreq_policy *policy)
{
	struct apple_cpu_priv *priv = cft_live_priv;
	unsigned long max_freq = arch_scale_freq_ref(policy->cpu) ?:
				 policy->cpuinfo.max_freq;

	priv->info = &soc_t6050_info;
	priv->policy = policy;
	priv->policy_bounds_valid = false;
	priv->cap_pstate = soc_t6050_info.max_pstate;
	priv->expected_pstate = 1;
	priv->pending_pstate = -1;
	priv->transition_failed = false;
	priv->failure_unreported = false;
	priv->waited = true;
	cft_set_cmd(cft_live_regs, 1);

	policy->min = cft_live_table[0].frequency;
	policy->max = cft_live_table[CFT_NR_STATES - 1].frequency;
	policy->cur = cft_live_table[0].frequency;
	cpufreq_stats_record_transition(policy, policy->cur);
	if (max_freq && cpumask_available(policy->related_cpus) &&
	    !cpumask_empty(policy->related_cpus))
		arch_set_freq_scale(policy->related_cpus, policy->cur, max_freq);
	policy->fast_switch_enabled = true;
}

/*
 * A QoS ceiling drop below a high accepted P-state must clamp immediately when
 * the policy worker runs, even with no pending request and no scheduler
 * utilization callback.
 */
static void cft_test_verify_policy_ceiling_drop(struct kunit *test)
{
	struct cpufreq_policy *policy = cft_live_policy_setup(test);
	struct apple_cpu_priv *priv = cft_live_priv;
	struct freq_qos_request *user_req, *min_req;
	unsigned int freq;

	cft_live_add_qos(test, policy, &user_req, FREQ_QOS_MAX, 3000000);
	cft_live_add_qos(test, policy, &min_req, FREQ_QOS_MIN, 600000);
	flush_work(&policy->update);
	cft_live_reset_t6050(policy);

	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 3000000U);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 15U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	cft_set_cmd(cft_live_regs, 15);

	/* Lower the QoS ceiling and flush policy->update without any fast_switch(). */
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 1200000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, policy->max, 1200000U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 4ULL);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);

	/* Dropping the ceiling to 0 clamps the accepted P-state to the lowest OPP. */
	cft_set_cmd(cft_live_regs, 4);
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 0), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, policy->max, 600000U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 1ULL);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 1U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, policy->cur, 600000U);

	/* Lifting the ceiling again leaves the cluster until the governor requests higher. */
	cft_set_cmd(cft_live_regs, 1);
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, policy->max, 3000000U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 1ULL);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 1U);
}

static void cft_test_verify_policy_cap_lift(struct kunit *test)
{
	struct cpufreq_policy *policy = cft_live_policy_setup(test);
	struct apple_cpu_priv *priv = cft_live_priv;
	struct freq_qos_request *thermal_req, *user_req, *min_req;
	unsigned long flags;
	unsigned int freq;
	int moved;

	KUNIT_EXPECT_TRUE(test,
			  apple_soc_cpufreq_driver.flags & CPUFREQ_NEED_UPDATE_LIMITS);
	cft_live_add_qos(test, policy, &thermal_req, FREQ_QOS_MAX, 3000000);
	cft_live_add_qos(test, policy, &user_req, FREQ_QOS_MAX, 3000000);
	cft_live_add_qos(test, policy, &min_req, FREQ_QOS_MIN, 600000);
	flush_work(&policy->update);
	cft_live_reset_t6050(policy);

	/* Schedutil asks for 3 GHz (pstate 15) through the core fast-switch wrapper. */
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 3000000U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 15ULL);

	/*
	 * Fast-die drops to 1.2 GHz while a separate userspace ceiling stays at
	 * 1.8 GHz. Before policy->update runs, policy->max still reads 3 GHz:
	 * a core fast-switch for 3 GHz is clamped to 1.2 GHz by the live QoS
	 * aggregate while asked_pstate stays at 15.
	 */
	scoped_guard(cpufreq_policy_write, policy) {
		KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 1800000), 0);
		KUNIT_ASSERT_GE(test, freq_qos_update_request(thermal_req, 1200000), 0);
		KUNIT_EXPECT_EQ(test, policy->max, 3000000U);
		local_irq_save(flags);
		moved = apple_soc_cpufreq_lazy_cap(priv, 4, 1200000);
		hrtimer_cancel(&priv->verify_timer);
		local_irq_restore(flags);
		KUNIT_EXPECT_EQ(test, moved, 4);
		KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 4ULL);

		freq = cpufreq_driver_fast_switch(policy, 3000000);
		hrtimer_cancel(&priv->verify_timer);
		KUNIT_EXPECT_EQ(test, freq, 1200000U);
		KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
		KUNIT_EXPECT_EQ(test, priv->asked_pstate, 15U);
	}
	flush_work(&policy->update);
	KUNIT_EXPECT_EQ(test, policy->max, 1200000U);

	/*
	 * On a tickless cluster where no fast_switch() ran after policy->max
	 * reconciled, lifting the thermal cap restores immediately to the
	 * 1.8 GHz userspace ceiling (pstate 7) once the QoS request lifts,
	 * even while policy->max still reads 1.2 GHz.
	 */
	scoped_guard(cpufreq_policy_write, policy) {
		KUNIT_ASSERT_GE(test, freq_qos_update_request(thermal_req, 3000000), 0);
		KUNIT_EXPECT_EQ(test, policy->max, 1200000U);
		local_irq_save(flags);
		moved = apple_soc_cpufreq_lazy_cap(priv, 15, 3000000);
		hrtimer_cancel(&priv->verify_timer);
		local_irq_restore(flags);
		KUNIT_EXPECT_EQ(test, moved, 7);
		KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 7ULL);
		KUNIT_EXPECT_EQ(test, policy->cur, 1800000U);
	}
	flush_work(&policy->update);
	KUNIT_EXPECT_EQ(test, policy->max, 1800000U);

	/*
	 * Once policy->max has reconciled to 1.2 GHz, a core fast-switch for
	 * 3 GHz passes 1.2 GHz to the driver and overwrites asked_pstate to 4,
	 * so lifting the thermal cap stays at 1.2 GHz until schedutil runs
	 * after policy->update reconciles policy->max back to 1.8 GHz.
	 */
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, priv->asked_pstate, 15U);

	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 1800000), 0);
	KUNIT_ASSERT_GE(test, freq_qos_update_request(thermal_req, 1200000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, policy->max, 1200000U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 4ULL);

	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	KUNIT_EXPECT_EQ(test, priv->asked_pstate, 4U);

	KUNIT_ASSERT_GE(test, freq_qos_update_request(thermal_req, 3000000), 0);
	local_irq_save(flags);
	moved = apple_soc_cpufreq_lazy_cap(priv, 15, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(test, moved, -1);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 4ULL);
	flush_work(&policy->update);
	KUNIT_EXPECT_EQ(test, policy->max, 1800000U);
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1800000U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 7ULL);

	/*
	 * Maximum-first OPP resolution: QoS min=max=1.5 GHz resolves max down
	 * to 1.2 GHz (pstate 4) instead of promoting 1.2 GHz up to 1.8 GHz,
	 * and a ceiling below the lowest OPP clamps to the lowest OPP (600 MHz).
	 */
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 1500000), 0);
	KUNIT_ASSERT_GE(test, freq_qos_update_request(min_req, 1500000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	raw_spin_lock_irqsave(&priv->lock, flags);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_clamp_pstate(priv, 4), 4U);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_clamp_pstate(priv, 15), 4U);
	raw_spin_unlock_irqrestore(&priv->lock, flags);

	KUNIT_ASSERT_GE(test, freq_qos_update_request(min_req, 600000), 0);
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 400000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	raw_spin_lock_irqsave(&priv->lock, flags);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_clamp_pstate(priv, 15), 1U);
	raw_spin_unlock_irqrestore(&priv->lock, flags);

	/* Lifting the userspace ceiling restores full 3 GHz on the next fast-switch. */
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	KUNIT_EXPECT_EQ(test, policy->max, 3000000U);
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 3000000U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 15ULL);

	/*
	 * Dropping the QoS ceiling to 0 (as store_scaling_max_freq("0") does)
	 * must clamp a queued pending fast request to the lowest valid OPP and
	 * publish policy->cur when the verify timer replays it, so policy->cur
	 * is already 600 MHz after policy->update runs with no further
	 * fast_switch().
	 */
	cft_set_cmd(cft_live_regs, 4 | APPLE_DVFS_CMD_BUSY);
	priv->expected_pstate = 4;
	policy->cur = 1200000;
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 3000000U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 15);

	scoped_guard(cpufreq_policy_write, policy) {
		KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 0), 0);
		KUNIT_EXPECT_EQ(test, policy->max, 3000000U);
		cft_set_cmd(cft_live_regs, 4);
		local_irq_save(flags);
		KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_verify(&priv->verify_timer),
				HRTIMER_NORESTART);
		hrtimer_cancel(&priv->verify_timer);
		local_irq_restore(flags);
		KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
		KUNIT_EXPECT_EQ(test, priv->expected_pstate, 1U);
		KUNIT_EXPECT_EQ(test,
				FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)),
				1ULL);
		KUNIT_EXPECT_EQ(test, policy->cur, 600000U);
	}
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, policy->max, 600000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 600000U);

	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	KUNIT_EXPECT_EQ(test, policy->max, 3000000U);

	/* Same replay when hardware was already at P-state 1: cur must still drop. */
	cft_set_cmd(cft_live_regs, 1 | APPLE_DVFS_CMD_BUSY);
	priv->expected_pstate = 1;
	policy->cur = 600000;
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 3000000U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 15);

	scoped_guard(cpufreq_policy_write, policy) {
		KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 0), 0);
		cft_set_cmd(cft_live_regs, 1);
		local_irq_save(flags);
		KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_verify(&priv->verify_timer),
				HRTIMER_NORESTART);
		hrtimer_cancel(&priv->verify_timer);
		local_irq_restore(flags);
		KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
		KUNIT_EXPECT_EQ(test, priv->expected_pstate, 1U);
		KUNIT_EXPECT_EQ(test, policy->cur, 600000U);
	}
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, policy->max, 600000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 600000U);

	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	KUNIT_EXPECT_EQ(test, policy->max, 3000000U);

	/*
	 * Suspend must enforce the live ceiling even when suspend_freq is set
	 * and policy->min is still stale at 3 GHz.
	 */
	KUNIT_ASSERT_GE(test, freq_qos_update_request(thermal_req, 1200000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	policy->min = 3000000;
	policy->suspend_freq = 3000000;
	priv->pending_pstate = 15;
	priv->cap_pstate = 4;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_suspend(policy), 0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 4ULL);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
}

static struct {
	struct kunit *test;
	struct freq_qos_request *user_req;
} cft_race_ctx;

static void cft_race_verify_after_unlock(struct cpufreq_policy *policy)
{
	struct apple_cpu_priv *priv = policy->driver_data;

	KUNIT_EXPECT_EQ(cft_race_ctx.test, policy->cur, 3000000U);
	KUNIT_EXPECT_EQ(cft_race_ctx.test, priv->expected_pstate, 15U);
	cft_set_cmd(cft_live_regs, 15);
	KUNIT_ASSERT_GE(cft_race_ctx.test,
			freq_qos_update_request(cft_race_ctx.user_req, 1200000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(cft_race_ctx.test, policy->cur, 1200000U);
}

static void cft_race_cap_after_unlock(struct cpufreq_policy *policy)
{
	struct apple_cpu_priv *priv = policy->driver_data;
	unsigned long flags;
	int moved;

	KUNIT_EXPECT_EQ(cft_race_ctx.test, policy->cur, 3000000U);
	KUNIT_EXPECT_EQ(cft_race_ctx.test, priv->expected_pstate, 15U);
	cft_set_cmd(cft_live_regs, 15);
	local_irq_save(flags);
	moved = apple_soc_cpufreq_lazy_cap(priv, 4, 1200000);
	hrtimer_cancel(&priv->verify_timer);
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(cft_race_ctx.test, moved, 4);
	KUNIT_EXPECT_EQ(cft_race_ctx.test, policy->cur, 1200000U);
}

/*
 * Reconciliation (from policy->update or lazy_cap) running after ->fast_switch()
 * drops priv->lock and before cpufreq_driver_fast_switch() finishes must not
 * have its lower policy->cur overwritten by the stale fast-switch return value.
 */
static void cft_test_fast_switch_reconcile_race(struct kunit *test)
{
	struct cpufreq_policy *policy = cft_live_policy_setup(test);
	struct apple_cpu_priv *priv = cft_live_priv;
	struct freq_qos_request *user_req, *min_req;
	unsigned int freq;

	cft_live_add_qos(test, policy, &user_req, FREQ_QOS_MAX, 3000000);
	cft_live_add_qos(test, policy, &min_req, FREQ_QOS_MIN, 600000);
	flush_work(&policy->update);
	cft_live_reset_t6050(policy);

	cft_race_ctx.test = test;
	cft_race_ctx.user_req = user_req;
	cft_live_after_fast_switch = cft_race_verify_after_unlock;
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	KUNIT_EXPECT_EQ(test, policy->max, 1200000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 4ULL);

	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	cft_set_cmd(cft_live_regs, 4);
	cft_live_after_fast_switch = cft_race_cap_after_unlock;
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 4ULL);
}

static void cft_read_stats_attr(struct kunit *test, struct cpufreq_policy *policy,
				const char *name, char *buf)
{
	struct kernfs_node *stats_kn, *attr_kn;
	ssize_t len;

	stats_kn = kernfs_find_and_get(policy->kobj.sd, "stats");
	KUNIT_ASSERT_NOT_NULL(test, stats_kn);
	attr_kn = kernfs_find_and_get(stats_kn, name);
	kernfs_put(stats_kn);
	KUNIT_ASSERT_NOT_NULL(test, attr_kn);
	KUNIT_ASSERT_NOT_NULL(test, policy->kobj.ktype);
	KUNIT_ASSERT_NOT_NULL(test, policy->kobj.ktype->sysfs_ops);
	KUNIT_ASSERT_NOT_NULL(test, policy->kobj.ktype->sysfs_ops->show);

	len = policy->kobj.ktype->sysfs_ops->show(&policy->kobj, attr_kn->priv, buf);
	kernfs_put(attr_kn);
	KUNIT_ASSERT_GT(test, len, 0);
}

static unsigned int cft_stats_total_trans(struct kunit *test,
					  struct cpufreq_policy *policy)
{
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	unsigned int total;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	cft_read_stats_attr(test, policy, "total_trans", buf);
	ret = kstrtouint(strim(buf), 10, &total);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return total;
}

static unsigned int cft_stats_trans_count(struct kunit *test,
					  struct cpufreq_policy *policy,
					  unsigned int from_freq,
					  unsigned int to_freq)
{
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	char *p, *line;
	int to_col = -1;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	cft_read_stats_attr(test, policy, "trans_table", buf);

	p = buf;
	while ((line = strsep(&p, "\n")) != NULL) {
		char *colon = strchr(line, ':');
		char *s;
		unsigned int row_freq;
		int col = 0;

		if (!colon)
			continue;
		*colon = '\0';
		s = skip_spaces(line);
		if (!*s) {
			s = colon + 1;
			while (*(s = skip_spaces(s))) {
				char *tok = strsep(&s, " ");
				unsigned int col_freq;
				int ret;

				ret = kstrtouint(tok, 10, &col_freq);
				KUNIT_ASSERT_EQ(test, ret, 0);
				if (col_freq == to_freq)
					to_col = col;
				col++;
				if (!s)
					break;
			}
			continue;
		}

		if (kstrtouint(strim(s), 10, &row_freq) || row_freq != from_freq)
			continue;

		KUNIT_ASSERT_GE(test, to_col, 0);
		s = colon + 1;
		while (*(s = skip_spaces(s))) {
			char *tok = strsep(&s, " ");
			unsigned int count;
			int ret;

			ret = kstrtouint(tok, 10, &count);
			KUNIT_ASSERT_EQ(test, ret, 0);
			if (col == to_col)
				return count;
			col++;
			if (!s)
				break;
		}
		KUNIT_FAIL(test, "missing column %d for %u->%u", to_col,
			   from_freq, to_freq);
		return 0;
	}

	KUNIT_FAIL(test, "missing row %u in trans_table", from_freq);
	return 0;
}

static u64 cft_stats_time_in_state(struct kunit *test,
				   struct cpufreq_policy *policy,
				   unsigned int freq)
{
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	char *p, *line;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	cft_read_stats_attr(test, policy, "time_in_state", buf);

	p = buf;
	while ((line = strsep(&p, "\n")) != NULL) {
		unsigned int row_freq;
		u64 ticks;

		if (!*line)
			continue;
		if (sscanf(line, "%u %llu", &row_freq, &ticks) == 2 &&
		    row_freq == freq)
			return ticks;
	}

	KUNIT_FAIL(test, "missing %u in time_in_state", freq);
	return 0;
}

static void cft_expect_freq_scale(struct kunit *test,
				  struct cpufreq_policy *policy,
				  unsigned int freq)
{
#ifdef arch_scale_freq_capacity
	unsigned long max_freq = arch_scale_freq_ref(policy->cpu) ?:
				 policy->cpuinfo.max_freq;

	if (max_freq)
		KUNIT_EXPECT_EQ(test,
				arch_scale_freq_capacity(policy->cpu),
				((unsigned long)freq << SCHED_CAPACITY_SHIFT) /
				max_freq);
#endif
}

/*
 * Standalone QoS clamps, pending-request replays, and lazy_cap() updates must
 * record cpufreq_stats transitions, residency, and frequency-invariance scale
 * immediately, and an older fast_switch() completing after a concurrent clamp
 * must not overwrite stats or scheduler freq scale.
 */
static void cft_test_stats_clamp_and_race(struct kunit *test)
{
	struct cpufreq_policy *policy;
	struct apple_cpu_priv *priv = cft_live_priv;
	struct freq_qos_request *user_req, *min_req;
	unsigned int total, c_600_3000, c_3000_1200, c_1200_3000, c_1200_600;
	unsigned int c_3000_600;
	unsigned long flags;
	unsigned int freq;
	u64 t3000_before, t3000_after, t1200_before, t1200_after;
	int moved;

	if (!IS_ENABLED(CONFIG_CPU_FREQ_STAT))
		kunit_skip(test, "CONFIG_CPU_FREQ_STAT is disabled");

	policy = cft_live_policy_setup(test);
	cft_live_add_qos(test, policy, &user_req, FREQ_QOS_MAX, 3000000);
	cft_live_add_qos(test, policy, &min_req, FREQ_QOS_MIN, 600000);
	flush_work(&policy->update);
	cft_live_reset_t6050(policy);

	/* Standalone QoS ceiling drop (3 GHz -> 1.2 GHz) updates stats and residency. */
	total = cft_stats_total_trans(test, policy);
	c_600_3000 = cft_stats_trans_count(test, policy, 600000, 3000000);
	c_3000_1200 = cft_stats_trans_count(test, policy, 3000000, 1200000);

	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	cft_expect_freq_scale(test, policy, 3000000);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 600000, 3000000),
			c_600_3000 + 1);
	cft_set_cmd(cft_live_regs, 15);

	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 1200000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total + 2);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 3000000, 1200000),
			c_3000_1200 + 1);

	t3000_before = cft_stats_time_in_state(test, policy, 3000000);
	t1200_before = cft_stats_time_in_state(test, policy, 1200000);
	msleep(25);
	t3000_after = cft_stats_time_in_state(test, policy, 3000000);
	t1200_after = cft_stats_time_in_state(test, policy, 1200000);
	KUNIT_EXPECT_EQ(test, t3000_after, t3000_before);
	KUNIT_EXPECT_GT(test, t1200_after, t1200_before);

	/*
	 * Busy request defers stats until hardware accepts the clamped replay:
	 * queueing 3 GHz behind busy 1.2 GHz leaves stats on 1.2 GHz, and
	 * clamped replay to 600 MHz records 1.2 GHz -> 600 MHz.
	 */
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	cft_set_cmd(cft_live_regs, 4 | APPLE_DVFS_CMD_BUSY);
	priv->expected_pstate = 4;
	total = cft_stats_total_trans(test, policy);
	c_1200_3000 = cft_stats_trans_count(test, policy, 1200000, 3000000);
	c_1200_600 = cft_stats_trans_count(test, policy, 1200000, 600000);
	c_3000_600 = cft_stats_trans_count(test, policy, 3000000, 600000);

	freq = cpufreq_driver_fast_switch(policy, 3000000);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 3000000U);
	cft_expect_freq_scale(test, policy, 1200000);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 3000000),
			c_1200_3000);

	scoped_guard(cpufreq_policy_write, policy) {
		KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 0), 0);
		cft_set_cmd(cft_live_regs, 4);
		local_irq_save(flags);
		KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_verify(&priv->verify_timer),
				HRTIMER_NORESTART);
		hrtimer_cancel(&priv->verify_timer);
		local_irq_restore(flags);
		KUNIT_EXPECT_EQ(test, policy->cur, 600000U);
		cft_expect_freq_scale(test, policy, 600000);
	}
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 600000),
			c_1200_600 + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 3000000, 600000),
			c_3000_600);

	/* Standalone lazy_cap() clamp (3 GHz -> 1.2 GHz) records transition. */
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	cft_set_cmd(cft_live_regs, 15);
	total = cft_stats_total_trans(test, policy);
	c_3000_1200 = cft_stats_trans_count(test, policy, 3000000, 1200000);

	local_irq_save(flags);
	moved = apple_soc_cpufreq_lazy_cap(priv, 4, 1200000);
	hrtimer_cancel(&priv->verify_timer);
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(test, moved, 4);
	cft_expect_freq_scale(test, policy, 1200000);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 3000000, 1200000),
			c_3000_1200 + 1);
	priv->cap_pstate = soc_t6050_info.max_pstate;

	/*
	 * Concurrent policy->update or lazy_cap() clamp after priv->lock unlock
	 * must not have its stats overwritten by the older fast_switch().
	 */
	freq = cpufreq_driver_fast_switch(policy, 600000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 600000U);
	cft_set_cmd(cft_live_regs, 1);

	total = cft_stats_total_trans(test, policy);
	c_600_3000 = cft_stats_trans_count(test, policy, 600000, 3000000);
	c_3000_1200 = cft_stats_trans_count(test, policy, 3000000, 1200000);
	c_1200_3000 = cft_stats_trans_count(test, policy, 1200000, 3000000);

	cft_race_ctx.test = test;
	cft_race_ctx.user_req = user_req;
	cft_live_after_fast_switch = cft_race_verify_after_unlock;
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total + 2);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 600000, 3000000),
			c_600_3000 + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 3000000, 1200000),
			c_3000_1200 + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 3000000),
			c_1200_3000);

	t3000_before = cft_stats_time_in_state(test, policy, 3000000);
	t1200_before = cft_stats_time_in_state(test, policy, 1200000);
	msleep(25);
	KUNIT_EXPECT_EQ(test, cft_stats_time_in_state(test, policy, 3000000),
			t3000_before);
	KUNIT_EXPECT_GT(test, cft_stats_time_in_state(test, policy, 1200000),
			t1200_before);

	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	freq = cpufreq_driver_fast_switch(policy, 600000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 600000U);
	cft_set_cmd(cft_live_regs, 1);

	total = cft_stats_total_trans(test, policy);
	c_600_3000 = cft_stats_trans_count(test, policy, 600000, 3000000);
	c_3000_1200 = cft_stats_trans_count(test, policy, 3000000, 1200000);
	c_1200_3000 = cft_stats_trans_count(test, policy, 1200000, 3000000);

	cft_live_after_fast_switch = cft_race_cap_after_unlock;
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total + 2);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 600000, 3000000),
			c_600_3000 + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 3000000, 1200000),
			c_3000_1200 + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 3000000),
			c_1200_3000);
}

/*
 * Busy DVFS during fast_switch(), policy->update, or lazy_cap() must not
 * record cpufreq_stats until hardware accepts the P-state; if busy persists
 * past the verify deadline and drops the pending request, stats must remain on
 * the frequency hardware actually entered.
 */
static void cft_test_stats_busy_then_discard(struct kunit *test)
{
	struct cpufreq_policy *policy;
	struct apple_cpu_priv *priv = cft_live_priv;
	struct freq_qos_request *user_req, *min_req;
	unsigned int total, c_1200_3000, c_1200_1800, c_1200_600;
	u64 t1200_before, t1800_before, t3000_before, t600_before;
	unsigned long flags;
	unsigned int freq;
	int moved;

	if (!IS_ENABLED(CONFIG_CPU_FREQ_STAT))
		kunit_skip(test, "CONFIG_CPU_FREQ_STAT is disabled");

	policy = cft_live_policy_setup(test);
	cft_live_add_qos(test, policy, &user_req, FREQ_QOS_MAX, 3000000);
	cft_live_add_qos(test, policy, &min_req, FREQ_QOS_MIN, 600000);
	flush_work(&policy->update);
	cft_live_reset_t6050(policy);

	/* Put hardware cleanly at 1.2 GHz (pstate 4). */
	freq = cpufreq_driver_fast_switch(policy, 1200000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	cft_set_cmd(cft_live_regs, 4);

	/*
	 * Busy during fast_switch() and policy->update clamp, then deadline
	 * expiry drops the queued request: stats stay on 1.2 GHz.
	 */
	cft_set_cmd(cft_live_regs, 4 | APPLE_DVFS_CMD_BUSY);
	total = cft_stats_total_trans(test, policy);
	c_1200_3000 = cft_stats_trans_count(test, policy, 1200000, 3000000);
	c_1200_1800 = cft_stats_trans_count(test, policy, 1200000, 1800000);
	t1200_before = cft_stats_time_in_state(test, policy, 1200000);
	t1800_before = cft_stats_time_in_state(test, policy, 1800000);
	t3000_before = cft_stats_time_in_state(test, policy, 3000000);

	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 15);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 3000000),
			c_1200_3000);

	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 1800000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 7);
	KUNIT_EXPECT_EQ(test, policy->cur, 1800000U);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 1800000),
			c_1200_1800);

	priv->verify_deadline = ktime_sub_us(ktime_get(), 1);
	local_irq_save(flags);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_verify(&priv->verify_timer),
			HRTIMER_NORESTART);
	hrtimer_cancel(&priv->verify_timer);
	local_irq_restore(flags);
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);

	msleep(25);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 3000000),
			c_1200_3000);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 1800000),
			c_1200_1800);
	KUNIT_EXPECT_GT(test, cft_stats_time_in_state(test, policy, 1200000),
			t1200_before);
	KUNIT_EXPECT_EQ(test, cft_stats_time_in_state(test, policy, 1800000),
			t1800_before);
	KUNIT_EXPECT_EQ(test, cft_stats_time_in_state(test, policy, 3000000),
			t3000_before);

	/*
	 * Busy during lazy_cap(), then deadline expiry drops the pending cap:
	 * stats also stay on 1.2 GHz and never charge 600 MHz.
	 */
	cft_live_reset_t6050(policy);
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	freq = cpufreq_driver_fast_switch(policy, 1200000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	cft_set_cmd(cft_live_regs, 4 | APPLE_DVFS_CMD_BUSY);

	total = cft_stats_total_trans(test, policy);
	c_1200_600 = cft_stats_trans_count(test, policy, 1200000, 600000);
	t1200_before = cft_stats_time_in_state(test, policy, 1200000);
	t600_before = cft_stats_time_in_state(test, policy, 600000);

	local_irq_save(flags);
	moved = apple_soc_cpufreq_lazy_cap(priv, 1, 600000);
	hrtimer_cancel(&priv->verify_timer);
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(test, moved, 1);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 1);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 600000),
			c_1200_600);

	priv->verify_deadline = ktime_sub_us(ktime_get(), 1);
	local_irq_save(flags);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_verify(&priv->verify_timer),
			HRTIMER_NORESTART);
	hrtimer_cancel(&priv->verify_timer);
	local_irq_restore(flags);
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);

	msleep(25);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 600000),
			c_1200_600);
	KUNIT_EXPECT_GT(test, cft_stats_time_in_state(test, policy, 1200000),
			t1200_before);
	KUNIT_EXPECT_EQ(test, cft_stats_time_in_state(test, policy, 600000),
			t600_before);

	/*
	 * When DVFS clears BUSY and the timer replay writes 3 GHz, stats
	 * record 1.2 GHz -> 3 GHz and start charging 3 GHz residency then.
	 */
	cft_live_reset_t6050(policy);
	freq = cpufreq_driver_fast_switch(policy, 1200000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	cft_set_cmd(cft_live_regs, 4 | APPLE_DVFS_CMD_BUSY);

	total = cft_stats_total_trans(test, policy);
	c_1200_3000 = cft_stats_trans_count(test, policy, 1200000, 3000000);
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 15);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);

	cft_set_cmd(cft_live_regs, 4);
	local_irq_save(flags);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_verify(&priv->verify_timer),
			HRTIMER_NORESTART);
	hrtimer_cancel(&priv->verify_timer);
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 15U);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 3000000),
			c_1200_3000 + 1);

	t1200_before = cft_stats_time_in_state(test, policy, 1200000);
	t3000_before = cft_stats_time_in_state(test, policy, 3000000);
	msleep(25);
	KUNIT_EXPECT_EQ(test, cft_stats_time_in_state(test, policy, 1200000),
			t1200_before);
	KUNIT_EXPECT_GT(test, cft_stats_time_in_state(test, policy, 3000000),
			t3000_before);
	cft_expect_freq_scale(test, policy, 3000000);
}

struct cft_freq_trace {
	unsigned int cpu;
	unsigned int count;
	unsigned int last_freq;
};

static void cft_trace_cpu_freq(void *data, unsigned int freq,
			       unsigned int cpu_id)
{
	struct cft_freq_trace *t = data;

	if (cpu_id != t->cpu)
		return;
	t->last_freq = freq;
	t->count++;
}

static void cft_unregister_freq_trace(void *data)
{
	unregister_trace_cpu_frequency(cft_trace_cpu_freq, data);
	tracepoint_synchronize_unregister();
}

/*
 * Queueing a fast-switch request behind BUSY and then hitting a failed suspend
 * toward suspend_freq must restore policy->cur to the accepted hardware state
 * and keep cpufreq_stats residency/transitions, FIE scale, and trace events
 * strictly on that state; a subsequent successful suspend must update
 * policy->cur, stats, FIE scale, and emit power:cpu_frequency once.
 */
static void cft_test_stats_busy_suspend_fail(struct kunit *test)
{
	struct cpufreq_policy *policy;
	struct apple_cpu_priv *priv = cft_live_priv;
	struct freq_qos_request *user_req, *min_req;
	struct cft_freq_trace *trace_ctx;
	unsigned int total, c_1200_3000, c_1200_600, c_600_3000, c_600_1200;
	unsigned int c_3000_600;
	u64 t1200_before, t3000_before, t600_before;
	unsigned int freq;

	if (!IS_ENABLED(CONFIG_CPU_FREQ_STAT))
		kunit_skip(test, "CONFIG_CPU_FREQ_STAT is disabled");

	policy = cft_live_policy_setup(test);
	cft_live_add_qos(test, policy, &user_req, FREQ_QOS_MAX, 3000000);
	cft_live_add_qos(test, policy, &min_req, FREQ_QOS_MIN, 600000);
	flush_work(&policy->update);
	cft_live_reset_t6050(policy);

	trace_ctx = kunit_kzalloc(test, sizeof(*trace_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, trace_ctx);
	if (IS_ENABLED(CONFIG_TRACEPOINTS)) {
		trace_ctx->cpu = policy->cpu;
		KUNIT_ASSERT_EQ(test,
				register_trace_cpu_frequency(cft_trace_cpu_freq,
							     trace_ctx),
				0);
		KUNIT_ASSERT_EQ(test,
				kunit_add_action_or_reset(test,
							  cft_unregister_freq_trace,
							  trace_ctx),
				0);
	}

	/* Put hardware cleanly at 1.2 GHz (pstate 4). */
	freq = cpufreq_driver_fast_switch(policy, 1200000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);
	cft_set_cmd(cft_live_regs, 4);

	total = cft_stats_total_trans(test, policy);
	c_1200_3000 = cft_stats_trans_count(test, policy, 1200000, 3000000);
	c_1200_600 = cft_stats_trans_count(test, policy, 1200000, 600000);
	c_600_3000 = cft_stats_trans_count(test, policy, 600000, 3000000);
	c_600_1200 = cft_stats_trans_count(test, policy, 600000, 1200000);
	c_3000_600 = cft_stats_trans_count(test, policy, 3000000, 600000);
	t1200_before = cft_stats_time_in_state(test, policy, 1200000);
	t3000_before = cft_stats_time_in_state(test, policy, 3000000);
	t600_before = cft_stats_time_in_state(test, policy, 600000);

	/* Queue 3 GHz behind BUSY; policy->cur is optimistic while stats stay at 1.2 GHz. */
	cft_set_cmd(cft_live_regs, 4 | APPLE_DVFS_CMD_BUSY);
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 3000000U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 15);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
	cft_expect_freq_scale(test, policy, 1200000);

	/*
	 * Suspend toward 600 MHz while BUSY persists: suspend fails without
	 * charging 3 GHz or 600 MHz or emitting power:cpu_frequency.
	 */
	trace_ctx->count = 0;
	trace_ctx->last_freq = 0;
	policy->suspend_freq = 600000;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_suspend(policy), -EIO);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);
	if (IS_ENABLED(CONFIG_TRACEPOINTS))
		KUNIT_EXPECT_EQ(test, trace_ctx->count, 0U);

	msleep(25);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 3000000),
			c_1200_3000);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 600000),
			c_1200_600);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 600000, 3000000),
			c_600_3000);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 600000, 1200000),
			c_600_1200);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 3000000, 600000),
			c_3000_600);
	KUNIT_EXPECT_GT(test, cft_stats_time_in_state(test, policy, 1200000),
			t1200_before);
	KUNIT_EXPECT_EQ(test, cft_stats_time_in_state(test, policy, 3000000),
			t3000_before);
	KUNIT_EXPECT_EQ(test, cft_stats_time_in_state(test, policy, 600000),
			t600_before);

	/*
	 * When DVFS clears BUSY before suspend runs, suspend drops the queued
	 * 3 GHz request and transitions directly from 1.2 GHz to 600 MHz,
	 * emitting power:cpu_frequency once.
	 */
	priv->transition_failed = false;
	cft_set_cmd(cft_live_regs, 4 | APPLE_DVFS_CMD_BUSY);
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 15);

	trace_ctx->count = 0;
	trace_ctx->last_freq = 0;
	cft_set_cmd(cft_live_regs, 4);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_suspend(policy), 0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_DVFS_CMD_PS1, cft_cmd(cft_live_regs)), 1ULL);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 1U);
	KUNIT_EXPECT_EQ(test, policy->cur, 600000U);
	cft_expect_freq_scale(test, policy, 600000);
	if (IS_ENABLED(CONFIG_TRACEPOINTS)) {
		KUNIT_EXPECT_EQ(test, trace_ctx->count, 1U);
		KUNIT_EXPECT_EQ(test, trace_ctx->last_freq, 600000U);
	}
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 600000),
			c_1200_600 + 1);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 3000000),
			c_1200_3000);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 3000000, 600000),
			c_3000_600);

	/* Suspending at the already-settled frequency emits no extra trace or transition. */
	trace_ctx->count = 0;
	trace_ctx->last_freq = 0;
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_suspend(policy), 0);
	if (IS_ENABLED(CONFIG_TRACEPOINTS))
		KUNIT_EXPECT_EQ(test, trace_ctx->count, 0U);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total + 1);
}

static void cft_live_queue_3g_at_1200(struct kunit *test,
				      struct cpufreq_policy *policy)
{
	struct apple_cpu_priv *priv = cft_live_priv;
	unsigned int freq;

	cft_live_reset_t6050(policy);
	freq = cpufreq_driver_fast_switch(policy, 1200000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 1200000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);

	cft_set_cmd(cft_live_regs, 4 | APPLE_DVFS_CMD_BUSY);
	freq = cpufreq_driver_fast_switch(policy, 3000000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 3000000U);
	KUNIT_EXPECT_EQ(test, policy->cur, 3000000U);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, 15);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
}

/*
 * Queueing a fast-switch request behind BUSY and then hitting an idle register
 * with an unexpected P-state on fast_switch(), lazy_cap(), policy->update, or
 * timer replay must restore policy->cur to expected_pstate and keep
 * cpufreq_stats and scheduler freq_scale strictly on that accepted state.
 */
static void cft_test_stats_idle_mismatch_discard(struct kunit *test)
{
	struct cpufreq_policy *policy;
	struct apple_cpu_priv *priv = cft_live_priv;
	struct freq_qos_request *user_req, *min_req;
	unsigned int total, c_1200_3000, c_1200_1800;
	u64 t1200_before, t1800_before, t3000_before;
	unsigned long flags;
	unsigned int freq;
	int moved;

	if (!IS_ENABLED(CONFIG_CPU_FREQ_STAT))
		kunit_skip(test, "CONFIG_CPU_FREQ_STAT is disabled");

	policy = cft_live_policy_setup(test);
	cft_live_add_qos(test, policy, &user_req, FREQ_QOS_MAX, 3000000);
	cft_live_add_qos(test, policy, &min_req, FREQ_QOS_MIN, 600000);
	flush_work(&policy->update);

	/* 1. Idle mismatch on follow-up fast_switch() after queueing 3 GHz. */
	cft_live_queue_3g_at_1200(test, policy);
	total = cft_stats_total_trans(test, policy);
	c_1200_3000 = cft_stats_trans_count(test, policy, 1200000, 3000000);
	c_1200_1800 = cft_stats_trans_count(test, policy, 1200000, 1800000);
	t1200_before = cft_stats_time_in_state(test, policy, 1200000);
	t1800_before = cft_stats_time_in_state(test, policy, 1800000);
	t3000_before = cft_stats_time_in_state(test, policy, 3000000);

	cft_set_cmd(cft_live_regs, 9);
	freq = cpufreq_driver_fast_switch(policy, 1800000);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_EQ(test, freq, 0U);
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);

	msleep(25);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 3000000),
			c_1200_3000);
	KUNIT_EXPECT_EQ(test, cft_stats_trans_count(test, policy, 1200000, 1800000),
			c_1200_1800);
	KUNIT_EXPECT_GT(test, cft_stats_time_in_state(test, policy, 1200000),
			t1200_before);
	KUNIT_EXPECT_EQ(test, cft_stats_time_in_state(test, policy, 1800000),
			t1800_before);
	KUNIT_EXPECT_EQ(test, cft_stats_time_in_state(test, policy, 3000000),
			t3000_before);

	/* 2. Idle mismatch on lazy_cap() after queueing 3 GHz. */
	cft_live_queue_3g_at_1200(test, policy);
	total = cft_stats_total_trans(test, policy);
	cft_set_cmd(cft_live_regs, 9);
	local_irq_save(flags);
	moved = apple_soc_cpufreq_lazy_cap(priv, 7, 1800000);
	hrtimer_cancel(&priv->verify_timer);
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(test, moved, -1);
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);

	/* 3. Idle mismatch on policy->update ceiling drop after queueing 3 GHz. */
	cft_live_queue_3g_at_1200(test, policy);
	total = cft_stats_total_trans(test, policy);
	cft_set_cmd(cft_live_regs, 9);
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 1800000), 0);
	flush_work(&policy->update);
	hrtimer_cancel(&priv->verify_timer);
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);

	/* 4. Idle mismatch on verify timer replay after queueing 3 GHz. */
	KUNIT_ASSERT_GE(test, freq_qos_update_request(user_req, 3000000), 0);
	flush_work(&policy->update);
	cft_live_queue_3g_at_1200(test, policy);
	total = cft_stats_total_trans(test, policy);
	cft_set_cmd(cft_live_regs, 9);
	local_irq_save(flags);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_verify(&priv->verify_timer),
			HRTIMER_NORESTART);
	hrtimer_cancel(&priv->verify_timer);
	local_irq_restore(flags);
	KUNIT_EXPECT_TRUE(test, priv->transition_failed);
	KUNIT_EXPECT_EQ(test, priv->pending_pstate, -1);
	KUNIT_EXPECT_EQ(test, priv->expected_pstate, 4U);
	KUNIT_EXPECT_EQ(test, policy->cur, 1200000U);
	cft_expect_freq_scale(test, policy, 1200000);
	KUNIT_EXPECT_EQ(test, cft_stats_total_trans(test, policy), total);
}

static struct kunit_case apple_soc_cpufreq_policy_test_cases[] = {
	KUNIT_CASE_PARAM(cft_test_get, cft_get_gen_params),
	KUNIT_CASE(cft_test_thermal_owner),
	KUNIT_CASE(cft_test_verify_policy_ceiling_drop),
	KUNIT_CASE(cft_test_verify_policy_cap_lift),
	KUNIT_CASE(cft_test_fast_switch_reconcile_race),
	KUNIT_CASE(cft_test_stats_clamp_and_race),
	KUNIT_CASE(cft_test_stats_busy_then_discard),
	KUNIT_CASE(cft_test_stats_busy_suspend_fail),
	KUNIT_CASE(cft_test_stats_idle_mismatch_discard),
	{}
};

static struct kunit_suite apple_soc_cpufreq_policy_test_suite = {
	.name = "apple_soc_cpufreq_policy",
	.suite_init = cft_live_suite_init,
	.suite_exit = cft_live_suite_exit,
	.init = cft_live_test_init,
	.exit = cft_live_test_exit,
	.test_cases = apple_soc_cpufreq_policy_test_cases,
};

kunit_test_suites(&apple_soc_cpufreq_test_suite,
		  &apple_soc_cpufreq_policy_test_suite);

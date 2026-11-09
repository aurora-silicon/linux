// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by pmgr-pwrstate.c: power transitions and apple,no-auto-pm.
 * The emulated register can delay or withhold the parent and actual ACKs.
 */
#include <kunit/device.h>
#include <kunit/test.h>

struct pmgr_ps_test_reg {
	u32 val;
	bool stuck;		/* the actual state no longer follows the target */
	unsigned int active_reads;	/* reads before a delayed power-on ACK */
	unsigned int parent_reads;	/* reads before the parent becomes available */
	bool parent_stuck;
	bool parent_read_error;
	unsigned int busy_writes;
	unsigned int auto_writes;	/* writes that enabled automatic PM */
	bool latched;		/* bit 11 latched: on stops at clock-gated, off still reaches 0 */
	bool latch_sticky;	/* not even a PS reset clears it */
	unsigned int reset_writes;	/* writes with the PS reset bit */
};

static int pmgr_ps_test_read(void *ctx, unsigned int reg, unsigned int *val)
{
	struct pmgr_ps_test_reg *r = ctx;

	if (FIELD_GET(APPLE_PMGR_PS_TARGET, r->val) != APPLE_PMGR_PS_ACTIVE)
		goto out;
	if (r->parent_read_error)
		return -EIO;
	if (!r->parent_stuck && r->parent_reads && !--r->parent_reads)
		r->val &= ~APPLE_PMGR_BUSY;
	if (!r->stuck && !(r->val & APPLE_PMGR_BUSY) &&
	    (!r->active_reads || !--r->active_reads)) {
		r->val &= ~APPLE_PMGR_PS_ACTUAL;
		r->val |= FIELD_PREP(APPLE_PMGR_PS_ACTUAL, APPLE_PMGR_PS_ACTIVE);
	}
out:
	*val = r->val;
	return 0;
}

static int pmgr_ps_test_write(void *ctx, unsigned int reg, unsigned int val)
{
	struct pmgr_ps_test_reg *r = ctx;
	u32 parent = r->val & APPLE_PMGR_BUSY;
	u32 actual;

	if (val & APPLE_PMGR_PS_RESET) {
		r->reset_writes++;
		if (r->latched && !r->latch_sticky) {
			r->latched = false;
			parent = 0;
		}
	}
	if (r->latched) {
		actual = FIELD_GET(APPLE_PMGR_PS_TARGET, val) ? APPLE_PMGR_PS_CLKGATE : 0;
		r->val = (val & ~(APPLE_PMGR_PS_ACTUAL | APPLE_PMGR_BUSY)) | APPLE_PMGR_BUSY |
			 FIELD_PREP(APPLE_PMGR_PS_ACTUAL, actual);
		return 0;
	}
	if (FIELD_GET(APPLE_PMGR_PS_TARGET, val) == APPLE_PMGR_PS_ACTIVE &&
	    (r->parent_stuck || r->parent_reads))
		parent = APPLE_PMGR_BUSY;
	actual = r->stuck || r->active_reads || parent ?
		 FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val) : FIELD_GET(APPLE_PMGR_PS_TARGET, val);

	if (val & APPLE_PMGR_BUSY)
		r->busy_writes++;
	if (val & APPLE_PMGR_AUTO_ENABLE)
		r->auto_writes++;
	r->val = (val & ~(APPLE_PMGR_PS_ACTUAL | APPLE_PMGR_BUSY)) | parent |
		 FIELD_PREP(APPLE_PMGR_PS_ACTUAL, actual);
	return 0;
}

static const struct regmap_config pmgr_ps_test_regmap = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.reg_read = pmgr_ps_test_read,
	.reg_write = pmgr_ps_test_write,
	.fast_io = true,
};

/* A domain in power state @pstate, actual and target, on a device of its own */
static struct apple_pmgr_ps *pmgr_ps_test_domain(struct kunit *test, u32 pstate,
						 bool no_auto_pm, struct pmgr_ps_test_reg **reg)
{
	static const char *const names[] = { "apple-pmgr-ps-auto", "apple-pmgr-ps-no-auto" };
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;
	struct device *dev;

	r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r);
	r->val = FIELD_PREP(APPLE_PMGR_PS_TARGET, pstate) |
		 FIELD_PREP(APPLE_PMGR_PS_ACTUAL, pstate);
	dev = kunit_device_register(test, names[no_auto_pm]);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	ps = kunit_kzalloc(test, sizeof(*ps), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ps);
	ps->dev = dev;
	ps->genpd.name = "test";
	ps->no_auto_pm = no_auto_pm;
	ps->regmap = devm_regmap_init(dev, NULL, r, &pmgr_ps_test_regmap);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ps->regmap);
	*reg = r;
	return ps;
}

/* Powering on enables automatic PM, unless the domain is apple,no-auto-pm. */
static void pmgr_ps_power_on_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;
	bool no_auto_pm;

	for (no_auto_pm = false; ; no_auto_pm = true) {
		ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_PWRGATE, no_auto_pm, &r);
		KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), 0);
		KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_TARGET, r->val),
				APPLE_PMGR_PS_ACTIVE);
		KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val),
				APPLE_PMGR_PS_ACTIVE);
		KUNIT_EXPECT_EQ_MSG(test, !!(r->val & APPLE_PMGR_AUTO_ENABLE), !no_auto_pm,
				    "no_auto_pm %d", no_auto_pm);
		KUNIT_EXPECT_EQ_MSG(test, r->auto_writes, no_auto_pm ? 0U : 1U,
				    "no_auto_pm %d", no_auto_pm);
		if (no_auto_pm)
			break;
	}
}

/*
 * A power-off that does not complete is undone: the domain is requested on
 * again, with automatic PM only when the domain allows it.
 */
static void pmgr_ps_power_off_undo_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;
	bool no_auto_pm;

	for (no_auto_pm = false; ; no_auto_pm = true) {
		ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_ACTIVE, no_auto_pm, &r);
		r->stuck = true;
		KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_off(&ps->genpd), -ETIMEDOUT);
		KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_TARGET, r->val),
				APPLE_PMGR_PS_ACTIVE);
		KUNIT_EXPECT_EQ_MSG(test, !!(r->val & APPLE_PMGR_AUTO_ENABLE), !no_auto_pm,
				    "no_auto_pm %d", no_auto_pm);
		KUNIT_EXPECT_EQ_MSG(test, r->auto_writes, no_auto_pm ? 0U : 1U,
				    "no_auto_pm %d", no_auto_pm);
		if (no_auto_pm)
			break;
	}
}

/* A power-on that does not complete never enabled automatic PM on such a domain. */
static void pmgr_ps_power_on_fails_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_PWRGATE, true, &r);
	r->stuck = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	/* Undone: requested off again */
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_TARGET, r->val), APPLE_PMGR_PS_PWRGATE);
	KUNIT_EXPECT_EQ(test, r->auto_writes, 0U);
}

/* Only the missing-parent phase gets more than 100 us, followed by an actual ACK. */
static void pmgr_ps_power_on_parent_delayed_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_CLKGATE, false, &r);
	ps->wait_parent = true;
	r->parent_reads = 250;
	r->active_reads = 2;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), 0);
	KUNIT_EXPECT_EQ(test, r->active_reads, 0U);
	KUNIT_EXPECT_EQ(test, r->parent_reads, 0U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val), APPLE_PMGR_PS_ACTIVE);
	KUNIT_EXPECT_EQ(test, r->auto_writes, 1U);
	KUNIT_EXPECT_EQ(test, r->busy_writes, 0U);
}

/* Ordinary domains retain the 100 us budget, even with a missing parent. */
static void pmgr_ps_power_on_other_domain_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_CLKGATE, true, &r);
	ps->system_sleep = true;
	r->parent_reads = 1000;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	KUNIT_EXPECT_GT(test, r->parent_reads, 0U);
}

/* With no missing parent, even GFX must acknowledge within the usual budget. */
static void pmgr_ps_power_on_actual_delayed_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_CLKGATE, true, &r);
	ps->wait_parent = true;
	ps->system_sleep = true;
	r->active_reads = 1000;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	KUNIT_EXPECT_GT(test, r->active_reads, 0U);
}

/* An unavailable parent is a bounded failure, including during noirq resume. */
static void pmgr_ps_power_on_parent_stuck_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_CLKGATE, true, &r);
	ps->wait_parent = true;
	ps->system_sleep = true;
	r->parent_stuck = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, r->val & APPLE_PMGR_BUSY);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val), APPLE_PMGR_PS_CLKGATE);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_TARGET, r->val), APPLE_PMGR_PS_ACTIVE);
	KUNIT_EXPECT_EQ(test, r->busy_writes, 0U);
	KUNIT_EXPECT_EQ(test, r->auto_writes, 0U);
}

/* Parent availability alone is not an actual-state acknowledgment. */
static void pmgr_ps_power_on_parent_only_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_CLKGATE, true, &r);
	ps->wait_parent = true;
	ps->system_sleep = true;
	r->parent_reads = 250;
	r->stuck = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, r->parent_reads, 0U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val), APPLE_PMGR_PS_CLKGATE);
}

static void pmgr_ps_power_on_parent_read_error_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_CLKGATE, true, &r);
	ps->wait_parent = true;
	ps->system_sleep = true;
	r->parent_read_error = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -EIO);
	KUNIT_EXPECT_EQ(test, r->auto_writes, 0U);
}

/* genpd ignores noirq power-on errors: retain the on request, but fail. */
static void pmgr_ps_power_on_sleep_stuck_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_CLKGATE, false, &r);
	ps->system_sleep = true;
	r->stuck = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val), APPLE_PMGR_PS_CLKGATE);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_TARGET, r->val), APPLE_PMGR_PS_ACTIVE);
}

/*
 * A latched bit 11 (T8140 ANE_SYS after sleep: 0x2b4f) is cleared by a PS reset
 * while the domain is off, and the power-on then completes.
 */
static void pmgr_ps_power_on_latch_cleared_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_PWRGATE, false, &r);
	r->latched = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), 0);
	KUNIT_EXPECT_EQ(test, r->reset_writes, 1U);
	KUNIT_EXPECT_FALSE(test, r->val & (APPLE_PMGR_BUSY | APPLE_PMGR_PS_RESET));
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_TARGET, r->val), APPLE_PMGR_PS_ACTIVE);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val), APPLE_PMGR_PS_ACTIVE);
	KUNIT_EXPECT_EQ(test, r->auto_writes, 1U);
}

/*
 * Not in the noirq phase of a system sleep (no PS reset there: it hangs the
 * T8140). The power-on fails, which genpd ignores in that phase, and the
 * domain stays requested on.
 */
static void pmgr_ps_power_on_latch_sleep_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_PWRGATE, true, &r);
	ps->system_sleep = true;
	r->latched = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, r->reset_writes, 0U);
	KUNIT_EXPECT_FALSE(test, r->val & APPLE_PMGR_PS_RESET);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_TARGET, r->val), APPLE_PMGR_PS_ACTIVE);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val), APPLE_PMGR_PS_CLKGATE);
	KUNIT_EXPECT_EQ(test, r->auto_writes, 0U);

	/* The same domain after the sleep: the recovery runs again. */
	ps->system_sleep = false;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), 0);
	KUNIT_EXPECT_EQ(test, r->reset_writes, 1U);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val), APPLE_PMGR_PS_ACTIVE);
}

/* A latch the reset does not clear still fails, and the domain is requested off. */
static void pmgr_ps_power_on_latch_sticky_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_PWRGATE, true, &r);
	r->latched = true;
	r->latch_sticky = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, r->reset_writes, 1U);
	KUNIT_EXPECT_FALSE(test, r->val & APPLE_PMGR_PS_RESET);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_TARGET, r->val), APPLE_PMGR_PS_PWRGATE);
	KUNIT_EXPECT_EQ(test, r->auto_writes, 0U);
}

/* No PS reset where bit 11 means a parent not up yet, nor on a plain stall. */
static void pmgr_ps_power_on_no_reset_test(struct kunit *test)
{
	struct pmgr_ps_test_reg *r;
	struct apple_pmgr_ps *ps;

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_PWRGATE, false, &r);
	ps->wait_parent = true;
	r->latched = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, r->reset_writes, 0U);

	ps = pmgr_ps_test_domain(test, APPLE_PMGR_PS_PWRGATE, true, &r);
	r->stuck = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_power_on(&ps->genpd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, r->reset_writes, 0U);
}

/*
 * Consumer-switched states: a state word at offset 0 whose actual state
 * follows the target (the history bits are set by a power-gate and cleared
 * by writing them), retention registers as plain words, and a write log.
 */
struct pmgr_ps_test_bank {
	u32 word[0x30000 / 4];
	bool stuck;		/* the actual state no longer follows the target */
	bool ignore_retention;	/* retention writes are dropped */
	unsigned int nr_log;
	struct { u32 reg, val; } log[16];
};

static int pmgr_ps_test_bank_read(void *ctx, unsigned int reg, unsigned int *val)
{
	struct pmgr_ps_test_bank *b = ctx;

	*val = b->word[reg / 4];
	return 0;
}

static int pmgr_ps_test_bank_write(void *ctx, unsigned int reg, unsigned int val)
{
	struct pmgr_ps_test_bank *b = ctx;
	u32 old = b->word[reg / 4], target, cleared;

	if (b->nr_log < ARRAY_SIZE(b->log)) {
		b->log[b->nr_log].reg = reg;
		b->log[b->nr_log++].val = val;
	}
	if (reg) {
		if (!b->ignore_retention)
			b->word[reg / 4] = val;
		return 0;
	}
	target = FIELD_GET(APPLE_PMGR_PS_TARGET, val);
	cleared = val & APPLE_PMGR_FLAGS;	/* written as 1: cleared */
	val &= ~(APPLE_PMGR_PS_ACTUAL | APPLE_PMGR_FLAGS);
	val |= old & APPLE_PMGR_FLAGS & ~cleared;
	val |= FIELD_PREP(APPLE_PMGR_PS_ACTUAL,
			  b->stuck ? FIELD_GET(APPLE_PMGR_PS_ACTUAL, old) : target);
	if (!b->stuck && target == APPLE_PMGR_PS_PWRGATE)
		val |= APPLE_PMGR_FLAGS;
	b->word[0] = val;
	return 0;
}

static const struct regmap_config pmgr_ps_test_bank_regmap = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x30000 - 4,
	.reg_read = pmgr_ps_test_bank_read,
	.reg_write = pmgr_ps_test_bank_write,
	.fast_io = true,
};

/* The J713 DCP CPU state: AUTO on, with its two retention workaround words. */
static struct apple_pmgr_ps *pmgr_ps_test_consumer(struct kunit *test, u32 word,
						   struct pmgr_ps_test_bank **bank)
{
	static atomic_t instance = ATOMIC_INIT(0);
	struct pmgr_ps_test_bank *b;
	struct apple_pmgr_ps *ps;
	struct device *dev;
	char name[32];

	b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b);
	b->word[0] = word;
	b->word[0x2504c / 4] = 0;
	b->word[0x25070 / 4] = 1;
	snprintf(name, sizeof(name), "apple-pmgr-consumer-%d", atomic_inc_return(&instance));
	dev = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	ps = kunit_kzalloc(test, sizeof(*ps), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ps);
	ps->dev = dev;
	ps->genpd.name = "disp-cpu";
	ps->firmware_owned = true;
	ps->consumer_switched = true;
	ps->nr_retention = 2;
	ps->retention[0].offset = 0x2504c;
	ps->retention[0].mask = 3;
	ps->retention[0].value = 3;
	ps->retention[1].offset = 0x25070;
	ps->retention[1].mask = 1;
	ps->retention[1].value = 0;
	ps->regmap = devm_regmap_init(dev, NULL, b, &pmgr_ps_test_bank_regmap);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ps->regmap);
	*bank = b;
	return ps;
}

static void pmgr_ps_test_expect_log(struct kunit *test, struct pmgr_ps_test_bank *b,
				    const u32 (*want)[2], unsigned int n)
{
	KUNIT_ASSERT_EQ(test, b->nr_log, n);
	for (unsigned int i = 0; i < n; i++) {
		KUNIT_EXPECT_EQ_MSG(test, b->log[i].reg, want[i][0], "write %u", i);
		KUNIT_EXPECT_EQ_MSG(test, b->log[i].val, want[i][1], "write %u", i);
	}
	b->nr_log = 0;
}

/* The native J713 DCP CPU writes, as captured on macOS, in order. */
static void pmgr_ps_consumer_native_test(struct kunit *test)
{
	static const u32 off[][2] = {
		{ 0, 0x0f0000ff }, { 0x2504c, 3 }, { 0x25070, 0 }, { 0, 0x0f0003f0 },
	};
	static const u32 on[][2] = {
		{ 0, 0x0f00000f }, { 0x2504c, 0 }, { 0x25070, 1 }, { 0, 0x1f00000f },
	};
	struct pmgr_ps_test_bank *b;
	struct apple_pmgr_ps *ps = pmgr_ps_test_consumer(test, 0x1f0003ff, &b);

	KUNIT_ASSERT_EQ(test, apple_pmgr_ps_consumer_state_off(ps), 0);
	pmgr_ps_test_expect_log(test, b, off, ARRAY_SIZE(off));
	KUNIT_EXPECT_EQ(test, b->word[0] & (APPLE_PMGR_PS_TARGET | APPLE_PMGR_PS_ACTUAL), 0U);
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_consumer_state_off(ps), -EALREADY);

	KUNIT_ASSERT_EQ(test, apple_pmgr_ps_consumer_state_on(ps), 0);
	pmgr_ps_test_expect_log(test, b, on, ARRAY_SIZE(on));
	KUNIT_EXPECT_EQ(test, b->word[0] & ~APPLE_PMGR_FLAGS, 0x1f0000ffU);
	KUNIT_EXPECT_EQ(test, b->word[0x2504c / 4], 0U);
	KUNIT_EXPECT_EQ(test, b->word[0x25070 / 4], 1U);
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_consumer_state_on(ps), -EALREADY);
}

/* Retention bits outside the workaround's mask that change while off are kept. */
static void pmgr_ps_consumer_retention_mask_test(struct kunit *test)
{
	struct pmgr_ps_test_bank *b;
	struct apple_pmgr_ps *ps = pmgr_ps_test_consumer(test, 0x1f0003ff, &b);

	KUNIT_ASSERT_EQ(test, apple_pmgr_ps_consumer_state_off(ps), 0);
	b->word[0x2504c / 4] |= 0x100;
	b->word[0x25070 / 4] |= 0x10;
	KUNIT_ASSERT_EQ(test, apple_pmgr_ps_consumer_state_on(ps), 0);
	KUNIT_EXPECT_EQ(test, b->word[0x2504c / 4], 0x100U);
	KUNIT_EXPECT_EQ(test, b->word[0x25070 / 4], 0x11U);
}

/* FE and SYS: no AUTO and no retention; other bits are kept. */
static void pmgr_ps_consumer_plain_test(struct kunit *test)
{
	static const u32 off[][2] = { { 0, 0x23f0 } };
	static const u32 on[][2] = { { 0, 0x200f } };
	struct pmgr_ps_test_bank *b;
	struct apple_pmgr_ps *ps = pmgr_ps_test_consumer(test, 0x20ff, &b);

	ps->nr_retention = 0;
	KUNIT_ASSERT_EQ(test, apple_pmgr_ps_consumer_state_off(ps), 0);
	pmgr_ps_test_expect_log(test, b, off, ARRAY_SIZE(off));
	KUNIT_ASSERT_EQ(test, apple_pmgr_ps_consumer_state_on(ps), 0);
	pmgr_ps_test_expect_log(test, b, on, ARRAY_SIZE(on));
}

static void pmgr_ps_consumer_refused_test(struct kunit *test)
{
	struct pmgr_ps_test_bank *b;
	struct apple_pmgr_ps *ps;

	/* Not fully on: left alone, and not failed. */
	ps = pmgr_ps_test_consumer(test, 0x1f000044, &b);
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_consumer_state_off(ps), -EBUSY);
	KUNIT_EXPECT_EQ(test, b->nr_log, 0U);
	KUNIT_EXPECT_FALSE(test, ps->consumer_failed);
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_consumer_state_on(ps), -EALREADY);

	/* The actual state does not follow: failed, and stays failed. */
	ps = pmgr_ps_test_consumer(test, 0x1f0003ff, &b);
	b->stuck = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_consumer_state_off(ps), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, ps->consumer_failed);
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_consumer_state_on(ps), -EIO);

	/* A retention register that does not take the value: failed before power-off. */
	ps = pmgr_ps_test_consumer(test, 0x1f0003ff, &b);
	b->ignore_retention = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_ps_consumer_state_off(ps), -EIO);
	KUNIT_EXPECT_TRUE(test, ps->consumer_failed);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_PMGR_PS_TARGET, b->word[0]),
			APPLE_PMGR_PS_ACTIVE);
}

static struct kunit_case pmgr_ps_test_cases[] = {
	KUNIT_CASE(pmgr_ps_power_on_test),
	KUNIT_CASE(pmgr_ps_power_off_undo_test),
	KUNIT_CASE(pmgr_ps_power_on_fails_test),
	KUNIT_CASE(pmgr_ps_power_on_parent_delayed_test),
	KUNIT_CASE(pmgr_ps_power_on_other_domain_test),
	KUNIT_CASE(pmgr_ps_power_on_actual_delayed_test),
	KUNIT_CASE(pmgr_ps_power_on_parent_stuck_test),
	KUNIT_CASE(pmgr_ps_power_on_parent_only_test),
	KUNIT_CASE(pmgr_ps_power_on_parent_read_error_test),
	KUNIT_CASE(pmgr_ps_power_on_sleep_stuck_test),
	KUNIT_CASE(pmgr_ps_power_on_latch_cleared_test),
	KUNIT_CASE(pmgr_ps_power_on_latch_sleep_test),
	KUNIT_CASE(pmgr_ps_power_on_latch_sticky_test),
	KUNIT_CASE(pmgr_ps_power_on_no_reset_test),
	KUNIT_CASE(pmgr_ps_consumer_native_test),
	KUNIT_CASE(pmgr_ps_consumer_retention_mask_test),
	KUNIT_CASE(pmgr_ps_consumer_plain_test),
	KUNIT_CASE(pmgr_ps_consumer_refused_test),
	{}
};

static struct kunit_suite pmgr_ps_test_suite = {
	.name = "apple-pmgr-pwrstate",
	.test_cases = pmgr_ps_test_cases,
};

kunit_test_suite(pmgr_ps_test_suite);

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
	{}
};

static struct kunit_suite pmgr_ps_test_suite = {
	.name = "apple-pmgr-pwrstate",
	.test_cases = pmgr_ps_test_cases,
};

kunit_test_suite(pmgr_ps_test_suite);

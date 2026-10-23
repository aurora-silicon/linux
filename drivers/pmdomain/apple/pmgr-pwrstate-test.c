// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by pmgr-pwrstate.c: apple,no-auto-pm on the power-on and
 * power-off paths. The power state register is emulated: its actual state
 * follows the target written, unless the test holds it where it is.
 */
#include <kunit/device.h>
#include <kunit/test.h>

struct pmgr_ps_test_reg {
	u32 val;
	bool stuck;		/* the actual state no longer follows the target */
	unsigned int auto_writes;	/* writes that enabled automatic PM */
};

static int pmgr_ps_test_read(void *ctx, unsigned int reg, unsigned int *val)
{
	struct pmgr_ps_test_reg *r = ctx;

	*val = r->val;
	return 0;
}

static int pmgr_ps_test_write(void *ctx, unsigned int reg, unsigned int val)
{
	struct pmgr_ps_test_reg *r = ctx;
	u32 actual = r->stuck ? FIELD_GET(APPLE_PMGR_PS_ACTUAL, r->val) :
				FIELD_GET(APPLE_PMGR_PS_TARGET, val);

	if (val & APPLE_PMGR_AUTO_ENABLE)
		r->auto_writes++;
	r->val = (val & ~APPLE_PMGR_PS_ACTUAL) | FIELD_PREP(APPLE_PMGR_PS_ACTUAL, actual);
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

static struct kunit_case pmgr_ps_test_cases[] = {
	KUNIT_CASE(pmgr_ps_power_on_test),
	KUNIT_CASE(pmgr_ps_power_off_undo_test),
	KUNIT_CASE(pmgr_ps_power_on_fails_test),
	{}
};

static struct kunit_suite pmgr_ps_test_suite = {
	.name = "apple-pmgr-pwrstate",
	.test_cases = pmgr_ps_test_cases,
};

kunit_test_suite(pmgr_ps_test_suite);

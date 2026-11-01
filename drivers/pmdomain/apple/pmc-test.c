// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple PMC voter interface: agent enables and the
 * AP's DCS vote, with memory standing in for the registers.
 *
 * Included by pmc.c.
 */

#include <kunit/device.h>
#include <kunit/test.h>

#define APPLE_PMC_TEST_RAILS_SIZE	0x4000

static struct apple_pmc *apple_pmc_test_alloc(struct kunit *test)
{
	const struct apple_pmc_hw *hw = &apple_pmc_hw_t8140;
	struct apple_pmc *pmc;

	pmc = kunit_kzalloc(test, sizeof(*pmc), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pmc);
	pmc->dev = kunit_device_register(test, "apple-pmc-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmc->dev);
	pmc->hw = hw;
	raw_spin_lock_init(&pmc->lock);
	pmc->voters = (void __iomem *)kunit_kzalloc(test,
						    hw->agents * hw->agent_stride,
						    GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, (void __force *)pmc->voters);
	pmc->rails = (void __iomem *)kunit_kzalloc(test,
						   APPLE_PMC_TEST_RAILS_SIZE,
						   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, (void __force *)pmc->rails);

	return pmc;
}

static void apple_pmc_test_set_dcs_vote(struct apple_pmc *pmc,
					unsigned int agent, unsigned int state)
{
	void __iomem *reg = pmc->voters + agent * pmc->hw->agent_stride;

	writel(state << APPLE_PMC_RAIL_SHIFT(pmc->hw->dcs_rail), reg);
}

/* Enabling and disabling an agent changes its bit on every rail only. */
static void apple_pmc_test_agent_enable(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	unsigned int rail;

	pmc->enable[0] = BIT(0);
	writel(BIT(0), pmc->rails + pmc->hw->rail_enable);

	KUNIT_ASSERT_EQ(test, apple_pmc_agent_enable(pmc, 1, true), 0);
	for (rail = 0; rail < pmc->hw->rails; rail++)
		KUNIT_EXPECT_EQ(test, apple_pmc_enable_read(pmc, rail),
				BIT(1) | (rail ? 0 : BIT(0)));

	KUNIT_ASSERT_EQ(test, apple_pmc_agent_enable(pmc, 1, false), 0);
	for (rail = 0; rail < pmc->hw->rails; rail++)
		KUNIT_EXPECT_EQ(test, apple_pmc_enable_read(pmc, rail),
				rail ? 0 : BIT(0));
}

/* A T6040 agent is enabled on the DCS and SOC rails only. */
static void apple_pmc_test_agent_rails(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	unsigned int rail;

	pmc->hw = &apple_pmc_hw_t6040;
	KUNIT_ASSERT_EQ(test, apple_pmc_agent_enable(pmc, 1, true), 0);
	for (rail = 0; rail < pmc->hw->rails; rail++)
		KUNIT_EXPECT_EQ(test, apple_pmc_enable_read(pmc, rail),
				rail < 2 ? BIT(1) : 0);
}

/* Lifting a DCS limit gives the AP's agent back its own vote, not 0. */
static void apple_pmc_test_dcs_limit_restores_ap_vote(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	const struct apple_pmc_hw *hw = pmc->hw;

	apple_pmc_test_set_dcs_vote(pmc, hw->ap_agent, 2);
	apple_pmc_test_set_dcs_vote(pmc, 0, 4);
	pmc->enable[hw->dcs_rail] = BIT(0) | BIT(hw->ap_agent);
	pmc->ap_dcs_vote = apple_pmc_ap_dcs_read(pmc);
	KUNIT_ASSERT_EQ(test, pmc->ap_dcs_vote, 2);

	/* A limit of 3 lets the AP vote at most DCS 1 for the PMP's 4. */
	pmc->dcs_state = 3;
	apple_pmc_dcs_vote(pmc);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), 1);

	pmc->dcs_state = 0;
	apple_pmc_dcs_vote(pmc);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), 2);
}

struct apple_pmc_test_reject {
	unsigned int rail;
	u32 enable;
	bool rejected;
};

static u32 apple_pmc_test_reject_read(struct apple_pmc *pmc, unsigned int rail)
{
	struct kunit *test = kunit_get_current_test();
	struct apple_pmc_test_reject *reject = test->priv;
	void __iomem *reg = pmc->rails + pmc->hw->rail_enable + 4 * rail;
	u32 val = readl(reg);

	if (rail == reject->rail && !reject->rejected && val != reject->enable) {
		reject->rejected = true;
		writel(reject->enable, reg);
		return reject->enable;
	}
	return val;
}

/* Genpd leaves a domain off after power_on() fails on an intermediate rail. */
static void apple_pmc_test_partial_enable(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	struct apple_pmc_test_reject reject = { .rail = 1, .enable = BIT(0) };
	struct apple_pmc_domain domain = { .pmc = pmc, .agent = 1 };
	unsigned int rail;

	for (rail = 0; rail < pmc->hw->rails; rail++) {
		pmc->enable[rail] = BIT(0);
		writel(BIT(0), pmc->rails + pmc->hw->rail_enable + 4 * rail);
	}
	test->priv = &reject;
	kunit_activate_static_stub(test, apple_pmc_enable_read, apple_pmc_test_reject_read);
	KUNIT_EXPECT_EQ(test, apple_pmc_domain_power_on(&domain.genpd), -EIO);
	KUNIT_EXPECT_TRUE(test, reject.rejected);
	for (rail = 0; rail < pmc->hw->rails; rail++) {
		KUNIT_EXPECT_EQ(test, pmc->enable[rail], BIT(0));
		KUNIT_EXPECT_EQ(test, apple_pmc_enable_read(pmc, rail), BIT(0));
	}
	kunit_deactivate_static_stub(test, apple_pmc_enable_read);
	test->priv = NULL;
}

/* Probe unwind must also restore a nonzero inherited AP DCS vote. */
static void apple_pmc_test_release_restores_ap_vote(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	const struct apple_pmc_hw *hw = pmc->hw;
	unsigned int rail;

	INIT_LIST_HEAD(&pmc->domains);
	INIT_DELAYED_WORK(&pmc->dcs_work, apple_pmc_dcs_work);
	apple_pmc_test_set_dcs_vote(pmc, hw->ap_agent, 2);
	pmc->ap_dcs_vote = apple_pmc_ap_dcs_read(pmc);
	apple_pmc_test_set_dcs_vote(pmc, 0, 4);
	pmc->enable[hw->dcs_rail] = BIT(0) | BIT(hw->ap_agent);
	pmc->dcs_state = 3;
	apple_pmc_dcs_vote(pmc);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), 1);
	apple_pmc_release(pmc);
	KUNIT_EXPECT_EQ(test, pmc->dcs_state, 0UL);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), 2);
	for (rail = 0; rail < hw->rails; rail++)
		KUNIT_EXPECT_EQ(test, apple_pmc_enable_read(pmc, rail), pmc->saved_enable[rail]);
}

/* A busy T8132 agent must not have its in-flight command overwritten. */
static void apple_pmc_test_pending_floor(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	struct device dev = {};
	void __iomem *reg;

	pmc->hw = &apple_pmc_hw_t8132;
	dev_set_drvdata(&dev, pmc);
	reg = pmc->voters + pmc->hw->ap_agent * pmc->hw->agent_stride;
	writel(0xa500, reg);
	writel(BIT(pmc->hw->ap_agent), reg + 4);
	KUNIT_EXPECT_EQ(test, apple_pmc_floor_update(&dev, 3, 0xff, 0x35), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, readl(reg), 0xa500U);
	writel(0, reg + 4);
	KUNIT_EXPECT_EQ(test, apple_pmc_floor_update(&dev, 3, 0xff, 0x35), 0);
	KUNIT_EXPECT_EQ(test, readl(reg), 0xa535U);
	KUNIT_EXPECT_EQ(test, apple_pmc_floor_update(&dev, 4, 0xff, 0x35), -EINVAL);
}

static struct apple_pmc_domain *apple_pmc_test_floor(struct kunit *test,
						     struct apple_pmc *pmc,
						     unsigned int rail,
						     unsigned int level)
{
	struct apple_pmc_domain *dom = kunit_kzalloc(test, sizeof(*dom), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dom);
	dom->pmc = pmc;
	dom->floor = true;
	dom->rail = rail;
	dom->level = level;
	list_add_tail(&dom->list, &pmc->domains);
	return dom;
}

static u32 apple_pmc_test_ap_floor(struct apple_pmc *pmc)
{
	return readl(pmc->voters + pmc->hw->ap_agent * pmc->hw->agent_stride);
}

/*
 * Floors on one rail combine to the highest one held, leave other rails and
 * the AP's DCS vote alone, and are gone once the last one is released.
 */
static void apple_pmc_test_floor_aggregate(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	struct apple_pmc_domain *mid, *max, *other;
	u32 soc = APPLE_PMC_RAIL_MASK(1);

	INIT_LIST_HEAD(&pmc->domains);
	pmc->hw = &apple_pmc_hw_t6040;
	apple_pmc_test_set_dcs_vote(pmc, pmc->hw->ap_agent, 3);
	mid = apple_pmc_test_floor(test, pmc, 1, 1);
	max = apple_pmc_test_floor(test, pmc, 1, 2);
	other = apple_pmc_test_floor(test, pmc, 3, 1);

	KUNIT_ASSERT_EQ(test, apple_pmc_floor_set(mid, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pmc_test_ap_floor(pmc) & soc, 1U << 4);
	KUNIT_ASSERT_EQ(test, apple_pmc_floor_set(max, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pmc_test_ap_floor(pmc) & soc, 2U << 4);
	KUNIT_ASSERT_EQ(test, apple_pmc_floor_set(max, false), 0);
	KUNIT_EXPECT_EQ(test, apple_pmc_test_ap_floor(pmc) & soc, 1U << 4);
	KUNIT_ASSERT_EQ(test, apple_pmc_floor_set(mid, false), 0);
	KUNIT_EXPECT_EQ(test, apple_pmc_test_ap_floor(pmc), 3U);
	KUNIT_EXPECT_FALSE(test, other->on);
}

/* A floor that the PMC does not take back is not left marked as held. */
static void apple_pmc_test_floor_pending_timeout(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	struct apple_pmc_domain *dom;
	void __iomem *reg;

	INIT_LIST_HEAD(&pmc->domains);
	pmc->hw = &apple_pmc_hw_t8132;
	reg = pmc->voters + pmc->hw->ap_agent * pmc->hw->agent_stride;
	writel(BIT(pmc->hw->ap_agent), reg + 4);
	dom = apple_pmc_test_floor(test, pmc, 1, 2);
	KUNIT_EXPECT_EQ(test, apple_pmc_floor_set(dom, true), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, dom->on);
}

static struct kunit_case apple_pmc_test_cases[] = {
	KUNIT_CASE(apple_pmc_test_partial_enable),
	KUNIT_CASE(apple_pmc_test_release_restores_ap_vote),
	KUNIT_CASE(apple_pmc_test_pending_floor),
	KUNIT_CASE(apple_pmc_test_agent_enable),
	KUNIT_CASE(apple_pmc_test_agent_rails),
	KUNIT_CASE(apple_pmc_test_dcs_limit_restores_ap_vote),
	KUNIT_CASE(apple_pmc_test_floor_aggregate),
	KUNIT_CASE(apple_pmc_test_floor_pending_timeout),
	{}
};

static struct kunit_suite apple_pmc_test_suite = {
	.name = "apple-pmc",
	.test_cases = apple_pmc_test_cases,
};

kunit_test_suite(apple_pmc_test_suite);

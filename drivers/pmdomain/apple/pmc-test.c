// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple PMC voter interface: agent enables and the
 * AP's DCS vote, with memory standing in for the registers.
 *
 * Included by pmc.c.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/pm_runtime.h>

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

/*
 * A DCS floor raises the AP's DCS vote above its inherited vote while held,
 * yields to the DCS limit, and gives the inherited vote back when released.
 */
static void apple_pmc_test_dcs_floor(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	const struct apple_pmc_hw *hw = pmc->hw;
	struct apple_pmc_domain *dom;

	INIT_LIST_HEAD(&pmc->domains);
	apple_pmc_test_set_dcs_vote(pmc, hw->ap_agent, 1);
	pmc->ap_dcs_vote = apple_pmc_ap_dcs_read(pmc);
	pmc->enable[hw->dcs_rail] = BIT(1) | BIT(hw->ap_agent);
	dom = apple_pmc_test_floor(test, pmc, hw->dcs_rail, hw->dcs_max);

	KUNIT_ASSERT_EQ(test, apple_pmc_floor_set(dom, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), hw->dcs_max);
	/* The limit caps the floor; the other agents ask for nothing. */
	pmc->dcs_state = 3;
	apple_pmc_dcs_vote(pmc);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), hw->dcs_max - 3);
	pmc->dcs_state = 0;
	apple_pmc_dcs_vote(pmc);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), hw->dcs_max);
	KUNIT_ASSERT_EQ(test, apple_pmc_floor_set(dom, false), 0);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), 1);
	/* Other rails of the AP's floor word are untouched. */
	KUNIT_EXPECT_EQ(test, apple_pmc_test_ap_floor(pmc) & ~APPLE_PMC_RAIL_MASK(hw->dcs_rail), 0U);
}

/*
 * A T8132 interface domain sets and clears its own BWR and boost enables
 * only: the internal display's boost enable, bit 0, stays set.
 */
static void apple_pmc_test_interface(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	struct apple_pmc_domain dom = {
		.pmc = pmc, .interface = true, .bwr_bit = 3, .boost_bit = 1,
	};
	const struct apple_pmc_hw *hw = &apple_pmc_hw_t8132;

	pmc->hw = hw;
	writel(BIT(0), pmc->rails + hw->boost_enable);

	KUNIT_ASSERT_EQ(test, apple_pmc_domain_power_on(&dom.genpd), 0);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->bwr_enable), BIT(3));
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->boost_enable), BIT(0) | BIT(1));
	KUNIT_ASSERT_EQ(test, apple_pmc_domain_power_off(&dom.genpd), 0);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->bwr_enable), 0U);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->boost_enable), BIT(0));
}

struct apple_pmc_test_stuck {
	u32 offset;
	u32 value;
};

/* The interface enable word at stuck->offset keeps stuck->value. */
static u32 apple_pmc_test_stuck_read(struct apple_pmc *pmc, u32 offset)
{
	struct kunit *test = kunit_get_current_test();
	struct apple_pmc_test_stuck *stuck = test->priv;

	if (offset == stuck->offset)
		writel(stuck->value, pmc->rails + offset);
	return readl(pmc->rails + offset);
}

/*
 * If the BWR enable does not change, the boost enable goes back too, both
 * when the domain powers on and when it powers off.
 */
static void apple_pmc_test_interface_stuck(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	struct apple_pmc_domain dom = {
		.pmc = pmc, .interface = true, .bwr_bit = 3, .boost_bit = 1,
	};
	const struct apple_pmc_hw *hw = &apple_pmc_hw_t8132;
	struct apple_pmc_test_stuck stuck = { .offset = hw->bwr_enable };

	pmc->hw = hw;
	writel(BIT(0), pmc->rails + hw->boost_enable);
	test->priv = &stuck;
	kunit_activate_static_stub(test, apple_pmc_interface_read,
				   apple_pmc_test_stuck_read);

	KUNIT_EXPECT_EQ(test, apple_pmc_domain_power_on(&dom.genpd), -EIO);
	KUNIT_EXPECT_EQ(test, apple_pmc_interface_read(pmc, hw->bwr_enable), 0U);
	KUNIT_EXPECT_EQ(test, apple_pmc_interface_read(pmc, hw->boost_enable), BIT(0));

	stuck.value = BIT(3);
	writel(BIT(0) | BIT(1), pmc->rails + hw->boost_enable);
	KUNIT_EXPECT_EQ(test, apple_pmc_domain_power_off(&dom.genpd), -EIO);
	KUNIT_EXPECT_EQ(test, apple_pmc_interface_read(pmc, hw->bwr_enable), BIT(3));
	KUNIT_EXPECT_EQ(test, apple_pmc_interface_read(pmc, hw->boost_enable),
			BIT(0) | BIT(1));

	kunit_deactivate_static_stub(test, apple_pmc_interface_read);
	test->priv = NULL;
}

/*
 * The internal display's consumer-switched interface (BWR and boost bit 0):
 * off clears its bits only, on gives back the values they had, whether the
 * boot loader left both set or only the boost enable.
 */
static void apple_pmc_test_interface_consumer(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	struct apple_pmc_domain dom = {
		.pmc = pmc, .interface = true, .bwr_bit = 0, .boost_bit = 0,
		.consumer_switched = true,
	};
	const struct apple_pmc_hw *hw = &apple_pmc_hw_t8132;

	pmc->hw = hw;
	/* As Linux finds them: the boost enable only, next to dispext0's. */
	writel(BIT(0) | BIT(1), pmc->rails + hw->boost_enable);
	KUNIT_EXPECT_EQ(test, apple_pmc_interface_consumer_set(&dom, true), -EALREADY);
	KUNIT_ASSERT_EQ(test, apple_pmc_interface_consumer_set(&dom, false), 0);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->bwr_enable), 0U);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->boost_enable), BIT(1));
	KUNIT_EXPECT_EQ(test, apple_pmc_interface_consumer_set(&dom, false), -EALREADY);
	KUNIT_ASSERT_EQ(test, apple_pmc_interface_consumer_set(&dom, true), 0);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->bwr_enable), 0U);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->boost_enable), BIT(0) | BIT(1));

	/* As macOS keeps them while the display is on: both set. */
	writel(BIT(0), pmc->rails + hw->bwr_enable);
	KUNIT_ASSERT_EQ(test, apple_pmc_interface_consumer_set(&dom, false), 0);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->bwr_enable), 0U);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->boost_enable), BIT(1));
	KUNIT_ASSERT_EQ(test, apple_pmc_interface_consumer_set(&dom, true), 0);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->bwr_enable), BIT(0));
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->boost_enable), BIT(0) | BIT(1));
}

/* A BWR enable that does not clear: the boost enable goes back, still on. */
static void apple_pmc_test_interface_consumer_stuck(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	struct apple_pmc_domain dom = {
		.pmc = pmc, .interface = true, .bwr_bit = 0, .boost_bit = 0,
		.consumer_switched = true,
	};
	const struct apple_pmc_hw *hw = &apple_pmc_hw_t8132;
	struct apple_pmc_test_stuck stuck = { .offset = hw->bwr_enable, .value = BIT(0) };

	pmc->hw = hw;
	writel(BIT(0), pmc->rails + hw->boost_enable);
	test->priv = &stuck;
	kunit_activate_static_stub(test, apple_pmc_interface_read, apple_pmc_test_stuck_read);
	KUNIT_EXPECT_EQ(test, apple_pmc_interface_consumer_set(&dom, false), -EIO);
	KUNIT_EXPECT_FALSE(test, dom.consumer_off);
	KUNIT_EXPECT_EQ(test, apple_pmc_interface_read(pmc, hw->boost_enable), BIT(0));
	kunit_deactivate_static_stub(test, apple_pmc_interface_read);
	test->priv = NULL;
}

static void apple_pmc_test_genpd_remove(void *genpd)
{
	pm_genpd_remove(genpd);
}

static void apple_pmc_test_genpd_detach(void *dev)
{
	pm_runtime_disable(dev);
	pm_genpd_remove_device(dev);
}

/* A genpd that is removed again when the test ends, children first. */
static struct generic_pm_domain *apple_pmc_test_genpd(struct kunit *test,
						      struct generic_pm_domain *genpd,
						      int ret)
{
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							apple_pmc_test_genpd_remove,
							genpd), 0);
	return genpd;
}

static struct apple_pmc_domain *apple_pmc_test_interface_dom(struct kunit *test,
							     struct apple_pmc *pmc,
							     const char *name)
{
	struct apple_pmc_domain *dom = kunit_kzalloc(test, sizeof(*dom), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dom);
	dom->pmc = pmc;
	dom->interface = true;
	dom->bwr_bit = 3;
	dom->boost_bit = 1;
	dom->genpd.name = name;
	apple_pmc_test_genpd(test, &dom->genpd, apple_pmc_interface_init(pmc, dom));
	return dom;
}

/*
 * Link the interface domain as the PMGR driver does: under a SYS power
 * state, with an FE power state below both, all of them IRQ safe. Powering
 * FE through a device sets the enables after SYS is on, and powering it
 * off clears them.
 */
static void apple_pmc_test_interface_genpd(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	const struct apple_pmc_hw *hw = &apple_pmc_hw_t8132;
	struct generic_pm_domain *sys, *fe;
	struct apple_pmc_domain *dom;
	struct device *dev;

	pmc->hw = hw;
	writel(BIT(0), pmc->rails + hw->boost_enable);
	pmc->saved_boost = BIT(0);
	dev = kunit_device_register(test, "apple-pmc-test-fe");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	sys = kunit_kzalloc(test, sizeof(*sys), GFP_KERNEL);
	fe = kunit_kzalloc(test, sizeof(*fe), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sys);
	KUNIT_ASSERT_NOT_NULL(test, fe);
	sys->name = "apple-pmc-test-sys";
	sys->flags = GENPD_FLAG_IRQ_SAFE;
	fe->name = "apple-pmc-test-fe";
	fe->flags = GENPD_FLAG_IRQ_SAFE;
	apple_pmc_test_genpd(test, sys, pm_genpd_init(sys, NULL, false));
	dom = apple_pmc_test_interface_dom(test, pmc, "apple-pmc-test-interface");
	KUNIT_EXPECT_EQ(test, dom->genpd.status, GENPD_STATE_OFF);
	apple_pmc_test_genpd(test, fe, pm_genpd_init(fe, NULL, true));

	KUNIT_ASSERT_EQ(test, pm_genpd_add_subdomain(sys, &dom->genpd), 0);
	KUNIT_ASSERT_EQ(test, pm_genpd_add_subdomain(sys, fe), 0);
	KUNIT_ASSERT_EQ(test, pm_genpd_add_subdomain(&dom->genpd, fe), 0);

	KUNIT_ASSERT_EQ(test, pm_genpd_add_device(fe, dev), 0);
	pm_runtime_enable(dev);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							apple_pmc_test_genpd_detach,
							dev), 0);

	KUNIT_ASSERT_EQ(test, pm_runtime_resume_and_get(dev), 0);
	KUNIT_EXPECT_EQ(test, dom->genpd.status, GENPD_STATE_ON);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->bwr_enable), BIT(3));
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->boost_enable), BIT(0) | BIT(1));

	KUNIT_EXPECT_EQ(test, pm_runtime_put_sync_suspend(dev), 0);
	KUNIT_EXPECT_EQ(test, dom->genpd.status, GENPD_STATE_OFF);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->bwr_enable), 0U);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->boost_enable), BIT(0));
}

/*
 * A pipe with both enables set at boot starts on. One with only its boost
 * enable set starts off, and a power on that fails on the BWR enable keeps
 * that boost enable instead of clearing it.
 */
static void apple_pmc_test_interface_boot_state(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	const struct apple_pmc_hw *hw = &apple_pmc_hw_t8132;
	struct apple_pmc_test_stuck stuck = { .offset = hw->bwr_enable };
	struct apple_pmc_domain *dom;

	pmc->hw = hw;
	pmc->saved_bwr = BIT(3);
	pmc->saved_boost = BIT(0) | BIT(1);
	dom = apple_pmc_test_interface_dom(test, pmc, "apple-pmc-test-on");
	KUNIT_EXPECT_EQ(test, dom->genpd.status, GENPD_STATE_ON);

	pmc->saved_bwr = 0;
	writel(BIT(0) | BIT(1), pmc->rails + hw->boost_enable);
	dom = apple_pmc_test_interface_dom(test, pmc, "apple-pmc-test-mixed");
	KUNIT_EXPECT_EQ(test, dom->genpd.status, GENPD_STATE_OFF);

	test->priv = &stuck;
	kunit_activate_static_stub(test, apple_pmc_interface_read,
				   apple_pmc_test_stuck_read);
	KUNIT_EXPECT_EQ(test, apple_pmc_domain_power_on(&dom->genpd), -EIO);
	KUNIT_EXPECT_EQ(test, apple_pmc_interface_read(pmc, hw->boost_enable),
			BIT(0) | BIT(1));
	kunit_deactivate_static_stub(test, apple_pmc_interface_read);
	test->priv = NULL;

	KUNIT_EXPECT_EQ(test, apple_pmc_domain_power_on(&dom->genpd), 0);
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->bwr_enable), BIT(3));
	KUNIT_EXPECT_EQ(test, readl(pmc->rails + hw->boost_enable), BIT(0) | BIT(1));
}

static struct kunit_case apple_pmc_test_cases[] = {
	KUNIT_CASE(apple_pmc_test_dcs_floor),
	KUNIT_CASE(apple_pmc_test_partial_enable),
	KUNIT_CASE(apple_pmc_test_release_restores_ap_vote),
	KUNIT_CASE(apple_pmc_test_pending_floor),
	KUNIT_CASE(apple_pmc_test_agent_enable),
	KUNIT_CASE(apple_pmc_test_agent_rails),
	KUNIT_CASE(apple_pmc_test_dcs_limit_restores_ap_vote),
	KUNIT_CASE(apple_pmc_test_floor_aggregate),
	KUNIT_CASE(apple_pmc_test_floor_pending_timeout),
	KUNIT_CASE(apple_pmc_test_interface),
	KUNIT_CASE(apple_pmc_test_interface_stuck),
	KUNIT_CASE(apple_pmc_test_interface_consumer),
	KUNIT_CASE(apple_pmc_test_interface_consumer_stuck),
	KUNIT_CASE(apple_pmc_test_interface_genpd),
	KUNIT_CASE(apple_pmc_test_interface_boot_state),
	{}
};

static struct kunit_suite apple_pmc_test_suite = {
	.name = "apple-pmc",
	.test_cases = apple_pmc_test_cases,
};

kunit_test_suite(apple_pmc_test_suite);

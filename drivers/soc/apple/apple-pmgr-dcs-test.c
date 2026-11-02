// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Synthetic OPPs and memory-backed registers; no hardware rate claims. */
#include <kunit/device.h>
#include <kunit/test.h>

static int apple_pmgr_dcs_test_init(struct kunit *test)
{
	struct apple_pmgr_misc *misc;
	void *reg;

	misc = kunit_kzalloc(test, sizeof(*misc), GFP_KERNEL);
	if (!misc)
		return -ENOMEM;
	reg = kunit_kzalloc(test, sizeof(u32), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;
	misc->dev = kunit_device_register(test, "pmgr-dcs-test");
	if (IS_ERR(misc->dev))
		return PTR_ERR(misc->dev);
	mutex_init(&misc->lock);
	misc->devices[DEV_DCS].base = (void __iomem *)reg;
	misc->devices[DEV_DCS].active_state = 10;
	misc->devices[DEV_DCS].suspend_state = DCS_DEV_PSTATE_MIN_T600X;
	misc->dcs_num_opps = 3;
	misc->dcs_opps[0] = (struct apple_pmgr_dcs_opp) { 7, 1000 };
	misc->dcs_opps[1] = (struct apple_pmgr_dcs_opp) { 9, 2000 };
	misc->dcs_opps[2] = (struct apple_pmgr_dcs_opp) { 10, 3000 };
	writel(0xa50000aa, misc->devices[DEV_DCS].base);
	misc->dcs_data = kunit_kzalloc(test, struct_size(misc->dcs_data, nodes, 2), GFP_KERNEL);
	if (!misc->dcs_data)
		return -ENOMEM;
	misc->dcs_data->num_nodes = 2;
	for (unsigned int i = 0; i < 2; i++) {
		misc->dcs_data->nodes[i] = kunit_kzalloc(test, sizeof(struct icc_node), GFP_KERNEL);
		if (!misc->dcs_data->nodes[i])
			return -ENOMEM;
		misc->dcs_data->nodes[i]->data = misc;
	}
	dev_set_drvdata(misc->dev, misc);
	test->priv = misc;
	return 0;
}

static void apple_pmgr_dcs_table_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;

	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), 0);
	swap(misc->dcs_opps[0], misc->dcs_opps[2]);
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), 0);
	KUNIT_EXPECT_EQ(test, misc->dcs_opps[0].state, 7U);
	misc->dcs_opps[1].state = 7;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), -EINVAL);
	misc->dcs_opps[1].state = 9;
	misc->dcs_opps[1].bw = 1000;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), -EINVAL);
	misc->dcs_opps[1].bw = 2000;
	misc->dcs_opps[0].bw = 0;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), -EINVAL);
	misc->dcs_opps[0].bw = 1000;
	misc->dcs_opps[0].state = 6;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), -EINVAL);
	misc->dcs_opps[0].state = 7;
	misc->devices[DEV_DCS].active_state = 9;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), -EINVAL);
	misc->dcs_num_opps = 17;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), -EINVAL);
	misc->dcs_num_opps = 1;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), -EINVAL);
}

static void apple_pmgr_dcs_selection_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	u32 state = 0;

	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_select(misc, 0, &state), 0);
	KUNIT_EXPECT_EQ(test, state, 7U);
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_select(misc, 1000, &state), 0);
	KUNIT_EXPECT_EQ(test, state, 7U);
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_select(misc, 1001, &state), 0);
	KUNIT_EXPECT_EQ(test, state, 9U);
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_select(misc, 3000, &state), 0);
	KUNIT_EXPECT_EQ(test, state, 10U);
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_select(misc, 3001, &state), -ERANGE);
	KUNIT_EXPECT_EQ(test, state, 10U);
}

static void apple_pmgr_dcs_aggregation_test(struct kunit *test)
{
	u32 avg = 0, peak = 0;

	apple_pmgr_dcs_aggregate(NULL, 0, 800, 1000, &avg, &peak);
	apple_pmgr_dcs_aggregate(NULL, 0, 900, 1200, &avg, &peak);
	KUNIT_EXPECT_EQ(test, avg, 1700U);
	KUNIT_EXPECT_EQ(test, peak, 1200U);
	apple_pmgr_dcs_aggregate(NULL, 0, U32_MAX, 0, &avg, &peak);
	KUNIT_EXPECT_EQ(test, avg, U32_MAX);
}

static void apple_pmgr_dcs_commit_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct icc_node *node = misc->dcs_data->nodes[0];

	node->avg_bw = 1001;

	writel(0xa500009a, misc->devices[DEV_DCS].base);
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_set(node, node), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 9U);
	KUNIT_EXPECT_TRUE(test, misc->dcs_bw_active);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0xa5000099U);
}

static void apple_pmgr_dcs_owner_sleep_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct icc_node *node = misc->dcs_data->nodes[0];

	node->peak_bw = 1000;

	misc->pmp_owned = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_set(node, node), -EBUSY);
	misc->pmp_owned = false;
	misc->sleeping = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_set(node, node), -EBUSY);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0xa50000aaU);
}

static void apple_pmgr_dcs_rollback_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct icc_node *node = misc->dcs_data->nodes[0];

	node->peak_bw = 1000;

	/* Actual stays at 10: request times out, rollback to 10 succeeds. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_set(node, node), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, misc->dcs_failed);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 10U);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0xa50000aaU);
	/* An unknown actual state makes rollback fail too. */
	writel(0xa500000a, misc->devices[DEV_DCS].base);
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_set(node, node), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, misc->dcs_failed);
	KUNIT_EXPECT_TRUE(test, misc->dcs_bw_active);
	KUNIT_EXPECT_EQ(test, apple_pmgr_misc_prepare(misc->dev), -EIO);
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_set(node, node), -EIO);
}

static void apple_pmgr_dcs_sync_constraints_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct icc_node *src = misc->dcs_data->nodes[0];
	struct icc_node *dst = misc->dcs_data->nodes[1];

	src->avg_bw = 1001;
	dst->init_peak = 3000;
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_set(src, src), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 10U);
	dst->init_peak = 0;
	writel(0xa500009a, misc->devices[DEV_DCS].base);
	/* Releasing destination's boot constraint must retain source demand. */
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_set(dst, dst), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 9U);
}

static void apple_pmgr_dcs_suspend_restore_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;

	dev_set_drvdata(misc->dev, misc);
	misc->devices[DEV_DCS].active_state = 9;
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_suspend_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0xa50000a7U);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0xa50000a9U);
}

static void apple_pmgr_dcs_no_table_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;

	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_init(misc), 0);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0xa50000aaU);
}

static void apple_pmgr_dcs_release_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct icc_node *node = misc->dcs_data->nodes[0];

	misc->dcs_bw_active = true;
	writel(0xa500007a, misc->devices[DEV_DCS].base);
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_set(node, node), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 7U);
	KUNIT_EXPECT_FALSE(test, misc->dcs_bw_active);
}

static struct kunit_case apple_pmgr_dcs_test_cases[] = {
	KUNIT_CASE(apple_pmgr_dcs_table_test),
	KUNIT_CASE(apple_pmgr_dcs_selection_test),
	KUNIT_CASE(apple_pmgr_dcs_aggregation_test),
	KUNIT_CASE(apple_pmgr_dcs_commit_test),
	KUNIT_CASE(apple_pmgr_dcs_owner_sleep_test),
	KUNIT_CASE(apple_pmgr_dcs_rollback_test),
	KUNIT_CASE(apple_pmgr_dcs_sync_constraints_test),
	KUNIT_CASE(apple_pmgr_dcs_suspend_restore_test),
	KUNIT_CASE(apple_pmgr_dcs_no_table_test),
	KUNIT_CASE(apple_pmgr_dcs_release_test),
	{}
};

static struct kunit_suite apple_pmgr_dcs_test_suite = {
	.name = "apple-pmgr-dcs",
	.init = apple_pmgr_dcs_test_init,
	.test_cases = apple_pmgr_dcs_test_cases,
};

kunit_test_suite(apple_pmgr_dcs_test_suite);

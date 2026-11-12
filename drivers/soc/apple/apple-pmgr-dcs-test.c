// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Synthetic OPPs and memory-backed registers; no hardware rate claims. */
#include <kunit/device.h>
#include <kunit/test.h>

struct apple_pmgr_dcs_fixture {
	struct apple_pmgr_misc misc;
	u32 commands[16];
	unsigned int num_commands;
	u32 fail_states;
	u32 fail_actual;
};

static void apple_pmgr_dcs_fake_command(u32 word, void __iomem *reg)
{
	struct kunit *test = kunit_get_current_test();
	struct apple_pmgr_dcs_fixture *f = test->priv;
	u32 actual = word;

	KUNIT_ASSERT_PTR_EQ(test, reg, f->misc.devices[DEV_DCS].base);
	KUNIT_ASSERT_LT(test, f->num_commands, ARRAY_SIZE(f->commands));
	f->commands[f->num_commands++] = word;
	if (f->fail_states & BIT(word))
		actual = f->fail_actual;
	/* Model hardware updating actual independently of the command word. */
	writel(word | FIELD_PREP(APPLE_CLKGEN_PSTATE_ACTUAL, actual), reg);
}

static int apple_pmgr_dcs_test_init(struct kunit *test)
{
	struct apple_pmgr_dcs_fixture *f;
	struct apple_pmgr_misc *misc;
	void *reg;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	misc = &f->misc;
	reg = kunit_kzalloc(test, 2 * sizeof(u32), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;
	misc->dev = kunit_device_register(test, "pmgr-dcs-test");
	if (IS_ERR(misc->dev))
		return PTR_ERR(misc->dev);
	mutex_init(&misc->lock);
	misc->devices[DEV_DCS].base = (void __iomem *)reg;
	misc->devices[DEV_DCS].active_state = 9;
	misc->devices[DEV_DCS].suspend_state = DCS_DEV_PSTATE_MIN_T600X;
	misc->dcs_valid_states = DCS_VALID_STATES_T600X;
	misc->devices[DEV_FABRIC].base = (void __iomem *)(reg + sizeof(u32));
	writel(0xa5000077, misc->devices[DEV_FABRIC].base);
	misc->fabric_now = 7;
	misc->devices[DEV_FABRIC].active_state = 7;
	misc->dcs_num_opps = 2;
	misc->dcs_opps[0] = (struct apple_pmgr_dcs_opp) { 7, 1000 };
	misc->dcs_opps[1] = (struct apple_pmgr_dcs_opp) { 9, 2000 };
	writel(0xa5000099, misc->devices[DEV_DCS].base);
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
	test->priv = f;
	kunit_activate_static_stub(test, apple_pmgr_dcs_command, apple_pmgr_dcs_fake_command);
	return 0;
}

static void apple_pmgr_dcs_table_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;

	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), 0);
	swap(misc->dcs_opps[0], misc->dcs_opps[1]);
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
	misc->dcs_opps[0].state = 8;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), -EINVAL);
	misc->dcs_opps[0].state = 7;
	misc->devices[DEV_DCS].active_state = 7;
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
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_select(misc, 2000, &state), 0);
	KUNIT_EXPECT_EQ(test, state, 9U);
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_select(misc, 2001, &state), -ERANGE);
	KUNIT_EXPECT_EQ(test, state, 9U);
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

	writel(0x999, misc->devices[DEV_DCS].base);
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_set(node, node), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 9U);
	KUNIT_EXPECT_TRUE(test, misc->dcs_bw_active);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0x99U);
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
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0xa5000099U);
}

static void apple_pmgr_dcs_rollback_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct icc_node *node = misc->dcs_data->nodes[0];
	struct apple_pmgr_dcs_fixture *f = test->priv;

	node->peak_bw = 1000;

	/* Failed down-command, followed by a successful whole-word restore. */
	f->fail_states = BIT(7);
	f->fail_actual = 9;
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_set(node, node), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, misc->dcs_failed);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 9U);
	KUNIT_EXPECT_EQ(test, f->commands[0], 7U);
	KUNIT_EXPECT_EQ(test, f->commands[1], 9U);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0x99U);
	/* Inject failure of the restoration as well; retain the fabric hold. */
	f->fail_states |= BIT(9);
	f->fail_actual = 0;
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

	src->avg_bw = 1000;
	dst->init_peak = 2000;
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_set(src, src), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 9U);
	dst->init_peak = 0;
	writel(0x999, misc->devices[DEV_DCS].base);
	/* Releasing destination's boot constraint must retain source demand. */
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_set(dst, dst), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 7U);
}

static void apple_pmgr_dcs_suspend_restore_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;

	dev_set_drvdata(misc->dev, misc);
	misc->devices[DEV_DCS].active_state = 9;
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_suspend_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0x77U);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0x99U);
}

static void apple_pmgr_dcs_no_table_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;

	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_init(misc), 0);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0xa5000099U);
}

static void apple_pmgr_dcs_release_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct icc_node *node = misc->dcs_data->nodes[0];

	misc->dcs_bw_active = true;
	writel(0x999, misc->devices[DEV_DCS].base);
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_set(node, node), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].active_state, 7U);
	KUNIT_EXPECT_FALSE(test, misc->dcs_bw_active);
}

static void apple_pmgr_dcs_whole_word_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct apple_pmgr_dcs_fixture *f = test->priv;

	writel(0xa5000999, misc->devices[DEV_DCS].base);
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_write(misc, 7), 0);
	KUNIT_EXPECT_EQ(test, f->num_commands, 1U);
	KUNIT_EXPECT_EQ(test, f->commands[0], 0x00000007U);
	KUNIT_ASSERT_EQ(test, apple_pmgr_dcs_write(misc, 9), 0);
	KUNIT_EXPECT_EQ(test, f->commands[1], 0x00000009U);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_FABRIC].base), 0xa5000077U);
}

static void apple_pmgr_dcs_invalid_state_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct apple_pmgr_dcs_fixture *f = test->priv;

	for (u32 state = 0; state < 32; state++) {
		bool valid = state == 5 || state == 6 || state == 7 || state == 9;

		KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_state_valid(misc, state), valid);
		if (!valid) {
			KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_write(misc, state), -EINVAL);
			misc->dcs_opps[0] = (struct apple_pmgr_dcs_opp) { state, 1000 };
			misc->dcs_opps[1] = (struct apple_pmgr_dcs_opp) { 9, 2000 };
			KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), -EINVAL);
		}
	}
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_write(misc, U32_MAX), -EINVAL);
	KUNIT_EXPECT_EQ(test, f->num_commands, 0U);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0xa5000099U);
	misc->dcs_num_opps = 4;
	misc->dcs_opps[0] = (struct apple_pmgr_dcs_opp) { 5, 1000 };
	misc->dcs_opps[1] = (struct apple_pmgr_dcs_opp) { 6, 2000 };
	misc->dcs_opps[2] = (struct apple_pmgr_dcs_opp) { 7, 3000 };
	misc->dcs_opps[3] = (struct apple_pmgr_dcs_opp) { 9, 4000 };
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_validate(misc), 0);
}

static void apple_pmgr_dcs_unsettled_test(struct kunit *test)
{
	struct apple_pmgr_misc *misc = test->priv;
	struct apple_pmgr_dcs_fixture *f = test->priv;
	struct icc_node *node = misc->dcs_data->nodes[0];

	writel(0x97, misc->devices[DEV_DCS].base);
	KUNIT_EXPECT_EQ(test, apple_pmgr_dcs_set(node, node), -EBUSY);
	KUNIT_EXPECT_EQ(test, f->num_commands, 0U);
	KUNIT_EXPECT_EQ(test, readl(misc->devices[DEV_DCS].base), 0x97U);
}

static struct kunit_case apple_pmgr_dcs_test_cases[] = {
	KUNIT_CASE(apple_pmgr_dcs_whole_word_test),
	KUNIT_CASE(apple_pmgr_dcs_invalid_state_test),
	KUNIT_CASE(apple_pmgr_dcs_unsettled_test),
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

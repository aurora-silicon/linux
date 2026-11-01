// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "ane_t6021_pm.h"

struct ane_sleep_test {
	struct kunit *test;
	struct device dev;
	/* Models the driver command mutex. */
	struct mutex fw_lock;
	struct ane_t6021_pm pm;
	unsigned int stops, restarts;
	int stop_error, restart_error;
};

static struct ane_t6021_pm *ane_sleep_get_pm(struct device *dev)
{
	return dev_get_drvdata(dev);
}

DEFINE_ANE_T6021_SYSTEM_PM_OPS(ane_sleep_dev_pm, ane_sleep_get_pm);

static int ane_sleep_stop(void *data)
{
	struct ane_sleep_test *ctx = data;

	KUNIT_EXPECT_TRUE(ctx->test, mutex_is_locked(&ctx->fw_lock));
	KUNIT_EXPECT_EQ(ctx->test, ane_t6021_pm_submit_error(ctx->pm.state), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(ctx->test, ctx->stops, ctx->restarts);
	ctx->stops++;
	return ctx->stop_error;
}

static int ane_sleep_restart(void *data)
{
	struct ane_sleep_test *ctx = data;

	KUNIT_EXPECT_TRUE(ctx->test, mutex_is_locked(&ctx->fw_lock));
	KUNIT_EXPECT_EQ(ctx->test, ane_t6021_pm_submit_error(ctx->pm.state), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(ctx->test, ctx->stops, ctx->restarts + 1);
	ctx->restarts++;
	return ctx->restart_error;
}

static const struct ane_t6021_pm_ops ane_sleep_ops = {
	.stop = ane_sleep_stop,
	.restart = ane_sleep_restart,
};

static int ane_sleep_init(struct kunit *test)
{
	struct ane_sleep_test *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;
	ctx->test = test;
	mutex_init(&ctx->fw_lock);
	ctx->pm = (struct ane_t6021_pm) {
		.fw_lock = &ctx->fw_lock,
		.ops = &ane_sleep_ops,
		.ctx = ctx,
		.state = ANE_T6021_PM_ACTIVE,
	};
	dev_set_drvdata(&ctx->dev, &ctx->pm);
	test->priv = ctx;
	return 0;
}

static void ane_sleep_exit(struct kunit *test)
{
	struct ane_sleep_test *ctx = test->priv;

	mutex_destroy(&ctx->fw_lock);
}

static void ane_sleep_cycles_test(struct kunit *test)
{
	struct ane_sleep_test *ctx = test->priv;
	unsigned int i;

	for (i = 0; i < 3; i++) {
		KUNIT_ASSERT_EQ(test, ane_sleep_dev_pm_suspend(&ctx->dev), 0);
		KUNIT_EXPECT_FALSE(test, mutex_is_locked(&ctx->fw_lock));
		KUNIT_EXPECT_EQ(test, ctx->pm.state, ANE_T6021_PM_SUSPENDED);
		KUNIT_EXPECT_EQ(test, ane_t6021_pm_submit_error(ctx->pm.state), -EHOSTDOWN);
		KUNIT_EXPECT_EQ(test, ane_sleep_dev_pm_suspend(&ctx->dev), -EHOSTDOWN);
		KUNIT_EXPECT_EQ(test, ctx->stops, i + 1);
		KUNIT_ASSERT_EQ(test, ane_sleep_dev_pm_resume(&ctx->dev), 0);
		KUNIT_EXPECT_FALSE(test, mutex_is_locked(&ctx->fw_lock));
		KUNIT_EXPECT_EQ(test, ane_t6021_pm_submit_error(ctx->pm.state), 0);
		KUNIT_EXPECT_EQ(test, ctx->restarts, i + 1);
	}
}

static void ane_sleep_stop_failure_test(struct kunit *test)
{
	struct ane_sleep_test *ctx = test->priv;

	ctx->stop_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, ane_sleep_dev_pm_suspend(&ctx->dev), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&ctx->fw_lock));
	KUNIT_EXPECT_EQ(test, ctx->pm.state, ANE_T6021_PM_FAILED);
	KUNIT_EXPECT_EQ(test, ane_t6021_pm_submit_error(ctx->pm.state), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ane_sleep_dev_pm_resume(&ctx->dev), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ane_sleep_dev_pm_suspend(&ctx->dev), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ctx->stops, 1U);
	KUNIT_EXPECT_EQ(test, ctx->restarts, 0U);
}

static void ane_sleep_restart_failure_test(struct kunit *test)
{
	struct ane_sleep_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, ane_sleep_dev_pm_suspend(&ctx->dev), 0);
	ctx->restart_error = -EIO;
	KUNIT_EXPECT_EQ(test, ane_sleep_dev_pm_resume(&ctx->dev), -EIO);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&ctx->fw_lock));
	KUNIT_EXPECT_EQ(test, ctx->pm.state, ANE_T6021_PM_FAILED);
	KUNIT_EXPECT_EQ(test, ane_t6021_pm_submit_error(ctx->pm.state), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ane_sleep_dev_pm_resume(&ctx->dev), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ctx->restarts, 1U);
}

static void ane_sleep_abort_test(struct kunit *test)
{
	struct ane_sleep_test *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, ane_sleep_dev_pm_resume(&ctx->dev), 0);
	KUNIT_EXPECT_EQ(test, ane_t6021_pm_submit_error(ctx->pm.state), 0);
	KUNIT_EXPECT_EQ(test, ctx->stops, 0U);
	KUNIT_EXPECT_EQ(test, ctx->restarts, 0U);
}

static void ane_sleep_callback_mapping_test(struct kunit *test)
{
	if (IS_ENABLED(CONFIG_PM_SLEEP)) {
		KUNIT_EXPECT_TRUE(test, ane_sleep_dev_pm.suspend == ane_sleep_dev_pm_suspend);
		KUNIT_EXPECT_TRUE(test, ane_sleep_dev_pm.freeze == ane_sleep_dev_pm_suspend);
		KUNIT_EXPECT_TRUE(test, ane_sleep_dev_pm.poweroff == ane_sleep_dev_pm_suspend);
		KUNIT_EXPECT_TRUE(test, ane_sleep_dev_pm.resume == ane_sleep_dev_pm_resume);
		KUNIT_EXPECT_TRUE(test, ane_sleep_dev_pm.thaw == ane_sleep_dev_pm_resume);
		KUNIT_EXPECT_TRUE(test, ane_sleep_dev_pm.restore == ane_sleep_dev_pm_resume);
	} else {
		KUNIT_EXPECT_NULL(test, ane_sleep_dev_pm.suspend);
		KUNIT_EXPECT_NULL(test, ane_sleep_dev_pm.freeze);
		KUNIT_EXPECT_NULL(test, ane_sleep_dev_pm.poweroff);
		KUNIT_EXPECT_NULL(test, ane_sleep_dev_pm.resume);
		KUNIT_EXPECT_NULL(test, ane_sleep_dev_pm.thaw);
		KUNIT_EXPECT_NULL(test, ane_sleep_dev_pm.restore);
	}
}

static struct kunit_case ane_sleep_cases[] = {
	KUNIT_CASE(ane_sleep_cycles_test),
	KUNIT_CASE(ane_sleep_stop_failure_test),
	KUNIT_CASE(ane_sleep_restart_failure_test),
	KUNIT_CASE(ane_sleep_abort_test),
	KUNIT_CASE(ane_sleep_callback_mapping_test),
	{}
};

static struct kunit_suite ane_sleep_suite = {
	.name = "ane-h14-sleep",
	.test_cases = ane_sleep_cases,
	.init = ane_sleep_init,
	.exit = ane_sleep_exit,
};

kunit_test_suite(ane_sleep_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("ANE H14 system sleep adapter tests");

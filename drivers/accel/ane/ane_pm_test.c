// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Exercise recovery against the real runtime-PM and device-link core. */
#include <kunit/test.h>
#include <linux/device.h>
#include <linux/slab.h>

#include "ane_pm.h"

struct ane_pm_test_dev {
	struct device dev;
	int suspend_error;
	int resume_error;
	unsigned int suspends;
	unsigned int resumes;
};

struct ane_pm_test_ctx {
	struct kunit *test;
	struct device *consumer;
	struct device *domains[2];
	struct device_link *links[2];
	unsigned int verified;
	int verify_error;
};

static int ane_pm_test_suspend(struct device *dev)
{
	struct ane_pm_test_dev *d = container_of(dev, struct ane_pm_test_dev, dev);

	d->suspends++;
	return d->suspend_error;
}

static int ane_pm_test_resume(struct device *dev)
{
	struct ane_pm_test_dev *d = container_of(dev, struct ane_pm_test_dev, dev);

	d->resumes++;
	return d->resume_error;
}

static const struct dev_pm_ops ane_pm_test_ops = {
	.runtime_suspend = ane_pm_test_suspend,
	.runtime_resume = ane_pm_test_resume,
};

static const struct device_type ane_pm_test_type = {
	.pm = &ane_pm_test_ops,
};

static void ane_pm_test_release(struct device *dev)
{
	kfree(container_of(dev, struct ane_pm_test_dev, dev));
}

static void ane_pm_test_unregister(void *data)
{
	struct device *dev = data;

	pm_runtime_barrier(dev);
	pm_runtime_disable(dev);
	/* Also releases supplier votes after an injected resume failure. */
	pm_runtime_set_suspended(dev);
	device_unregister(dev);
}

static struct device *ane_pm_test_register(struct kunit *test, const char *name)
{
	struct ane_pm_test_dev *d;
	int ret;

	d = kzalloc_obj(*d);
	if (!d)
		return NULL;
	device_initialize(&d->dev);
	d->dev.release = ane_pm_test_release;
	d->dev.type = &ane_pm_test_type;
	ret = dev_set_name(&d->dev, "%s", name);
	if (!ret)
		ret = device_add(&d->dev);
	if (ret) {
		put_device(&d->dev);
		return NULL;
	}
	pm_runtime_enable(&d->dev);
	ret = kunit_add_action_or_reset(test, ane_pm_test_unregister, &d->dev);
	return ret ? NULL : &d->dev;
}

static void ane_pm_test_unlink(void *data)
{
	device_link_del(data);
}

static int ane_pm_test_verify(void *data)
{
	struct ane_pm_test_ctx *ctx = data;

	ctx->verified++;
	KUNIT_EXPECT_TRUE(ctx->test, pm_runtime_suspended(ctx->consumer));
	KUNIT_EXPECT_EQ(ctx->test, atomic_read(&ctx->consumer->power.usage_count), 0);
	for (int i = 0; i < ARRAY_SIZE(ctx->domains); i++) {
		if (!ctx->domains[i])
			continue;
		KUNIT_EXPECT_TRUE(ctx->test, pm_runtime_suspended(ctx->domains[i]));
		KUNIT_EXPECT_EQ(ctx->test, atomic_read(&ctx->domains[i]->power.usage_count), 0);
	}
	return ctx->verify_error;
}

static int ane_pm_test_setup(struct kunit *test, struct ane_pm_test_ctx *ctx,
			     bool multiple)
{
	static const char * const names[] = { "ane-pm-domain0", "ane-pm-domain1" };

	ctx->test = test;
	ctx->consumer = ane_pm_test_register(test, "ane-pm-consumer");
	if (!ctx->consumer)
		return -ENOMEM;
	if (multiple) {
		for (int i = 0; i < ARRAY_SIZE(ctx->domains); i++) {
			ctx->domains[i] = ane_pm_test_register(test, names[i]);
			if (!ctx->domains[i])
				return -ENOMEM;
			ctx->links[i] = device_link_add(ctx->consumer, ctx->domains[i],
							DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME |
						   DL_FLAG_RPM_ACTIVE);
			if (!ctx->links[i])
				return -ENOMEM;
			if (kunit_add_action_or_reset(test, ane_pm_test_unlink,
						      ctx->links[i]))
				return -ENOMEM;
		}
	}
	return pm_runtime_resume_and_get(ctx->consumer);
}

static void ane_pm_test_finish(struct kunit *test, struct ane_pm_test_ctx *ctx)
{
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->consumer->power.usage_count), 1);
	KUNIT_EXPECT_GE(test, pm_runtime_put_sync_suspend(ctx->consumer), 0);
}

static void ane_pm_cycle_domains_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, true), 0);
	for (int repeat = 0; repeat < 3; repeat++) {
		KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, ctx.domains, 2,
						   ane_pm_test_verify, &ctx), 0);
		KUNIT_EXPECT_TRUE(test, pm_runtime_active(ctx.consumer));
		for (int i = 0; i < ARRAY_SIZE(ctx.domains); i++) {
			KUNIT_EXPECT_TRUE(test, pm_runtime_active(ctx.domains[i]));
			KUNIT_EXPECT_EQ(test, atomic_read(&ctx.domains[i]->power.usage_count), 1);
		}
	}
	KUNIT_EXPECT_EQ(test, ctx.verified, 3U);
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_single_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, false), 0);
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, NULL, 1,
					   ane_pm_test_verify, &ctx), 0);
	KUNIT_EXPECT_EQ(test, ctx.verified, 1U);
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_suspend_error_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};
	struct ane_pm_test_dev *d;

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, true), 0);
	d = container_of(ctx.consumer, struct ane_pm_test_dev, dev);
	d->suspend_error = -EBUSY;
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, ctx.domains, 2,
					   ane_pm_test_verify, &ctx), -EBUSY);
	KUNIT_EXPECT_EQ(test, ctx.verified, 0U);
	d->suspend_error = 0;
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_supplier_error_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};
	struct ane_pm_test_dev *d;

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, true), 0);
	d = container_of(ctx.domains[1], struct ane_pm_test_dev, dev);
	d->suspend_error = -EBUSY;
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, ctx.domains, 2,
					   ane_pm_test_verify, &ctx), -EBUSY);
	KUNIT_EXPECT_EQ(test, ctx.verified, 0U);
	d->suspend_error = 0;
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_verify_error_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = { .verify_error = -ETIMEDOUT };

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, true), 0);
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, ctx.domains, 2,
					   ane_pm_test_verify, &ctx), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ctx.verified, 1U);
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(ctx.consumer));
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_resume_error_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};
	struct ane_pm_test_dev *d;

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, false), 0);
	d = container_of(ctx.consumer, struct ane_pm_test_dev, dev);
	d->resume_error = -EIO;
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, NULL, 1,
					   ane_pm_test_verify, &ctx), -EIO);
	KUNIT_EXPECT_EQ(test, ctx.verified, 1U);
	/* get_sync must leave exactly the reference the caller will put. */
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx.consumer->power.usage_count), 1);
	pm_runtime_put_noidle(ctx.consumer);
}

static struct kunit_case ane_pm_test_cases[] = {
	KUNIT_CASE(ane_pm_cycle_domains_test),
	KUNIT_CASE(ane_pm_cycle_single_test),
	KUNIT_CASE(ane_pm_cycle_suspend_error_test),
	KUNIT_CASE(ane_pm_cycle_supplier_error_test),
	KUNIT_CASE(ane_pm_cycle_verify_error_test),
	KUNIT_CASE(ane_pm_cycle_resume_error_test),
	{}
};

static struct kunit_suite ane_pm_test_suite = {
	.name = "ane-pm-recovery",
	.test_cases = ane_pm_test_cases,
};

kunit_test_suite(ane_pm_test_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("ANE recovery runtime-PM transaction tests");

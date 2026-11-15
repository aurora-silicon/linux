// SPDX-License-Identifier: GPL-2.0-only
/* Included by i2c-pasemi-core.c to exercise the real transfer entry points. */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/pm_domain.h>

struct pasemi_pm_test {
	struct pasemi_smbus smbus;
	int resume_error;
	int resumes;
};

static int pasemi_test_resume(struct device *dev)
{
	struct pasemi_pm_test *ctx = dev_get_drvdata(dev);

	ctx->resumes++;
	return ctx->resume_error;
}

static struct dev_pm_domain pasemi_test_domain = {
	.ops.runtime_resume = pasemi_test_resume,
};

static void pasemi_test_disable_pm(void *arg)
{
	struct device *dev = arg;

	pm_runtime_disable(dev);
	pm_runtime_dont_use_autosuspend(dev);
	dev->pm_domain = NULL;
}

static int pasemi_pm_test_init(struct kunit *test)
{
	struct pasemi_pm_test *ctx;
	struct device *dev;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	dev = kunit_device_register(test, "pasemi-pm");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	dev_set_drvdata(dev, ctx);
	dev->pm_domain = &pasemi_test_domain;
	ctx->smbus.dev = dev;
	ctx->smbus.runtime_pm = true;
	ctx->smbus.adapter.algo_data = &ctx->smbus;
	/* A baseline reference of one prevents autosuspend racing assertions. */
	pm_runtime_set_autosuspend_delay(dev, -1);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	test->priv = ctx;
	return kunit_add_action_or_reset(test, pasemi_test_disable_pm, dev);
}

static void pasemi_i2c_resume_failure_test(struct kunit *test)
{
	struct pasemi_pm_test *ctx = test->priv;
	struct i2c_msg msg = { .addr = 0x10 };

	ctx->resume_error = -EIO;
	/* ioaddr is NULL: neither path may access MMIO after a failed resume. */
	KUNIT_EXPECT_EQ(test, pasemi_i2c_xfer(&ctx->smbus.adapter, &msg, 1), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->resumes, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->smbus.dev->power.usage_count), 1);
}

static void pasemi_smb_resume_failure_test(struct kunit *test)
{
	struct pasemi_pm_test *ctx = test->priv;
	union i2c_smbus_data data;
	int ret;

	ctx->resume_error = -EIO;
	ret = pasemi_smb_xfer(&ctx->smbus.adapter, 0x10, 0, I2C_SMBUS_READ,
			      0, I2C_SMBUS_BYTE_DATA, &data);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, ctx->resumes, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->smbus.dev->power.usage_count), 1);
}

static void pasemi_test_mmio(struct kunit *test, u32 status)
{
	struct pasemi_pm_test *ctx = test->priv;
	void *regs = kunit_kzalloc(test, 0x30, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, regs);
	ctx->smbus.ioaddr = (void __iomem __force *)regs;
	iowrite32(status, ctx->smbus.ioaddr + REG_SMSTA);
}

static void pasemi_i2c_busy_failure_test(struct kunit *test)
{
	struct pasemi_pm_test *ctx = test->priv;
	struct i2c_msg msg = { .addr = 0x10 };

	pasemi_test_mmio(test, SMSTA_XIP);
	KUNIT_EXPECT_EQ(test, pasemi_i2c_xfer(&ctx->smbus.adapter, &msg, 1), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->resumes, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->smbus.dev->power.usage_count), 1);
}

static void pasemi_smb_invalid_failure_test(struct kunit *test)
{
	struct pasemi_pm_test *ctx = test->priv;
	union i2c_smbus_data data;
	int ret;

	pasemi_test_mmio(test, SMSTA_MTE);
	ret = pasemi_smb_xfer(&ctx->smbus.adapter, 0x10, 0, I2C_SMBUS_READ,
			      0, -1, &data);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->resumes, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->smbus.dev->power.usage_count), 1);
}

static struct kunit_case pasemi_pm_test_cases[] = {
	KUNIT_CASE(pasemi_i2c_resume_failure_test),
	KUNIT_CASE(pasemi_smb_resume_failure_test),
	KUNIT_CASE(pasemi_i2c_busy_failure_test),
	KUNIT_CASE(pasemi_smb_invalid_failure_test),
	{}
};

static struct kunit_suite pasemi_pm_test_suite = {
	.name = "i2c-pasemi-runtime-pm",
	.init = pasemi_pm_test_init,
	.test_cases = pasemi_pm_test_cases,
};

kunit_test_suite(pasemi_pm_test_suite);

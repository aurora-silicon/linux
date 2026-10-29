// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for SN201202x system sleep
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spmi.h>

#include "tps6598x.h"

/*
 * A controller on a simulated SPMI bus. It confirms SLEEP and WAKEUP with the
 * interrupts the driver waits for, unless the SLEEP confirmation gets lost,
 * and it does not answer register accesses while it is asleep.
 */
struct sn201202x_test {
	struct sn201202x sn;
	struct device *dev;
	int irq;
	bool asleep;
	bool sleep_unconfirmed;
	int sleep_error;
	int wakeup_error;
	unsigned int wakeups;
	bool irq_enabled_at_wakeup;
};

static bool sn201202x_test_irq_enabled(struct sn201202x_test *t)
{
	return !irqd_irq_disabled(irq_get_irq_data(t->irq));
}

static int sn201202x_test_cmd(struct spmi_controller *ctrl, u8 opcode, u8 sid)
{
	struct sn201202x_test *t = spmi_controller_get_drvdata(ctrl);

	switch (opcode) {
	case SPMI_CMD_SLEEP:
		if (t->sleep_error)
			return t->sleep_error;
		t->asleep = true;
		if (!t->sleep_unconfirmed)
			complete(&t->sn.sleep_completion);
		return 0;
	case SPMI_CMD_WAKEUP:
		t->wakeups++;
		t->irq_enabled_at_wakeup = sn201202x_test_irq_enabled(t);
		if (t->wakeup_error)
			return t->wakeup_error;
		t->asleep = false;
		complete(&t->sn.wake_completion);
		return 0;
	}

	return -EOPNOTSUPP;
}

static int sn201202x_test_read(void *context, const void *reg, size_t reg_size,
			       void *val, size_t val_size)
{
	struct sn201202x_test *t = context;

	/* A sleeping controller never confirms the register selection. */
	if (t->asleep)
		return -ETIMEDOUT;
	memset(val, 0, val_size);
	if (*(const u8 *)reg == TPS_REG_MODE)
		memcpy(val, "APP ", min(val_size, 4));

	return 0;
}

static int sn201202x_test_write(void *context, const void *data, size_t count)
{
	struct sn201202x_test *t = context;

	return t->asleep ? -ETIMEDOUT : 0;
}

static const struct regmap_bus sn201202x_test_bus = {
	.read = sn201202x_test_read,
	.write = sn201202x_test_write,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

static irqreturn_t sn201202x_test_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}

static void sn201202x_test_put_device(void *data)
{
	spmi_device_put(data);
}

static void sn201202x_test_free_irq_desc(void *data)
{
	irq_free_desc((unsigned long)data);
}

static void sn201202x_test_free_irq(void *data)
{
	struct sn201202x_test *t = data;

	free_irq(t->irq, t);
}

static struct sn201202x_test *sn201202x_test_port(struct kunit *test)
{
	struct spmi_controller *ctrl;
	struct sn201202x_test *t;
	struct tps6598x *tps;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dev = kunit_device_register(test, "sn201202x-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->dev);

	ctrl = devm_spmi_controller_alloc(t->dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctrl);
	spmi_controller_set_drvdata(ctrl, t);
	ctrl->cmd = sn201202x_test_cmd;
	t->sn.sdev = spmi_device_alloc(ctrl);
	KUNIT_ASSERT_NOT_NULL(test, t->sn.sdev);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sn201202x_test_put_device,
							t->sn.sdev), 0);
	init_completion(&t->sn.select_completion);
	init_completion(&t->sn.sleep_completion);
	init_completion(&t->sn.wake_completion);

	tps = &t->sn.cd.tps;
	tps->dev = t->dev;
	tps->data = tipd_hw_variants[TIPD_VARIANT_SN201202X];
	tps->regmap = devm_regmap_init(t->dev, &sn201202x_test_bus, t,
				       &tps6598x_regmap_config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tps->regmap);
	dev_set_drvdata(t->dev, tps);

	/* The event interrupt, which is never raised. */
	t->irq = irq_alloc_desc(NUMA_NO_NODE);
	KUNIT_ASSERT_GE(test, t->irq, 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sn201202x_test_free_irq_desc,
							(void *)(unsigned long)t->irq), 0);
	irq_set_chip_and_handler(t->irq, &dummy_irq_chip, handle_simple_irq);
	irq_clear_status_flags(t->irq, IRQ_NOREQUEST);
	KUNIT_ASSERT_EQ(test, request_irq(t->irq, sn201202x_test_irq, 0,
					  "sn201202x-test", t), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sn201202x_test_free_irq, t), 0);
	tps->irq = t->irq;

	return t;
}

static void sn201202x_sleep_cycle_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	KUNIT_ASSERT_EQ(test, sn201202x_suspend(t->dev), 0);
	KUNIT_EXPECT_TRUE(test, t->asleep);
	/* No event may be handled while the controller cannot be read. */
	KUNIT_EXPECT_FALSE(test, sn201202x_test_irq_enabled(t));

	KUNIT_EXPECT_EQ(test, sn201202x_resume(t->dev), 0);
	KUNIT_EXPECT_EQ(test, t->wakeups, 1U);
	KUNIT_EXPECT_FALSE(test, t->irq_enabled_at_wakeup);
	KUNIT_EXPECT_TRUE(test, sn201202x_test_irq_enabled(t));
}

static void sn201202x_failed_wakeup_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	KUNIT_ASSERT_EQ(test, sn201202x_suspend(t->dev), 0);
	t->wakeup_error = -EIO;
	KUNIT_EXPECT_EQ(test, sn201202x_resume(t->dev), -EIO);
	/* Events are handled again once the controller answers. */
	KUNIT_EXPECT_TRUE(test, sn201202x_test_irq_enabled(t));
}

static void sn201202x_rejected_sleep_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	t->sleep_error = -EIO;
	KUNIT_EXPECT_EQ(test, sn201202x_suspend(t->dev), -EIO);
	KUNIT_EXPECT_FALSE(test, t->asleep);
	KUNIT_EXPECT_EQ(test, t->wakeups, 0U);
	KUNIT_EXPECT_TRUE(test, sn201202x_test_irq_enabled(t));
}

static void sn201202x_unconfirmed_sleep_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	t->sleep_unconfirmed = true;
	KUNIT_EXPECT_EQ(test, sn201202x_suspend(t->dev), -ETIMEDOUT);
	/* No resume follows a failed suspend, so the rollback must wake it... */
	KUNIT_EXPECT_EQ(test, t->wakeups, 1U);
	KUNIT_EXPECT_FALSE(test, t->asleep);
	/* ...before its events are handled again. */
	KUNIT_EXPECT_FALSE(test, t->irq_enabled_at_wakeup);
	KUNIT_EXPECT_TRUE(test, sn201202x_test_irq_enabled(t));
}

static void sn201202x_unconfirmed_sleep_failed_wakeup_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	t->sleep_unconfirmed = true;
	t->wakeup_error = -EIO;
	/* The suspend error is reported, not the one from the rollback. */
	KUNIT_EXPECT_EQ(test, sn201202x_suspend(t->dev), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->wakeups, 1U);
	KUNIT_EXPECT_TRUE(test, sn201202x_test_irq_enabled(t));
}

/* As read on a J713, the dock aside */
static const struct cd321x_sleep_state sn201202x_test_charger_attach = {
	/* Attached as a USB3 device, before the data role swap */
	.data_status = 0x800000e1, .status = 0x1000600d,
};

static const struct cd321x_sleep_state sn201202x_test_charger_swapped = {
	/* A USB3 link as host for a moment after it, then none */
	.data_status = 0x80000061, .status = 0x1000904d,
};

static const struct cd321x_sleep_state sn201202x_test_charger = {
	.data_status = 0x80000001, .status = 0x100c90cd,
};

static const struct cd321x_sleep_state sn201202x_test_keyboard = {
	/* Powered by this port, as are hubs */
	.data_status = 0x00000073, .status = 0x100000fd,
};

static const struct cd321x_sleep_state sn201202x_test_dock = {
	/* Powers this port and passes USB: a USB capable source */
	.data_status = 0x80000073, .status = 0x100c90cd, .usb_comm = true,
};

/* In system sleep a charger is no link; a USB device, dock or display is. */
static void sn201202x_sleep_links_test(struct kunit *test)
{
	struct cd321x_sleep_state dp = {
		.data_status = TPS_DATA_STATUS_DATA_CONNECTION |
			       TPS_DATA_STATUS_DP_CONNECTION,
		.status = 0x100c90cd,
	};

	KUNIT_EXPECT_EQ(test, sn201202x_sleep_links(&sn201202x_test_charger_attach), 0);
	KUNIT_EXPECT_EQ(test, sn201202x_sleep_links(&sn201202x_test_charger_swapped), 0);
	KUNIT_EXPECT_EQ(test, sn201202x_sleep_links(&sn201202x_test_charger), 0);
	KUNIT_EXPECT_NE(test, sn201202x_sleep_links(&sn201202x_test_keyboard), 0);
	KUNIT_EXPECT_NE(test, sn201202x_sleep_links(&sn201202x_test_dock), 0);
	KUNIT_EXPECT_EQ(test, sn201202x_sleep_links(&dp), TPS_DATA_STATUS_DP_CONNECTION);
}

static struct kunit_case sn201202x_pm_cases[] = {
	KUNIT_CASE(sn201202x_sleep_cycle_test),
	KUNIT_CASE(sn201202x_failed_wakeup_test),
	KUNIT_CASE(sn201202x_rejected_sleep_test),
	KUNIT_CASE(sn201202x_unconfirmed_sleep_test),
	KUNIT_CASE(sn201202x_unconfirmed_sleep_failed_wakeup_test),
	KUNIT_CASE(sn201202x_sleep_links_test),
	{}
};

static struct kunit_suite sn201202x_pm_suite = {
	.name = "sn201202x-pm",
	.test_cases = sn201202x_pm_cases,
};

kunit_test_suite(sn201202x_pm_suite);
MODULE_DESCRIPTION("KUnit tests for SN201202x system sleep");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

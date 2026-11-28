// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for SN201202x system sleep and idle sleep
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/regmap.h>
#include <linux/spmi.h>
#include <linux/suspend.h>

#include "tps6598x.h"

/*
 * A controller on a simulated SPMI bus. It confirms SLEEP and WAKEUP with the
 * interrupts the driver waits for, unless the SLEEP confirmation gets lost
 * (every time, or once). Asleep, it does not confirm a register selection,
 * and it reads as zeros.
 */
struct sn201202x_test {
	struct kunit *test;
	struct sn201202x sn;
	struct device *dev;
	int irq;
	/* Bound as the SPMI bus binds it, with its runtime PM reference */
	bool bound;
	bool asleep;
	u8 selected;
	bool sleep_unconfirmed;
	bool sleep_unconfirmed_once;
	unsigned int sleeps;
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
		t->sleeps++;
		if (t->sleep_error)
			return t->sleep_error;
		t->asleep = true;
		if (t->sleep_unconfirmed_once)
			t->sleep_unconfirmed_once = false;
		else if (!t->sleep_unconfirmed)
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

/* The selected register reads back at 0, its size at 0x1f, its payload from 0x20 */
static int sn201202x_test_read_cmd(struct spmi_controller *ctrl, u8 opcode, u8 sid,
				   u16 addr, u8 *buf, size_t len)
{
	struct sn201202x_test *t = spmi_controller_get_drvdata(ctrl);

	memset(buf, 0, len);
	if (t->asleep)
		return 0;
	if (opcode == SPMI_CMD_READ && addr == 0)
		buf[0] = t->selected;
	else if (opcode == SPMI_CMD_READ && addr == 0x1f)
		buf[0] = TPS_MAX_LEN;
	else if (opcode == SPMI_CMD_EXT_READ && addr == 0x20 &&
		 t->selected == TPS_REG_MODE)
		memcpy(buf, "APP ", min(len, 4));

	return 0;
}

static int sn201202x_test_write_cmd(struct spmi_controller *ctrl, u8 opcode, u8 sid,
				    u16 addr, const u8 *buf, size_t len)
{
	struct sn201202x_test *t = spmi_controller_get_drvdata(ctrl);

	if (opcode == SPMI_CMD_ZERO_WRITE && !t->asleep)
		t->selected = buf[0];

	return 0;
}

/* A register access, through the driver's regmap bus */
static int sn201202x_test_access(struct sn201202x_test *t)
{
	u8 mode[4];
	int err;

	err = regmap_raw_read(t->sn.cd.tps.regmap, TPS_REG_MODE, mode, sizeof(mode));
	if (!err && memcmp(mode, "APP ", 4))
		err = -EIO;
	return err;
}

static irqreturn_t sn201202x_test_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}

/* The driver's runtime PM callbacks, for the test device */
static const struct dev_pm_ops sn201202x_test_pm_ops = {
	SET_RUNTIME_PM_OPS(sn201202x_runtime_suspend, sn201202x_runtime_resume, NULL)
};

static const struct device_type sn201202x_test_type = {
	.name = "sn201202x-test",
	.pm = &sn201202x_test_pm_ops,
};

static bool sn201202x_test_sleep_event(struct tps6598x *tps)
{
	return true;
}

/* As the SPMI bus probes the driver, from its runtime PM setup */
static void sn201202x_test_bind(struct sn201202x_test *t)
{
	pm_runtime_get_noresume(t->dev);
	pm_runtime_set_active(t->dev);
	pm_runtime_enable(t->dev);
	sn201202x_idle_sleep_start(&t->sn.cd.tps);
	t->bound = true;
}

/* As the SPMI bus removes the driver, without the driver's own teardown */
static void sn201202x_test_unbind(struct sn201202x_test *t)
{
	pm_runtime_get_sync(t->dev);
	sn201202x_idle_sleep_stop(&t->sn.cd.tps);
	pm_runtime_put_noidle(t->dev);
	/* Held until runtime PM is off: no idle sleep after the driver's remove */
	KUNIT_EXPECT_EQ(t->test, atomic_read(&t->dev->power.usage_count), 1);

	pm_runtime_disable(t->dev);
	pm_runtime_set_suspended(t->dev);
	pm_runtime_put_noidle(t->dev);
	t->bound = false;
}

static void sn201202x_test_unbind_action(void *data)
{
	struct sn201202x_test *t = data;

	if (t->bound)
		sn201202x_test_unbind(t);
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

/* A T8132 controller, or with @idle_sleep false, one without idle sleep */
static struct sn201202x_test *__sn201202x_test_port(struct kunit *test, bool idle_sleep)
{
	struct spmi_controller *ctrl;
	struct sn201202x_test *t;
	struct tps6598x *tps;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->test = test;
	t->dev = kunit_device_register(test, "sn201202x-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->dev);

	ctrl = devm_spmi_controller_alloc(t->dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctrl);
	spmi_controller_set_drvdata(ctrl, t);
	ctrl->cmd = sn201202x_test_cmd;
	ctrl->read_cmd = sn201202x_test_read_cmd;
	ctrl->write_cmd = sn201202x_test_write_cmd;
	t->sn.sdev = spmi_device_alloc(ctrl);
	KUNIT_ASSERT_NOT_NULL(test, t->sn.sdev);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sn201202x_test_put_device,
							t->sn.sdev), 0);
	init_completion(&t->sn.select_completion);
	init_completion(&t->sn.sleep_completion);
	init_completion(&t->sn.wake_completion);
	/* T8132 has no select notification */
	t->sn.select_polled = true;

	tps = &t->sn.cd.tps;
	tps->dev = t->dev;
	tps->data = tipd_hw_variants[TIPD_VARIANT_SN201202X];
	t->sn.idle_sleep = idle_sleep;
	if (idle_sleep)
		tps->sleep_event = sn201202x_test_sleep_event;
	spmi_device_set_drvdata(t->sn.sdev, tps);
	tps->regmap = devm_regmap_init(t->dev, &regmap_sn201202x, t->sn.sdev,
				       &tps6598x_regmap_config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tps->regmap);
	dev_set_drvdata(t->dev, tps);
	mutex_init(&tps->lock);

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

	/*
	 * Awake as probe leaves it; no case waits out the autosuspend delay.
	 * Bound last, so it is unbound first: no runtime PM callback can
	 * reach the interrupt or the SPMI device once they are gone.
	 */
	t->dev->type = &sn201202x_test_type;
	sn201202x_test_bind(t);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sn201202x_test_unbind_action,
							t), 0);
	pm_runtime_set_autosuspend_delay(t->dev, 60 * MSEC_PER_SEC);

	return t;
}

static struct sn201202x_test *sn201202x_test_port(struct kunit *test)
{
	return __sn201202x_test_port(test, true);
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
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
}

static void sn201202x_failed_wakeup_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	KUNIT_ASSERT_EQ(test, sn201202x_suspend(t->dev), 0);
	t->wakeup_error = -EIO;
	KUNIT_EXPECT_EQ(test, sn201202x_resume(t->dev), -EIO);
	/* Events are handled again once the controller answers. */
	KUNIT_EXPECT_TRUE(test, sn201202x_test_irq_enabled(t));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
}

static void sn201202x_rejected_sleep_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	t->sleep_error = -EIO;
	KUNIT_EXPECT_EQ(test, sn201202x_suspend(t->dev), -EIO);
	KUNIT_EXPECT_FALSE(test, t->asleep);
	KUNIT_EXPECT_EQ(test, t->wakeups, 0U);
	KUNIT_EXPECT_TRUE(test, sn201202x_test_irq_enabled(t));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
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
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
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
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
}

/* An idle controller sleeps, and the next register access wakes it. */
static void sn201202x_idle_sleep_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	KUNIT_ASSERT_EQ(test, pm_runtime_suspend(t->dev), 0);
	KUNIT_EXPECT_TRUE(test, t->asleep);

	KUNIT_EXPECT_EQ(test, sn201202x_test_access(t), 0);
	KUNIT_EXPECT_EQ(test, t->wakeups, 1U);
	KUNIT_EXPECT_FALSE(test, t->asleep);
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(t->dev));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
}

/* The system sleep event path wakes it itself; its accesses leave runtime PM alone. */
static void sn201202x_atomic_access_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	KUNIT_ASSERT_EQ(test, pm_runtime_suspend(t->dev), 0);
	t->sn.atomic = true;
	KUNIT_EXPECT_EQ(test, sn201202x_test_access(t), -ETIMEDOUT);
	t->sn.atomic = false;
	KUNIT_EXPECT_EQ(test, t->wakeups, 0U);
	KUNIT_EXPECT_TRUE(test, pm_runtime_suspended(t->dev));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
}

/* A lost SLEEP confirmation leaves it awake, and runtime PM tries again. */
static void sn201202x_idle_sleep_retry_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);
	int i;

	/* Busy from now, a short delay schedules the first attempt, which loses its SLEEP. */
	t->sleep_unconfirmed_once = true;
	pm_runtime_mark_last_busy(t->dev);
	pm_runtime_set_autosuspend_delay(t->dev, 10);
	for (i = 0; i < 100 && !pm_runtime_suspended(t->dev); i++)
		msleep(20);
	KUNIT_EXPECT_TRUE(test, pm_runtime_suspended(t->dev));
	KUNIT_EXPECT_EQ(test, t->sleeps, 2U);
	KUNIT_EXPECT_EQ(test, t->wakeups, 1U);
	KUNIT_EXPECT_TRUE(test, t->asleep);
}

/* Without SLEEP confirmations it gives up after a few tries, until the next access. */
static void sn201202x_idle_sleep_gives_up_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);
	int i;

	t->sleep_unconfirmed = true;
	pm_runtime_mark_last_busy(t->dev);
	pm_runtime_set_autosuspend_delay(t->dev, 10);
	for (i = 0; i < 100 && t->sleeps < 3; i++)
		msleep(20);
	/* Long enough for a fourth try, which must not come */
	msleep(200);
	KUNIT_EXPECT_EQ(test, t->sleeps, 3U);
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(t->dev));
	KUNIT_EXPECT_FALSE(test, t->asleep);

	/* The next access lets it try again */
	KUNIT_ASSERT_EQ(test, sn201202x_test_access(t), 0);
	for (i = 0; i < 100 && t->sleeps < 6; i++)
		msleep(20);
	msleep(200);
	KUNIT_EXPECT_EQ(test, t->sleeps, 6U);
}

/* An access between failed tries counts them afresh. */
static void sn201202x_idle_sleep_tries_restart_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	t->sleep_unconfirmed = true;
	/* Not autosuspend, so runtime PM does not try again by itself */
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), -EAGAIN);
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), -EAGAIN);
	KUNIT_ASSERT_EQ(test, sn201202x_test_access(t), 0);
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), -EAGAIN);
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), -EAGAIN);
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->sleeps, 5U);
}

/* Not woken after an unconfirmed SLEEP, it is taken as asleep; the port works on. */
static void sn201202x_unconfirmed_idle_sleep_failed_wakeup_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	t->sleep_unconfirmed = true;
	t->wakeup_error = -EIO;
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), 0);
	KUNIT_EXPECT_TRUE(test, pm_runtime_suspended(t->dev));

	t->wakeup_error = 0;
	KUNIT_EXPECT_EQ(test, sn201202x_test_access(t), 0);
	KUNIT_EXPECT_FALSE(test, t->asleep);
}

/* A display link keeps it awake. */
static void sn201202x_display_link_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	t->sn.cd.tps.status = TPS_STATUS_PLUG_PRESENT;
	t->sn.cd.tps.data_status = TPS_DATA_STATUS_DATA_CONNECTION |
				   TPS_DATA_STATUS_DP_CONNECTION;
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->sleeps, 0U);
	KUNIT_EXPECT_FALSE(test, t->asleep);
}

/* So does Apple's DBMa debug mode, found at probe... */
static void sn201202x_dbma_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	t->sn.cd.tps.dbma_at_probe = 1;
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->sleeps, 0U);
	KUNIT_EXPECT_FALSE(test, t->asleep);
}

static void sn201202x_test_debugfs_off(void *data)
{
	struct tps6598x *tps = data;

	tps->debugfs = NULL;
}

/* ...or entered through debugfs, for as long as a file holds it. */
static void sn201202x_dbma_held_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);
	struct tipd_debugfs *debugfs;

	if (!IS_ENABLED(CONFIG_DEBUG_FS))
		kunit_skip(test, "needs CONFIG_DEBUG_FS");
	debugfs = kunit_kzalloc(test, sizeof(*debugfs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, debugfs);
	debugfs->dbma_holders = 1;
	t->sn.cd.tps.debugfs = debugfs;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sn201202x_test_debugfs_off,
							&t->sn.cd.tps), 0);

	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->sleeps, 0U);

	debugfs->dbma_holders = 0;
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), 0);
	KUNIT_EXPECT_TRUE(test, t->asleep);
}

/* Unbound from idle sleep, it is woken and the bus gets its reference back. */
static void sn201202x_unbind_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	KUNIT_ASSERT_EQ(test, pm_runtime_suspend(t->dev), 0);
	sn201202x_test_unbind(t);
	KUNIT_EXPECT_EQ(test, t->wakeups, 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);

	/* Bound again, it sleeps idle again */
	sn201202x_test_bind(t);
	pm_runtime_set_autosuspend_delay(t->dev, 60 * MSEC_PER_SEC);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), 0);
	KUNIT_EXPECT_TRUE(test, t->asleep);
}

/* Without the system sleep event path there is no idle sleep. */
static void sn201202x_no_idle_sleep_test(struct kunit *test)
{
	struct sn201202x_test *t = __sn201202x_test_port(test, false);

	KUNIT_EXPECT_LT(test, pm_runtime_suspend(t->dev), 0);
	KUNIT_EXPECT_EQ(test, sn201202x_test_access(t), 0);
	KUNIT_EXPECT_EQ(test, t->sleeps, 0U);
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(t->dev));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 1);

	sn201202x_test_unbind(t);
	KUNIT_EXPECT_EQ(test, t->sleeps, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
}

/* Unplugged in system sleep, the cached data status stays, but not the plug. */
static void sn201202x_display_unplugged_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	t->sn.cd.tps.data_status = TPS_DATA_STATUS_DATA_CONNECTION |
				   TPS_DATA_STATUS_DP_CONNECTION;
	KUNIT_EXPECT_EQ(test, pm_runtime_suspend(t->dev), 0);
	KUNIT_EXPECT_TRUE(test, t->asleep);
}

/* System sleep wakes an idle controller before its own SLEEP handshake. */
static void sn201202x_sleep_from_idle_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	KUNIT_ASSERT_EQ(test, pm_runtime_suspend(t->dev), 0);
	KUNIT_ASSERT_EQ(test, sn201202x_suspend(t->dev), 0);
	KUNIT_EXPECT_EQ(test, t->wakeups, 1U);
	KUNIT_EXPECT_TRUE(test, t->asleep);
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(t->dev));

	KUNIT_EXPECT_EQ(test, sn201202x_resume(t->dev), 0);
	KUNIT_EXPECT_EQ(test, t->wakeups, 2U);
	KUNIT_EXPECT_FALSE(test, t->asleep);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
}

/* An idle controller that does not wake sleeps on through system sleep. */
static void sn201202x_sleep_from_idle_failed_wakeup_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);

	KUNIT_ASSERT_EQ(test, pm_runtime_suspend(t->dev), 0);
	t->wakeup_error = -EIO;
	KUNIT_EXPECT_EQ(test, sn201202x_suspend(t->dev), 0);
	KUNIT_EXPECT_TRUE(test, t->asleep);
	KUNIT_EXPECT_FALSE(test, t->sn.sleep_watch);
	KUNIT_EXPECT_FALSE(test, sn201202x_test_irq_enabled(t));

	t->wakeup_error = 0;
	KUNIT_EXPECT_EQ(test, sn201202x_resume(t->dev), 0);
	KUNIT_EXPECT_FALSE(test, t->asleep);
	/* Woken through runtime PM, so the next access sends no second WAKEUP */
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(t->dev));
	KUNIT_EXPECT_TRUE(test, sn201202x_test_irq_enabled(t));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
}

static void sn201202x_test_restore_target(void *data)
{
	pm_suspend_target_state = (suspend_state_t)(unsigned long)data;
}

static void sn201202x_test_wakeup_off(void *data)
{
	device_init_wakeup(data, false);
}

/* So does suspend-to-idle with the wakeup watch, whose baseline read needs it awake. */
static void sn201202x_watched_sleep_from_idle_test(struct kunit *test)
{
	struct sn201202x_test *t = sn201202x_test_port(test);
	suspend_state_t target;

	KUNIT_ASSERT_EQ(test, device_init_wakeup(t->dev, true), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sn201202x_test_wakeup_off,
							t->dev), 0);
	target = pm_suspend_target_state;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sn201202x_test_restore_target,
							(void *)(unsigned long)target), 0);
	pm_suspend_target_state = PM_SUSPEND_TO_IDLE;

	KUNIT_ASSERT_EQ(test, pm_runtime_suspend(t->dev), 0);
	KUNIT_ASSERT_EQ(test, sn201202x_suspend(t->dev), 0);
	KUNIT_EXPECT_TRUE(test, t->sn.sleep_watch);
	KUNIT_EXPECT_TRUE(test, t->asleep);

	KUNIT_EXPECT_EQ(test, sn201202x_resume(t->dev), 0);
	KUNIT_EXPECT_FALSE(test, t->sn.sleep_watch);
	KUNIT_EXPECT_FALSE(test, t->asleep);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->dev->power.usage_count), 0);
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

static void sn201202x_ldcm_dry_test(struct kunit *test)
{
	u8 status[SN201202X_LDCM_STATUS_LEN] = {};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_decode(status),
			TYPEC_CONTAMINANT_NONE);
}

static void sn201202x_ldcm_liquid_test(struct kunit *test)
{
	u8 status[SN201202X_LDCM_STATUS_LEN] = {};

	status[0] = BIT(2);
	status[1] = BIT(4);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_decode(status),
			TYPEC_CONTAMINANT_LIQUID);

	status[0] = 0;
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_decode(status),
			TYPEC_CONTAMINANT_NONE);
	status[0] = BIT(2);
	status[1] = 0;
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_decode(status),
			TYPEC_CONTAMINANT_NONE);
}

static void sn201202x_ldcm_busy_test(struct kunit *test)
{
	u8 status[SN201202X_LDCM_STATUS_LEN] = {
		[0] = BIT(2),
		[1] = BIT(1) | BIT(4),
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_decode(status),
			TYPEC_CONTAMINANT_UNKNOWN);
}

enum sn201202x_ldcm_test_action {
	LDCM_TEST_READ,
	LDCM_TEST_MITIGATE_ON,
	LDCM_TEST_MITIGATE_OFF,
	LDCM_TEST_PUBLISH_UNKNOWN,
	LDCM_TEST_PUBLISH_NONE,
	LDCM_TEST_PUBLISH_LIQUID,
	LDCM_TEST_OVERDUE,
	LDCM_TEST_RERUN,
	LDCM_TEST_HELD,
	LDCM_TEST_NOT_HELD,
};

struct sn201202x_ldcm_test {
	enum typec_contaminant reads[2];
	int read_rets[2];
	int mitigation_rets[2];
	/* Waits overrun in a row, as the wait for a first result answers */
	unsigned int overdue;
	int rerun_ret;
	/* Liquid reported since the last dry result: kept as the driver keeps it */
	bool wet;
	unsigned int results;
	/* Status reads that failed in a row, as the driver counts them */
	unsigned int read_failures;
	enum sn201202x_ldcm_test_action actions[8];
	unsigned int read_count;
	unsigned int mitigation_count;
	unsigned int action_count;
};

static int sn201202x_ldcm_test_read(void *ctx,
				    enum typec_contaminant *contaminant)
{
	struct sn201202x_ldcm_test *t = ctx;
	unsigned int i = t->read_count++;

	t->actions[t->action_count++] = LDCM_TEST_READ;
	*contaminant = t->reads[i];
	return t->read_rets[i];
}

static int sn201202x_ldcm_test_mitigate(void *ctx, bool enable)
{
	struct sn201202x_ldcm_test *t = ctx;
	unsigned int i = t->mitigation_count++;

	t->actions[t->action_count++] = enable ? LDCM_TEST_MITIGATE_ON :
						      LDCM_TEST_MITIGATE_OFF;
	return t->mitigation_rets[i];
}

static void sn201202x_ldcm_test_publish(void *ctx,
				       enum typec_contaminant contaminant)
{
	struct sn201202x_ldcm_test *t = ctx;

	switch (contaminant) {
	case TYPEC_CONTAMINANT_UNKNOWN:
		t->actions[t->action_count++] = LDCM_TEST_PUBLISH_UNKNOWN;
		break;
	case TYPEC_CONTAMINANT_NONE:
		t->actions[t->action_count++] = LDCM_TEST_PUBLISH_NONE;
		break;
	case TYPEC_CONTAMINANT_LIQUID:
		t->actions[t->action_count++] = LDCM_TEST_PUBLISH_LIQUID;
		break;
	}
}

static void sn201202x_ldcm_test_result(void *ctx,
				       enum typec_contaminant contaminant)
{
	struct sn201202x_ldcm_test *t = ctx;

	t->results++;
	t->wet = contaminant == TYPEC_CONTAMINANT_LIQUID;
}

static bool sn201202x_ldcm_test_wet(void *ctx)
{
	struct sn201202x_ldcm_test *t = ctx;

	return t->wet;
}

static unsigned int sn201202x_ldcm_test_overdue(void *ctx)
{
	struct sn201202x_ldcm_test *t = ctx;

	t->actions[t->action_count++] = LDCM_TEST_OVERDUE;
	return t->overdue;
}

static void sn201202x_ldcm_test_held(void *ctx, bool mitigated)
{
	struct sn201202x_ldcm_test *t = ctx;

	t->actions[t->action_count++] = mitigated ? LDCM_TEST_HELD :
						    LDCM_TEST_NOT_HELD;
}

static int sn201202x_ldcm_test_rerun(void *ctx)
{
	struct sn201202x_ldcm_test *t = ctx;

	t->actions[t->action_count++] = LDCM_TEST_RERUN;
	return t->rerun_ret;
}

static unsigned int sn201202x_ldcm_test_read_failures(void *ctx)
{
	struct sn201202x_ldcm_test *t = ctx;

	return t->read_failures;
}

static const struct sn201202x_ldcm_ops sn201202x_ldcm_test_ops = {
	.read = sn201202x_ldcm_test_read,
	.set_mitigation = sn201202x_ldcm_test_mitigate,
	.publish = sn201202x_ldcm_test_publish,
	.result = sn201202x_ldcm_test_result,
	.wet = sn201202x_ldcm_test_wet,
	.overdue = sn201202x_ldcm_test_overdue,
	.held = sn201202x_ldcm_test_held,
	.rerun = sn201202x_ldcm_test_rerun,
	.read_failures = sn201202x_ldcm_test_read_failures,
};

static void sn201202x_ldcm_test_actions(struct kunit *test,
					struct sn201202x_ldcm_test *t,
					const enum sn201202x_ldcm_test_action *actions,
					size_t count)
{
	KUNIT_ASSERT_EQ(test, t->action_count, count);
	KUNIT_EXPECT_MEMEQ(test, t->actions, actions, count * sizeof(*actions));
}

static void sn201202x_ldcm_liquid_order_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_LIQUID,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_LIQUID },
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN), 0);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
	KUNIT_EXPECT_TRUE(test, t.wet);
}

static void sn201202x_ldcm_read_failure_order_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_UNKNOWN,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN },
		.read_rets = { -EIO },
		/* A failed read is never taken for an overdue measurement. */
		.overdue = 2,
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN), -EIO);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

static void sn201202x_ldcm_mitigation_failure_order_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_UNKNOWN,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_LIQUID },
		.mitigation_rets = { -ETIMEDOUT },
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-ETIMEDOUT);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

static void sn201202x_ldcm_dry_order_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_OFF,
		LDCM_TEST_READ,
		LDCM_TEST_PUBLISH_NONE,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_NONE, TYPEC_CONTAMINANT_NONE },
		.wet = true,
	};

	/* A dry result ends what liquid reported before it. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_LIQUID), 0);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
	KUNIT_EXPECT_FALSE(test, t.wet);
	KUNIT_EXPECT_EQ(test, t.results, 2U);
}

static void sn201202x_ldcm_dry_then_busy_test(struct kunit *test)
{
	static const enum typec_contaminant last[] = {
		TYPEC_CONTAMINANT_UNKNOWN,
		TYPEC_CONTAMINANT_NONE,
		TYPEC_CONTAMINANT_LIQUID,
	};
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_OFF,
		LDCM_TEST_READ,
		LDCM_TEST_PUBLISH_NONE,
	};
	unsigned int i;

	/* Removing mitigation can start a measurement after a fresh dry result. */
	for (i = 0; i < ARRAY_SIZE(last); i++) {
		struct sn201202x_ldcm_test t = {
			.reads = { TYPEC_CONTAMINANT_NONE, TYPEC_CONTAMINANT_UNKNOWN },
		};

		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
								  &sn201202x_ldcm_test_ops,
								  last[i]),
				-EINPROGRESS);
		sn201202x_ldcm_test_actions(test, &t, actions,
					    ARRAY_SIZE(actions));
	}
}

static void sn201202x_ldcm_dry_then_read_failure_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_OFF,
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_UNKNOWN,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_NONE, TYPEC_CONTAMINANT_UNKNOWN },
		.read_rets = { 0, -EIO },
	};

	/* A failed second read must still mitigate, even after a dry result. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN), -EIO);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

static void sn201202x_ldcm_dry_race_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_OFF,
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_LIQUID,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_NONE, TYPEC_CONTAMINANT_LIQUID },
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN), 0);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
	KUNIT_EXPECT_TRUE(test, t.wet);
}

static void sn201202x_ldcm_dry_race_mitigation_failure_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_OFF,
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_UNKNOWN,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_NONE, TYPEC_CONTAMINANT_LIQUID },
		.mitigation_rets = { 0, -ETIMEDOUT },
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-ETIMEDOUT);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

static void sn201202x_ldcm_busy_keeps_result_test(struct kunit *test)
{
	static const enum typec_contaminant last[] = {
		TYPEC_CONTAMINANT_NONE,
		TYPEC_CONTAMINANT_LIQUID,
	};
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
	};
	unsigned int i;

	/*
	 * A running measurement neither mitigates nor republishes anything.
	 * With a result, however long it runs: liquid stays mitigated.
	 */
	for (i = 0; i < ARRAY_SIZE(last); i++) {
		struct sn201202x_ldcm_test t = {
			.reads = { TYPEC_CONTAMINANT_UNKNOWN },
			.overdue = 2,
		};

		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
								  &sn201202x_ldcm_test_ops,
								  last[i]),
				-EINPROGRESS);
		sn201202x_ldcm_test_actions(test, &t, actions,
					    ARRAY_SIZE(actions));
	}
}

static void sn201202x_ldcm_busy_without_result_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_OVERDUE,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN },
	};

	/*
	 * Without an earlier result the port stays down (unknown is already
	 * published), but mitigation is not forced on: a measurement that a
	 * mitigation command interrupts need not finish.
	 */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-EINPROGRESS);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

static void sn201202x_ldcm_overdue_once_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_OVERDUE,
		LDCM_TEST_RERUN,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN },
		.overdue = 1,
	};

	/*
	 * A first overrun makes the controller measure again, and the port
	 * stays down meanwhile: nothing is published.
	 */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-EINPROGRESS);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

static void sn201202x_ldcm_overdue_once_rerun_failure_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_OVERDUE,
		LDCM_TEST_RERUN,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN },
		.overdue = 1,
		.rerun_ret = -ETIMEDOUT,
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-ETIMEDOUT);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

/*
 * The measurement run again overran too: the port is held mitigated and
 * stays "unknown", never taken for dry. Nothing else is read or sent. The
 * hold starts with the mitigation that went through.
 */
static void sn201202x_ldcm_overdue_twice_holds_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_OVERDUE,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_HELD,
		LDCM_TEST_PUBLISH_UNKNOWN,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN, TYPEC_CONTAMINANT_NONE },
		.overdue = 2,
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-EAGAIN);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
	KUNIT_EXPECT_EQ(test, t.read_count, 1U);
	KUNIT_EXPECT_EQ(test, t.results, 0U);
}

/* A dry result read during a hold enables the port, as any dry result. */
static void sn201202x_ldcm_hold_dry_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_OFF,
		LDCM_TEST_READ,
		LDCM_TEST_PUBLISH_NONE,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_NONE, TYPEC_CONTAMINANT_NONE },
		.overdue = 2,
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN), 0);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
	KUNIT_EXPECT_EQ(test, t.results, 2U);
}

/* Liquid read during a hold is mitigated and reported, as any liquid. */
static void sn201202x_ldcm_hold_liquid_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_LIQUID,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_LIQUID },
		.overdue = 2,
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN), 0);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
	KUNIT_EXPECT_TRUE(test, t.wet);
}

static void sn201202x_ldcm_overdue_mitigation_failure_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_OVERDUE,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_NOT_HELD,
		LDCM_TEST_PUBLISH_UNKNOWN,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN },
		.mitigation_rets = { -ETIMEDOUT },
		.overdue = 2,
	};

	/* A hold whose command fails keeps the port down, and is no hold yet. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-ETIMEDOUT);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

/*
 * Liquid, then a mitigation command that fails: "unknown" is published, but
 * the port is still known to be wet. The measurement that follows is not
 * waited for as one without a result: the port is mitigated, and the liquid
 * reported again.
 */
static void sn201202x_ldcm_wet_never_overdue_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action first[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_UNKNOWN,
	};
	const enum sn201202x_ldcm_test_action then[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_LIQUID,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_LIQUID },
		.mitigation_rets = { -ETIMEDOUT },
	};
	struct sn201202x_ldcm_test t2 = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN },
		.overdue = 2,
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-ETIMEDOUT);
	sn201202x_ldcm_test_actions(test, &t, first, ARRAY_SIZE(first));
	KUNIT_ASSERT_TRUE(test, t.wet);

	t2.wet = t.wet;
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t2,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-EINPROGRESS);
	sn201202x_ldcm_test_actions(test, &t2, then, ARRAY_SIZE(then));
	KUNIT_EXPECT_TRUE(test, t2.wet);
}

static void sn201202x_ldcm_wet_mitigation_failure_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN },
		.mitigation_rets = { -ETIMEDOUT },
		.overdue = 2,
		.wet = true,
	};

	/* Still wet: the port stays down, and the command is retried. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_UNKNOWN),
			-ETIMEDOUT);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

static void sn201202x_ldcm_liquid_read_failure_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_LIQUID,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN },
		.read_rets = { -EIO },
		.wet = true,
	};

	/* A failed read is no result: the liquid stays reported. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_LIQUID), -EIO);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
	KUNIT_EXPECT_TRUE(test, t.wet);
	KUNIT_EXPECT_EQ(test, t.results, 0U);
}

static void sn201202x_ldcm_liquid_mitigation_failure_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_LIQUID,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_LIQUID },
		.mitigation_rets = { -ETIMEDOUT },
	};

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_LIQUID),
			-ETIMEDOUT);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

static void sn201202x_ldcm_wait_test(struct kunit *test)
{
	unsigned long stuck = msecs_to_jiffies(SN201202X_LDCM_STUCK_MS);
	struct sn201202x_ldcm_wait wait = {};
	unsigned long start = -(stuck / 2);	/* across a jiffies wrap */

	/* The first measurement without a result starts the wait. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start), 0U);
	KUNIT_EXPECT_TRUE(test, wait.waiting);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 0, start), SN201202X_LDCM_RETRY_MS);
	/* A failed read is retried slowly too. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 1, start),
			SN201202X_LDCM_SLOW_RETRY_MS);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + stuck - 1), 0U);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + stuck), 1U);
	/* Overrun once: retried slowly, and the measurement run again gets a whole wait. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 0, start + stuck),
			SN201202X_LDCM_SLOW_RETRY_MS);
	sn201202x_ldcm_wait_rerun(&wait, start + stuck);
	KUNIT_EXPECT_FALSE(test, wait.rerun_due);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + 2 * stuck - 1), 0U);
	/* Overrun again: held, and the hold lasts, it does not end in dry. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + 2 * stuck), 2U);
	KUNIT_EXPECT_EQ(test, wait.holds, 1U);
	KUNIT_EXPECT_FALSE(test, wait.held);
	sn201202x_ldcm_wait_held(&wait, true, start + 2 * stuck);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + 3 * stuck), 2U);

	/* A result ends the wait; the next measurement starts another. */
	wait = (struct sn201202x_ldcm_wait){};
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 0, start), SN201202X_LDCM_RETRY_MS);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + 10 * stuck), 0U);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + 11 * stuck - 1), 0U);
}

/*
 * A measurement that was not started again (the command failed or was not
 * sent) does not use up the second wait: every update tries again, and the
 * second wait runs from the restart that went through.
 */
static void sn201202x_ldcm_wait_rerun_failure_test(struct kunit *test)
{
	unsigned long stuck = msecs_to_jiffies(SN201202X_LDCM_STUCK_MS);
	struct sn201202x_ldcm_wait wait = {};
	unsigned long start = -(stuck / 2);	/* across a jiffies wrap */
	unsigned long rerun = start + 3 * stuck;

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start), 0U);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + stuck), 1U);
	KUNIT_EXPECT_TRUE(test, wait.rerun_due);
	/* No restart: the next update tries again, slowly, never the second overrun. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + stuck + 1), 1U);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 0, start + stuck + 1),
			SN201202X_LDCM_SLOW_RETRY_MS);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, start + 2 * stuck), 1U);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, rerun), 1U);

	/* The restart went through: a whole wait from then before the hold. */
	sn201202x_ldcm_wait_rerun(&wait, rerun);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, rerun + stuck - 1), 0U);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 0, rerun + stuck - 1),
			SN201202X_LDCM_SLOW_RETRY_MS);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, rerun + stuck), 2U);
	KUNIT_EXPECT_FALSE(test, wait.rerun_due);
}

/*
 * Holds: each ends in a restart of the measurement, which gets a whole wait,
 * and is then twice as long as the last, up to SN201202X_LDCM_HOLD_MAX_MS.
 * A hold is not polled: the retry comes at its end. A restart that fails at
 * the end of a hold stays due, as the first one.
 */
static void sn201202x_ldcm_wait_hold_backoff_test(struct kunit *test)
{
	static const unsigned int hold_ms[] = {
		10000, 20000, 40000, 80000, 160000, 300000, 300000, 300000,
	};
	unsigned long stuck = msecs_to_jiffies(SN201202X_LDCM_STUCK_MS);
	struct sn201202x_ldcm_wait wait = {};
	unsigned long now = -(stuck / 2);	/* across a jiffies wrap */
	unsigned int i;

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_hold_ms(UINT_MAX), SN201202X_LDCM_HOLD_MAX_MS);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, now), 0U);
	now += stuck;
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, now), 1U);
	sn201202x_ldcm_wait_rerun(&wait, now);

	for (i = 0; i < ARRAY_SIZE(hold_ms); i++) {
		unsigned long hold = msecs_to_jiffies(hold_ms[i]);

		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_hold_ms(i + 1), hold_ms[i]);
		now += stuck;
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, now), 2U);
		KUNIT_EXPECT_EQ(test, wait.holds, i + 1);
		/* Its mitigation fails first: retried as a failed command, no hold yet. */
		sn201202x_ldcm_wait_held(&wait, false, now);
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 0, now),
				SN201202X_LDCM_SLOW_RETRY_MS);
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, now + stuck), 2U);
		now += stuck;
		sn201202x_ldcm_wait_held(&wait, true, now);
		/* Further mitigations that go through do not stretch the hold. */
		sn201202x_ldcm_wait_held(&wait, true, now + hold / 2);
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 0, now),
				jiffies_to_msecs(hold));
		/* Not even a failed read polls a hold. */
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 1, now + hold / 2),
				max_t(unsigned int, jiffies_to_msecs(hold - hold / 2),
				      SN201202X_LDCM_SLOW_RETRY_MS));
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, now + hold - 1), 2U);
		/* Late (an update that came after the end) is retried at once, slowly. */
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 0, now + hold + stuck),
				SN201202X_LDCM_SLOW_RETRY_MS);
		now += hold;
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, now), 1U);
		KUNIT_EXPECT_TRUE(test, wait.rerun_due);
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&wait, 0, now),
				SN201202X_LDCM_SLOW_RETRY_MS);
		/* The first restart attempt fails; the second goes through. */
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, now + stuck), 1U);
		now += stuck;
		sn201202x_ldcm_wait_rerun(&wait, now);
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_wait_overdue(&wait, now + stuck - 1), 0U);
	}
}

/*
 * The whole path of a port without an earlier result, through
 * sn201202x_ldcm_update_state() with the driver's wait and a clock that runs
 * by the driver's retry delays: a controller that reports no result, or one
 * from a given time on.
 */
struct sn201202x_ldcm_sim {
	struct sn201202x_ldcm_wait wait;
	unsigned long now;
	/* The controller reports dry from dry_at on, if it dries */
	bool dries;
	unsigned long dry_at;
	bool mitigated;
	bool wet;
	enum typec_contaminant published;
	unsigned int reruns;
	unsigned int updates;
	/* Mitigation commands that fail before the first one goes through */
	unsigned int mitigation_failures;
	int mitigation_err;
	unsigned int mitigations;
};

static int sn201202x_ldcm_sim_read(void *ctx, enum typec_contaminant *contaminant)
{
	struct sn201202x_ldcm_sim *sim = ctx;

	*contaminant = sim->dries && !time_before(sim->now, sim->dry_at) ?
		       TYPEC_CONTAMINANT_NONE : TYPEC_CONTAMINANT_UNKNOWN;
	return 0;
}

static int sn201202x_ldcm_sim_mitigate(void *ctx, bool enable)
{
	struct sn201202x_ldcm_sim *sim = ctx;

	sim->mitigations++;
	if (sim->mitigation_failures) {
		sim->mitigation_failures--;
		return sim->mitigation_err;
	}
	sim->mitigated = enable;
	return 0;
}

static void sn201202x_ldcm_sim_publish(void *ctx, enum typec_contaminant contaminant)
{
	struct sn201202x_ldcm_sim *sim = ctx;

	sim->published = contaminant;
}

static void sn201202x_ldcm_sim_result(void *ctx, enum typec_contaminant contaminant)
{
	struct sn201202x_ldcm_sim *sim = ctx;

	sim->wet = contaminant == TYPEC_CONTAMINANT_LIQUID;
	sim->wait = (struct sn201202x_ldcm_wait){};
}

static bool sn201202x_ldcm_sim_wet(void *ctx)
{
	struct sn201202x_ldcm_sim *sim = ctx;

	return sim->wet;
}

static unsigned int sn201202x_ldcm_sim_overdue(void *ctx)
{
	struct sn201202x_ldcm_sim *sim = ctx;

	return sn201202x_ldcm_wait_overdue(&sim->wait, sim->now);
}

static void sn201202x_ldcm_sim_held(void *ctx, bool mitigated)
{
	struct sn201202x_ldcm_sim *sim = ctx;

	sn201202x_ldcm_wait_held(&sim->wait, mitigated, sim->now);
}

static int sn201202x_ldcm_sim_rerun(void *ctx)
{
	struct sn201202x_ldcm_sim *sim = ctx;

	sim->reruns++;
	sim->mitigated = false;
	sn201202x_ldcm_wait_rerun(&sim->wait, sim->now);
	return 0;
}

static unsigned int sn201202x_ldcm_sim_read_failures(void *ctx)
{
	return 0;
}

static const struct sn201202x_ldcm_ops sn201202x_ldcm_sim_ops = {
	.read = sn201202x_ldcm_sim_read,
	.set_mitigation = sn201202x_ldcm_sim_mitigate,
	.publish = sn201202x_ldcm_sim_publish,
	.result = sn201202x_ldcm_sim_result,
	.wet = sn201202x_ldcm_sim_wet,
	.overdue = sn201202x_ldcm_sim_overdue,
	.held = sn201202x_ldcm_sim_held,
	.rerun = sn201202x_ldcm_sim_rerun,
	.read_failures = sn201202x_ldcm_sim_read_failures,
};

/* Updates until @end or until the port is enabled; the last return */
static int sn201202x_ldcm_sim_run(struct kunit *test, struct sn201202x_ldcm_sim *sim,
				  unsigned long end)
{
	int ret = -EINPROGRESS;

	while (time_before(sim->now, end)) {
		sim->updates++;
		ret = sn201202x_ldcm_update_state(sim, &sn201202x_ldcm_sim_ops,
						  sim->published);
		if (sim->published == TYPEC_CONTAMINANT_NONE)
			break;
		KUNIT_ASSERT_TRUE(test, ret == -EINPROGRESS || ret == -EAGAIN);
		sim->now += msecs_to_jiffies(sn201202x_ldcm_retry_ms(&sim->wait, 0,
								     sim->now));
	}
	return ret;
}

/*
 * A port without an earlier result (wet at probe, say) whose measurement
 * never gives one: never enabled, held mitigated between restarts that come
 * further and further apart, with few updates. A dry result enables it.
 */
static void sn201202x_ldcm_never_a_result_test(struct kunit *test)
{
	unsigned long start = -msecs_to_jiffies(SN201202X_LDCM_STUCK_MS);
	unsigned long hour = msecs_to_jiffies(3600 * MSEC_PER_SEC);
	unsigned long hold_max = msecs_to_jiffies(SN201202X_LDCM_HOLD_MAX_MS);
	struct sn201202x_ldcm_sim sim = {
		.now = start,
		.published = TYPEC_CONTAMINANT_UNKNOWN,
	};
	unsigned long dried;

	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_sim_run(test, &sim, start + hour), -EAGAIN);
	KUNIT_EXPECT_EQ(test, sim.published, TYPEC_CONTAMINANT_UNKNOWN);
	KUNIT_EXPECT_TRUE(test, sim.mitigated);
	KUNIT_EXPECT_EQ(test, sim.reruns, sim.wait.holds);
	/* Restarts at 5 s, then after holds of 10 s to 5 min: 16 in the hour */
	KUNIT_EXPECT_GE(test, sim.reruns, 10U);
	KUNIT_EXPECT_LE(test, sim.reruns, 20U);
	/* No busy loop: a few updates per restart */
	KUNIT_EXPECT_LE(test, sim.updates, 200U);

	/* Dry from now on: enabled at the end of the hold at the latest. */
	sim.dries = true;
	sim.dry_at = sim.now;
	dried = sim.now;
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_sim_run(test, &sim, dried + 2 * hold_max), 0);
	KUNIT_EXPECT_EQ(test, sim.published, TYPEC_CONTAMINANT_NONE);
	KUNIT_EXPECT_FALSE(test, sim.mitigated);
	KUNIT_EXPECT_LE(test, sim.now - dried, hold_max);
	KUNIT_EXPECT_EQ(test, sim.wait.holds, 0U);
}

/*
 * A slow controller, the case the waits are for: a measurement that ends
 * within the first wait, after the restart, or during a hold enables the port
 * as soon as it is read, and is never forced into mitigation before the
 * restart had its whole wait. Without a controller event (as here), a result
 * that comes during a hold is read at its end.
 */
static void sn201202x_ldcm_slow_result_test(struct kunit *test)
{
	static const struct {
		unsigned int dry_ms;
		unsigned int reruns;
		unsigned int by_ms;
	} cases[] = {
		/* Read at once, in the first wait and after the restart */
		{ 3000, 0, 3500 },
		{ 7000, 1, 7500 },
		/* Read at the end of the hold (11-21 s, then 27-47 s and 53-93 s) */
		{ 12000, 1, 22000 },
		{ 60000, 3, 95000 },
	};
	unsigned long hour = msecs_to_jiffies(3600 * MSEC_PER_SEC);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned long start = -msecs_to_jiffies(SN201202X_LDCM_STUCK_MS);
		struct sn201202x_ldcm_sim sim = {
			.now = start,
			.published = TYPEC_CONTAMINANT_UNKNOWN,
			.dries = true,
			.dry_at = start + msecs_to_jiffies(cases[i].dry_ms),
		};

		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_sim_run(test, &sim, start + hour), 0);
		KUNIT_EXPECT_EQ(test, sim.published, TYPEC_CONTAMINANT_NONE);
		KUNIT_EXPECT_FALSE(test, sim.mitigated);
		KUNIT_EXPECT_EQ(test, sim.reruns, cases[i].reruns);
		KUNIT_EXPECT_LE(test, jiffies_to_msecs(sim.now - start), cases[i].by_ms);
	}
}

static int sn201202x_ldcm_sim_step(struct sn201202x_ldcm_sim *sim)
{
	sim->updates++;
	return sn201202x_ldcm_update_state(sim, &sn201202x_ldcm_sim_ops,
					   sim->published);
}

/*
 * A hold starts only once its mitigation went through. A failed command is
 * retried after SN201202X_LDCM_SLOW_RETRY_MS, as a failed restart is, and the
 * update that follows, however late it comes, sends the mitigation again: the
 * measurement is never started again from a hold that was not mitigated. The
 * hold runs whole from the mitigation that went through, and then ends in a
 * restart.
 */
static void sn201202x_ldcm_hold_mitigation_failure_retried_test(struct kunit *test)
{
	static const struct {
		unsigned int failures;
		int err;
		/* Each retry comes only after the longest hold */
		bool late;
	} cases[] = {
		{ 0, 0, false },
		{ 1, -ETIMEDOUT, false },
		{ 3, -EIO, false },
		{ 3, -ETIMEDOUT, true },
	};
	unsigned long stuck = msecs_to_jiffies(SN201202X_LDCM_STUCK_MS);
	unsigned long hold = msecs_to_jiffies(SN201202X_LDCM_HOLD_MS);
	unsigned long hold_max = msecs_to_jiffies(SN201202X_LDCM_HOLD_MAX_MS);
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned long start = -stuck;	/* across a jiffies wrap */
		struct sn201202x_ldcm_sim sim = {
			.now = start,
			.published = TYPEC_CONTAMINANT_UNKNOWN,
			.mitigation_failures = cases[i].failures,
			.mitigation_err = cases[i].err,
		};
		unsigned int delay;

		/* The first wait and the restart's wait run out: the hold is due. */
		KUNIT_ASSERT_EQ(test, sn201202x_ldcm_sim_step(&sim), -EINPROGRESS);
		sim.now += stuck;
		KUNIT_ASSERT_EQ(test, sn201202x_ldcm_sim_step(&sim), -EINPROGRESS);
		KUNIT_ASSERT_EQ(test, sim.reruns, 1U);
		sim.now += stuck;

		for (j = 0; j < cases[i].failures; j++) {
			KUNIT_EXPECT_EQ(test, sn201202x_ldcm_sim_step(&sim), cases[i].err);
			KUNIT_EXPECT_EQ(test, sim.mitigations, j + 1);
			KUNIT_EXPECT_FALSE(test, sim.mitigated);
			KUNIT_EXPECT_FALSE(test, sim.wait.held);
			KUNIT_EXPECT_EQ(test, sim.published, TYPEC_CONTAMINANT_UNKNOWN);
			KUNIT_EXPECT_EQ(test, sim.reruns, 1U);
			delay = sn201202x_ldcm_retry_ms(&sim.wait, 0, sim.now);
			KUNIT_EXPECT_EQ(test, delay, SN201202X_LDCM_SLOW_RETRY_MS);
			if (cases[i].late)
				sim.now += hold_max;
			else
				sim.now += msecs_to_jiffies(delay);
		}

		/* The command goes through: the hold runs whole from now. */
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_sim_step(&sim), -EAGAIN);
		KUNIT_EXPECT_EQ(test, sim.mitigations, cases[i].failures + 1);
		KUNIT_EXPECT_TRUE(test, sim.mitigated);
		KUNIT_EXPECT_TRUE(test, sim.wait.held);
		KUNIT_EXPECT_EQ(test, sim.published, TYPEC_CONTAMINANT_UNKNOWN);
		KUNIT_EXPECT_EQ(test, sim.reruns, 1U);
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&sim.wait, 0, sim.now),
				jiffies_to_msecs(hold));

		/* Just before its end nothing is restarted, nor is the hold stretched. */
		sim.now += hold - 1;
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_sim_step(&sim), -EAGAIN);
		KUNIT_EXPECT_EQ(test, sim.reruns, 1U);
		KUNIT_EXPECT_TRUE(test, sim.mitigated);

		/* At its end the measurement is started again, retried as a restart. */
		sim.now += 1;
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_sim_step(&sim), -EINPROGRESS);
		KUNIT_EXPECT_EQ(test, sim.reruns, 2U);
		KUNIT_EXPECT_FALSE(test, sim.mitigated);
		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_retry_ms(&sim.wait, 0, sim.now),
				SN201202X_LDCM_SLOW_RETRY_MS);
	}
}

/* In the busy-wait of suspend-to-idle, mitigation is never removed. */
static void sn201202x_ldcm_atomic_test(struct kunit *test)
{
	struct sn201202x *sn = kunit_kzalloc(test, sizeof(*sn), GFP_KERNEL);
	struct tps6598x *tps;

	KUNIT_ASSERT_NOT_NULL(test, sn);
	tps = &sn->cd.tps;
	sn->atomic = true;
	sn->ldcm_mitigation_known = true;
	sn->ldcm_mitigated = true;

	/* Refused without touching the bus (there is none here) */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_set_mitigation(tps, false), -EAGAIN);
	KUNIT_EXPECT_TRUE(test, sn->ldcm_mitigation_known);
	KUNIT_EXPECT_TRUE(test, sn->ldcm_mitigated);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_set_mitigation(tps, true), 0);
	/* A restart refused is still due: its second wait does not start. */
	sn->ldcm_wait.waiting = true;
	sn->ldcm_wait.overdue = 1;
	sn->ldcm_wait.rerun_due = true;
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_rerun(tps), -EAGAIN);
	KUNIT_EXPECT_TRUE(test, sn->ldcm_mitigation_known);
	KUNIT_EXPECT_TRUE(test, sn->ldcm_wait.rerun_due);
	sn->ldcm_wait = (struct sn201202x_ldcm_wait){};

	/* No wait runs out there: the update after the resume decides. */
	sn->ldcm_wait.waiting = true;
	sn->ldcm_wait.since = jiffies - msecs_to_jiffies(10 * SN201202X_LDCM_STUCK_MS);
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_overdue(tps), 0U);
	KUNIT_EXPECT_EQ(test, sn->ldcm_wait.overdue, 0U);
}

static void sn201202x_ldcm_read_failure_with_result_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action actions[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_UNKNOWN,
	};
	struct sn201202x_ldcm_test t = {
		.reads = { TYPEC_CONTAMINANT_UNKNOWN },
		.read_rets = { -EIO },
		.read_failures = SN201202X_LDCM_READ_TRIES,
	};

	/* A failed read is never mistaken for a running measurement. */
	KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
							  &sn201202x_ldcm_test_ops,
							  TYPEC_CONTAMINANT_NONE),
			-EIO);
	sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
}

/*
 * A dry port, maybe with a dock or a display on it, is not mitigated for a
 * failed read: only for SN201202X_LDCM_READ_TRIES failures in a row.
 */
static void sn201202x_ldcm_dry_read_failure_retried_test(struct kunit *test)
{
	const enum sn201202x_ldcm_test_action retried[] = {
		LDCM_TEST_READ,
	};
	const enum sn201202x_ldcm_test_action mitigated[] = {
		LDCM_TEST_READ,
		LDCM_TEST_MITIGATE_ON,
		LDCM_TEST_PUBLISH_UNKNOWN,
	};
	unsigned int i;

	for (i = 1; i < SN201202X_LDCM_READ_TRIES; i++) {
		struct sn201202x_ldcm_test t = {
			.reads = { TYPEC_CONTAMINANT_UNKNOWN },
			.read_rets = { -EIO },
			.read_failures = i,
			.overdue = 2,
		};

		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
								  &sn201202x_ldcm_test_ops,
								  TYPEC_CONTAMINANT_NONE),
				-EIO);
		sn201202x_ldcm_test_actions(test, &t, retried, ARRAY_SIZE(retried));
	}

	for (i = SN201202X_LDCM_READ_TRIES; i <= SN201202X_LDCM_READ_TRIES + 1; i++) {
		struct sn201202x_ldcm_test t = {
			.reads = { TYPEC_CONTAMINANT_UNKNOWN },
			.read_rets = { -EIO },
			.read_failures = i,
		};

		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
								  &sn201202x_ldcm_test_ops,
								  TYPEC_CONTAMINANT_NONE),
				-EIO);
		sn201202x_ldcm_test_actions(test, &t, mitigated, ARRAY_SIZE(mitigated));
	}
}

/* Without a result, or wet, a failed read mitigates at once, as before. */
static void sn201202x_ldcm_read_failure_not_dry_test(struct kunit *test)
{
	const struct {
		enum typec_contaminant last;
		enum sn201202x_ldcm_test_action publish;
	} cases[] = {
		{ TYPEC_CONTAMINANT_UNKNOWN, LDCM_TEST_PUBLISH_UNKNOWN },
		{ TYPEC_CONTAMINANT_LIQUID, LDCM_TEST_PUBLISH_LIQUID },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const enum sn201202x_ldcm_test_action actions[] = {
			LDCM_TEST_READ,
			LDCM_TEST_MITIGATE_ON,
			cases[i].publish,
		};
		struct sn201202x_ldcm_test t = {
			.reads = { TYPEC_CONTAMINANT_UNKNOWN },
			.read_rets = { -EIO },
			.read_failures = 1,
		};

		KUNIT_EXPECT_EQ(test, sn201202x_ldcm_update_state(&t,
								  &sn201202x_ldcm_test_ops,
								  cases[i].last),
				-EIO);
		sn201202x_ldcm_test_actions(test, &t, actions, ARRAY_SIZE(actions));
	}
}

struct sn201202x_ldcm_stop_test {
	struct sn201202x sn;
	atomic_t runs;
};

static void sn201202x_ldcm_test_work(struct work_struct *work)
{
	struct sn201202x *sn = container_of(to_delayed_work(work),
					  struct sn201202x, ldcm_work);
	struct sn201202x_ldcm_stop_test *t = container_of(sn,
					struct sn201202x_ldcm_stop_test, sn);

	atomic_inc(&t->runs);
}

static void sn201202x_ldcm_test_cancel(void *data)
{
	cancel_delayed_work_sync(data);
}

static void sn201202x_ldcm_stop_requeue_test(struct kunit *test)
{
	struct sn201202x_ldcm_stop_test *t;
	struct tps6598x *tps;
	int ret;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	tps = &t->sn.cd.tps;
	mutex_init(&tps->lock);
	atomic_set(&t->runs, 0);
	INIT_DELAYED_WORK(&t->sn.ldcm_work, sn201202x_ldcm_test_work);
	ret = kunit_add_action_or_reset(test, sn201202x_ldcm_test_cancel, &t->sn.ldcm_work);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Removal must cancel a pending retry and prevent later producers. */
	mod_delayed_work(system_freezable_wq, &t->sn.ldcm_work, 60 * HZ);
	sn201202x_contaminant_ops.stop(tps);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->sn.ldcm_work));

	/* Reset recovery can still invalidate the detector before it is drained. */
	mutex_lock(&tps->lock);
	sn201202x_contaminant_ops.invalidate(tps);
	mutex_unlock(&tps->lock);
	flush_delayed_work(&t->sn.ldcm_work);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->runs), 0);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->sn.ldcm_work));
}

static struct kunit_case sn201202x_pm_cases[] = {
	KUNIT_CASE(sn201202x_sleep_cycle_test),
	KUNIT_CASE(sn201202x_failed_wakeup_test),
	KUNIT_CASE(sn201202x_rejected_sleep_test),
	KUNIT_CASE(sn201202x_unconfirmed_sleep_test),
	KUNIT_CASE(sn201202x_unconfirmed_sleep_failed_wakeup_test),
	KUNIT_CASE(sn201202x_sleep_links_test),
	KUNIT_CASE(sn201202x_ldcm_dry_test),
	KUNIT_CASE(sn201202x_ldcm_liquid_test),
	KUNIT_CASE(sn201202x_ldcm_busy_test),
	KUNIT_CASE(sn201202x_ldcm_liquid_order_test),
	KUNIT_CASE(sn201202x_ldcm_read_failure_order_test),
	KUNIT_CASE(sn201202x_ldcm_mitigation_failure_order_test),
	KUNIT_CASE(sn201202x_ldcm_dry_order_test),
	KUNIT_CASE(sn201202x_ldcm_dry_then_busy_test),
	KUNIT_CASE(sn201202x_ldcm_dry_then_read_failure_test),
	KUNIT_CASE(sn201202x_ldcm_dry_race_test),
	KUNIT_CASE(sn201202x_ldcm_dry_race_mitigation_failure_test),
	KUNIT_CASE(sn201202x_ldcm_busy_keeps_result_test),
	KUNIT_CASE(sn201202x_ldcm_busy_without_result_test),
	KUNIT_CASE(sn201202x_ldcm_overdue_once_test),
	KUNIT_CASE(sn201202x_ldcm_overdue_once_rerun_failure_test),
	KUNIT_CASE(sn201202x_ldcm_overdue_twice_holds_test),
	KUNIT_CASE(sn201202x_ldcm_hold_dry_test),
	KUNIT_CASE(sn201202x_ldcm_hold_liquid_test),
	KUNIT_CASE(sn201202x_ldcm_overdue_mitigation_failure_test),
	KUNIT_CASE(sn201202x_ldcm_wet_never_overdue_test),
	KUNIT_CASE(sn201202x_ldcm_wet_mitigation_failure_test),
	KUNIT_CASE(sn201202x_ldcm_liquid_read_failure_test),
	KUNIT_CASE(sn201202x_ldcm_liquid_mitigation_failure_test),
	KUNIT_CASE(sn201202x_ldcm_wait_test),
	KUNIT_CASE(sn201202x_ldcm_wait_rerun_failure_test),
	KUNIT_CASE(sn201202x_ldcm_wait_hold_backoff_test),
	KUNIT_CASE(sn201202x_ldcm_never_a_result_test),
	KUNIT_CASE(sn201202x_ldcm_slow_result_test),
	KUNIT_CASE(sn201202x_ldcm_hold_mitigation_failure_retried_test),
	KUNIT_CASE(sn201202x_ldcm_atomic_test),
	KUNIT_CASE(sn201202x_ldcm_read_failure_with_result_test),
	KUNIT_CASE(sn201202x_ldcm_dry_read_failure_retried_test),
	KUNIT_CASE(sn201202x_ldcm_read_failure_not_dry_test),
	KUNIT_CASE(sn201202x_ldcm_stop_requeue_test),
	KUNIT_CASE(sn201202x_idle_sleep_test),
	KUNIT_CASE(sn201202x_atomic_access_test),
	KUNIT_CASE(sn201202x_idle_sleep_retry_test),
	KUNIT_CASE(sn201202x_idle_sleep_gives_up_test),
	KUNIT_CASE(sn201202x_idle_sleep_tries_restart_test),
	KUNIT_CASE(sn201202x_unconfirmed_idle_sleep_failed_wakeup_test),
	KUNIT_CASE(sn201202x_display_link_test),
	KUNIT_CASE(sn201202x_display_unplugged_test),
	KUNIT_CASE(sn201202x_dbma_test),
	KUNIT_CASE(sn201202x_dbma_held_test),
	KUNIT_CASE(sn201202x_unbind_test),
	KUNIT_CASE(sn201202x_no_idle_sleep_test),
	KUNIT_CASE(sn201202x_sleep_from_idle_test),
	KUNIT_CASE(sn201202x_sleep_from_idle_failed_wakeup_test),
	KUNIT_CASE(sn201202x_watched_sleep_from_idle_test),
	{}
};

static struct kunit_suite sn201202x_pm_suite = {
	.name = "sn201202x-pm",
	.test_cases = sn201202x_pm_cases,
};

kunit_test_suite(sn201202x_pm_suite);
MODULE_DESCRIPTION("KUnit tests for SN201202x system sleep and idle sleep");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

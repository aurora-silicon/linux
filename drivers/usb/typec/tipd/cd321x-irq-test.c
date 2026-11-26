// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for CD321x interrupt event acknowledgment
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "tps6598x.h"

#define TEST_REG_INT_EVENT1	0x14
#define TEST_REG_INT_CLEAR1	0x18
#define TEST_REG_STATUS		0x1a
#define TEST_REG_POWER_STATUS	0x3f
#define TEST_REG_RX_IDENTITY_SOP 0x48

/*
 * The controller latches events in INT_EVENT1 until they are written to
 * INT_CLEAR1, and raises its interrupt only when an event is latched while
 * none were pending.
 */
struct cd321x_test_ctrl {
	u64 latched;
	unsigned int interrupts;
	unsigned int failing_event_reads;
	unsigned int failing_clear_writes;
	unsigned int failing_status_reads;
	unsigned int failing_identity_reads;
	u32 status;
	u16 pwr_status;
	struct usb_pd_identity identity;
};

struct cd321x_test {
	struct cd321x cd;
	struct cd321x_test_ctrl ctrl;
};

static void cd321x_test_raise(struct cd321x_test_ctrl *ctrl, u64 event)
{
	if (!ctrl->latched)
		ctrl->interrupts++;
	ctrl->latched |= event;
}

static int cd321x_test_read(void *context, const void *reg, size_t reg_size,
			    void *val, size_t val_size)
{
	struct cd321x_test_ctrl *ctrl = context;

	memset(val, 0, val_size);
	switch (*(const u8 *)reg) {
	case TEST_REG_INT_EVENT1:
		if (ctrl->failing_event_reads) {
			ctrl->failing_event_reads--;
			return -ETIMEDOUT;
		}
		memcpy(val, &ctrl->latched, min(val_size, sizeof(ctrl->latched)));
		break;
	case TEST_REG_STATUS:
		if (ctrl->failing_status_reads) {
			ctrl->failing_status_reads--;
			return -ETIMEDOUT;
		}
		memcpy(val, &ctrl->status, min(val_size, sizeof(ctrl->status)));
		break;
	case TEST_REG_POWER_STATUS:
		memcpy(val, &ctrl->pwr_status, min(val_size, sizeof(ctrl->pwr_status)));
		break;
	case TEST_REG_RX_IDENTITY_SOP:
		if (ctrl->failing_identity_reads) {
			ctrl->failing_identity_reads--;
			return -ETIMEDOUT;
		}
		/* The identity follows a status byte. */
		memcpy(val + 1, &ctrl->identity,
		       min(val_size - 1, sizeof(ctrl->identity)));
		break;
	}

	return 0;
}

static int cd321x_test_write(void *context, const void *data, size_t count)
{
	struct cd321x_test_ctrl *ctrl = context;
	const u8 *buf = data;
	u64 clear = 0;

	if (buf[0] == TEST_REG_INT_CLEAR1) {
		if (ctrl->failing_clear_writes) {
			ctrl->failing_clear_writes--;
			return -ETIMEDOUT;
		}
		memcpy(&clear, buf + 1, min(count - 1, sizeof(clear)));
		ctrl->latched &= ~clear;
	}

	return 0;
}

static const struct regmap_bus cd321x_test_bus = {
	.read = cd321x_test_read,
	.write = cd321x_test_write,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

static void cd321x_test_stop(void *data)
{
	struct cd321x *cd = data;

	scoped_guard(mutex, &cd->tps.lock)
		cd321x_pm_remove(&cd->pm);
	cancel_delayed_work_sync(&cd->resume_work);
	cancel_delayed_work_sync(&cd->update_work);
}

static struct cd321x_test *cd321x_test_port(struct kunit *test)
{
	struct cd321x_test *t;
	struct device *dev;

	dev = kunit_device_register(test, "cd321x-irq-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);

	mutex_init(&t->cd.tps.lock);
	t->cd.tps.dev = dev;
	t->cd.tps.data = tipd_hw_variants[TIPD_VARIANT_CD321X];
	t->cd.tps.regmap = devm_regmap_init(dev, &cd321x_test_bus, &t->ctrl,
					    &tps6598x_regmap_config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->cd.tps.regmap);
	dev_set_drvdata(dev, &t->cd.tps);

	cd321x_init_work(&t->cd);
	t->cd.pm.phase = CD321X_PM_RUNNING;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, cd321x_test_stop, &t->cd), 0);

	return t;
}

static irqreturn_t cd321x_test_interrupt(struct cd321x_test *t)
{
	return t->cd.tps.data->irq_handler(0, &t->cd.tps);
}

/*
 * Run the snapshot read that was scheduled to recover the events. It is queued
 * without delay, so on SMP a worker may already have run it: check its effect,
 * not whether it is still pending.
 */
static void cd321x_test_recover(struct kunit *test, struct cd321x_test *t)
{
	flush_delayed_work(&t->cd.resume_work);
	scoped_guard(mutex, &t->cd.tps.lock)
		KUNIT_EXPECT_TRUE(test, cd321x_pm_can_update(&t->cd.pm));
}

/* The time until the next recovery read, which must be scheduled. */
static unsigned long cd321x_test_retry_delay(struct kunit *test,
					     struct cd321x_test *t)
{
	KUNIT_ASSERT_TRUE(test, delayed_work_pending(&t->cd.resume_work));
	return t->cd.resume_work.timer.expires - jiffies;
}

/*
 * Keep the events from being acknowledged, by failing every access counted by
 * @failures, for much longer than the cable-state budget lasts. Once the bus
 * works again, the events must still be acknowledged, so that the next cable
 * event raises an interrupt.
 */
static void cd321x_test_ack_outage(struct kunit *test, struct cd321x_test *t,
				   unsigned int *failures)
{
	unsigned long first, delay = 0;
	unsigned int i;

	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	*failures = UINT_MAX;
	cd321x_test_interrupt(t);
	first = cd321x_test_retry_delay(test, t);

	for (i = 0; i < 4 * CD321X_RESUME_ATTEMPTS; i++) {
		flush_delayed_work(&t->cd.resume_work);
		KUNIT_EXPECT_NE(test, t->cd.pm.phase, CD321X_PM_STALE);
		delay = cd321x_test_retry_delay(test, t);
	}
	/* Retried more and more slowly, but still at least once a minute. */
	KUNIT_EXPECT_GE(test, delay, 8 * first);
	KUNIT_EXPECT_LE(test, delay, msecs_to_jiffies(60 * MSEC_PER_SEC));
	KUNIT_EXPECT_EQ(test, t->ctrl.interrupts, 1U);

	*failures = 0;
	cd321x_test_recover(test, t);
	KUNIT_EXPECT_EQ(test, t->ctrl.latched, 0);
	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
	KUNIT_EXPECT_EQ(test, t->ctrl.interrupts, 2U);
	KUNIT_EXPECT_EQ(test, cd321x_test_interrupt(t), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, t->ctrl.latched, 0);
}

static void cd321x_irq_acknowledges_events_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	KUNIT_EXPECT_EQ(test, cd321x_test_interrupt(t), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, t->ctrl.latched, 0);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
}

static void cd321x_failed_event_read_recovers_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	t->ctrl.failing_event_reads = 1;
	cd321x_test_interrupt(t);
	KUNIT_ASSERT_NE(test, t->ctrl.latched, 0);

	cd321x_test_recover(test, t);
	KUNIT_EXPECT_EQ(test, t->ctrl.latched, 0);
	/* The next cable event must reach the host again. */
	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	KUNIT_EXPECT_EQ(test, t->ctrl.interrupts, 2U);
}

static void cd321x_failed_status_read_recovers_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	t->ctrl.failing_status_reads = 1;
	cd321x_test_interrupt(t);
	/* Acknowledged, but the cable state behind the event was not read. */
	KUNIT_ASSERT_EQ(test, t->ctrl.latched, 0);

	cd321x_test_recover(test, t);
}

static void cd321x_failed_read_after_giving_up_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	t->cd.pm.phase = CD321X_PM_STALE;
	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
	t->ctrl.failing_event_reads = 1;
	cd321x_test_interrupt(t);

	cd321x_test_recover(test, t);
	KUNIT_EXPECT_EQ(test, t->ctrl.latched, 0);
}

static void cd321x_failed_identity_read_recovers_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);
	const struct usb_pd_identity old = { .id_header = 0x11 };

	t->cd.tps.partner_identity = old;
	t->cd.update_status.partner_identity = old;
	t->ctrl.status = TPS_STATUS_PLUG_PRESENT;
	t->ctrl.pwr_status = FIELD_PREP(TPS_POWER_STATUS_TYPEC_CURRENT_MASK,
					TYPEC_PWR_MODE_PD);
	t->ctrl.identity.id_header = 0x22;
	t->ctrl.failing_identity_reads = 1;
	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_POWER_STATUS_UPDATE);
	cd321x_test_interrupt(t);
	KUNIT_ASSERT_EQ(test, t->ctrl.latched, 0);

	/* An incomplete read must not replace the identity in use... */
	KUNIT_EXPECT_EQ(test, t->cd.update_status.partner_identity.id_header,
			old.id_header);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.update_work));
	/* ...but be completed by a fresh one. */
	cd321x_test_recover(test, t);
	KUNIT_EXPECT_EQ(test, t->cd.update_status.partner_identity.id_header, 0x22U);
}

static void cd321x_failed_clear_recovers_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	t->ctrl.failing_clear_writes = 1;
	cd321x_test_interrupt(t);
	KUNIT_ASSERT_NE(test, t->ctrl.latched, 0);

	cd321x_test_recover(test, t);
	KUNIT_EXPECT_EQ(test, t->ctrl.latched, 0);
	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	KUNIT_EXPECT_EQ(test, t->ctrl.interrupts, 2U);
}

static void cd321x_event_reads_failing_for_long_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	cd321x_test_ack_outage(test, t, &t->ctrl.failing_event_reads);
}

static void cd321x_clear_writes_failing_for_long_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	cd321x_test_ack_outage(test, t, &t->ctrl.failing_clear_writes);
}

static void cd321x_ack_retry_across_sleep_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);
	struct device *dev = t->cd.tps.dev;
	unsigned int i, left;
	unsigned int failures;

	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	t->ctrl.failing_event_reads = UINT_MAX;
	cd321x_test_interrupt(t);
	flush_delayed_work(&t->cd.resume_work);

	/* Sleep cancels the retry, and the resume read takes it over. */
	KUNIT_ASSERT_EQ(test, tipd_prepare(dev), 0);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	left = t->ctrl.failing_event_reads;
	tipd_complete(dev);
	/*
	 * The resume read is queued without delay, so on SMP a worker may be
	 * running it already. Wait for it, without running the retry it
	 * schedules early, and check that it tried to read the events.
	 */
	flush_work(&t->cd.resume_work.work);
	scoped_guard(mutex, &t->cd.tps.lock) {
		KUNIT_EXPECT_EQ(test, t->ctrl.failing_event_reads, left - 1);
		KUNIT_EXPECT_LT(test, t->ctrl.failing_event_reads, left);
	}
	for (i = 0; i < 2 * CD321X_RESUME_ATTEMPTS; i++) {
		scoped_guard(mutex, &t->cd.tps.lock)
			failures = t->ctrl.failing_event_reads;
		flush_delayed_work(&t->cd.resume_work);
		scoped_guard(mutex, &t->cd.tps.lock) {
			KUNIT_EXPECT_LT(test, t->ctrl.failing_event_reads, failures);
			KUNIT_EXPECT_NE(test, t->cd.pm.phase, CD321X_PM_STALE);
			KUNIT_EXPECT_GT(test, t->cd.pm.ack_retry_ms, 0U);
		}
	}

	scoped_guard(mutex, &t->cd.tps.lock)
		t->ctrl.failing_event_reads = 0;
	cd321x_test_recover(test, t);
	KUNIT_EXPECT_EQ(test, t->ctrl.latched, 0);
	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	KUNIT_EXPECT_EQ(test, t->ctrl.interrupts, 2U);
}

static void cd321x_ack_retry_ends_on_removal_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	t->ctrl.failing_event_reads = UINT_MAX;
	cd321x_test_interrupt(t);
	flush_delayed_work(&t->cd.resume_work);
	KUNIT_ASSERT_TRUE(test, delayed_work_pending(&t->cd.resume_work));

	scoped_guard(mutex, &t->cd.tps.lock)
		cd321x_pm_remove(&t->cd.pm);
	/* A retry that was already queued neither reads nor queues another. */
	flush_delayed_work(&t->cd.resume_work);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_EQ(test, t->ctrl.failing_event_reads, UINT_MAX - 2);
}

static void cd321x_resume_acknowledges_events_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);
	struct device *dev = t->cd.tps.dev;

	KUNIT_ASSERT_EQ(test, tipd_prepare(dev), 0);
	/* Latched while the interrupt was off, and never delivered. */
	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_POWER_STATUS_UPDATE);
	tipd_complete(dev);

	cd321x_test_recover(test, t);
	KUNIT_EXPECT_EQ(test, t->ctrl.latched, 0);
	cd321x_test_raise(&t->ctrl, APPLE_CD_REG_INT_PLUG_EVENT);
	KUNIT_EXPECT_EQ(test, t->ctrl.interrupts, 2U);
}

/* A polled controller whose plug event never reached the driver */
static void cd321x_poll_finds_lost_plug_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	t->ctrl.status = TPS_STATUS_PLUG_PRESENT;
	KUNIT_EXPECT_EQ(test, cd321x_test_interrupt(t), IRQ_NONE);
	cd321x_test_recover(test, t);
	KUNIT_EXPECT_TRUE(test, t->cd.tps.status & TPS_STATUS_PLUG_PRESENT);
	KUNIT_EXPECT_TRUE(test, t->cd.update_status.status_changed & TPS_STATUS_PLUG_PRESENT);

	/* Applied: the next poll finds nothing to do. */
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
}

static void cd321x_poll_finds_lost_unplug_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	t->cd.tps.status = TPS_STATUS_PLUG_PRESENT;
	t->cd.update_status.status = TPS_STATUS_PLUG_PRESENT;
	cd321x_test_interrupt(t);
	cd321x_test_recover(test, t);
	KUNIT_EXPECT_FALSE(test, t->cd.tps.status & TPS_STATUS_PLUG_PRESENT);
	KUNIT_EXPECT_TRUE(test, t->cd.update_status.status_changed & TPS_STATUS_PLUG_PRESENT);
}

static void cd321x_poll_same_plug_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	/* Other status bits change without an event too: only the plug counts. */
	t->ctrl.status = TPS_STATUS_PLUG_UPSIDE_DOWN;
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_EQ(test, t->cd.pm.phase, CD321X_PM_RUNNING);
}

static const struct tipd_contaminant_ops cd321x_test_contaminant_ops = {};

static void cd321x_poll_plug_not_dry_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	/* A plug on a connector not known to be dry is kept down, as connect does. */
	t->cd.tps.contaminant_ops = &cd321x_test_contaminant_ops;
	t->cd.tps.contaminant = TYPEC_CONTAMINANT_UNKNOWN;
	t->ctrl.status = TPS_STATUS_PLUG_PRESENT;
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));

	/* Found dry with no event of its own: the plug is applied. */
	t->cd.tps.contaminant = TYPEC_CONTAMINANT_NONE;
	cd321x_test_interrupt(t);
	cd321x_test_recover(test, t);
	KUNIT_EXPECT_TRUE(test, t->cd.tps.status & TPS_STATUS_PLUG_PRESENT);
}

static void cd321x_poll_plug_during_revalidation_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	/* A revalidation under way is not requeued, which would delay it. */
	t->cd.pm.phase = CD321X_PM_REVALIDATE;
	t->cd.pm.attempts_left = CD321X_RESUME_ATTEMPTS;
	t->ctrl.status = TPS_STATUS_PLUG_PRESENT;
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));

	/* One that gave up is restarted by the plug it missed. */
	t->cd.pm.phase = CD321X_PM_STALE;
	t->cd.pm.attempts_left = 0;
	cd321x_test_interrupt(t);
	cd321x_test_recover(test, t);
	KUNIT_EXPECT_TRUE(test, t->cd.tps.status & TPS_STATUS_PLUG_PRESENT);
}

/* Snapshot taken, but the update that registers the partner gave up */
static void cd321x_poll_plug_without_partner_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	t->ctrl.status = TPS_STATUS_PLUG_PRESENT;
	t->cd.tps.status = TPS_STATUS_PLUG_PRESENT;
	t->cd.update_status.status = TPS_STATUS_PLUG_PRESENT;

	/* Not while the update is still to run */
	mod_delayed_work(system_freezable_wq, &t->cd.update_work, 60 * HZ);
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	cancel_delayed_work_sync(&t->cd.update_work);

	cd321x_test_interrupt(t);
	cd321x_test_recover(test, t);
	KUNIT_EXPECT_TRUE(test, t->cd.tps.status & TPS_STATUS_PLUG_PRESENT);
}

/* One poll-requested read per plug state that is not applied */
static void cd321x_poll_revalidates_once_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	t->cd.pm.phase = CD321X_PM_STALE;
	t->ctrl.status = TPS_STATUS_PLUG_PRESENT;
	t->cd.tps.status = TPS_STATUS_PLUG_PRESENT;
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&t->cd.resume_work));

	/* That read applied the snapshot, but the update gave up again. */
	cancel_delayed_work_sync(&t->cd.resume_work);
	t->cd.pm.phase = CD321X_PM_STALE;
	t->cd.pm.attempts_left = 0;
	cd321x_test_interrupt(t);
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_EQ(test, t->cd.pm.phase, CD321X_PM_STALE);

	/* A different plug state gets its own read. */
	t->ctrl.status = 0;
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&t->cd.resume_work));
	cancel_delayed_work_sync(&t->cd.resume_work);

	/* Once the state is applied, a later loss is recovered again. */
	t->cd.pm.phase = CD321X_PM_STALE;
	t->cd.tps.status = 0;
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_FALSE(test, t->cd.poll_revalidated);
	t->ctrl.status = TPS_STATUS_PLUG_PRESENT;
	cd321x_test_interrupt(t);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&t->cd.resume_work));
}

static void cd321x_poll_status_read_fails_test(struct kunit *test)
{
	struct cd321x_test *t = cd321x_test_port(test);

	t->cd.tps.status = TPS_STATUS_PLUG_PRESENT;
	t->ctrl.failing_status_reads = 1;
	cd321x_test_interrupt(t);
	/* Not taken for an unplug */
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_EQ(test, t->cd.pm.phase, CD321X_PM_RUNNING);
}

static struct kunit_case cd321x_irq_cases[] = {
	KUNIT_CASE(cd321x_irq_acknowledges_events_test),
	KUNIT_CASE(cd321x_failed_event_read_recovers_test),
	KUNIT_CASE(cd321x_failed_status_read_recovers_test),
	KUNIT_CASE(cd321x_failed_read_after_giving_up_test),
	KUNIT_CASE(cd321x_failed_identity_read_recovers_test),
	KUNIT_CASE(cd321x_failed_clear_recovers_test),
	KUNIT_CASE(cd321x_event_reads_failing_for_long_test),
	KUNIT_CASE(cd321x_clear_writes_failing_for_long_test),
	KUNIT_CASE(cd321x_ack_retry_across_sleep_test),
	KUNIT_CASE(cd321x_ack_retry_ends_on_removal_test),
	KUNIT_CASE(cd321x_resume_acknowledges_events_test),
	KUNIT_CASE(cd321x_poll_finds_lost_plug_test),
	KUNIT_CASE(cd321x_poll_finds_lost_unplug_test),
	KUNIT_CASE(cd321x_poll_same_plug_test),
	KUNIT_CASE(cd321x_poll_plug_not_dry_test),
	KUNIT_CASE(cd321x_poll_plug_during_revalidation_test),
	KUNIT_CASE(cd321x_poll_plug_without_partner_test),
	KUNIT_CASE(cd321x_poll_revalidates_once_test),
	KUNIT_CASE(cd321x_poll_status_read_fails_test),
	{}
};

static struct kunit_suite cd321x_irq_suite = {
	.name = "cd321x-irq",
	.test_cases = cd321x_irq_cases,
};

kunit_test_suite(cd321x_irq_suite);
MODULE_DESCRIPTION("KUnit tests for CD321x interrupt event acknowledgment");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

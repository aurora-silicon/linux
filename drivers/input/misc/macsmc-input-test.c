// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple SMC input driver: decoding of the SMC's button
 * and lid notifications into KEY_POWER and SW_LID, and the sleep window in
 * which they are wake events instead, with the lid-open confirmation
 * against MSLD and the settle time.
 *
 * MSLD reads go to a static stub on apple_smc_read() (see
 * MFD_MACSMC_KUNIT_TEST). Events go to a registered input device; wakeups
 * are seen on the device's wakeup source and, for hard wakeups, through
 * pm_wakeup_pending().
 *
 * The cases touch global PM state (pm_abort_suspend, the dark wake flag)
 * and report KEY_POWER and SW_LID through a real input device, which a
 * running system's logind would act on by powering off or suspending. So
 * the suite runs only at boot, before user space; run later (from the
 * kunit debugfs interface or a module) it skips every case.
 *
 * Included by macsmc-input.c.
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

#define INPUT_TEST_EV(type, hi, lo)	(((type) << 16) | ((hi) << 8) | (lo))
#define INPUT_TEST_BTN(button, state)	INPUT_TEST_EV(SMC_EV_BTN, button, state)
#define INPUT_TEST_LID(closed, lo)	INPUT_TEST_EV(SMC_EV_LID, closed, lo)

struct macsmc_input_test {
	struct apple_smc smc;
	u8 shmem[256];
	struct macsmc_input smcin;
	/* MSLD: 1 while the lid is closed, and what its read returns */
	u8 msld;
	int msld_ret;
	unsigned int msld_reads;
	unsigned int saved_flags;
};

static int macsmc_input_test_read(struct apple_smc *smc, smc_key key, void *buf,
				  size_t size)
{
	struct macsmc_input_test *ctx = container_of(smc, struct macsmc_input_test, smc);
	struct kunit *test = kunit_get_current_test();

	KUNIT_EXPECT_EQ(test, key, SMC_KEY(MSLD));
	KUNIT_EXPECT_EQ(test, size, 1);
	ctx->msld_reads++;
	if (ctx->msld_ret < 0)
		return ctx->msld_ret;
	*(u8 *)buf = ctx->msld;
	return size;
}

static void macsmc_input_test_exit(void *data)
{
	struct macsmc_input_test *ctx = data;

	pm_suspend_global_flags = ctx->saved_flags;
	pm_wakeup_clear(0);
}

static struct macsmc_input_test *macsmc_input_test_setup(struct kunit *test)
{
	struct macsmc_input_test *ctx;
	struct macsmc_input *smcin;
	struct device *dev;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	mutex_init(&ctx->smc.mutex);
	spin_lock_init(&ctx->smc.lock);
	/* SMC commands other than the stubbed read fail: key_exists() is false. */
	ctx->smc.shmem.iomem = (void __force __iomem *)ctx->shmem;
	ctx->saved_flags = pm_suspend_global_flags;
	pm_wakeup_clear(0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, macsmc_input_test_exit, ctx), 0);

	dev = kunit_device_register(test, "input");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	KUNIT_ASSERT_EQ(test, devm_device_init_wakeup(dev), 0);

	smcin = &ctx->smcin;
	smcin->dev = dev;
	smcin->smc = &ctx->smc;
	smcin->nb.notifier_call = macsmc_input_event;
	smcin->pm_nb.notifier_call = macsmc_input_pm_notify;
	mutex_init(&smcin->lid_lock);
	dev_set_drvdata(dev, smcin);

	smcin->input = devm_input_allocate_device(dev);
	KUNIT_ASSERT_NOT_NULL(test, smcin->input);
	smcin->input->name = "macsmc-input-test";
	input_set_capability(smcin->input, EV_SW, SW_LID);
	input_set_capability(smcin->input, EV_KEY, KEY_POWER);
	KUNIT_ASSERT_EQ(test, input_register_device(smcin->input), 0);

	kunit_activate_static_stub(test, apple_smc_read, macsmc_input_test_read);
	return ctx;
}

static int macsmc_input_test_event(struct macsmc_input_test *ctx, unsigned long event)
{
	return macsmc_input_event(&ctx->smcin.nb, event, NULL);
}

static bool macsmc_input_test_key(struct macsmc_input_test *ctx)
{
	return test_bit(KEY_POWER, ctx->smcin.input->key);
}

static bool macsmc_input_test_lid(struct macsmc_input_test *ctx)
{
	return test_bit(SW_LID, ctx->smcin.input->sw);
}

static unsigned long macsmc_input_test_wake_events(struct macsmc_input_test *ctx)
{
	return ctx->smcin.dev->power.wakeup->event_count;
}

/* Was a hard wakeup (one that aborts a suspend) asked for? Clears it. */
static bool macsmc_input_test_hard_wake(void)
{
	bool pending = pm_wakeup_pending();

	pm_wakeup_clear(0);
	return pending;
}

static void macsmc_input_test_sleep(struct macsmc_input_test *ctx, unsigned int settle_ms,
				    unsigned int slept_ms)
{
	ctx->smcin.lid_settle_ms = settle_ms;
	ctx->smcin.wakeup_mode = true;
	ctx->smcin.sleep_start = ktime_sub_ms(ktime_get_boottime(), slept_ms);
}

/* Only the button and lid event types are this driver's. */
static void macsmc_input_test_dispatch(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);

	KUNIT_EXPECT_EQ(test, macsmc_input_test_event(ctx, INPUT_TEST_EV(0x7202, 0x01, 0x01)),
			NOTIFY_DONE);
	KUNIT_EXPECT_EQ(test, macsmc_input_test_event(ctx, INPUT_TEST_EV(0x7101, 0x01, 0x01)),
			NOTIFY_DONE);
	KUNIT_EXPECT_EQ(test, macsmc_input_test_event(ctx, INPUT_TEST_EV(0x0000, 0x01, 0x01)),
			NOTIFY_DONE);
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_key(ctx));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_lid(ctx));
	KUNIT_EXPECT_EQ(test, macsmc_input_test_wake_events(ctx), 0);

	KUNIT_EXPECT_EQ(test, macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_POWER, 1)),
			NOTIFY_OK);
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_key(ctx));
	KUNIT_EXPECT_EQ(test, macsmc_input_test_event(ctx, INPUT_TEST_LID(1, 0)), NOTIFY_OK);
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_lid(ctx));
}

/* Power and Touch ID buttons: bits 15:8 the button, any nonzero low byte is down. */
static void macsmc_input_test_power_button(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);

	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_POWER, 1));
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_key(ctx));
	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_POWER, 0));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_key(ctx));

	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_TOUCHID, 0x80));
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_key(ctx));
	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_TOUCHID, 0));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_key(ctx));

	/* Awake, presses are wakeup events but never hard wakeups. */
	KUNIT_EXPECT_EQ(test, macsmc_input_test_wake_events(ctx), 4);
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_hard_wake());
}

/* "Held" notices and unknown buttons are not key presses. */
static void macsmc_input_test_other_buttons(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);

	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_POWER_HELD_SHORT, 1));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_key(ctx));
	/* The long hold's release, never its press: that powers off. */
	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_POWER_HELD_LONG, 0));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_key(ctx));
	macsmc_input_test_event(ctx, INPUT_TEST_BTN(0x07, 1));
	macsmc_input_test_event(ctx, INPUT_TEST_BTN(0x02, 1));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_key(ctx));
	KUNIT_EXPECT_EQ(test, macsmc_input_test_wake_events(ctx), 0);
}

/* In the sleep window a press is a hard wakeup and no KEY_POWER. */
static void macsmc_input_test_button_in_sleep(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);

	macsmc_input_test_sleep(ctx, 0, 0);
	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_TOUCHID, 1));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_key(ctx));
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_hard_wake());

	/* A release does not wake. */
	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_TOUCHID, 0));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_hard_wake());
	KUNIT_EXPECT_EQ(test, macsmc_input_test_wake_events(ctx), 2);
	KUNIT_EXPECT_EQ(test, ctx->msld_reads, 0);
}

/* Lid: bits 15:8 nonzero is closed; the low byte is not the state. */
static void macsmc_input_test_lid_awake(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);

	macsmc_input_test_event(ctx, INPUT_TEST_LID(1, 0));
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_lid(ctx));
	macsmc_input_test_event(ctx, INPUT_TEST_LID(0, 1));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_lid(ctx));
	macsmc_input_test_event(ctx, INPUT_TEST_LID(0x80, 0));
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_lid(ctx));
	macsmc_input_test_event(ctx, INPUT_TEST_LID(0, 0xff));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_lid(ctx));

	/* Awake, MSLD is not consulted and nothing is a hard wakeup. */
	KUNIT_EXPECT_EQ(test, ctx->msld_reads, 0);
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_hard_wake());
}

/* Closing the lid in the sleep window is neither reported nor a wakeup. */
static void macsmc_input_test_lid_close_in_sleep(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);

	macsmc_input_test_sleep(ctx, 0, 0);
	macsmc_input_test_event(ctx, INPUT_TEST_LID(1, 0));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_lid(ctx));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_hard_wake());
	KUNIT_EXPECT_EQ(test, ctx->msld_reads, 0);
	KUNIT_EXPECT_FALSE(test, ctx->smcin.lid_held);
}

/* An open report while MSLD still says closed is held, not a wakeup. */
static void macsmc_input_test_lid_open_msld_closed(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);

	macsmc_input_test_event(ctx, INPUT_TEST_LID(1, 0));
	macsmc_input_test_sleep(ctx, 0, 5000);
	ctx->msld = 1;
	macsmc_input_test_event(ctx, INPUT_TEST_LID(0, 1));
	KUNIT_EXPECT_EQ(test, ctx->msld_reads, 1);
	KUNIT_EXPECT_TRUE(test, ctx->smcin.lid_held);
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_lid(ctx));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_hard_wake());
	KUNIT_EXPECT_EQ(test, macsmc_input_test_wake_events(ctx), 1);
}

/* With a settle time, an open within it is held; after it, it wakes. */
static void macsmc_input_test_lid_open_settle(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);

	macsmc_input_test_event(ctx, INPUT_TEST_LID(1, 0));
	macsmc_input_test_sleep(ctx, LID_WAKE_SETTLE_MS_J716S, 0);
	macsmc_input_test_event(ctx, INPUT_TEST_LID(0, 1));
	KUNIT_EXPECT_TRUE(test, ctx->smcin.lid_held);
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_hard_wake());

	ctx->smcin.lid_held = false;
	ctx->smcin.sleep_start = ktime_sub_ms(ktime_get_boottime(),
					      LID_WAKE_SETTLE_MS_J716S + 500);
	macsmc_input_test_event(ctx, INPUT_TEST_LID(0, 1));
	KUNIT_EXPECT_FALSE(test, ctx->smcin.lid_held);
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_hard_wake());
	/* Reported by .complete from MSLD, not here. */
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_lid(ctx));
	KUNIT_EXPECT_EQ(test, ctx->msld_reads, 2);
}

/* Without a settle time an open with MSLD open wakes at once. */
static void macsmc_input_test_lid_open_wakes(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);

	macsmc_input_test_sleep(ctx, 0, 0);
	macsmc_input_test_event(ctx, INPUT_TEST_LID(0, 1));
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_hard_wake());
	KUNIT_EXPECT_FALSE(test, ctx->smcin.lid_held);

	/* An unreadable MSLD does not hold the report back. */
	ctx->msld_ret = -ETIMEDOUT;
	macsmc_input_test_event(ctx, INPUT_TEST_LID(0, 1));
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_hard_wake());
	KUNIT_EXPECT_FALSE(test, ctx->smcin.lid_held);
	KUNIT_EXPECT_EQ(test, ctx->msld_reads, 2);
}

/* The PM notifier opens the window at PM_SUSPEND_PREPARE and closes it after. */
static void macsmc_input_test_pm_notify(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);
	struct macsmc_input *smcin = &ctx->smcin;
	ktime_t before = ktime_get_boottime();

	smcin->lid_held = true;
	KUNIT_EXPECT_EQ(test, macsmc_input_pm_notify(&smcin->pm_nb, PM_SUSPEND_PREPARE, NULL),
			NOTIFY_DONE);
	KUNIT_EXPECT_TRUE(test, smcin->wakeup_mode);
	KUNIT_EXPECT_FALSE(test, smcin->lid_held);
	KUNIT_EXPECT_GE(test, ktime_to_ns(smcin->sleep_start), ktime_to_ns(before));

	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_POWER, 1));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_key(ctx));
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_hard_wake());

	smcin->dark = true;
	smcin->lid_held = true;
	KUNIT_EXPECT_EQ(test, macsmc_input_pm_notify(&smcin->pm_nb, PM_POST_SUSPEND, NULL),
			NOTIFY_DONE);
	KUNIT_EXPECT_FALSE(test, smcin->wakeup_mode);
	KUNIT_EXPECT_FALSE(test, smcin->dark);
	KUNIT_EXPECT_FALSE(test, smcin->lid_held);

	/* Other actions change nothing. */
	macsmc_input_pm_notify(&smcin->pm_nb, PM_HIBERNATION_PREPARE, NULL);
	KUNIT_EXPECT_FALSE(test, smcin->wakeup_mode);

	macsmc_input_test_event(ctx, INPUT_TEST_BTN(BTN_POWER, 1));
	KUNIT_EXPECT_TRUE(test, macsmc_input_test_key(ctx));
	KUNIT_EXPECT_FALSE(test, macsmc_input_test_hard_wake());
}

/* .prepare opens the window; a full .complete closes it and drops a held open. */
static void macsmc_input_test_pm_prepare_complete(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);
	struct macsmc_input *smcin = &ctx->smcin;

	pm_suspend_global_flags &= ~PM_SUSPEND_FLAG_DARK_WAKE;
	KUNIT_EXPECT_EQ(test, macsmc_input_pm_prepare(smcin->dev), 0);
	KUNIT_EXPECT_TRUE(test, smcin->wakeup_mode);

	smcin->lid_held = true;
	macsmc_input_pm_complete(smcin->dev);
	KUNIT_EXPECT_FALSE(test, smcin->wakeup_mode);
	KUNIT_EXPECT_FALSE(test, smcin->lid_held);
	KUNIT_EXPECT_FALSE(test, smcin->dark);
}

/*
 * A dark wake keeps the window and the held open, and marks the next
 * .prepare to recheck the lid unless MSLD reads open.
 */
static void macsmc_input_test_pm_dark_wake(struct kunit *test)
{
	struct macsmc_input_test *ctx = macsmc_input_test_setup(test);
	struct macsmc_input *smcin = &ctx->smcin;

	KUNIT_EXPECT_EQ(test, macsmc_input_pm_prepare(smcin->dev), 0);
	smcin->lid_held = true;
	pm_set_dark_wake();

	ctx->msld = 1;
	macsmc_input_pm_complete(smcin->dev);
	KUNIT_EXPECT_TRUE(test, smcin->wakeup_mode);
	KUNIT_EXPECT_TRUE(test, smcin->lid_held);
	KUNIT_EXPECT_TRUE(test, smcin->dark);

	ctx->msld = 0;
	macsmc_input_pm_complete(smcin->dev);
	KUNIT_EXPECT_FALSE(test, smcin->dark);

	ctx->msld_ret = -EIO;
	macsmc_input_pm_complete(smcin->dev);
	KUNIT_EXPECT_TRUE(test, smcin->dark);
	KUNIT_EXPECT_EQ(test, ctx->msld_reads, 3);
	KUNIT_EXPECT_TRUE(test, smcin->wakeup_mode);
}

static int macsmc_input_test_init(struct kunit *test)
{
	if (system_state == SYSTEM_RUNNING)
		kunit_skip(test, "reports KEY_POWER/SW_LID and changes PM state: boot only");
	return 0;
}

static struct kunit_case macsmc_input_test_cases[] = {
	KUNIT_CASE(macsmc_input_test_dispatch),
	KUNIT_CASE(macsmc_input_test_power_button),
	KUNIT_CASE(macsmc_input_test_other_buttons),
	KUNIT_CASE(macsmc_input_test_button_in_sleep),
	KUNIT_CASE(macsmc_input_test_lid_awake),
	KUNIT_CASE(macsmc_input_test_lid_close_in_sleep),
	KUNIT_CASE(macsmc_input_test_lid_open_msld_closed),
	KUNIT_CASE(macsmc_input_test_lid_open_settle),
	KUNIT_CASE(macsmc_input_test_lid_open_wakes),
	KUNIT_CASE(macsmc_input_test_pm_notify),
	KUNIT_CASE(macsmc_input_test_pm_prepare_complete),
	KUNIT_CASE(macsmc_input_test_pm_dark_wake),
	{}
};

static struct kunit_suite macsmc_input_test_suite = {
	.name = "macsmc-input",
	.init = macsmc_input_test_init,
	.test_cases = macsmc_input_test_cases,
};

kunit_test_suite(macsmc_input_test_suite);

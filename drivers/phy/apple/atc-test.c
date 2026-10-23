// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * KUnit tests for the Type-C mux decisions of the Apple Type-C PHY driver.
 * This file is included from atc.c so that it can test static functions.
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

static struct apple_atcphy *atcphy_test_alloc(struct kunit *test, bool fixed_usb2)
{
	struct apple_atcphy *atcphy;

	atcphy = kunit_kzalloc(test, sizeof(*atcphy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, atcphy);
	atcphy->fixed_usb2 = fixed_usb2;
	atcphy->mode = APPLE_ATCPHY_MODE_USB3_DP;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
	atcphy->host_active = fixed_usb2;

	return atcphy;
}

/* The mode lookup reports unsupported states against the PHY device */
static struct apple_atcphy *atcphy_test_alloc_dev(struct kunit *test, bool fixed_usb2)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, fixed_usb2);

	atcphy->dev = kunit_device_register(test, "atcphy-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, atcphy->dev);

	return atcphy;
}

static void atcphy_test_target_mode(struct kunit *test)
{
	struct typec_altmode dp = { .svid = USB_TYPEC_DP_SID };
	struct typec_mux_state state = {};
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, false);

	state.mode = TYPEC_STATE_SAFE;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_OFF);

	state.mode = TYPEC_STATE_USB;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_USB3);

	state.alt = &dp;
	state.mode = TYPEC_DP_STATE_C;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_DP);
	state.mode = TYPEC_DP_STATE_D;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state),
			APPLE_ATCPHY_MODE_USB3_DP);
	state.mode = TYPEC_DP_STATE_E;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_DP);
	state.mode = TYPEC_DP_STATE_A;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_OFF);
}

/* A port behind a fixed hub keeps USB2 for its hub whenever a mode turns the PHY off */
static void atcphy_test_target_mode_fixed_hub(struct kunit *test)
{
	struct typec_altmode dp = { .svid = USB_TYPEC_DP_SID };
	struct typec_mux_state state = {};
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, true);

	state.mode = TYPEC_STATE_SAFE;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_USB2);

	state.alt = &dp;
	state.mode = TYPEC_DP_STATE_A;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_USB2);
	state.mode = TYPEC_DP_STATE_D;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state),
			APPLE_ATCPHY_MODE_USB3_DP);
}

static void atcphy_test_noop_same_mode(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, true);

	/* A DisplayPort HPD or status update keeps the pin assignment */
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* The same holds while the controller does not use the USB3 PHY */
	atcphy->host_active = false;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* Ports without a fixed hub never route the PIPE from the mux */
	atcphy = atcphy_test_alloc(test, false);
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* A mode without USB3 leaves the SuperSpeed port off */
	atcphy = atcphy_test_alloc(test, true);
	atcphy->mode = APPLE_ATCPHY_MODE_DP;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	atcphy->usb3_port_off = true;
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_DP));
}

static void atcphy_test_noop_needs_work(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, true);

	/* A new pin assignment or the safe state rebuild the lanes */
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_DP));
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB2));

	/* So does a failed transition or an orientation change */
	atcphy->mode_dirty = true;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
	atcphy->mode_dirty = false;

	/* A running controller is still owed its USB3 backend */
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* or a handoff that has not completed */
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
	atcphy->pipe_pending = false;

	/* or its SuperSpeed port, still off since the last handoff */
	atcphy->usb3_port_off = true;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
}

/* The controller's SuperSpeed port only runs on a completed USB3 route */
static void atcphy_test_usb3_port_ready(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, true);

	KUNIT_EXPECT_TRUE(test, atcphy_usb3_port_ready(atcphy));

	atcphy->mode_dirty = true;
	KUNIT_EXPECT_FALSE(test, atcphy_usb3_port_ready(atcphy));
	atcphy->mode_dirty = false;

	atcphy->pipe_pending = true;
	KUNIT_EXPECT_FALSE(test, atcphy_usb3_port_ready(atcphy));
	atcphy->pipe_pending = false;

	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_FALSE(test, atcphy_usb3_port_ready(atcphy));
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;

	atcphy->host_active = false;
	KUNIT_EXPECT_FALSE(test, atcphy_usb3_port_ready(atcphy));

	/* A Type-C port's controller is never handled here */
	atcphy = atcphy_test_alloc(test, false);
	atcphy->host_active = true;
	KUNIT_EXPECT_FALSE(test, atcphy_usb3_port_ready(atcphy));
}

static void atcphy_test_cancel_port_work(void *data)
{
	struct apple_atcphy *atcphy = data;

	cancel_delayed_work_sync(&atcphy->usb3_port_work);
}

/*
 * A J700-style PHY behind a fixed hub, in DisplayPort + USB3 with the PIPE on
 * USB3 for a running host, but without a device tree node. A mux update that
 * goes for the USB3 root hub then fails with -ENODEV at the controller lookup,
 * before any register access, which tells the two paths apart.
 */
static struct apple_atcphy *atcphy_test_alloc_fixed_hub(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, true);
	int ret;

	INIT_DELAYED_WORK(&atcphy->usb3_port_work, atcphy_usb3_port_work);
	ret = kunit_add_action_or_reset(test, atcphy_test_cancel_port_work, atcphy);
	KUNIT_ASSERT_EQ(test, ret, 0);

	atcphy->hw = &atcphy_hw_t8140;
	/* The lane and DisplayPort tunables a J700 has */
	atcphy->ss_tunables = true;
	atcphy->dp_tunables = true;
	mutex_init(&atcphy->lock);

	return atcphy;
}

static const struct typec_altmode atcphy_test_dp = { .svid = USB_TYPEC_DP_SID };

static int atcphy_test_update_dp(struct apple_atcphy *atcphy, unsigned long pin)
{
	struct typec_mux_state state = {
		.alt = (struct typec_altmode *)&atcphy_test_dp,
		.mode = pin,
	};

	return atcphy_mux_update(atcphy, &state);
}

static void atcphy_test_update_skips_usb(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);

	/* An HPD or status update with the same pin assignment */
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3_DP);
	KUNIT_EXPECT_FALSE(test, atcphy->mode_dirty);

	/* The orientation the lanes already use */
	KUNIT_EXPECT_EQ(test, atcphy_set_orientation(atcphy, TYPEC_ORIENTATION_NORMAL), 0);
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
}

static void atcphy_test_update_takes_usb(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);

	/* A new pin assignment */
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -ENODEV);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3_DP);

	/* A PIPE handoff that has not completed */
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -ENODEV);
	atcphy->pipe_pending = false;

	/* A running host whose PIPE is not on the USB3 backend */
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -ENODEV);
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;

	/* A cable flip moves the USB3 and DisplayPort lanes */
	KUNIT_EXPECT_EQ(test, atcphy_set_orientation(atcphy, TYPEC_ORIENTATION_REVERSE), 0);
	KUNIT_EXPECT_TRUE(test, atcphy->mode_dirty);
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -ENODEV);
}

/*
 * The steps of a mux update on a fixed-hub port, recorded by static stubs
 * for the USB side and for the locked mode change: g(et the root hub),
 * 0 (SuperSpeed port off), m(ode change) and 1 (SuperSpeed port on).
 */
struct atcphy_test_steps {
	char log[16];
	int pm_get_ret;
	int port_off_ret;
	int port_on_ret;
	int mode_ret;
	bool mode_port_on;
};

static struct atcphy_test_steps *atcphy_test_steps(void)
{
	return kunit_get_current_test()->priv;
}

static void atcphy_test_step(char step)
{
	struct atcphy_test_steps *steps = atcphy_test_steps();
	size_t len = strlen(steps->log);

	if (len < sizeof(steps->log) - 1)
		steps->log[len] = step;
}

static int atcphy_test_pm_get(struct apple_atcphy *atcphy, struct atcphy_usb3_pm *pm)
{
	atcphy_test_step('g');
	return atcphy_test_steps()->pm_get_ret;
}

static int atcphy_test_port_off(struct apple_atcphy *atcphy, struct atcphy_usb3_pm *pm)
{
	atcphy_test_step('0');
	return atcphy_test_steps()->port_off_ret;
}

static int atcphy_test_port_on(struct apple_atcphy *atcphy, struct atcphy_usb3_pm *pm)
{
	atcphy_test_step('1');
	return atcphy_test_steps()->port_on_ret;
}

static int atcphy_test_mode(struct apple_atcphy *atcphy, enum atcphy_mode target_mode,
			    bool *port_on)
{
	atcphy_test_step('m');
	*port_on = atcphy_test_steps()->mode_port_on;
	return atcphy_test_steps()->mode_ret;
}

/* Retries stay queued until a test runs them, in its own context. */
static unsigned long atcphy_test_retry_delay(unsigned int retry)
{
	return MAX_JIFFY_OFFSET;
}

static bool atcphy_test_run_retry(struct apple_atcphy *atcphy)
{
	if (!cancel_delayed_work_sync(&atcphy->usb3_port_work))
		return false;
	atcphy_usb3_port_work(&atcphy->usb3_port_work.work);
	return true;
}

static struct atcphy_test_steps *atcphy_test_stub_steps(struct kunit *test)
{
	struct atcphy_test_steps *steps = kunit_kzalloc(test, sizeof(*steps), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, steps);
	test->priv = steps;
	kunit_activate_static_stub(test, atcphy_usb3_pm_get, atcphy_test_pm_get);
	kunit_activate_static_stub(test, atcphy_usb3_port_off, atcphy_test_port_off);
	kunit_activate_static_stub(test, atcphy_usb3_port_on, atcphy_test_port_on);
	kunit_activate_static_stub(test, atcphy_mux_set_locked, atcphy_test_mode);
	kunit_activate_static_stub(test, atcphy_usb3_port_retry_delay, atcphy_test_retry_delay);

	return steps;
}

/* The SuperSpeed port goes down before the PHY changes and comes back after it */
static void atcphy_test_update_port_order(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	struct atcphy_test_steps *steps = atcphy_test_stub_steps(test);

	/* A new pin assignment with USB3 routed again */
	steps->mode_port_on = true;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), 0);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m1");
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);

	/* An update keeping the mode touches neither */
	memset(steps->log, 0, sizeof(steps->log));
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	KUNIT_EXPECT_STREQ(test, steps->log, "");

	/* A mode without USB3: the port stays down */
	steps->mode_port_on = false;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), 0);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m");
	KUNIT_EXPECT_TRUE(test, atcphy->usb3_port_off);
}

/* No PIPE or lane work unless the port went down, and no port up after a failure */
static void atcphy_test_update_port_failure(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	struct atcphy_test_steps *steps = atcphy_test_stub_steps(test);

	steps->port_off_ret = -EPIPE;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -EPIPE);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0");

	memset(steps->log, 0, sizeof(steps->log));
	steps->port_off_ret = 0;
	steps->mode_ret = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -ETIMEDOUT);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m");

	/* A change refused before it touched the USB3 route brings the port back */
	memset(steps->log, 0, sizeof(steps->log));
	steps->mode_ret = -EOPNOTSUPP;
	steps->mode_port_on = true;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -EOPNOTSUPP);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m1");

	/* and if that fails, the Type-C core must not take the refusal for success */
	memset(steps->log, 0, sizeof(steps->log));
	steps->port_on_ret = -ENOMEM;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -ENOMEM);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m1");
	steps->port_on_ret = 0;

	/* A hub that cannot switch the port is no success the Type-C core skips */
	memset(steps->log, 0, sizeof(steps->log));
	steps->port_off_ret = -EOPNOTSUPP;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -ENODEV);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0");
}

/* A port owed only its power is brought up, and USB alone never fails the display */
static void atcphy_test_update_port_owed(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	struct atcphy_test_steps *steps = atcphy_test_stub_steps(test);

	atcphy->usb3_port_off = true;

	/* An update keeping the pin assignment */
	steps->mode_port_on = true;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m1");
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);
	atcphy->usb3_port_off = true;

	/* The same, with the root hub gone */
	memset(steps->log, 0, sizeof(steps->log));
	steps->pm_get_ret = -ENODEV;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	KUNIT_EXPECT_STREQ(test, steps->log, "g");

	/* or failing to take the port down */
	memset(steps->log, 0, sizeof(steps->log));
	steps->pm_get_ret = 0;
	steps->port_off_ret = -EPIPE;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0");

	/* A new pin assignment still needs the USB side */
	memset(steps->log, 0, sizeof(steps->log));
	steps->pm_get_ret = -ENODEV;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -ENODEV);
	KUNIT_EXPECT_STREQ(test, steps->log, "g");

	/* A failed mode change is no USB failure */
	memset(steps->log, 0, sizeof(steps->log));
	steps->pm_get_ret = 0;
	steps->port_off_ret = 0;
	steps->mode_ret = -EIO;
	steps->mode_port_on = false;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -EIO);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m");
}

/* A power-on owed after a USB failure is retried, while it is owed, a few times */
static void atcphy_test_update_port_retry(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	struct atcphy_test_steps *steps = atcphy_test_stub_steps(test);
	unsigned int i;

	atcphy->usb3_port_off = true;
	steps->mode_port_on = true;

	/* Out of memory for the hub request: the update keeps the display */
	steps->port_on_ret = -ENOMEM;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m1");

	/* and with no further Type-C event, the retry brings the port up */
	memset(steps->log, 0, sizeof(steps->log));
	steps->port_on_ret = 0;
	KUNIT_EXPECT_TRUE(test, atcphy_test_run_retry(atcphy));
	KUNIT_EXPECT_STREQ(test, steps->log, "g1");
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);
	KUNIT_EXPECT_FALSE(test, atcphy_test_run_retry(atcphy));

	/* A retry finding the host gone leaves the port to the next host */
	atcphy->usb3_port_off = true;
	steps->port_on_ret = -ENOMEM;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	atcphy->host_active = false;
	memset(steps->log, 0, sizeof(steps->log));
	KUNIT_EXPECT_TRUE(test, atcphy_test_run_retry(atcphy));
	KUNIT_EXPECT_STREQ(test, steps->log, "g");
	KUNIT_EXPECT_FALSE(test, atcphy_test_run_retry(atcphy));
	atcphy->host_active = true;

	/* A port that keeps failing is given up after a few retries */
	memset(steps->log, 0, sizeof(steps->log));
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	for (i = 0; i < ATCPHY_USB3_PORT_RETRIES; i++)
		KUNIT_EXPECT_TRUE(test, atcphy_test_run_retry(atcphy));
	KUNIT_EXPECT_FALSE(test, atcphy_test_run_retry(atcphy));
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m1g1g1g1g1g1");
	KUNIT_EXPECT_TRUE(test, atcphy->usb3_port_off);

	/* A mode change fails on the last step, and is retried from there too */
	memset(steps->log, 0, sizeof(steps->log));
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -ENOMEM);
	steps->port_on_ret = 0;
	KUNIT_EXPECT_TRUE(test, atcphy_test_run_retry(atcphy));
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m1g1");
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);
}

/* A mode the PHY cannot run is turned down before USB3 goes down for it */
static void atcphy_test_update_unsupported(struct kunit *test)
{
	struct typec_altmode tbt = { .svid = USB_TYPEC_TBT_SID };
	struct typec_mux_state state = { .alt = &tbt, .mode = TYPEC_STATE_MODAL };
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	struct atcphy_test_steps *steps = atcphy_test_stub_steps(test);

	/* Thunderbolt, which this PHY does not have */
	KUNIT_EXPECT_EQ(test, atcphy_mux_update(atcphy, &state), -EOPNOTSUPP);
	KUNIT_EXPECT_STREQ(test, steps->log, "");

	/* DisplayPort without its tunables */
	atcphy->dp_tunables = false;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -EOPNOTSUPP);
	KUNIT_EXPECT_STREQ(test, steps->log, "");
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);
}

/* The locked mode change as it reports the port: up once the PHY took it down */
static int atcphy_test_mode_ready(struct apple_atcphy *atcphy, enum atcphy_mode target_mode,
				  bool *port_on)
{
	atcphy_test_step('m');
	*port_on = atcphy->usb3_port_off && atcphy_test_steps()->mode_port_on;
	return atcphy_test_steps()->mode_ret;
}

/* A port that userspace switched off while the PHY had it down stays off */
static void atcphy_test_update_port_taken(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	struct atcphy_test_steps *steps = atcphy_test_stub_steps(test);

	kunit_activate_static_stub(test, atcphy_mux_set_locked, atcphy_test_mode_ready);
	steps->mode_port_on = true;

	/* Unplugged, with the port down for the PHY, then disabled through sysfs */
	atcphy->mode = APPLE_ATCPHY_MODE_USB2;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	atcphy->usb3_port_off = true;

	/* The replug finds the port off for someone else, and leaves it to them */
	steps->port_off_ret = 1;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m");
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);

	/* Disabled while the PHY moved the lanes: nothing is owed afterwards */
	memset(steps->log, 0, sizeof(steps->log));
	steps->port_off_ret = 0;
	steps->port_on_ret = 1;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), 0);
	KUNIT_EXPECT_STREQ(test, steps->log, "g0m1");
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);
}

/*
 * Zeroed memory in place of the core, PIPE handler and USB2 PHY registers:
 * enough to park the PIPE and power the PHY off, with a PIPE lock request
 * that is never acknowledged.
 */
static void atcphy_test_fake_regs(struct kunit *test, struct apple_atcphy *atcphy)
{
	void *core = kunit_kzalloc(test, ATCPHY_MISC + 4, GFP_KERNEL);
	void *pipehandler = kunit_kzalloc(test, PIPEHANDLER_NONSELECTED_OVERRIDE + 4, GFP_KERNEL);
	void *usb2phy = kunit_kzalloc(test, USB2PHY_MISCTUNE + 4, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, core);
	KUNIT_ASSERT_NOT_NULL(test, pipehandler);
	KUNIT_ASSERT_NOT_NULL(test, usb2phy);
	atcphy->regs.core = (__force void __iomem *)core;
	atcphy->regs.pipehandler = (__force void __iomem *)pipehandler;
	atcphy->regs.usb2phy = (__force void __iomem *)usb2phy;
}

/* The locked mode change reports a port that can come up again */
static void atcphy_test_set_port_on(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	bool port_on = true;

	/* The port was not taken down */
	KUNIT_EXPECT_EQ(test, atcphy_mux_set_locked(atcphy, APPLE_ATCPHY_MODE_USB3_DP, &port_on),
			0);
	KUNIT_EXPECT_FALSE(test, port_on);

	/* The port is down and the PIPE already on the USB3 backend */
	atcphy->usb3_port_off = true;
	KUNIT_EXPECT_EQ(test, atcphy_mux_set_locked(atcphy, APPLE_ATCPHY_MODE_USB3_DP, &port_on),
			0);
	KUNIT_EXPECT_TRUE(test, port_on);

	/* A change that failed, here an unplug whose PIPE lock was refused */
	atcphy_test_fake_regs(test, atcphy);
	KUNIT_EXPECT_EQ(test, atcphy_mux_set_locked(atcphy, APPLE_ATCPHY_MODE_USB2, &port_on),
			-ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, atcphy->mode_dirty);
	KUNIT_EXPECT_FALSE(test, port_on);
}

/* A host that starts up powers its SuperSpeed port on itself */
static void atcphy_test_host_start(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	struct phy *phy = kunit_kzalloc(test, sizeof(*phy), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, phy);
	phy_set_drvdata(phy, atcphy);
	atcphy->usb3_port_off = true;

	KUNIT_EXPECT_EQ(test, atcphy_usb3_set_mode(phy, PHY_MODE_USB_HOST, 0), 0);
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
}

/* A fixed-hub PHY whose USB3 handoff failed while the host was up */
static struct apple_atcphy *atcphy_test_alloc_failed_handoff(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);

	atcphy_test_fake_regs(test, atcphy);
	atcphy->pipe_pending = true;
	atcphy->usb3_port_off = true;

	/* The running host has to acknowledge the PIPE lock */
	scoped_guard(mutex, &atcphy->lock)
		KUNIT_EXPECT_EQ(test, atcphy_park_pipehandler(atcphy, false), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_USB3);
	KUNIT_EXPECT_TRUE(test, atcphy->pipe_pending);

	return atcphy;
}

/* Once dwc3 has stopped, its USB3 PHY power-off parks the PIPE without one */
static void atcphy_test_stopped_power_off_common(struct kunit *test, bool dp)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_failed_handoff(test);
	struct phy *phy = kunit_kzalloc(test, sizeof(*phy), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, phy);
	phy_set_drvdata(phy, atcphy);
	if (!dp)
		atcphy->mode = APPLE_ATCPHY_MODE_USB3;

	KUNIT_EXPECT_EQ(test, atcphy_usb3_power_off(phy), 0);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_DUMMY);
	KUNIT_EXPECT_FALSE(test, atcphy->pipe_pending);
	KUNIT_EXPECT_EQ(test, atcphy->mode,
			dp ? APPLE_ATCPHY_MODE_USB3_DP : APPLE_ATCPHY_MODE_OFF);
	KUNIT_EXPECT_FALSE(test, atcphy->host_active);
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);
}

static void atcphy_test_stopped_power_off(struct kunit *test)
{
	atcphy_test_stopped_power_off_common(test, false);
}

/* USB shutdown must leave an active DisplayPort link powered. */
static void atcphy_test_stopped_power_off_dp(struct kunit *test)
{
	atcphy_test_stopped_power_off_common(test, true);
}

/* and so does the reset that dwc3 asserts once it has stopped */
static void atcphy_test_stopped_reset(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_failed_handoff(test);
	u32 aon;

	KUNIT_EXPECT_EQ(test, atcphy_dwc3_reset_assert(&atcphy->rcdev, 0), 0);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_DUMMY);
	KUNIT_EXPECT_FALSE(test, atcphy->pipe_pending);
	KUNIT_EXPECT_FALSE(test, atcphy->host_active);
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);
	aon = readl(atcphy->regs.pipehandler + PIPEHANDLER_AON_GEN);
	KUNIT_EXPECT_TRUE(test, aon & PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
	KUNIT_EXPECT_FALSE(test, aon & PIPEHANDLER_AON_GEN_DWC3_RESET_N);
}

/* A SuperSpeed problem does not keep the host behind a fixed hub from starting */
static void atcphy_test_host_start_without_usb3(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	struct phy *phy = kunit_kzalloc(test, sizeof(*phy), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, phy);
	phy_set_drvdata(phy, atcphy);
	atcphy_test_fake_regs(test, atcphy);
	atcphy->host_active = false;

	/* An unfinished lane change: the PIPE stays parked for the next mux update */
	atcphy->mode_dirty = true;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_EQ(test, atcphy_usb3_set_mode(phy, PHY_MODE_USB_HOST, 0), 0);
	KUNIT_EXPECT_TRUE(test, atcphy->host_active);
	KUNIT_EXPECT_TRUE(test, atcphy->mode_dirty);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_DUMMY);

	/* A handoff that cannot finish, as the PIPE lock is refused */
	atcphy->mode_dirty = false;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_EQ(test, atcphy_usb3_set_mode(phy, PHY_MODE_USB_HOST, 0), 0);
	KUNIT_EXPECT_TRUE(test, atcphy->pipe_pending);

	/* A Type-C port's controller still waits for the lanes */
	atcphy = atcphy_test_alloc(test, false);
	atcphy->hw = &atcphy_hw_t8140;
	mutex_init(&atcphy->lock);
	phy_set_drvdata(phy, atcphy);
	atcphy->mode_dirty = true;
	KUNIT_EXPECT_EQ(test, atcphy_usb3_set_mode(phy, PHY_MODE_USB_HOST, 0), -EBUSY);
}

/*
 * The controller as the PIPE lock sees it: while @held, as through the PD
 * elections of a Mac taking another data role, it does not acknowledge the
 * lock. The USB side stubs share @steps.
 */
struct atcphy_test_election {
	struct atcphy_test_steps steps;
	bool held;
};

static int atcphy_test_lock(struct apple_atcphy *atcphy)
{
	struct atcphy_test_election *election = kunit_get_current_test()->priv;

	return election->held ? -ETIMEDOUT : 0;
}

/* The host BIST of a USB3 handoff, which zeroed registers cannot run */
static int atcphy_test_bist(struct apple_atcphy *atcphy)
{
	return 0;
}

/* The lane change, as far as the PIPE handling depends on it */
static int atcphy_test_lanes(struct apple_atcphy *atcphy, enum atcphy_mode mode)
{
	lockdep_assert_held(&atcphy->lock);
	atcphy->mode = mode;
	atcphy->mode_dirty = false;
	return 0;
}

/*
 * The rear port of a J700 with a Mac on it: USB3, with the PIPE on the USB3
 * backend of the running host. The real mode change runs between the recorded
 * USB side steps of atcphy_test_stub_steps().
 */
static struct apple_atcphy *atcphy_test_alloc_election(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);
	struct atcphy_test_election *election;

	election = kunit_kzalloc(test, sizeof(*election), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, election);
	test->priv = election;
	kunit_activate_static_stub(test, atcphy_pipehandler_lock, atcphy_test_lock);
	kunit_activate_static_stub(test, atcphy_configure_pipehandler_usb3_host_t8122,
				   atcphy_test_bist);
	kunit_activate_static_stub(test, atcphy_configure, atcphy_test_lanes);
	kunit_activate_static_stub(test, atcphy_usb3_pm_get, atcphy_test_pm_get);
	kunit_activate_static_stub(test, atcphy_usb3_port_off, atcphy_test_port_off);
	kunit_activate_static_stub(test, atcphy_usb3_port_on, atcphy_test_port_on);
	kunit_activate_static_stub(test, atcphy_usb3_port_retry_delay, atcphy_test_retry_delay);
	atcphy_test_fake_regs(test, atcphy);
	atcphy->mode = APPLE_ATCPHY_MODE_USB3;
	atcphy->typec_mode = APPLE_ATCPHY_MODE_USB3;

	return atcphy;
}

static const struct typec_mux_state atcphy_test_safe = { .mode = TYPEC_STATE_SAFE };
static const struct typec_mux_state atcphy_test_usb = { .mode = TYPEC_STATE_USB };

/* A refused PIPE change leaves the port down, and nothing queued */
static void atcphy_test_election_refused(struct kunit *test, struct apple_atcphy *atcphy,
					 const struct typec_mux_state *state)
{
	struct atcphy_test_election *election = test->priv;

	memset(election->steps.log, 0, sizeof(election->steps.log));
	KUNIT_EXPECT_EQ(test, atcphy_mux_update(atcphy, state), -ETIMEDOUT);
	KUNIT_EXPECT_STREQ(test, election->steps.log, "g0");
	KUNIT_EXPECT_TRUE(test, atcphy->usb3_port_off);
	KUNIT_EXPECT_FALSE(test, atcphy_usb3_port_ready(atcphy));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&atcphy->usb3_port_work));
}

/* The safe state and then the USB state, as the Type-C driver applies them again */
static void atcphy_test_election_redrive(struct kunit *test, struct apple_atcphy *atcphy)
{
	struct atcphy_test_election *election = test->priv;
	char *log = election->steps.log;

	memset(log, 0, sizeof(election->steps.log));
	KUNIT_EXPECT_EQ(test, atcphy_mux_update(atcphy, &atcphy_test_safe), 0);
	KUNIT_EXPECT_STREQ(test, log, "g0");
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB2);
	KUNIT_EXPECT_FALSE(test, atcphy->mode_dirty);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_DUMMY);
	KUNIT_EXPECT_TRUE(test, atcphy->usb3_port_off);

	memset(log, 0, sizeof(election->steps.log));
	KUNIT_EXPECT_EQ(test, atcphy_mux_update(atcphy, &atcphy_test_usb), 0);
	KUNIT_EXPECT_STREQ(test, log, "g01");
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_USB3);
	KUNIT_EXPECT_FALSE(test, atcphy->pipe_pending);
	KUNIT_EXPECT_FALSE(test, atcphy->usb3_port_off);
	KUNIT_EXPECT_TRUE(test, atcphy_usb3_port_ready(atcphy));
}

/*
 * SuperSpeed comes back after a data role swap whose safe state found the PIPE
 * held, however many times it was refused, once the Type-C driver applies its
 * whole state again after the elections.
 */
static void atcphy_test_election_held_safe(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_election(test);
	struct atcphy_test_election *election = test->priv;
	unsigned int i;

	election->held = true;
	/* Many more times than the Type-C driver used to retry */
	for (i = 0; i < 8; i++)
		atcphy_test_election_refused(test, atcphy, &atcphy_test_safe);
	/* The PIPE could not be parked, so the lanes were left alone */
	KUNIT_EXPECT_TRUE(test, atcphy->mode_dirty);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_USB3);

	election->held = false;
	atcphy_test_election_redrive(test, atcphy);
}

/* The same if the elections go on into the USB state that follows the safe one */
static void atcphy_test_election_held_usb(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_election(test);
	struct atcphy_test_election *election = test->priv;

	election->held = true;
	atcphy_test_election_refused(test, atcphy, &atcphy_test_safe);
	election->held = false;
	KUNIT_EXPECT_EQ(test, atcphy_mux_update(atcphy, &atcphy_test_safe), 0);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_DUMMY);

	/* The lanes are rebuilt for USB3, but the PIPE cannot follow */
	election->held = true;
	atcphy_test_election_refused(test, atcphy, &atcphy_test_usb);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_DUMMY);

	election->held = false;
	atcphy_test_election_redrive(test, atcphy);
}

/*
 * Only the fixed-hub ports of a J700 have the Type-C driver retry a timeout
 * until it passes; every M1 and M2 port, and fixed-hub ports on other PHYs,
 * keep its bounded retries.
 */
static void atcphy_test_transient_timeouts(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, true);

	atcphy->hw = &atcphy_hw_t8140;
	KUNIT_EXPECT_TRUE(test, atcphy_mux_transient_timeouts(atcphy));
	atcphy->fixed_usb2 = false;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_transient_timeouts(atcphy));

	atcphy->hw = &atcphy_hw_t8103;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_transient_timeouts(atcphy));
	atcphy->hw = &atcphy_hw_t8122;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_transient_timeouts(atcphy));
	atcphy->fixed_usb2 = true;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_transient_timeouts(atcphy));
	atcphy->hw = &atcphy_hw_t8142;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_transient_timeouts(atcphy));
}

#if IS_REACHABLE(CONFIG_USB)
/* Only the controller's SuperSpeed root hub is ever handed the port changes */
static void atcphy_test_usb3_root(struct kunit *test)
{
	struct device *controller = kunit_kzalloc(test, 2 * sizeof(*controller), GFP_KERNEL);
	struct usb_bus *bus = kunit_kzalloc(test, 2 * sizeof(*bus), GFP_KERNEL);
	struct usb_device *udev = kunit_kzalloc(test, 2 * sizeof(*udev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, controller);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	KUNIT_ASSERT_NOT_NULL(test, udev);
	bus[0].sysdev = &controller[0];
	bus[1].sysdev = &controller[1];

	udev[0].bus = &bus[0];
	udev[0].speed = USB_SPEED_SUPER_PLUS;
	KUNIT_EXPECT_TRUE(test, atcphy_usb3_is_root(&udev[0], &controller[0]));

	/* The USB2 root hub, with the fixed hub on it */
	udev[1].bus = &bus[0];
	udev[1].speed = USB_SPEED_HIGH;
	KUNIT_EXPECT_FALSE(test, atcphy_usb3_is_root(&udev[1], &controller[0]));

	/* A device on the SuperSpeed root port */
	udev[1].speed = USB_SPEED_SUPER;
	udev[1].parent = &udev[0];
	KUNIT_EXPECT_FALSE(test, atcphy_usb3_is_root(&udev[1], &controller[0]));

	/* The SuperSpeed root hub of another controller */
	KUNIT_EXPECT_FALSE(test, atcphy_usb3_is_root(&udev[0], &controller[1]));
}
#endif

/* USB shutdown can turn the lanes off before the SAFE mux request arrives. */
static struct apple_atcphy *atcphy_test_alloc_powered_off(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, false);

	atcphy->hw = &atcphy_hw_t8103;
	atcphy->mode = APPLE_ATCPHY_MODE_OFF;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	atcphy->typec_mode = APPLE_ATCPHY_MODE_USB3;
	mutex_init(&atcphy->lock);

	return atcphy;
}

static void atcphy_test_safe_after_poweroff(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_powered_off(test);
	struct typec_mux_state safe = { .mode = TYPEC_STATE_SAFE };

	KUNIT_EXPECT_EQ(test, atcphy_mux_update(atcphy, &safe), 0);
	KUNIT_EXPECT_EQ(test, atcphy->typec_mode, APPLE_ATCPHY_MODE_OFF);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_DUMMY);
}

/* The locked recheck must also record SAFE if shutdown raced the mode lookup. */
static void atcphy_test_locked_safe_after_poweroff(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_powered_off(test);
	bool port_on;

	KUNIT_EXPECT_EQ(test, atcphy_mux_set_locked(atcphy, APPLE_ATCPHY_MODE_OFF, &port_on), 0);
	KUNIT_EXPECT_EQ(test, atcphy->typec_mode, APPLE_ATCPHY_MODE_OFF);
}

struct atcphy_test_configure {
	unsigned int calls;
	unsigned int lane_requests;
};

/* Count lane requests instead of touching MMIO, including stale USB3 requests. */
static int atcphy_test_configure_stub(struct apple_atcphy *atcphy, enum atcphy_mode mode)
{
	struct kunit *test = kunit_get_current_test();
	struct atcphy_test_configure *config = test->priv;

	lockdep_assert_held(&atcphy->lock);
	config->calls++;
	if (mode != APPLE_ATCPHY_MODE_OFF) {
		config->lane_requests++;
		return -EINVAL;
	}
	return 0;
}

/* Rebinding after a failed role shutdown must not touch a live PIPE. */
static void atcphy_test_safe_live_pipe(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, false);
	struct atcphy_test_configure config = {};
	bool port_on;

	test->priv = &config;
	kunit_activate_static_stub(test, atcphy_configure, atcphy_test_configure_stub);
	atcphy->hw = &atcphy_hw_t8103;
	atcphy->typec_mode = APPLE_ATCPHY_MODE_USB3;
	mutex_init(&atcphy->lock);
	KUNIT_EXPECT_EQ(test, atcphy_mux_set_locked(atcphy, APPLE_ATCPHY_MODE_OFF, &port_on),
			-EBUSY);
	KUNIT_EXPECT_EQ(test, config.calls, 0U);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3_DP);
	KUNIT_EXPECT_EQ(test, atcphy->typec_mode, APPLE_ATCPHY_MODE_USB3);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_USB3);
}

static void atcphy_test_usb2_init_after_safe(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_powered_off(test);
	struct typec_mux_state safe = { .mode = TYPEC_STATE_SAFE };
	struct atcphy_test_configure config = {};
	struct phy *phy;

	phy = kunit_kzalloc(test, sizeof(*phy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, phy);
	phy_set_drvdata(phy, atcphy);
	test->priv = &config;
	kunit_activate_static_stub(test, atcphy_configure, atcphy_test_configure_stub);

	KUNIT_ASSERT_EQ(test, atcphy_mux_update(atcphy, &safe), 0);
	KUNIT_EXPECT_EQ(test, atcphy_usb2_init(phy), 0);
	KUNIT_EXPECT_EQ(test, config.calls, 1U);
	KUNIT_EXPECT_EQ(test, config.lane_requests, 0U);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_OFF);
}

static void atcphy_test_t8103_usb3_machine_scope(struct kunit *test)
{
	static const struct {
		const char *compatible;
		bool enabled;
	} cases[] = {
		{ "apple,t8103", true },
		{ "apple,t6000", false },
		{ "apple,t6001", false },
		{ "apple,t6002", false },
		{ "apple,t8112", false },
		{ "apple,t6020", false },
		{ "apple,t6021", false },
		{ "apple,t6022", false },
		{ "apple,t8122", false },
		{ "apple,t6030", false },
		{ "apple,t6031", false },
		{ "apple,t6034", false },
		{ "apple,t8140", false },
		{ "apple,t8142", false },
	};
	struct property compatible = { .name = "compatible" };
	struct device_node root = { .properties = &compatible };

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		compatible.value = (void *)cases[i].compatible;
		compatible.length = strlen(cases[i].compatible) + 1;
		/* M2 and Pro/Max PHYs select this same fallback hardware row. */
		KUNIT_EXPECT_EQ(test, atcphy_has_t8103_usb3_workaround(&atcphy_hw_t8103, &root),
				cases[i].enabled);
		KUNIT_EXPECT_FALSE(test,
			atcphy_has_t8103_usb3_workaround(&atcphy_hw_t8122, &root));
	}
}

static struct kunit_case atcphy_mux_test_cases[] = {
	KUNIT_CASE(atcphy_test_t8103_usb3_machine_scope),
	KUNIT_CASE(atcphy_test_target_mode),
	KUNIT_CASE(atcphy_test_target_mode_fixed_hub),
	KUNIT_CASE(atcphy_test_noop_same_mode),
	KUNIT_CASE(atcphy_test_noop_needs_work),
	KUNIT_CASE(atcphy_test_usb3_port_ready),
	KUNIT_CASE(atcphy_test_update_skips_usb),
	KUNIT_CASE(atcphy_test_update_takes_usb),
	KUNIT_CASE(atcphy_test_update_port_order),
	KUNIT_CASE(atcphy_test_update_port_failure),
	KUNIT_CASE(atcphy_test_update_port_owed),
	KUNIT_CASE(atcphy_test_update_port_retry),
	KUNIT_CASE(atcphy_test_update_unsupported),
	KUNIT_CASE(atcphy_test_update_port_taken),
	KUNIT_CASE(atcphy_test_set_port_on),
	KUNIT_CASE(atcphy_test_host_start),
	KUNIT_CASE(atcphy_test_stopped_power_off),
	KUNIT_CASE(atcphy_test_stopped_power_off_dp),
	KUNIT_CASE(atcphy_test_stopped_reset),
	KUNIT_CASE(atcphy_test_host_start_without_usb3),
	KUNIT_CASE(atcphy_test_election_held_safe),
	KUNIT_CASE(atcphy_test_election_held_usb),
	KUNIT_CASE(atcphy_test_transient_timeouts),
#if IS_REACHABLE(CONFIG_USB)
	KUNIT_CASE(atcphy_test_usb3_root),
#endif
	KUNIT_CASE(atcphy_test_safe_after_poweroff),
	KUNIT_CASE(atcphy_test_locked_safe_after_poweroff),
	KUNIT_CASE(atcphy_test_safe_live_pipe),
	KUNIT_CASE(atcphy_test_usb2_init_after_safe),
	{}
};

static struct kunit_suite atcphy_mux_test_suite = {
	.name = "phy-apple-atc-mux",
	.test_cases = atcphy_mux_test_cases,
};

kunit_test_suite(atcphy_mux_test_suite);

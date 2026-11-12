// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for CD321x Type-C states that the mux holds up
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_mux.h>

#include "tps6598x.h"

#define TEST_REG_INT_EVENT1	0x14
#define TEST_REG_INT_CLEAR1	0x18
#define TEST_REG_STATUS		0x1a
#define TEST_REG_DATA_STATUS	0x5f

/* Longer than the bursts of power elections seen so far, up to ~5.5 s */
#define TEST_ELECTIONS_MS	8000

/* A connector whose mode switch is the PHY, as on Apple machines */
static const struct property_entry cd321x_mux_test_switch_props[] = {
	PROPERTY_ENTRY_BOOL("mode-switch"),
	{ }
};

static const struct software_node cd321x_mux_test_phy_node = {
	.name = "cd321x-mux-test-phy",
	.properties = cd321x_mux_test_switch_props,
};

/* The DCP display route, the second mode switch of a J700's rear connector */
static const struct software_node cd321x_mux_test_display_node = {
	.name = "cd321x-mux-test-display",
	.properties = cd321x_mux_test_switch_props,
};

static const struct software_node_ref_args cd321x_mux_test_refs[] = {
	SOFTWARE_NODE_REFERENCE(&cd321x_mux_test_phy_node),
};

static const struct property_entry cd321x_mux_test_connector_props[] = {
	PROPERTY_ENTRY_REF_ARRAY("mode-switch", cd321x_mux_test_refs),
	{ }
};

static const struct software_node cd321x_mux_test_connector = {
	.name = "cd321x-mux-test-connector",
	.properties = cd321x_mux_test_connector_props,
};

/* The rear connector of a J700, as in t8140-j700.dts */
static const struct software_node_ref_args cd321x_mux_test_j700_refs[] = {
	SOFTWARE_NODE_REFERENCE(&cd321x_mux_test_phy_node),
	SOFTWARE_NODE_REFERENCE(&cd321x_mux_test_display_node),
};

static const struct property_entry cd321x_mux_test_j700_connector_props[] = {
	PROPERTY_ENTRY_REF_ARRAY("mode-switch", cd321x_mux_test_j700_refs),
	{ }
};

static const struct software_node cd321x_mux_test_j700_connector = {
	.name = "cd321x-mux-test-j700-connector",
	.properties = cd321x_mux_test_j700_connector_props,
};

static const struct software_node * const cd321x_mux_test_nodes[] = {
	&cd321x_mux_test_phy_node,
	&cd321x_mux_test_display_node,
	&cd321x_mux_test_connector,
	&cd321x_mux_test_j700_connector,
	NULL
};

struct cd321x_mux_test;

/*
 * A mode switch on the connector. While @held, as the rear port PHY of a J700
 * is through the power elections of a Mac taking another data role, when it
 * cannot park the PIPE, it fails every change with @held_ret. @log records the
 * states asked for: s(afe), u(sb) or o(ther), in capitals when refused, and
 * @refused counts the refusals. The display route also fails to quiesce,
 * before any provider changes, while @prepare_held.
 */
struct cd321x_mux_test_phy {
	struct cd321x_mux_test *test;
	bool held;
	bool prepare_held;
	int held_ret;
	char log[64];
	unsigned int refused;
	unsigned int last_refused_ms;
};

struct cd321x_mux_test {
	struct cd321x cd;
	u64 latched;
	u32 status;
	u32 data_status;
	struct cd321x_mux_test_phy phy;
	/* Only on a J700's rear connector, where it does not declare anything */
	struct cd321x_mux_test_phy display;
	/* The time the scheduled work has been run at */
	unsigned int now_ms;
	/* The time the driver's latest series of retries of a held mux began */
	unsigned int series_ms;
};

static int cd321x_mux_test_read(void *context, const void *reg, size_t reg_size,
				void *val, size_t val_size)
{
	struct cd321x_mux_test *t = context;

	memset(val, 0, val_size);
	switch (*(const u8 *)reg) {
	case TEST_REG_INT_EVENT1:
		memcpy(val, &t->latched, min(val_size, sizeof(t->latched)));
		break;
	case TEST_REG_STATUS:
		memcpy(val, &t->status, min(val_size, sizeof(t->status)));
		break;
	case TEST_REG_DATA_STATUS:
		memcpy(val, &t->data_status, min(val_size, sizeof(t->data_status)));
		break;
	}

	return 0;
}

static int cd321x_mux_test_write(void *context, const void *data, size_t count)
{
	struct cd321x_mux_test *t = context;
	const u8 *buf = data;
	u64 clear = 0;

	if (buf[0] == TEST_REG_INT_CLEAR1) {
		memcpy(&clear, buf + 1, min(count - 1, sizeof(clear)));
		t->latched &= ~clear;
	}

	return 0;
}

static const struct regmap_bus cd321x_mux_test_bus = {
	.read = cd321x_mux_test_read,
	.write = cd321x_mux_test_write,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

static int cd321x_mux_test_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct cd321x_mux_test_phy *phy = typec_mux_get_drvdata(mux);
	size_t len = strlen(phy->log);
	char step;

	if (state->mode == TYPEC_STATE_SAFE)
		step = 's';
	else if (state->mode == TYPEC_STATE_USB && !state->alt)
		step = 'u';
	else
		step = 'o';
	if (phy->held) {
		step = toupper(step);
		phy->refused++;
		phy->last_refused_ms = phy->test->now_ms;
	}
	if (len < sizeof(phy->log) - 1)
		phy->log[len] = step;

	return phy->held ? phy->held_ret : 0;
}

static int cd321x_mux_test_prepare(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct cd321x_mux_test_phy *phy = typec_mux_get_drvdata(mux);

	return phy->prepare_held ? phy->held_ret : 0;
}

static int cd321x_mux_test_psy_get(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	val->intval = 0;
	return 0;
}

static const enum power_supply_property cd321x_mux_test_psy_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc cd321x_mux_test_psy_desc = {
	.name = "cd321x-mux-test",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = cd321x_mux_test_psy_props,
	.num_properties = ARRAY_SIZE(cd321x_mux_test_psy_props),
	.get_property = cd321x_mux_test_psy_get,
};

static void cd321x_mux_test_unregister_nodes(void *unused)
{
	/*
	 * Software nodes take part in fw_devlink, so the port is a consumer
	 * of the PHY. The port is released only when that device link is,
	 * from device_link_wq, and the release drops the port's references
	 * to the nodes. Let it happen before the nodes go.
	 */
	device_link_wait_removal();
	software_node_unregister_node_group(cd321x_mux_test_nodes);
}

static void cd321x_mux_test_unregister_mux(void *mux)
{
	typec_mux_unregister(mux);
}

static void cd321x_mux_test_unregister_port(void *port)
{
	typec_unregister_port(port);
}

static void cd321x_mux_test_put_mux(void *mux)
{
	typec_mux_put(mux);
}

static void cd321x_mux_test_unregister_psy(void *psy)
{
	power_supply_unregister(psy);
}

static void cd321x_mux_test_stop(void *data)
{
	struct cd321x *cd = data;

	scoped_guard(mutex, &cd->tps.lock)
		cd321x_pm_remove(&cd->pm);
	cancel_delayed_work_sync(&cd->resume_work);
	cancel_delayed_work_sync(&cd->update_work);
	if (!IS_ERR_OR_NULL(cd->tps.partner))
		typec_unregister_partner(cd->tps.partner);
	cd->tps.partner = NULL;
}

/*
 * A running port with nothing attached, whose mux is the test PHY, and on a
 * @j700 rear connector the display route after it. Only a PHY with @transient
 * timeouts, the rear port PHY of a J700, has the driver retry its timeouts
 * until they pass.
 */
static struct cd321x_mux_test *cd321x_mux_test_setup(struct kunit *test, bool transient,
						     bool j700)
{
	struct typec_mux_desc mux_desc = {
		.set = cd321x_mux_test_set,
		.transient_timeouts = transient,
	};
	struct typec_mux_desc display_desc = {
		.set = cd321x_mux_test_set,
		.prepare = cd321x_mux_test_prepare,
		.name = "cd321x-mux-test-display",
	};
	struct typec_capability cap = {
		.type = TYPEC_PORT_DRP,
		.data = TYPEC_PORT_DRD,
		.revision = USB_TYPEC_REV_1_2,
		.prefer_role = TYPEC_NO_PREFERRED_ROLE,
	};
	struct power_supply_config psy_cfg = {};
	struct fwnode_handle *connector;
	struct typec_mux_dev *mux_dev;
	struct cd321x_mux_test *t;
	struct device *dev;
	int ret;

	ret = software_node_register_node_group(cd321x_mux_test_nodes);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, cd321x_mux_test_unregister_nodes, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	connector = software_node_fwnode(j700 ? &cd321x_mux_test_j700_connector :
					 &cd321x_mux_test_connector);
	mux_desc.fwnode = software_node_fwnode(&cd321x_mux_test_phy_node);
	display_desc.fwnode = software_node_fwnode(&cd321x_mux_test_display_node);

	dev = kunit_device_register(test, "cd321x-mux-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->phy.test = t;
	t->phy.held_ret = -ETIMEDOUT;
	t->display.test = t;
	t->display.held_ret = -ETIMEDOUT;

	mux_desc.drvdata = &t->phy;
	mux_dev = typec_mux_register(dev, &mux_desc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mux_dev);
	ret = kunit_add_action_or_reset(test, cd321x_mux_test_unregister_mux, mux_dev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	if (j700) {
		display_desc.drvdata = &t->display;
		mux_dev = typec_mux_register(dev, &display_desc);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mux_dev);
		ret = kunit_add_action_or_reset(test, cd321x_mux_test_unregister_mux, mux_dev);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	mutex_init(&t->cd.tps.lock);
	t->cd.tps.dev = dev;
	t->cd.tps.data = tipd_hw_variants[TIPD_VARIANT_CD321X];
	t->cd.tps.regmap = devm_regmap_init(dev, &cd321x_mux_test_bus, t,
					    &tps6598x_regmap_config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->cd.tps.regmap);
	dev_set_drvdata(dev, &t->cd.tps);

	cap.fwnode = connector;
	cap.driver_data = &t->cd.tps;
	t->cd.tps.port = typec_register_port(dev, &cap);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->cd.tps.port);
	ret = kunit_add_action_or_reset(test, cd321x_mux_test_unregister_port, t->cd.tps.port);
	KUNIT_ASSERT_EQ(test, ret, 0);

	t->cd.mux = fwnode_typec_mux_get(connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->cd.mux);
	ret = kunit_add_action_or_reset(test, cd321x_mux_test_put_mux, t->cd.mux);
	KUNIT_ASSERT_EQ(test, ret, 0);

	psy_cfg.drv_data = &t->cd.tps;
	t->cd.tps.psy = power_supply_register(dev, &cd321x_mux_test_psy_desc, &psy_cfg);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->cd.tps.psy);
	ret = kunit_add_action_or_reset(test, cd321x_mux_test_unregister_psy, t->cd.tps.psy);
	KUNIT_ASSERT_EQ(test, ret, 0);

	cd321x_init_work(&t->cd);
	t->cd.pm.phase = CD321X_PM_RUNNING;
	ret = kunit_add_action_or_reset(test, cd321x_mux_test_stop, &t->cd);
	KUNIT_ASSERT_EQ(test, ret, 0);

	return t;
}

static struct cd321x_mux_test *cd321x_mux_test_port(struct kunit *test, bool transient)
{
	return cd321x_mux_test_setup(test, transient, false);
}

/* The rear port of a J700: its PHY declares its timeouts transient. */
static struct cd321x_mux_test *cd321x_mux_test_j700_port(struct kunit *test)
{
	return cd321x_mux_test_setup(test, true, true);
}

/* A controller event, delivered as the interrupt handler would get it */
static void cd321x_mux_test_event(struct cd321x_mux_test *t, u64 event)
{
	t->latched |= event;
	t->cd.tps.data->irq_handler(0, &t->cd.tps);
}

/* A Mac attaches, the port being the USB host */
static void cd321x_mux_test_attach(struct cd321x_mux_test *t)
{
	t->status = TPS_STATUS_PLUG_PRESENT;
	t->data_status = TPS_DATA_STATUS_DATA_CONNECTION | TPS_DATA_STATUS_USB2_CONNECTION |
			 TPS_DATA_STATUS_USB3_CONNECTION;
	cd321x_mux_test_event(t, APPLE_CD_REG_INT_PLUG_EVENT |
			      APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
}

/* The delay the work was scheduled with, from now */
static unsigned int cd321x_mux_test_due_ms(struct delayed_work *dwork)
{
	long left = (long)(dwork->timer.expires - jiffies);

	return left > 0 ? jiffies_to_msecs(left) : 0;
}

/*
 * Run @dwork now, rather than after its delay. The driver also measures how
 * long it has retried a held mux, on its own clock, so a series that is under
 * way is made as old there as it is in the time of the tests.
 */
static void cd321x_mux_test_run(struct cd321x_mux_test *t, struct delayed_work *dwork)
{
	struct cd321x_pm_state *pm = &t->cd.pm;
	bool retrying;

	scoped_guard(mutex, &t->cd.tps.lock) {
		retrying = pm->mux_retry_ms;
		if (retrying)
			pm->mux_since_ms = jiffies_to_msecs(jiffies) -
					   (t->now_ms - t->series_ms);
	}
	flush_delayed_work(dwork);
	scoped_guard(mutex, &t->cd.tps.lock) {
		if (!retrying && pm->mux_retry_ms)
			t->series_ms = t->now_ms;
	}
}

/*
 * Run the next work of the port, the one due first, as if its delay had passed.
 * Returns false if none is scheduled.
 */
static bool cd321x_mux_test_next(struct cd321x_mux_test *t)
{
	struct delayed_work *resume = &t->cd.resume_work, *update = &t->cd.update_work;
	struct delayed_work *next;

	/* A work whose delay ran out meanwhile may be running already. */
	if (!delayed_work_pending(resume) && !delayed_work_pending(update)) {
		flush_delayed_work(resume);
		flush_delayed_work(update);
	}
	if (delayed_work_pending(resume) &&
	    (!delayed_work_pending(update) ||
	     time_before_eq(resume->timer.expires, update->timer.expires)))
		next = resume;
	else if (delayed_work_pending(update))
		next = update;
	else
		return false;

	t->now_ms += cd321x_mux_test_due_ms(next);
	cd321x_mux_test_run(t, next);
	return true;
}

/* Run the work of the port until none is left, which must be soon. */
static void cd321x_mux_test_settle(struct kunit *test, struct cd321x_mux_test *t)
{
	unsigned int i;

	for (i = 0; i < 16 && cd321x_mux_test_next(t); i++)
		;
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.update_work));
}

static enum cd321x_pm_phase cd321x_mux_test_phase(struct cd321x_mux_test *t)
{
	guard(mutex)(&t->cd.tps.lock);
	return t->cd.pm.phase;
}

/*
 * The delay of the retry of a held mux. Unlike what is left of it, this does
 * not shrink as real time passes while the tests run.
 */
static unsigned int cd321x_mux_test_backoff_ms(struct cd321x_mux_test *t)
{
	guard(mutex)(&t->cd.tps.lock);
	return t->cd.pm.mux_retry_ms;
}

/*
 * The resume read is queued without delay, so with no timer, rather than after
 * a backoff. A worker may run it already.
 */
static void cd321x_mux_test_expect_read_now(struct kunit *test, struct cd321x_mux_test *t)
{
	struct delayed_work *resume = &t->cd.resume_work;

	KUNIT_EXPECT_FALSE(test, timer_pending(&resume->timer));
	flush_delayed_work(resume);
}

/* The state is applied: the mux set to USB, and nothing retried any more */
static void cd321x_mux_test_expect_usb(struct kunit *test, struct cd321x_mux_test *t)
{
	KUNIT_EXPECT_EQ(test, cd321x_mux_test_phase(t), CD321X_PM_RUNNING);
	KUNIT_EXPECT_TRUE(test, t->cd.state_valid);
	KUNIT_EXPECT_EQ(test, t->cd.state.mode, TYPEC_STATE_USB);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0);
	KUNIT_EXPECT_NOT_NULL(test, t->cd.tps.partner);
}

/*
 * Through the elections the PHY refuses the safe state of the new connection
 * for longer than the three cable-state attempts last. A mux request for USB
 * must still follow once they settle, with no further controller event.
 */
static void cd321x_mux_held_past_budget_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, true);

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	while (t->now_ms < TEST_ELECTIONS_MS) {
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));
		KUNIT_ASSERT_NE(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	}
	/* Refused well past the third attempt, never asked for USB */
	KUNIT_EXPECT_GT(test, t->phy.last_refused_ms, 3000U);
	KUNIT_EXPECT_NULL(test, strpbrk(t->phy.log, "uU"));
	KUNIT_EXPECT_GE(test, strlen(t->phy.log), (size_t)CD321X_RESUME_ATTEMPTS + 1);

	t->phy.held = false;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	cd321x_mux_test_expect_usb(test, t);
}

/* Retried more and more slowly while held, but at least every ten seconds */
static void cd321x_mux_held_backs_off_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, true);
	unsigned int i, first = 0, delay = 0;

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	for (i = 0; i < 8 * CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));
		if (!delayed_work_pending(&t->cd.resume_work))
			continue;
		delay = cd321x_mux_test_backoff_ms(t);
		if (!first)
			first = delay;
		KUNIT_EXPECT_LE(test, delay, (unsigned int)CD321X_MUX_RETRY_MAX_MS);
		KUNIT_EXPECT_NE(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
		KUNIT_EXPECT_GT(test, t->cd.pm.attempts_left, 0);
	}
	KUNIT_EXPECT_GE(test, delay, 8 * first);
	KUNIT_EXPECT_GT(test, t->now_ms, 30000U);

	t->phy.held = false;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	cd321x_mux_test_expect_usb(test, t);
}

/*
 * A controller event during the backoff takes over from the retry: its state
 * is applied after the usual debounce, and the retry that was due later steps
 * aside rather than repeating the change.
 */
static void cd321x_mux_held_event_takes_over_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, true);

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	while (!delayed_work_pending(&t->cd.resume_work) ||
	       cd321x_mux_test_backoff_ms(t) < 4000)
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));

	/* The elections end with the Mac taking the device role */
	t->phy.held = false;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	t->data_status |= TPS_DATA_STATUS_USB_DATA_ROLE;
	cd321x_mux_test_event(t, APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
	KUNIT_ASSERT_TRUE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_LE(test, cd321x_mux_test_due_ms(&t->cd.resume_work), 500U);

	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	cd321x_mux_test_expect_usb(test, t);
	KUNIT_EXPECT_LT(test, t->now_ms, TEST_ELECTIONS_MS);
}

/* Sleep cancels the retry, and the resume read takes it over */
static void cd321x_mux_held_across_sleep_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, true);
	struct device *dev = t->cd.tps.dev;
	unsigned int i;

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	for (i = 0; i < 4; i++)
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));

	KUNIT_ASSERT_EQ(test, tipd_prepare(dev), 0);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.update_work));
	tipd_complete(dev);
	/* The resume read is queued without delay: a worker may run it already. */
	flush_delayed_work(&t->cd.resume_work);

	/* Still held after resume: the state is retried, not given up */
	memset(t->phy.log, 0, sizeof(t->phy.log));
	for (i = 0; i < 6 * CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));
		KUNIT_EXPECT_NE(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	}
	KUNIT_EXPECT_GT(test, strlen(t->phy.log), (size_t)CD321X_RESUME_ATTEMPTS + 1);

	t->phy.held = false;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	cd321x_mux_test_expect_usb(test, t);
}

/* Removal ends the retry */
static void cd321x_mux_held_ends_on_removal_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, true);

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));
	KUNIT_ASSERT_TRUE(test, delayed_work_pending(&t->cd.resume_work));

	scoped_guard(mutex, &t->cd.tps.lock)
		cd321x_pm_remove(&t->cd.pm);
	memset(t->phy.log, 0, sizeof(t->phy.log));
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "");
}

/*
 * Every other mux, such as the PHY of any M1 or M2 port, keeps the bounded
 * cable-state retries: the driver gives up after them, and the next event
 * starts over.
 */
static void cd321x_mux_timeout_bounded_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, false);

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "SSSS");
	KUNIT_EXPECT_EQ(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0);

	t->phy.held = false;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	cd321x_mux_test_event(t, APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
	flush_delayed_work(&t->cd.resume_work);
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	cd321x_mux_test_expect_usb(test, t);
}

/* and on the J700, any failure but a timeout does too */
static void cd321x_mux_other_error_bounded_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, true);

	t->phy.held = true;
	t->phy.held_ret = -EBUSY;
	cd321x_mux_test_attach(t);
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "SSSS");
	KUNIT_EXPECT_EQ(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0);
}

/*
 * The rear connector of a J700 has a second mode switch after the PHY, the DCP
 * display route, which does not declare its timeouts transient. A refusal of
 * the PHY is still retried past the budget, and once the elections settle
 * both providers get the safe state and then the USB one.
 */
static void cd321x_mux_j700_held_past_budget_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_j700_port(test);

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	while (t->now_ms < TEST_ELECTIONS_MS) {
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));
		KUNIT_ASSERT_NE(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	}
	KUNIT_EXPECT_GT(test, t->phy.last_refused_ms, 3000U);
	KUNIT_EXPECT_NULL(test, strpbrk(t->phy.log, "uU"));
	KUNIT_EXPECT_GE(test, strlen(t->phy.log), (size_t)CD321X_RESUME_ATTEMPTS + 1);

	t->phy.held = false;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	memset(t->display.log, 0, sizeof(t->display.log));
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	KUNIT_EXPECT_STREQ(test, t->display.log, "su");
	cd321x_mux_test_expect_usb(test, t);
}

/* There a timeout of the display route keeps the bounded retries. */
static void cd321x_mux_j700_display_timeout_bounded_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_j700_port(test);

	t->display.held = true;
	cd321x_mux_test_attach(t);
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->display.log, "SSSS");
	KUNIT_EXPECT_EQ(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0);

	t->display.held = false;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	memset(t->display.log, 0, sizeof(t->display.log));
	cd321x_mux_test_event(t, APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
	flush_delayed_work(&t->cd.resume_work);
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	KUNIT_EXPECT_STREQ(test, t->display.log, "su");
	cd321x_mux_test_expect_usb(test, t);
}

/*
 * So does a timeout of the display route while it quiesces the display, before
 * any provider changes, even right after the PHY held changes up.
 */
static void cd321x_mux_j700_display_prepare_timeout_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_j700_port(test);

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	while (!delayed_work_pending(&t->cd.resume_work) ||
	       cd321x_mux_test_backoff_ms(t) < 2000)
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));

	t->phy.held = false;
	t->display.prepare_held = true;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "");
	KUNIT_EXPECT_EQ(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0);

	t->display.prepare_held = false;
	memset(t->display.log, 0, sizeof(t->display.log));
	cd321x_mux_test_event(t, APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
	cd321x_mux_test_expect_read_now(test, t);
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	KUNIT_EXPECT_STREQ(test, t->display.log, "su");
	cd321x_mux_test_expect_usb(test, t);
}

/*
 * A PHY that stays stuck is taken for a fault two minutes after its first
 * refusal, at the first retry from then on. The port then waits for the next
 * controller event, which starts the retries over.
 */
static void cd321x_mux_held_gives_up_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_j700_port(test);
	unsigned int i;

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	while (t->now_ms < 2 * CD321X_MUX_HELD_MS && cd321x_mux_test_next(t))
		;
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.update_work));
	KUNIT_EXPECT_EQ(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	KUNIT_EXPECT_EQ(test, strlen(t->phy.log), (size_t)t->phy.refused);
	KUNIT_EXPECT_NULL(test, strpbrk(t->phy.log, "suU"));
	KUNIT_EXPECT_GE(test, t->phy.last_refused_ms, t->series_ms + CD321X_MUX_HELD_MS);
	KUNIT_EXPECT_LE(test, t->phy.last_refused_ms,
			t->series_ms + CD321X_MUX_HELD_MS + CD321X_MUX_RETRY_MAX_MS + 1000);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0);
	KUNIT_EXPECT_FALSE(test, t->cd.state_valid);

	/* Still stuck at the next event: the retries start over */
	memset(t->phy.log, 0, sizeof(t->phy.log));
	cd321x_mux_test_event(t, APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
	cd321x_mux_test_expect_read_now(test, t);
	for (i = 0; i < 8 && !strchr(t->phy.log, 'S'); i++)
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));
	KUNIT_ASSERT_STREQ(test, t->phy.log, "S");
	KUNIT_ASSERT_TRUE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_LE(test, cd321x_mux_test_due_ms(&t->cd.resume_work),
			(unsigned int)CD321X_MUX_RETRY_MS);

	t->phy.held = false;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	cd321x_mux_test_expect_usb(test, t);
}

/*
 * Wait for the next refusal of the PHY, which needs a retry to be scheduled,
 * injecting a controller event, as a partner that keeps renegotiating raises,
 * into each backoff that started at least @gap_ms after the last event.
 * Returns the number of events.
 */
static unsigned int cd321x_mux_test_refusal_events(struct kunit *test,
						   struct cd321x_mux_test *t,
						   unsigned int gap_ms,
						   unsigned int *last_event_ms)
{
	unsigned int refused = t->phy.refused, events = 0;

	while (t->phy.refused == refused) {
		if (t->now_ms - *last_event_ms >= gap_ms && cd321x_mux_test_backoff_ms(t) &&
		    cd321x_mux_test_phase(t) == CD321X_PM_REVALIDATE &&
		    delayed_work_pending(&t->cd.resume_work)) {
			cd321x_mux_test_event(t, APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
			*last_event_ms = t->now_ms;
			events++;
		}
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));
	}
	return events;
}

/*
 * An event in every backoff brings each retry forward, to about a second after
 * the last, but does not make the hold-up any shorter: the port still gives
 * up only two minutes after the first refusal, not after so many retries.
 */
static void cd321x_mux_held_dense_events_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_j700_port(test);
	unsigned int events = 0, last_event_ms = 0;

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	cd321x_mux_test_refusal_events(test, t, UINT_MAX, &last_event_ms);
	KUNIT_ASSERT_EQ(test, t->phy.refused, 1);
	while (cd321x_mux_test_phase(t) != CD321X_PM_STALE) {
		KUNIT_ASSERT_LT(test, t->now_ms, t->series_ms + 2 * CD321X_MUX_HELD_MS);
		events += cd321x_mux_test_refusal_events(test, t, 0, &last_event_ms);
	}
	KUNIT_EXPECT_GE(test, t->phy.last_refused_ms, t->series_ms + CD321X_MUX_HELD_MS);
	KUNIT_EXPECT_LE(test, t->phy.last_refused_ms, t->series_ms + CD321X_MUX_HELD_MS + 1000);
	/* So the retries came about a second apart, each one brought forward. */
	KUNIT_EXPECT_GE(test, t->phy.refused, CD321X_MUX_HELD_MS / 1100);
	KUNIT_EXPECT_EQ(test, events, t->phy.refused - 1);
	KUNIT_EXPECT_NULL(test, strpbrk(t->phy.log, "suU"));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.update_work));
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0);
}

/*
 * When the PHY lets go after the last of such events, the next retry of the
 * backoff applies the state, with no further event, provided the hold-up was
 * shorter than two minutes, however many retries events brought forward.
 */
static void cd321x_mux_held_release_after_events_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_j700_port(test);
	unsigned int last_event_ms = 0, released_ms;

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	cd321x_mux_test_refusal_events(test, t, UINT_MAX, &last_event_ms);
	while (t->now_ms < t->series_ms + CD321X_MUX_HELD_MS - 2 * CD321X_MUX_RETRY_MAX_MS) {
		cd321x_mux_test_refusal_events(test, t, 1500, &last_event_ms);
		KUNIT_ASSERT_NE(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	}
	KUNIT_ASSERT_TRUE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_ASSERT_GT(test, t->phy.refused, 16);

	t->phy.held = false;
	released_ms = t->now_ms;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	memset(t->display.log, 0, sizeof(t->display.log));
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	KUNIT_EXPECT_STREQ(test, t->display.log, "su");
	cd321x_mux_test_expect_usb(test, t);
	KUNIT_EXPECT_LE(test, t->now_ms, released_ms + CD321X_MUX_RETRY_MAX_MS + 1000);
}

/*
 * A series of retries that ends in another failure leaves no backoff behind:
 * the next event is read without delay, and the next hold-up starts over at
 * the first delay.
 */
static void cd321x_mux_held_then_other_error_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, true);
	unsigned int i;

	t->phy.held = true;
	cd321x_mux_test_attach(t);
	while (!delayed_work_pending(&t->cd.resume_work) ||
	       cd321x_mux_test_backoff_ms(t) < 4000)
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));

	t->phy.held_ret = -EBUSY;
	cd321x_mux_test_settle(test, t);
	KUNIT_ASSERT_EQ(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0);

	t->phy.held_ret = -ETIMEDOUT;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	t->data_status |= TPS_DATA_STATUS_USB_DATA_ROLE;
	cd321x_mux_test_event(t, APPLE_CD_REG_INT_DATA_STATUS_UPDATE);
	cd321x_mux_test_expect_read_now(test, t);
	for (i = 0; i < 8 && !strchr(t->phy.log, 'S'); i++)
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));
	KUNIT_ASSERT_NOT_NULL(test, strchr(t->phy.log, 'S'));
	KUNIT_ASSERT_TRUE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_LE(test, cd321x_mux_test_due_ms(&t->cd.resume_work),
			(unsigned int)CD321X_MUX_RETRY_MS);

	t->phy.held = false;
	cd321x_mux_test_settle(test, t);
	cd321x_mux_test_expect_usb(test, t);
}

/* A dock already attached when probe's first SAFE request times out. */
static struct cd321x_mux_test *cd321x_mux_test_boot(struct kunit *test, bool transient)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, transient);

	cd321x_pm_init(&t->cd.pm);
	t->status = TPS_STATUS_PLUG_PRESENT;
	t->data_status = TPS_DATA_STATUS_DATA_CONNECTION | TPS_DATA_STATUS_USB2_CONNECTION |
			 TPS_DATA_STATUS_USB3_CONNECTION;
	t->phy.held = true;
	KUNIT_ASSERT_EQ(test, cd321x_init_mux(&t->cd), 0);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "S");
	KUNIT_EXPECT_EQ(test, t->cd.pm.phase, CD321X_PM_INITIALIZING);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.update_work));
	KUNIT_EXPECT_FALSE(test, t->cd.state_valid);

	return t;
}

static void cd321x_mux_boot_retries_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_boot(test, true);

	KUNIT_ASSERT_NE(test, t->cd.pm.mux_retry_ms, 0U);
	t->cd.tps.data->ready(&t->cd.tps);
	cd321x_mux_test_expect_read_now(test, t);
	while (t->now_ms < TEST_ELECTIONS_MS)
		KUNIT_ASSERT_TRUE(test, cd321x_mux_test_next(t));
	KUNIT_EXPECT_NE(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	/* No interrupt or physical replug is needed after the lock is released. */
	t->phy.held = false;
	memset(t->phy.log, 0, sizeof(t->phy.log));
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_STREQ(test, t->phy.log, "su");
	cd321x_mux_test_expect_usb(test, t);
}

static void cd321x_mux_boot_ceiling_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_boot(test, true);
	unsigned int i;

	t->cd.tps.data->ready(&t->cd.tps);
	cd321x_mux_test_expect_read_now(test, t);
	for (i = 0; i < 4 * CD321X_MUX_RETRIES && cd321x_mux_test_next(t); i++)
		;
	KUNIT_EXPECT_GE(test, t->now_ms, CD321X_MUX_HELD_MS);
	KUNIT_EXPECT_EQ(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0U);
	KUNIT_EXPECT_FALSE(test, t->cd.state_valid);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.update_work));
}

/* M1/M2 providers retain their ordinary three-attempt recovery budget. */
static void cd321x_mux_boot_nontransient_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_boot(test, false);

	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0U);
	t->cd.tps.data->ready(&t->cd.tps);
	cd321x_mux_test_expect_read_now(test, t);
	cd321x_mux_test_settle(test, t);
	KUNIT_EXPECT_EQ(test, cd321x_mux_test_phase(t), CD321X_PM_STALE);
	KUNIT_EXPECT_LT(test, t->now_ms, TEST_ELECTIONS_MS);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0U);
}

static void cd321x_mux_boot_defer_test(struct kunit *test)
{
	struct cd321x_mux_test *t = cd321x_mux_test_port(test, true);

	cd321x_pm_init(&t->cd.pm);
	t->phy.held = true;
	t->phy.held_ret = -EAGAIN;
	KUNIT_EXPECT_EQ(test, cd321x_init_mux(&t->cd), -EPROBE_DEFER);
	KUNIT_EXPECT_EQ(test, t->cd.pm.mux_retry_ms, 0U);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->cd.resume_work));
}

static struct kunit_case cd321x_mux_cases[] = {
	KUNIT_CASE(cd321x_mux_boot_retries_test),
	KUNIT_CASE(cd321x_mux_boot_ceiling_test),
	KUNIT_CASE(cd321x_mux_boot_nontransient_test),
	KUNIT_CASE(cd321x_mux_boot_defer_test),
	KUNIT_CASE(cd321x_mux_held_past_budget_test),
	KUNIT_CASE(cd321x_mux_held_backs_off_test),
	KUNIT_CASE(cd321x_mux_held_event_takes_over_test),
	KUNIT_CASE(cd321x_mux_held_across_sleep_test),
	KUNIT_CASE(cd321x_mux_held_ends_on_removal_test),
	KUNIT_CASE(cd321x_mux_timeout_bounded_test),
	KUNIT_CASE(cd321x_mux_other_error_bounded_test),
	KUNIT_CASE(cd321x_mux_j700_held_past_budget_test),
	KUNIT_CASE(cd321x_mux_j700_display_timeout_bounded_test),
	KUNIT_CASE(cd321x_mux_j700_display_prepare_timeout_test),
	KUNIT_CASE(cd321x_mux_held_gives_up_test),
	KUNIT_CASE(cd321x_mux_held_dense_events_test),
	KUNIT_CASE(cd321x_mux_held_release_after_events_test),
	KUNIT_CASE(cd321x_mux_held_then_other_error_test),
	{}
};

static struct kunit_suite cd321x_mux_suite = {
	.name = "cd321x-mux",
	.test_cases = cd321x_mux_cases,
};

kunit_test_suite(cd321x_mux_suite);
MODULE_DESCRIPTION("KUnit tests for CD321x Type-C states that the mux holds up");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

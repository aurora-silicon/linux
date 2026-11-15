// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/device.h>
#include <kunit/platform_device.h>
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "connector.h"
#include "dcp.h"
#include "dcp-internal.h"

/*
 * An external H17P DCP back from system sleep on the same cable: suspend
 * stopped its pipe and advanced the cable generation, and the CRTC is off.
 */
struct external_resume_test {
	struct apple_dcp *dcp;
};

/* What the firmware hotplug callback does, under the same lock. */
static bool external_resume_hotplug(struct apple_dcp *dcp, bool connected)
{
	guard(mutex)(&dcp->iomfb.lock);
	return dcp_external_hotplug(dcp, connected);
}

static struct external_resume_test *external_resume_init(struct kunit *test)
{
	struct external_resume_test *priv;
	struct apple_dcp *dcp;
	struct device *dev;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dev = kunit_device_register(test, "dcp-external-resume");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dcp->dev = dev;
	dcp->hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL;
	spin_lock_init(&dcp->metadata.lock);
	mutex_init(&dcp->iomfb.lock);

	/* Connected and announced before the sleep. */
	dcp->typec_cable_connected = true;
	dcp->typec_generation = 1;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true);
	dcp_firmware_metadata_hotplug(dcp, true);
	dcp->external_cable = true;
	dcp->external_power_attempted = true;
	dcp->external_generation = 1;
	dcp->external_rx_generation = 1;
	dcp->external_crtc_generation = 1;

	/* Suspend: the pipe stopped, then the cable generation advanced. */
	dcp->external_stopped = true;
	dcp->iomfb.presentation_paused = true;
	dcp->typec_generation++;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
	/* Resume: the reconnect work requested the link again. */
	dcp->dptxport[0].connected = true;

	priv->dcp = dcp;

	return priv;
}

/* The announcement after the link returns must count with the pipe stopped. */
static void external_paused_announce_recorded_test(struct kunit *test)
{
	struct external_resume_test *priv = external_resume_init(test);
	struct apple_dcp *dcp = priv->dcp;

	KUNIT_ASSERT_FALSE(test, dcp->metadata.epoch.connected);
	/* A stopped pipe consumes the callback. */
	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, true));

	KUNIT_EXPECT_TRUE(test, dcp->metadata.epoch.connected);
	KUNIT_EXPECT_EQ(test, dcp->metadata.epoch.generation, dcp->typec_generation);
	/* It is replayed to DRM only when the pipe starts again. */
	KUNIT_EXPECT_TRUE(test, dcp->external_hotplug_pending);
	KUNIT_EXPECT_TRUE(test, dcp->external_hotplug_connected);
}

/* The unplug a DPMS-off stop causes must not revoke the link proof. */
static void external_paused_unplug_ignored_test(struct kunit *test)
{
	struct external_resume_test *priv = external_resume_init(test);
	struct apple_dcp *dcp = priv->dcp;
	u64 connection_sequence;

	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, true));
	connection_sequence = dcp->rate_proof.connection_sequence;
	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, false));

	KUNIT_EXPECT_TRUE(test, dcp->metadata.epoch.connected);
	KUNIT_EXPECT_EQ(test, dcp->rate_proof.connection_sequence, connection_sequence);
}

/* An announcement on an older cable generation is not current. */
static void external_paused_stale_announce_test(struct kunit *test)
{
	struct external_resume_test *priv = external_resume_init(test);
	struct apple_dcp *dcp = priv->dcp;

	dcp->typec_generation++;
	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, true));

	KUNIT_EXPECT_FALSE(test, dcp->metadata.epoch.connected);
}

/*
 * A callback delayed past a release of the link is not an announcement of
 * the next one, even once software has moved both generations on.
 */
static void external_paused_released_link_test(struct kunit *test)
{
	struct external_resume_test *priv = external_resume_init(test);
	struct apple_dcp *dcp = priv->dcp;

	dcp->dptxport[0].connected = false;
	dcp->typec_generation++;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, true));

	KUNIT_EXPECT_FALSE(test, dcp->metadata.epoch.connected);
}

/*
 * The same display as apple_platform_resume() finds it: reported before the
 * sleep, its CRTC just resumed disabled, the link not back yet.
 */
struct resume_hold_test {
	struct platform_device *pdev;
	struct apple_dcp *dcp;
	struct apple_connector *con;
	struct apple_connector connector;
	atomic_t events;
	atomic_t timer_runs;
};

static void resume_hold_event(struct work_struct *work)
{
	struct resume_hold_test *t = container_of(work, struct resume_hold_test,
						  connector.hotplug_wq);

	atomic_inc(&t->events);
}

static void resume_hold_timer(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp,
					     announce_wq);
	struct resume_hold_test *t = container_of(dcp->connector, struct resume_hold_test,
						  connector);

	atomic_inc(&t->timer_runs);
}

static void resume_hold_exit(void *data)
{
	struct resume_hold_test *t = data;

	cancel_delayed_work_sync(&t->dcp->announce_wq);
	cancel_work_sync(&t->con->hotplug_wq);
}

static struct resume_hold_test *resume_hold_init(struct kunit *test)
{
	struct resume_hold_test *t;
	struct apple_dcp *dcp;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	t->dcp = dcp;
	t->con = &t->connector;
	t->pdev = kunit_platform_device_alloc(test, "dcp-resume-hold", PLATFORM_DEVID_AUTO);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->pdev);
	KUNIT_ASSERT_EQ(test, kunit_platform_device_add(test, t->pdev), 0);
	platform_set_drvdata(t->pdev, dcp);

	dcp->dev = &t->pdev->dev;
	dcp->hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL;
	mutex_init(&dcp->hpd_mutex);
	mutex_init(&dcp->dcpavserv.lock);
	spin_lock_init(&dcp->metadata.lock);
	INIT_DELAYED_WORK(&dcp->announce_wq, resume_hold_timer);
	INIT_WORK(&t->con->hotplug_wq, resume_hold_event);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, resume_hold_exit, t), 0);

	t->con->dcp = t->pdev;
	t->con->connected = true;
	dcp->connector = t->con;
	dcp->announce.announced = true;
	dcp->typec_cable_connected = true;
	dcp->typec_generation = 2;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);

	return t;
}

static bool resume_hold_detect(struct resume_hold_test *t)
{
	bool connected = dcp_connector_connected(t->con);

	flush_work(&t->con->hotplug_wq);
	return connected;
}

/* The event is held, never the connection: a probe still finds it. */
static void resume_hold_armed_test(struct kunit *test)
{
	struct resume_hold_test *t = resume_hold_init(test);

	KUNIT_EXPECT_TRUE(test, dcp_hold_resume_event(t->pdev));
	KUNIT_EXPECT_TRUE(test, t->dcp->announce.holding);
	KUNIT_EXPECT_EQ(test, t->dcp->announce.generation, 2);
	/*
	 * The timer work starts the timeout once it can run, so it is queued
	 * at once.  It may have run already: count its runs after a flush.
	 */
	KUNIT_EXPECT_FALSE(test, t->dcp->announce.timed);
	flush_delayed_work(&t->dcp->announce_wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->timer_runs), 1);

	KUNIT_EXPECT_TRUE(test, resume_hold_detect(t));
	KUNIT_EXPECT_TRUE(test, t->dcp->announce.holding);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->events), 0);
}

/* A connection never reported has no resume event to hold. */
static void resume_hold_unreported_test(struct kunit *test)
{
	struct resume_hold_test *t = resume_hold_init(test);

	t->dcp->announce.announced = false;
	KUNIT_EXPECT_FALSE(test, dcp_hold_resume_event(t->pdev));
	KUNIT_EXPECT_FALSE(test, t->dcp->announce.holding);
}

/* Other pipelines keep sending the event at once. */
static void resume_hold_internal_test(struct kunit *test)
{
	struct resume_hold_test *t = resume_hold_init(test);

	t->dcp->hw.iomfb_method_profile = DCP_IOMFB_METHODS_DEFAULT;
	KUNIT_EXPECT_FALSE(test, dcp_hold_resume_event(t->pdev));
	KUNIT_EXPECT_FALSE(test, t->dcp->announce.holding);
}

/* A cable unplugged during the sleep is reported at once, with one event. */
static void resume_hold_unplug_test(struct kunit *test)
{
	struct resume_hold_test *t = resume_hold_init(test);

	KUNIT_ASSERT_TRUE(test, dcp_hold_resume_event(t->pdev));
	t->dcp->external_physically_absent = true;

	KUNIT_EXPECT_FALSE(test, resume_hold_detect(t));
	KUNIT_EXPECT_FALSE(test, t->dcp->announce.holding);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->events), 1);
}

/* A link that failed will not get ready: the event is not held for it. */
static void resume_hold_failed_link_test(struct kunit *test)
{
	struct resume_hold_test *t = resume_hold_init(test);

	KUNIT_ASSERT_TRUE(test, dcp_hold_resume_event(t->pdev));
	t->dcp->external_error = -ETIMEDOUT;

	KUNIT_EXPECT_TRUE(test, resume_hold_detect(t));
	KUNIT_EXPECT_FALSE(test, t->dcp->announce.holding);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->events), 1);
}

static struct kunit_case external_resume_cases[] = {
	KUNIT_CASE(external_paused_announce_recorded_test),
	KUNIT_CASE(external_paused_unplug_ignored_test),
	KUNIT_CASE(external_paused_stale_announce_test),
	KUNIT_CASE(external_paused_released_link_test),
	KUNIT_CASE(resume_hold_armed_test),
	KUNIT_CASE(resume_hold_unreported_test),
	KUNIT_CASE(resume_hold_internal_test),
	KUNIT_CASE(resume_hold_unplug_test),
	KUNIT_CASE(resume_hold_failed_link_test),
	{}
};

static struct kunit_suite external_resume_suite = {
	.name = "apple-dcp-external-resume",
	.test_cases = external_resume_cases,
};

kunit_test_suite(external_resume_suite);

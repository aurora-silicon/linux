// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_client.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_modeset_lock.h>

#include "connector.h"

struct hotplug_fixture {
	struct drm_device *drm;
	struct apple_connector connector;
	struct apple_dcp dcp;
	struct platform_device pipeline;
	struct drm_client_dev observer;
	enum drm_connector_status observed_status;
	u64 observed_link_status;
	unsigned int notifications;
	unsigned int detects;
	unsigned int mode_probes;
	bool notification_locked;
};

static enum drm_connector_status hotplug_detect(struct drm_connector *connector, bool force)
{
	struct hotplug_fixture *fixture = container_of(to_apple_connector(connector),
							    struct hotplug_fixture, connector);

	fixture->detects++;
	return apple_connector_detect(connector, force);
}

static int hotplug_get_modes(struct drm_connector *connector)
{
	struct hotplug_fixture *fixture = container_of(to_apple_connector(connector),
							    struct hotplug_fixture, connector);

	fixture->mode_probes++;
	return 0;
}

static const struct drm_connector_funcs hotplug_connector_funcs = {
	.detect = hotplug_detect,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs hotplug_connector_helper_funcs = {
	.get_modes = hotplug_get_modes,
};

static int hotplug_observe(struct drm_client_dev *client)
{
	struct hotplug_fixture *fixture = container_of(client, struct hotplug_fixture, observer);

	fixture->notifications++;
	fixture->observed_status = fixture->connector.base.status;
	fixture->observed_link_status = fixture->connector.base.state->link_status;
	fixture->notification_locked = mutex_is_locked(&client->dev->mode_config.mutex) ||
		drm_modeset_is_locked(&client->dev->mode_config.connection_mutex);
	return 0;
}

static const struct drm_client_funcs hotplug_observer_funcs = {
	.hotplug = hotplug_observe,
};

static void hotplug_cleanup(void *data)
{
	struct hotplug_fixture *fixture = data;

	mutex_lock(&fixture->drm->clientlist_mutex);
	list_del_init(&fixture->observer.list);
	mutex_unlock(&fixture->drm->clientlist_mutex);
	cancel_work_sync(&fixture->connector.hotplug_wq);
	cancel_work_sync(&fixture->connector.bl_sync_wq);
	cancel_work_sync(&fixture->connector.bl_commit_wq);
	apple_connector_invalidate_edid(&fixture->connector);
	mutex_destroy(&fixture->connector.edid_lock);
	mutex_destroy(&fixture->connector.bl_lock);
}

static int hotplug_test_init(struct kunit *test)
{
	struct hotplug_fixture *fixture;
	struct drm_connector *connector;
	struct device *dev;
	int ret;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	test->priv = fixture;
	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	fixture->drm = __drm_kunit_helper_alloc_drm_device(test, dev,
							   sizeof(*fixture->drm), 0,
							   DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fixture->drm);
	connector = &fixture->connector.base;
	ret = drmm_connector_init(fixture->drm, connector, &hotplug_connector_funcs,
				  DRM_MODE_CONNECTOR_DisplayPort, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	drm_connector_helper_add(connector, &hotplug_connector_helper_funcs);
	connector->polled = DRM_CONNECTOR_POLL_HPD;
	drm_atomic_helper_connector_reset(connector);
	KUNIT_ASSERT_NOT_NULL(test, connector->state);
	apple_connector_edid_init(&fixture->connector);
	apple_connector_backlight_init(&fixture->connector);
	INIT_WORK(&fixture->connector.hotplug_wq, dcp_hotplug);
	fixture->dcp.dev = dev;
	fixture->dcp.connector = &fixture->connector;
	fixture->dcp.nr_modes = 1;
	spin_lock_init(&fixture->dcp.mode_state.lock);
	fixture->dcp.mode_state.valid = true;
	platform_set_drvdata(&fixture->pipeline, &fixture->dcp);
	apple_connector_set_pipeline(&fixture->connector, &fixture->pipeline);
	fixture->observer.dev = fixture->drm;
	fixture->observer.name = "apple-hotplug-observer";
	fixture->observer.funcs = &hotplug_observer_funcs;
	mutex_lock(&fixture->drm->clientlist_mutex);
	list_add(&fixture->observer.list, &fixture->drm->clientlist);
	mutex_unlock(&fixture->drm->clientlist_mutex);
	return kunit_add_action_or_reset(test, hotplug_cleanup, fixture);
}

static void hotplug_expect_notification(struct kunit *test,
					enum drm_connector_status status)
{
	struct hotplug_fixture *fixture = test->priv;

	KUNIT_EXPECT_EQ(test, fixture->connector.base.status, status);
	KUNIT_EXPECT_EQ(test, fixture->observed_status, status);
	KUNIT_EXPECT_EQ(test, fixture->notifications, 1U);
	KUNIT_EXPECT_EQ(test, fixture->detects, 1U);
	KUNIT_EXPECT_EQ(test, fixture->mode_probes, 0U);
	KUNIT_EXPECT_FALSE(test, fixture->notification_locked);
}

static void hotplug_connected(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;

	fixture->connector.base.status = connector_status_disconnected;
	fixture->connector.connected = true;
	dcp_hotplug(&fixture->connector.hotplug_wq);
	hotplug_expect_notification(test, connector_status_connected);
}

static void hotplug_disconnected(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;

	fixture->connector.base.status = connector_status_connected;
	fixture->connector.connected = false;
	dcp_hotplug(&fixture->connector.hotplug_wq);
	hotplug_expect_notification(test, connector_status_disconnected);
}

static void hotplug_retrain(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;

	fixture->connector.base.status = connector_status_connected;
	fixture->connector.connected = true;
	fixture->dcp.mode_state.valid = false;
	dcp_hotplug(&fixture->connector.hotplug_wq);
	hotplug_expect_notification(test, connector_status_connected);
	KUNIT_EXPECT_EQ(test, fixture->observed_link_status, (u64)DRM_MODE_LINK_STATUS_BAD);
}

static void hotplug_unrouted(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;

	fixture->connector.base.status = connector_status_connected;
	fixture->connector.connected = false;
	apple_connector_set_pipeline(&fixture->connector, NULL);
	dcp_hotplug(&fixture->connector.hotplug_wq);
	hotplug_expect_notification(test, connector_status_disconnected);
}

static void hotplug_forced_on(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;

	fixture->connector.base.force = DRM_FORCE_ON;
	fixture->connector.base.status = connector_status_connected;
	fixture->connector.connected = false;
	dcp_hotplug(&fixture->connector.hotplug_wq);
	hotplug_expect_notification(test, connector_status_connected);
}

static void hotplug_forced_digital(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;

	fixture->connector.base.force = DRM_FORCE_ON_DIGITAL;
	fixture->connector.base.status = connector_status_connected;
	fixture->connector.connected = false;
	dcp_hotplug(&fixture->connector.hotplug_wq);
	hotplug_expect_notification(test, connector_status_connected);
}

static void hotplug_forced_off(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;

	fixture->connector.base.force = DRM_FORCE_OFF;
	fixture->connector.base.status = connector_status_disconnected;
	fixture->connector.connected = true;
	dcp_hotplug(&fixture->connector.hotplug_wq);
	hotplug_expect_notification(test, connector_status_disconnected);
}

static void hotplug_without_hpd(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;

	fixture->connector.base.polled = 0;
	fixture->connector.base.status = connector_status_disconnected;
	fixture->connector.connected = true;
	dcp_hotplug(&fixture->connector.hotplug_wq);
	KUNIT_EXPECT_EQ(test, fixture->connector.base.status, connector_status_disconnected);
	KUNIT_EXPECT_EQ(test, fixture->observed_status, connector_status_disconnected);
	KUNIT_EXPECT_EQ(test, fixture->notifications, 1U);
	KUNIT_EXPECT_EQ(test, fixture->detects, 0U);
	KUNIT_EXPECT_EQ(test, fixture->mode_probes, 0U);
	KUNIT_EXPECT_FALSE(test, fixture->notification_locked);
}

static void hotplug_route_failure_notify(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;

	fixture->connector.connected = true;
	WRITE_ONCE(fixture->dcp.mode_state.valid, false);
	dcp_route_failure_notify(&fixture->connector);
	flush_work(&fixture->connector.hotplug_wq);
	KUNIT_EXPECT_EQ(test, fixture->notifications, 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&fixture->connector.hotplug_reasons), 0);
	KUNIT_EXPECT_FALSE(test, work_pending(&fixture->connector.hotplug_wq));
}

static void hotplug_coalesced_ready(struct kunit *test)
{
	struct hotplug_fixture *fixture = test->priv;
	unsigned int order;

	fixture->connector.connected = true;
	WRITE_ONCE(fixture->dcp.mode_state.valid, false);
	for (order = 0; order < 2; order++) {
		/* Hold the work so both reasons are pending before consumption. */
		disable_work_sync(&fixture->connector.hotplug_wq);
		if (order) {
			dcp_queue_hotplug(&fixture->connector);
			dcp_route_failure_notify(&fixture->connector);
		} else {
			dcp_route_failure_notify(&fixture->connector);
			dcp_queue_hotplug(&fixture->connector);
		}
		fixture->connector.base.state->link_status = DRM_MODE_LINK_STATUS_GOOD;
		enable_work(&fixture->connector.hotplug_wq);
		schedule_work(&fixture->connector.hotplug_wq);
		flush_work(&fixture->connector.hotplug_wq);
		KUNIT_EXPECT_EQ(test, fixture->observed_link_status,
				(u64)DRM_MODE_LINK_STATUS_BAD);
		KUNIT_EXPECT_EQ(test, atomic_read(&fixture->connector.hotplug_reasons), 0);
		KUNIT_EXPECT_FALSE(test, work_pending(&fixture->connector.hotplug_wq));
	}
	KUNIT_EXPECT_EQ(test, fixture->notifications, 2U);
}

static struct kunit_case hotplug_test_cases[] = {
	KUNIT_CASE(hotplug_route_failure_notify),
	KUNIT_CASE(hotplug_coalesced_ready),
	KUNIT_CASE(hotplug_connected),
	KUNIT_CASE(hotplug_disconnected),
	KUNIT_CASE(hotplug_retrain),
	KUNIT_CASE(hotplug_unrouted),
	KUNIT_CASE(hotplug_forced_on),
	KUNIT_CASE(hotplug_forced_digital),
	KUNIT_CASE(hotplug_forced_off),
	KUNIT_CASE(hotplug_without_hpd),
	{ }
};

static struct kunit_suite hotplug_test_suite = {
	.name = "apple-display-hotplug",
	.init = hotplug_test_init,
	.test_cases = hotplug_test_cases,
};

kunit_test_suite(hotplug_test_suite);

MODULE_LICENSE("Dual MIT/GPL");

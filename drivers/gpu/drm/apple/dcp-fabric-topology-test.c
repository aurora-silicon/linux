// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by dcp-fabric.c to exercise its connector publication paths. */
#include <kunit/platform_device.h>
#include <kunit/test.h>

struct dcp_topology_test {
	struct apple_dcp_typec_port port[3];
	struct apple_dcp_typec_route route[3][2];
	struct apple_connector connector[3][2];
	struct drm_encoder encoder[3][2];
	struct apple_dcp dcp[2];
	struct apple_crtc crtc[2];
	struct platform_device *pdev[2];
	struct drm_device drm;
};

static bool topology_fixed_crtcs(void)
{
	return true;
}

static void topology_noop(struct work_struct *work)
{
}

static void topology_cleanup(void *data)
{
	struct dcp_topology_test *t = data;
	unsigned int p, i;

	for (i = 0; i < 2; i++)
		cancel_work_sync(&t->dcp[i].tb_event_work);
	for (p = 0; p < 3; p++)
		for (i = 0; i < 2; i++)
			cancel_work_sync(&t->connector[p][i].bl_sync_wq);
}

static struct dcp_topology_test *topology_init(struct kunit *test)
{
	struct dcp_topology_test *t;
	unsigned int p, i;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	kunit_activate_static_stub(test, dcp_typec_fixed_crtcs, topology_fixed_crtcs);
	for (i = 0; i < 2; i++) {
		struct apple_dcp *dcp = &t->dcp[i];

		t->pdev[i] = kunit_platform_device_alloc(test, "dcp-topology",
							 PLATFORM_DEVID_AUTO);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->pdev[i]);
		KUNIT_ASSERT_EQ(test, kunit_platform_device_add(test, t->pdev[i]), 0);
		platform_set_drvdata(t->pdev[i], dcp);
		dcp->dev = &t->pdev[i]->dev;
		dcp->crtc = &t->crtc[i];
		t->crtc[i].base.index = 1 + 2 * i;
		t->crtc[i].base.dev = &t->drm;
		dcp->fixed_connector_type = DRM_MODE_CONNECTOR_USB;
		mutex_init(&dcp->tb_event_lock);
		mutex_init(&dcp->tb_lock);
		mutex_init(&dcp->external_lock);
		mutex_init(&dcp->modes_lock);
		spin_lock_init(&dcp->metadata.lock);
		INIT_WORK(&dcp->tb_event_work, topology_noop);
	}
	for (p = 0; p < 3; p++) {
		INIT_LIST_HEAD(&t->port[p].routes);
		for (i = 0; i < 2; i++) {
			struct apple_connector *connector = &t->connector[p][i];
			struct apple_dcp_typec_route *route = &t->route[p][i];

			route->dcp = &t->dcp[i];
			route->port = &t->port[p];
			list_add_tail(&route->port_link, &t->port[p].routes);
			connector->base.dev = &t->drm;
			connector->port_encoder = &t->encoder[p][i];
			connector->candidate_crtcs = drm_crtc_mask(&t->crtc[i].base);
			t->encoder[p][i].possible_crtcs = connector->candidate_crtcs;
			apple_connector_edid_init(connector);
			INIT_WORK(&connector->bl_sync_wq, topology_noop);
		}
	}
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, topology_cleanup, t), 0);
	return t;
}

/* The DP IN ordinal must never choose the connector on fixed-CRTC ports. */
static void topology_pipeline_identity_test(struct kunit *test)
{
	struct dcp_topology_test *t = topology_init(test);
	unsigned int p, i, dpin;

	guard(mutex)(&dcp_typec_fabric_lock);
	for (p = 0; p < 3; p++) {
		for (i = 0; i < 2; i++) {
			struct apple_dcp_typec_route *route = &t->route[p][i];
			struct apple_connector *connector = &t->connector[p][i];

			dcp_typec_port_bind_connector(&t->port[p], false, connector, t->pdev[i]);
			KUNIT_EXPECT_PTR_EQ(test, connector->dcp, NULL);
			KUNIT_EXPECT_FALSE(test, dcp_typec_narrows(&t->dcp[i]));
			for (dpin = 0; dpin < DCP_FABRIC_DPIN_MAX; dpin++) {
				route->tunnel_dpin = dpin;
				KUNIT_EXPECT_PTR_EQ(test,
						    dcp_typec_route_connector(route, true),
						    connector);
				KUNIT_EXPECT_PTR_EQ(test,
						    dcp_typec_route_connector(route, false),
						    connector);
			}
		}
	}
}

/* Boot adoption and replug can put either tunnel slot on either processor. */
static void topology_adopt_release_test(struct kunit *test)
{
	struct dcp_topology_test *t = topology_init(test);
	unsigned int p, i;

	guard(mutex)(&dcp_typec_fabric_lock);
	for (p = 0; p < 3; p++) {
		for (i = 0; i < 2; i++) {
			struct apple_dcp_typec_route *route = &t->route[p][i];
			struct apple_connector *connector = &t->connector[p][i];
			u32 mask = connector->port_encoder->possible_crtcs;

			/* Opposite of the connector creation order, as after replug. */
			t->port[p].owner = &t->route[p][1];
			t->port[p].secondary_owner = &t->route[p][0];
			t->dcp[i].active_typec_route = route;
			dcp_typec_port_bind_connector(&t->port[p], false, connector, t->pdev[i]);
			KUNIT_EXPECT_PTR_EQ(test, t->dcp[i].typec_connector, connector);
			KUNIT_EXPECT_PTR_EQ(test, connector->dcp, t->pdev[i]);
			KUNIT_EXPECT_EQ(test, connector->port_encoder->possible_crtcs, mask);
			connector->connected = true;
			/* The hardware has acknowledged release before this retry. */
			route->deactivate_xbar_done = true;
			KUNIT_ASSERT_EQ(test, dcp_typec_route_deactivate(route), 0);
			KUNIT_EXPECT_FALSE(test, connector->connected);
			KUNIT_EXPECT_PTR_EQ(test, connector->dcp, NULL);
			KUNIT_EXPECT_PTR_EQ(test, t->dcp[i].typec_connector, NULL);
			KUNIT_EXPECT_EQ(test, connector->port_encoder->possible_crtcs, mask);
			KUNIT_EXPECT_PTR_EQ(test, route->connector, connector);
		}
		t->port[p].owner = NULL;
		t->port[p].secondary_owner = NULL;
	}
}

static void topology_unbind_test(struct kunit *test)
{
	struct dcp_topology_test *t = topology_init(test);
	unsigned int p, i;

	guard(mutex)(&dcp_typec_fabric_lock);
	for (p = 0; p < 3; p++) {
		for (i = 0; i < 2; i++)
			dcp_typec_port_bind_connector(&t->port[p], false,
						      &t->connector[p][i], t->pdev[i]);
		dcp_typec_port_unlink(&t->port[p], &t->drm);
		for (i = 0; i < 2; i++)
			KUNIT_EXPECT_PTR_EQ(test, t->route[p][i].connector, NULL);
	}
}

static struct kunit_case dcp_topology_cases[] = {
	KUNIT_CASE(topology_pipeline_identity_test),
	KUNIT_CASE(topology_adopt_release_test),
	KUNIT_CASE(topology_unbind_test),
	{}
};

static struct kunit_suite dcp_topology_suite = {
	.name = "apple-dcp-topology",
	.test_cases = dcp_topology_cases,
};

kunit_test_suite(dcp_topology_suite);

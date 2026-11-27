// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

/* The HDCP services' matching and the session's answers. Included from dcp-hdcp.c. */

#include <kunit/test.h>

struct glue_fixture {
	struct apple_dcp dcp;
	struct apple_dcp_afkep ep;
	struct dcp_hdcp h;
	struct apple_epic_service service;
};

static struct glue_fixture *glue_new(struct kunit *test, bool with_hdcp)
{
	struct glue_fixture *g = kunit_kzalloc(test, sizeof(*g), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, g);
	g->ep.dcp = &g->dcp;
	g->service.ep = &g->ep;
	g->h.dcp = &g->dcp;
	if (with_hdcp)
		g->dcp.hdcp = &g->h;
	return g;
}

static void glue_match_by_name(struct kunit *test)
{
	struct glue_fixture *g = glue_new(test, true);
	const struct apple_epic_service_ops *ops;

	ops = dcp_hdcp_match_service(&g->ep, "dcpdp-device-epic:0", false);
	KUNIT_ASSERT_NOT_NULL(test, ops);
	KUNIT_EXPECT_PTR_EQ(test, (void *)ops->init, (void *)dcp_hdcp_relay_init);
	ops = dcp_hdcp_match_service(&g->ep, "dcpdptx-hdcp-interface", false);
	KUNIT_ASSERT_NOT_NULL(test, ops);
	KUNIT_EXPECT_PTR_EQ(test, (void *)ops->init, (void *)dcp_hdcp_iface_init);
	ops = dcp_hdcp_match_service(&g->ep, "dcpdptx-hdcp-auth-session", false);
	KUNIT_ASSERT_NOT_NULL(test, ops);
	KUNIT_EXPECT_PTR_EQ(test, (void *)ops->call_v2, (void *)dcp_hdcp_session_call);

	/* A cut long name matches only when the announcement filled its field. */
	KUNIT_EXPECT_NOT_NULL(test, dcp_hdcp_match_service(&g->ep,
							    "dcpdptx-hdcp-auth-sess", true));
	KUNIT_EXPECT_NULL(test, dcp_hdcp_match_service(&g->ep,
							"dcpdptx-hdcp-auth-sess", false));
	/* Other names, and a longer name that only starts the same. */
	KUNIT_EXPECT_NULL(test, dcp_hdcp_match_service(&g->ep, "dcpav-device-epic", true));
	KUNIT_EXPECT_NULL(test, dcp_hdcp_match_service(&g->ep, "dcpdp-device-epicx", false));
	KUNIT_EXPECT_NULL(test, dcp_hdcp_match_service(&g->ep, NULL, false));

	/* Without HDCP on this processor nothing is taken. */
	g = glue_new(test, false);
	KUNIT_EXPECT_NULL(test, dcp_hdcp_match_service(&g->ep, "dcpdp-device-epic", false));
}

static void glue_session_without_hdcp_declines(struct kunit *test)
{
	struct glue_fixture *g = glue_new(test, false);
	struct dcp_hdcp_word in = {}, out;

	KUNIT_EXPECT_EQ(test, dcp_hdcp_session_call(&g->service, 0, DCP_HDCP_AUTH_OPEN,
						    &in, sizeof(in), &out, sizeof(out)), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(out.value), 0);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_session_call(&g->service, 0, 0x55, &in,
						    sizeof(in), &out, sizeof(out)),
			-EOPNOTSUPP);
}

/* A declined open in the slot of an earlier session leaves it not ours. */
static void glue_declined_open_forgets_slot(struct kunit *test)
{
	struct glue_fixture *g = glue_new(test, true);
	struct dcp_hdcp_word in = {}, out;

	mutex_init(&g->h.lock);
	spin_lock_init(&g->h.svc_lock);
	init_waitqueue_head(&g->h.ev_wq);
	g->h.wq = alloc_ordered_workqueue("hdcp-test", 0);
	KUNIT_ASSERT_NOT_NULL(test, g->h.wq);
	INIT_WORK(&g->h.event_work, dcp_hdcp_event_work);
	INIT_WORK(&g->h.teardown_work, dcp_hdcp_teardown_work);
	INIT_WORK(&g->h.prop_work, dcp_hdcp_prop_work);
	INIT_WORK(&g->h.caps_work, dcp_hdcp_caps_work);
	g->h.session = &g->service;

	/* Nothing asks for protection here: the open is declined. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_session_call(&g->service, 0, DCP_HDCP_AUTH_OPEN,
						    &in, sizeof(in), &out, sizeof(out)), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(out.value), 0);
	KUNIT_EXPECT_NULL(test, g->h.session);
	/* Its later events are answered and not acted on. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_session_call(&g->service, 0, DCP_HDCP_AUTH_H_PRIME,
						    &in, sizeof(in), &out, sizeof(out)), 0);
	KUNIT_EXPECT_EQ(test, g->h.ev_bits, 0);
	KUNIT_EXPECT_EQ(test, g->h.q_head, 0);
	flush_workqueue(g->h.wq);
	destroy_workqueue(g->h.wq);
}

/* Sessions announced for HDCP 1.x are recorded, never taken, and forgotten on teardown. */
static void glue_hdcp1_session_declined(struct kunit *test)
{
	struct glue_fixture *g = glue_new(test, true);
	struct apple_epic_service other = { .ep = &g->ep };
	const struct apple_epic_service_ops *ops;

	spin_lock_init(&g->h.svc_lock);
	ops = dcp_hdcp_match_service(&g->ep, "dcpdptx-hdcp-auth-session", false);
	KUNIT_ASSERT_NOT_NULL(test, ops);
	/* Every announced session keeps its own handler. */
	KUNIT_EXPECT_FALSE(test, ops->singleton);

	ops->init(&g->service, "dcpdptx-hdcp-auth-session", "TxHDCP1Session", 0);
	ops->init(&other, "dcpdptx-hdcp-auth-session", "TxHDCP2Session", 0);
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_is_hdcp1(&g->h, &g->service));
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_is_hdcp1(&g->h, &other));
	/* No class: taken for HDCP 2.2. */
	ops->init(&other, "dcpdptx-hdcp-auth-session", NULL, 0);
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_is_hdcp1(&g->h, &other));

	dcp_hdcp_session_teardown(&g->service);
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_is_hdcp1(&g->h, &g->service));
}

/* A teardown asked for before DCP opened a newer session leaves that session alone. */
static void glue_stale_teardown_skipped(struct kunit *test)
{
	struct glue_fixture *g = glue_new(test, true);

	spin_lock_init(&g->h.svc_lock);
	g->h.td_seq = 1;
	g->h.open_seq = 2;
	g->h.tx.sep_open = true;
	dcp_hdcp_teardown_work(&g->h.teardown_work);
	KUNIT_EXPECT_TRUE(test, g->h.tx.sep_open);
}

/* Unbind stops all work and unpublishes the state. */
static void glue_fini_unpublishes(struct kunit *test)
{
	struct glue_fixture *g = glue_new(test, true);

	mutex_init(&g->h.lock);
	spin_lock_init(&g->h.svc_lock);
	init_waitqueue_head(&g->h.ev_wq);
	INIT_WORK(&g->h.event_work, dcp_hdcp_event_work);
	INIT_WORK(&g->h.teardown_work, dcp_hdcp_teardown_work);
	INIT_WORK(&g->h.prop_work, dcp_hdcp_prop_work);
	INIT_WORK(&g->h.relay_work, dcp_hdcp_relay_work);
	INIT_WORK(&g->h.caps_work, dcp_hdcp_caps_work);
	dcp_hdcp_fini(&g->dcp);
	KUNIT_EXPECT_NULL(test, READ_ONCE(g->dcp.hdcp));
	KUNIT_EXPECT_TRUE(test, g->h.cancel);
	/* Work queued after unbind never runs. */
	KUNIT_EXPECT_FALSE(test, queue_work(system_wq, &g->h.teardown_work));
	KUNIT_EXPECT_NULL(test, dcp_hdcp_match_service(&g->ep, "dcpdp-device-epic", false));
}

static struct kunit_case dcp_hdcp_glue_cases[] = {
	KUNIT_CASE(glue_match_by_name),
	KUNIT_CASE(glue_session_without_hdcp_declines),
	KUNIT_CASE(glue_declined_open_forgets_slot),
	KUNIT_CASE(glue_hdcp1_session_declined),
	KUNIT_CASE(glue_stale_teardown_skipped),
	KUNIT_CASE(glue_fini_unpublishes),
	{ }
};

static struct kunit_suite dcp_hdcp_glue_suite = {
	.name = "apple-dcp-hdcp-glue",
	.test_cases = dcp_hdcp_glue_cases,
};
kunit_test_suite(dcp_hdcp_glue_suite);

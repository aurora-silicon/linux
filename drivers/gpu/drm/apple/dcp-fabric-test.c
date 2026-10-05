// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/limits.h>

#include "dcp-fabric-core.h"

struct fabric_fixture {
	struct dcp_fabric_pipeline pipeline[2];
	struct dcp_fabric_route route[3][2];
	struct dcp_fabric_port port[3];
	struct dcp_fabric_plan plan[3];
	struct dcp_fabric_policy policy;
};

static void fabric_init(struct fabric_fixture *f, bool dual)
{
	unsigned int p, r;

	memset(f, 0, sizeof(*f));
	f->policy.dual_stream = dual;
	f->pipeline[0].next = &f->pipeline[1];
	for (r = 0; r < 2; r++) {
		f->pipeline[r].bound = true;
		f->pipeline[r].crtc_index = r + 1;
		f->pipeline[r].services_ready = true;
	}
	f->pipeline[0].has_fixed = true;
	for (p = 0; p < 3; p++) {
		f->port[p].key = p + 1;
		f->port[p].routes = &f->route[p][0];
		f->port[p].plan = &f->plan[p];
		f->port[p].candidate_crtcs[0] = BIT(1) | BIT(2);
		f->port[p].candidate_crtcs[1] = BIT(2);
		if (p < 2)
			f->port[p].next = &f->port[p + 1];
		for (r = 0; r < 2; r++) {
			f->route[p][r].pipeline = &f->pipeline[r];
			if (!r)
				f->route[p][r].next = &f->route[p][r + 1];
		}
	}
}

/* Fake effects only commit ownership after the real decision succeeds. */
static int fabric_direct(struct fabric_fixture *f, unsigned int p)
{
	struct dcp_fabric_route *route;

	f->port[p].wanted = true;
	f->port[p].hpd = true;
	route = dcp_fabric_free_route(&f->port[p], &f->policy);
	if (!route)
		return -EBUSY;
	route->pipeline->owned = true;
	f->port[p].owner[0] = route;
	return 0;
}

static int fabric_tunnel(struct fabric_fixture *f, unsigned int p,
			 unsigned int dpin)
{
	struct dcp_fabric_route *route;
	int error;

	error = dcp_fabric_tunnel_slot(&f->port[p], dpin);
	if (error)
		return error > 0 ? 0 : error;
	route = dcp_fabric_tunnel_candidate(&f->port[p], &f->policy, NULL,
					    false, dpin, true, &error);
	if (!route)
		return error;
	route->pipeline->owned = true;
	route->pipeline->tunnel_held = true;
	route->tunnel = true;
	route->dpin = dpin;
	f->port[p].owner[dpin] = route;
	return 0;
}

static void fabric_release(struct fabric_fixture *f, unsigned int p,
			   unsigned int dpin)
{
	struct dcp_fabric_route *route = f->port[p].owner[dpin];

	if (!route)
		return;
	route->pipeline->owned = false;
	route->pipeline->tunnel_held = false;
	route->tunnel = false;
	f->port[p].preferred = route;
	f->port[p].owner[dpin] = NULL;
	f->port[p].wanted = false;
	f->port[p].hpd = false;
}

static void fabric_promote(struct fabric_fixture *f)
{
	unsigned int p;

	if (dcp_fabric_capacity_action(false) != DCP_FABRIC_PROMOTE)
		return;
	for (p = 0; p < 3; p++)
		if (dcp_fabric_waiting(&f->port[p]))
			fabric_direct(f, p);
}

/* Both borrowers consume the same hardirq/sample/expiry decisions. */
static void fabric_presence_scenario(struct kunit *test,
				     struct fabric_fixture *f, unsigned int id)
{
	struct dcp_fabric_presence presence = {};
	const unsigned long window = 10000;
	u64 edge, newer;
	unsigned int events = 0;

	f->pipeline[1].owned = true;
	edge = dcp_fabric_presence_edge(&presence, 100, window);
	KUNIT_EXPECT_EQ(test, presence.deadline, 10100UL);
	f->pipeline[0].presence = presence.state;
	KUNIT_EXPECT_EQ(test, fabric_tunnel(f, 0, 0), -EBUSY);
	KUNIT_EXPECT_EQ(test, fabric_direct(f, 1), -EBUSY);
	KUNIT_EXPECT_FALSE(test,
			   dcp_fabric_presence_expire(&presence, edge, false, 10100 - 1));

	if (id == 10) {
		/* A waking monitor returns before the window ends. */
		newer = dcp_fabric_presence_edge(&presence, 1100, window);
		KUNIT_ASSERT_TRUE(test,
				  dcp_fabric_presence_sample(&presence, newer, true, 1100, window));
		KUNIT_EXPECT_EQ(test, presence.state, DCP_FABRIC_PRESENT);
		KUNIT_EXPECT_EQ(test, presence.deadline, 0UL);
		KUNIT_EXPECT_FALSE(test,
				   dcp_fabric_presence_expire(&presence, edge, false, 10100));
		KUNIT_EXPECT_FALSE(test,
				   dcp_fabric_presence_expire(&presence, newer, false, 11100));
		f->pipeline[0].presence = presence.state;
		f->pipeline[0].fixed_busy = true;
		KUNIT_EXPECT_EQ(test, fabric_tunnel(f, 0, 0), -EBUSY);
		return;
	}
	if (id == 16) {
		/* Resume starts after IRQ enable: low gets the full window. */
		newer = dcp_fabric_presence_edge(&presence, 20100, window);
		KUNIT_ASSERT_TRUE(test,
				  dcp_fabric_presence_sample(&presence, newer, false,
							     20200, window));
		KUNIT_EXPECT_EQ(test, presence.deadline, 30200UL);
		KUNIT_EXPECT_FALSE(test,
				   dcp_fabric_presence_expire(&presence, edge, false, 30200));
		/* A high sample clears the edge hold and invalidates its expiry. */
		KUNIT_ASSERT_TRUE(test,
				  dcp_fabric_presence_sample(&presence, newer, true,
							     20300, window));
		KUNIT_EXPECT_EQ(test, presence.state, DCP_FABRIC_PRESENT);
		KUNIT_EXPECT_EQ(test, presence.deadline, 0UL);
		KUNIT_EXPECT_FALSE(test,
				   dcp_fabric_presence_expire(&presence, newer, false, 30200));
		/* A falling edge after the sample wins; stale high cannot undo it. */
		edge = dcp_fabric_presence_edge(&presence, 20400, window);
		KUNIT_EXPECT_FALSE(test,
				   dcp_fabric_presence_sample(&presence, newer, true,
							      20500, window));
		KUNIT_EXPECT_EQ(test, presence.state, DCP_FABRIC_SETTLING);
		KUNIT_EXPECT_EQ(test, presence.deadline, 30400UL);
		f->pipeline[0].presence = presence.state;
		KUNIT_EXPECT_EQ(test, fabric_tunnel(f, 0, 0), -EBUSY);
		KUNIT_EXPECT_EQ(test, fabric_direct(f, 1), -EBUSY);
		return;
	}

	KUNIT_ASSERT_TRUE(test,
			  dcp_fabric_presence_expire(&presence, edge, false, 10100));
	events++;
	f->pipeline[0].presence = presence.state;
	KUNIT_EXPECT_EQ(test, presence.state, DCP_FABRIC_ABSENT);
	/* The event is consumed once, even if the expiry callback is repeated. */
	if (dcp_fabric_presence_expire(&presence, edge, false, 10101))
		events++;
	KUNIT_EXPECT_EQ(test, events, 1U);
	if (id == 11) {
		KUNIT_EXPECT_EQ(test, fabric_tunnel(f, 0, 0), 0);
		KUNIT_EXPECT_PTR_EQ(test, f->port[0].owner[0], &f->route[0][0]);
	} else {
		fabric_promote(f);
		KUNIT_EXPECT_PTR_EQ(test, f->port[1].owner[0], &f->route[1][0]);
	}
}

struct fabric_scenario {
	const char *name;
	unsigned int id;
};

static const struct fabric_scenario scenarios[] = {
	{ "S1_lone_direct", 1 },
	{ "S2_direct_then_hdmi", 2 },
	{ "S3_lone_tunnel", 3 },
	{ "S4_both_typec_orders", 4 },
	{ "S5_full_hdmi_direct_tunnel", 5 },
	{ "S6_direct_release_then_tunnel", 6 },
	{ "S7_tunnel_release_promotes_direct", 7 },
	{ "S8_frozen_hdmi_waits", 8 },
	{ "S9_parked_fixed_reselect", 9 },
	{ "S10_hdmi_blink", 10 },
	{ "S11_hdmi_absence", 11 },
	{ "S12_direct_settling", 12 },
	{ "S13_boot_three_outputs", 13 },
	{ "S14_boot_two_typec", 14 },
	{ "S15_boot_hdmi_tunnel", 15 },
	{ "S16_resume_arms_settle", 16 },
	{ "S17_occupied_slot_errors", 17 },
	{ "S18_connector_mask", 18 },
	{ "S20_base_capacity", 20 },
	{ "S21_base_lone", 21 },
	{ "S30_dual_pairing_plan", 30 },
	{ "S31_dual_dpin_candidates", 31 },
	{ "S32_dual_hdmi_reclaim", 32 },
	{ "S33_dual_scores", 33 },
	{ "S34_dual_park_reselect", 34 },
	{ "S35_dual_retry_off", 35 },
	{ "S40_desktop_promotion", 40 },
	{ "S41_dedicated_hdmi", 41 },
	{ "S50a_panel_has_no_routes", 50 },
	{ "S50b_external_service_readiness", 51 },
	{ "S50c_terminal_scanout", 52 },
	{ "S50d_external_attach_dispatch", 53 },
	{ "S50e_t6030_crossbar", 54 },
	{ "S50f_unwired_m3_ports", 55 },
};

static void fabric_scenario_desc(const struct fabric_scenario *scenario,
				 char *desc)
{
	strscpy(desc, scenario->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(fabric_scenario, scenarios, fabric_scenario_desc);

static void fabric_scenario_test(struct kunit *test)
{
	const struct fabric_scenario *scenario = test->param_value;
	struct fabric_fixture f;
	const struct dcp_fabric_port *found;
	enum dcp_fabric_fixed_step steps[2];
	unsigned int count, order;

	fabric_init(&f, scenario->id >= 30 && scenario->id <= 35);
	switch (scenario->id) {
	case 1:
		f.port[0].preferred = &f.route[0][0];
		KUNIT_ASSERT_EQ(test, fabric_direct(&f, 0), 0);
		KUNIT_EXPECT_PTR_EQ(test, f.port[0].owner[0], &f.route[0][1]);
		break;
	case 2:
		KUNIT_ASSERT_EQ(test, fabric_direct(&f, 0), 0);
		f.pipeline[0].fixed_busy = true;
		KUNIT_EXPECT_PTR_EQ(test, f.port[0].owner[0], &f.route[0][1]);
		KUNIT_EXPECT_FALSE(test, dcp_fabric_available(&f.pipeline[0], &f.policy));
		break;
	case 3:
		KUNIT_ASSERT_EQ(test, fabric_tunnel(&f, 0, 0), 0);
		KUNIT_EXPECT_PTR_EQ(test, f.port[0].owner[0], &f.route[0][1]);
		break;
	case 4:
	case 14:
		for (order = 0; order < 2; order++) {
			fabric_init(&f, false);
			KUNIT_ASSERT_EQ(test,
					order ? fabric_direct(&f, 0) :
					fabric_tunnel(&f, 0, 0),
					0);
			KUNIT_ASSERT_EQ(test,
					order ? fabric_tunnel(&f, 1, 0) :
					fabric_direct(&f, 1),
					0);
			KUNIT_EXPECT_PTR_EQ(test, f.port[0].owner[0],
					    &f.route[0][1]);
			KUNIT_EXPECT_PTR_EQ(test, f.port[1].owner[0],
					    &f.route[1][0]);
		}
		break;
	case 5:
	case 6:
	case 13:
		f.pipeline[0].fixed_busy = true;
		KUNIT_ASSERT_EQ(test, fabric_direct(&f, 0), 0);
		KUNIT_EXPECT_EQ(test, fabric_tunnel(&f, 1, 0), -EBUSY);
		KUNIT_EXPECT_PTR_EQ(test, f.port[1].owner[0], NULL);
		if (scenario->id == 6) {
			fabric_release(&f, 0, 0);
			KUNIT_ASSERT_EQ(test, fabric_tunnel(&f, 1, 0), 0);
			KUNIT_EXPECT_PTR_EQ(test, f.port[1].owner[0],
					    &f.route[1][1]);
		}
		break;
	case 7:
	case 20:
	case 40:
		if (scenario->id != 7) {
			for (order = 0; order < 3; order++)
				f.port[order].routes = &f.route[order][1];
		}
		f.pipeline[0].fixed_busy = true;
		KUNIT_ASSERT_EQ(test, fabric_tunnel(&f, 0, 0), 0);
		KUNIT_EXPECT_EQ(test, fabric_direct(&f, 1), -EBUSY);
		fabric_release(&f, 0, 0);
		fabric_promote(&f);
		KUNIT_EXPECT_PTR_EQ(test, f.port[1].owner[0], &f.route[1][1]);
		break;
	case 8:
		KUNIT_ASSERT_EQ(test, fabric_direct(&f, 0), 0);
		KUNIT_ASSERT_EQ(test, fabric_direct(&f, 1), 0);
		fabric_release(&f, 0, 0);
		f.pipeline[0].fixed_busy = true;
		KUNIT_EXPECT_FALSE(test,
				   dcp_fabric_keep_order(false, true, true));
		KUNIT_EXPECT_PTR_EQ(test, f.port[1].owner[0], &f.route[1][0]);
		KUNIT_EXPECT_EQ(test,
				dcp_fabric_fixed_steps(true, true, true, steps),
				0U);
		fabric_release(&f, 1, 0);
		KUNIT_EXPECT_EQ(test,
				dcp_fabric_fixed_steps(true, false, true, steps),
				2U);
		break;
	case 9:
	case 34:
		count = dcp_fabric_fixed_steps(true, false, true, steps);
		KUNIT_ASSERT_EQ(test, count, 2U);
		/* Fake PHY/mux trace consumes the production decision in order. */
		KUNIT_EXPECT_EQ(test, steps[0], DCP_FABRIC_RESTORE_PHY);
		KUNIT_EXPECT_EQ(test, steps[1], DCP_FABRIC_SELECT_MUX);
		break;
	case 10:
	case 11:
	case 12:
	case 16:
		fabric_presence_scenario(test, &f, scenario->id);
		break;
	case 15:
		f.pipeline[0].fixed_busy = true;
		KUNIT_ASSERT_EQ(test, fabric_tunnel(&f, 0, 0), 0);
		KUNIT_EXPECT_PTR_EQ(test, f.port[0].owner[0], &f.route[0][1]);
		break;
	case 17:
		KUNIT_ASSERT_EQ(test, fabric_tunnel(&f, 0, 0), 0);
		f.route[0][1].dpin = 1;
		KUNIT_EXPECT_EQ(test, dcp_fabric_tunnel_slot(&f.port[0], 0),
				-EADDRINUSE);
		fabric_init(&f, false);
		KUNIT_ASSERT_EQ(test, fabric_direct(&f, 0), 0);
		KUNIT_EXPECT_EQ(test, dcp_fabric_tunnel_slot(&f.port[0], 1),
				-EBUSY);
		/* Preserve the original occupied-primary errno in this extraction. */
		KUNIT_EXPECT_EQ(test, dcp_fabric_tunnel_slot(&f.port[0], 0),
				-EADDRINUSE);
		break;
	case 18:
		KUNIT_EXPECT_EQ(test,
				dcp_fabric_connector_mask(false, true, true, 2,
							  BIT(1) | BIT(2)),
				(u32)BIT(2));
		KUNIT_EXPECT_EQ(test,
				dcp_fabric_connector_mask(false, true, false, 2,
							  BIT(1) | BIT(2)),
				(u32)(BIT(1) | BIT(2)));
		break;
	case 21:
	case 41:
		/* A dedicated fixed pipeline has no edge in this port topology. */
		f.port[0].routes = &f.route[0][1];
		KUNIT_ASSERT_EQ(test, fabric_direct(&f, 0), 0);
		KUNIT_EXPECT_FALSE(test, f.pipeline[0].owned);
		KUNIT_EXPECT_PTR_EQ(test, f.port[0].owner[0], &f.route[0][1]);
		break;
	case 30:
		f.port[0].wanted = true;
		f.port[0].hpd = true;
		f.port[1].wanted = true;
		f.port[1].hpd = true;
		dcp_fabric_plan(f.pipeline, f.port, NULL, 0);
		KUNIT_EXPECT_PTR_EQ(test, f.plan[0].target[0], &f.route[0][0]);
		KUNIT_EXPECT_PTR_EQ(test, f.plan[1].target[0], &f.route[1][1]);
		KUNIT_EXPECT_TRUE(test,
				  dcp_fabric_keep_order(true, true, false));
		KUNIT_EXPECT_FALSE(test,
				   dcp_fabric_keep_order(true, true, true));
		break;
	case 31:
		KUNIT_ASSERT_EQ(test, fabric_tunnel(&f, 0, 0), 0);
		KUNIT_EXPECT_PTR_EQ(test, f.port[0].owner[0], &f.route[0][0]);
		KUNIT_ASSERT_EQ(test, fabric_tunnel(&f, 0, 1), 0);
		KUNIT_EXPECT_PTR_EQ(test, f.port[0].owner[1], &f.route[0][1]);
		break;
	case 32:
		f.port[0].wanted = true;
		f.port[0].hpd = true;
		f.pipeline[0].fixed_busy = true;
		dcp_fabric_plan(f.pipeline, f.port, NULL, 0);
		KUNIT_EXPECT_PTR_EQ(test, f.plan[0].target[0], &f.route[0][1]);
		KUNIT_EXPECT_FALSE(test,
				   dcp_fabric_keep_order(true, true, true));
		break;
	case 33:
		KUNIT_EXPECT_EQ(test,
				dcp_fabric_score(&f.pipeline[0], &f.policy), 1U);
		KUNIT_EXPECT_EQ(test,
				dcp_fabric_score(&f.pipeline[1], &f.policy), 2U);
		f.port[0].preferred = &f.route[0][1];
		KUNIT_EXPECT_PTR_EQ(test,
				    dcp_fabric_free_route(&f.port[0], &f.policy),
				    &f.route[0][1]);
		f.pipeline[0].presence = DCP_FABRIC_SETTLING;
		KUNIT_EXPECT_EQ(test, fabric_tunnel(&f, 1, 0), 0);
		break;
	case 35:
		kunit_skip(test, "TB match-data retry policy is tested by W3");
		break;
	case 50:
		f.port[0].routes = NULL;
		KUNIT_EXPECT_PTR_EQ(test,
				    dcp_fabric_free_route(&f.port[0], &f.policy),
				    NULL);
		KUNIT_EXPECT_FALSE(test, f.pipeline[0].owned);
		break;
	case 51:
	case 52:
		f.port[0].routes = &f.route[0][1];
		f.pipeline[1].external = true;
		f.pipeline[1].services_ready = false;
		f.pipeline[1].terminal = scenario->id == 52;
		KUNIT_EXPECT_EQ(test, fabric_tunnel(&f, 0, 0),
				scenario->id == 52 ? -ESHUTDOWN : -EAGAIN);
		KUNIT_EXPECT_PTR_EQ(test, f.port[0].owner[0], NULL);
		f.pipeline[1].services_ready = true;
		if (scenario->id == 51)
			KUNIT_EXPECT_EQ(test, fabric_tunnel(&f, 0, 0), 0);
		break;
	case 53:
		KUNIT_EXPECT_EQ(test, dcp_fabric_attach_action(true),
				DCP_FABRIC_ATTACH_WORK);
		KUNIT_EXPECT_EQ(test, dcp_fabric_attach_action(false),
				DCP_FABRIC_ATTACH_OOB);
		break;
	case 54:
		KUNIT_EXPECT_TRUE(test, dcp_fabric_t6030_link(true, true, true));
		KUNIT_EXPECT_FALSE(test, dcp_fabric_t6030_link(true, true, false));
		KUNIT_EXPECT_FALSE(test, dcp_fabric_t6030_link(true, false, true));
		break;
	case 55:
		f.port[0].next = NULL;
		KUNIT_EXPECT_EQ(test, dcp_fabric_tunnel_request(f.port, 2, 0, &found), -ENODEV);
		KUNIT_EXPECT_PTR_EQ(test, found, NULL);
		KUNIT_EXPECT_EQ(test, dcp_fabric_tunnel_request(f.port, 3, 0, &found), -ENODEV);
		KUNIT_EXPECT_EQ(test, dcp_fabric_tunnel_request(f.port, 0, 0, &found), -EINVAL);
		KUNIT_EXPECT_EQ(test, dcp_fabric_tunnel_request(f.port, 1, 2, &found), -EINVAL);
		break;
	}
}

static void fabric_dark_tunnel_test(struct kunit *test)
{
	struct fabric_fixture f;

	fabric_init(&f, true);
	/* A direct stream precedes an immutable tunnel on its planned pipeline. */
	f.port[0].wanted = true;
	f.port[0].hpd = true;
	f.port[1].owner[0] = &f.route[1][0];
	f.route[1][0].tunnel = true;
	f.pipeline[0].tunnel_held = true;
	f.pipeline[0].owned = true;
	dcp_fabric_plan(f.pipeline, f.port, NULL, 0);
	KUNIT_EXPECT_TRUE(test, f.plan[0].dark);
	KUNIT_EXPECT_PTR_EQ(test, f.plan[0].target[0], NULL);
	KUNIT_EXPECT_PTR_EQ(test, f.plan[1].target[0], &f.route[1][0]);
	KUNIT_EXPECT_FALSE(test,
			   dcp_fabric_movable(&f.route[1][0], &f.route[1][1]));
}

static void fabric_effect_failure_test(struct kunit *test)
{
	struct fabric_fixture f;

	fabric_init(&f, true);
	f.port[0].wanted = true;
	f.port[0].hpd = true;
	f.port[1].wanted = true;
	f.port[1].hpd = true;
	dcp_fabric_plan(f.pipeline, f.port, NULL, 0);
	KUNIT_EXPECT_PTR_EQ(test, f.plan[0].target[0], &f.route[0][0]);
	/* Failed activation rolled back to a live fixed output: fresh sample. */
	f.pipeline[0].fixed_busy = true;
	dcp_fabric_plan(f.pipeline, f.port, NULL, 0);
	KUNIT_EXPECT_PTR_EQ(test, f.plan[0].target[0], &f.route[0][1]);
	KUNIT_EXPECT_PTR_EQ(test, f.plan[1].target[0], NULL);
	KUNIT_EXPECT_FALSE(test, f.pipeline[0].owned);

	fabric_init(&f, false);
	f.port[0].wanted = true;
	f.port[0].hpd = true;
	f.port[1].wanted = true;
	f.port[1].hpd = true;
	/* Port zero's failed mux acquisition committed no ownership. */
	KUNIT_EXPECT_PTR_EQ(test, dcp_fabric_free_route(&f.port[0], &f.policy),
			    &f.route[0][1]);
	KUNIT_EXPECT_EQ(test, fabric_direct(&f, 1), 0);
	KUNIT_EXPECT_PTR_EQ(test, f.port[1].owner[0], &f.route[1][1]);
}

static void fabric_unbound_and_mask_test(struct kunit *test)
{
	struct fabric_fixture f;

	fabric_init(&f, false);
	f.pipeline[0].bound = false;
	KUNIT_EXPECT_EQ(test, dcp_fabric_score(&f.pipeline[0], &f.policy),
			UINT_MAX - 1);
	KUNIT_EXPECT_TRUE(test,
			  dcp_fabric_fixed_busy(false, false, true, false));
	KUNIT_EXPECT_FALSE(test,
			   dcp_fabric_fixed_busy(false, true, true, true));
	KUNIT_EXPECT_FALSE(test,
			   dcp_fabric_fixed_busy(true, false, true, true));
	f.policy.dual_stream = true;
	KUNIT_EXPECT_TRUE(test,
			  dcp_fabric_fits(&f.pipeline[0], &f.policy, 0, true));
	KUNIT_EXPECT_FALSE(test, dcp_fabric_fits(&f.pipeline[1], &f.policy,
						 BIT(1), true));
	KUNIT_EXPECT_EQ(test,
			dcp_fabric_connector_mask(true, true, true, 2,
						  BIT(1) | BIT(2)),
			(u32)(BIT(1) | BIT(2)));
}

static void fabric_presence_wrap_test(struct kunit *test)
{
	struct dcp_fabric_presence presence = {};
	u64 generation;

	generation = dcp_fabric_presence_edge(&presence, ULONG_MAX - 50, 100);
	KUNIT_EXPECT_FALSE(test,
			   dcp_fabric_presence_expire(&presence, generation, false, ULONG_MAX - 1));
	KUNIT_EXPECT_FALSE(test,
			   dcp_fabric_presence_expire(&presence, generation, false, 48));
	KUNIT_EXPECT_TRUE(test,
			  dcp_fabric_presence_expire(&presence, generation, false, 49));
	generation = dcp_fabric_presence_edge(&presence, 100, 100);
	/* An expiry which samples high cannot advertise capacity. */
	KUNIT_EXPECT_FALSE(test,
			   dcp_fabric_presence_expire(&presence, generation, true, 200));
	KUNIT_EXPECT_EQ(test, presence.state, DCP_FABRIC_PRESENT);
	KUNIT_EXPECT_EQ(test, presence.deadline, 0UL);
}

static struct kunit_case fabric_tests[] = {
	KUNIT_CASE_PARAM(fabric_scenario_test, fabric_scenario_gen_params),
	KUNIT_CASE(fabric_dark_tunnel_test),
	KUNIT_CASE(fabric_effect_failure_test),
	KUNIT_CASE(fabric_unbound_and_mask_test),
	KUNIT_CASE(fabric_presence_wrap_test),
	{}
};

static struct kunit_suite fabric_suite = {
	.name = "apple-dcp-fabric",
	.test_cases = fabric_tests,
};

kunit_test_suite(fabric_suite);

MODULE_LICENSE("Dual MIT/GPL");

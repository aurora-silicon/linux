// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>

#include "dcp_backlight.h"

static void backlight_millinits_takeover_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	u32 nits = 123;

	KUNIT_EXPECT_EQ(test, dcp_bl_takeover_nits(0, 100000, &nits), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_bl_takeover_nits(U32_MAX, 100000, &nits), -EINVAL);
	KUNIT_EXPECT_EQ(test, nits, 123U);
	/* Reports above the panel ceiling clamp to it. */
	KUNIT_ASSERT_EQ(test, dcp_bl_takeover_nits(525, 525999, &nits), 0);
	KUNIT_EXPECT_EQ(test, nits, 525U);
	KUNIT_ASSERT_EQ(test, dcp_bl_takeover_nits(509, 509001, &nits), 0);
	KUNIT_EXPECT_EQ(test, nits, 509U);
	KUNIT_ASSERT_EQ(test, dcp_bl_takeover_nits(509, 1018999, &nits), 0);
	KUNIT_EXPECT_EQ(test, nits, 509U);
	/* More than twice the ceiling is not a panel level. */
	nits = 123;
	KUNIT_EXPECT_EQ(test, dcp_bl_takeover_nits(509, 1019000, &nits), -ERANGE);
	KUNIT_EXPECT_EQ(test, dcp_bl_takeover_nits(509, U32_MAX, &nits), -ERANGE);
	KUNIT_EXPECT_EQ(test, nits, 123U);
	KUNIT_ASSERT_EQ(test, dcp_bl_takeover_nits(509, 100999, &nits), 0);
	KUNIT_EXPECT_EQ(test, nits, 100U);
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 509, true, nits, false, 0), 0);
	KUNIT_EXPECT_EQ(test, state.actual, 100U);
	KUNIT_EXPECT_FALSE(test, state.dirty);
	/* Registration freezes the hint before exposing the controls. */
	state.controlled = true;
	KUNIT_EXPECT_FALSE(test, dcp_bl_seed(&state, 400));
	KUNIT_EXPECT_FALSE(test, dcp_bl_seed(&state, 0));
	KUNIT_EXPECT_EQ(test, state.target, 100U);
	KUNIT_ASSERT_EQ(test, dcp_bl_takeover_nits(509, 0, &nits), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 509, true, nits, false, 0), 0);
	KUNIT_EXPECT_EQ(test, state.target, 0U);
	KUNIT_EXPECT_FALSE(test, state.dirty);
	KUNIT_ASSERT_EQ(test, dcp_bl_takeover_nits(509, 509000, &nits), 0);
	KUNIT_EXPECT_EQ(test, nits, 509U);
}

static void backlight_takeover_test(struct kunit *test)
{
	struct dcp_backlight_state state = {};

	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, true, 140, true, 36), 0);
	KUNIT_EXPECT_EQ(test, state.target, 140U);
	KUNIT_EXPECT_EQ(test, state.actual, 140U);
	KUNIT_EXPECT_FALSE(test, state.dirty);
	KUNIT_EXPECT_TRUE(test, dcp_bl_seed(&state, 400));
	KUNIT_EXPECT_EQ(test, state.target, 400U);
	KUNIT_EXPECT_FALSE(test, dcp_bl_seed(&state, 601));
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 400, false, false), 0);
	KUNIT_EXPECT_FALSE(test, dcp_bl_seed(&state, 0));
	KUNIT_EXPECT_EQ(test, state.target, 400U);
	KUNIT_EXPECT_EQ(test, state.actual, 400U);
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, true, 0, true, 36), 0);
	KUNIT_EXPECT_EQ(test, state.target, 0U);
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, false, 0, true, 36), 0);
	KUNIT_EXPECT_EQ(test, state.target, 36U);
}

static void backlight_range_test(struct kunit *test)
{
	struct dcp_backlight_state state = {};

	KUNIT_EXPECT_EQ(test, dcp_bl_init(&state, 600, false, 0, false, 0), -ENODATA);
	KUNIT_EXPECT_FALSE(test, state.ready);
	KUNIT_EXPECT_EQ(test, dcp_bl_request(&state, 140, false, false), -ENODATA);
	KUNIT_EXPECT_EQ(test, dcp_bl_dpms(&state, false), -ENODATA);
	KUNIT_EXPECT_EQ(test, dcp_bl_init(&state, 0, true, 0, false, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_bl_init(&state, U32_MAX, true, 0, false, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_bl_init(&state, 600, true, 601, false, 0), -ERANGE);
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, true, 140, false, 0), 0);
	KUNIT_EXPECT_EQ(test, dcp_bl_request(&state, 601, false, false), -ERANGE);
	KUNIT_EXPECT_EQ(test, state.target, 140U);
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 600, false, false), 0);
	KUNIT_EXPECT_EQ(test, dcp_bl_effective(&state), 600U);
}

static void backlight_dpms_cycles_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	struct dcp_backlight_present present;
	int i;

	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, true, 400, false, 0), 0);
	for (i = 0; i < 20; i++) {
		KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
		KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
		KUNIT_EXPECT_EQ(test, present.nits, 0U);
		KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
		KUNIT_EXPECT_EQ(test, state.actual, 0U);
		KUNIT_EXPECT_EQ(test, state.target, 400U);
		KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, true), 0);
		KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
		KUNIT_EXPECT_EQ(test, present.nits, 400U);
		KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
		KUNIT_EXPECT_EQ(test, state.actual, 400U);
	}
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
	KUNIT_EXPECT_EQ(test, dcp_bl_effective(&state), 0U);
	KUNIT_EXPECT_EQ(test, state.target, 400U);
}

static void backlight_blanked_target_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	struct dcp_backlight_present present;

	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, true, 400, false, 0), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 140, false, false), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 0U);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_EXPECT_EQ(test, state.target, 140U);
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, true), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 140U);
	KUNIT_EXPECT_EQ(test, state.actual, 0U);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_EXPECT_EQ(test, state.actual, 140U);
}

static void backlight_suspend_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	int off_first;

	for (off_first = 0; off_first < 2; off_first++) {
		KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, true, 140, false, 0), 0);
		if (off_first)
			KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
		KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 140, true, true), 0);
		if (!off_first)
			KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
		KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 36, true, true), 0);
		KUNIT_EXPECT_EQ(test, dcp_bl_effective(&state), 0U);
		KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, true), 0);
		KUNIT_EXPECT_EQ(test, dcp_bl_effective(&state), 0U);
		KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
		KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 36, false, false), 0);
		KUNIT_EXPECT_EQ(test, dcp_bl_effective(&state), 0U);
		KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, true), 0);
		KUNIT_EXPECT_EQ(test, dcp_bl_effective(&state), 36U);
		KUNIT_EXPECT_EQ(test, state.target, 36U);
	}
	/* A core blank also keeps the panel dark independently of DRM DPMS. */
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 400, true, false), 0);
	KUNIT_EXPECT_EQ(test, dcp_bl_effective(&state), 0U);
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 400, false, false), 0);
	KUNIT_EXPECT_EQ(test, dcp_bl_effective(&state), 400U);
}

static void backlight_surface_and_busy_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	struct dcp_backlight_present first, second;

	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, true, 140, false, 0), 0);
	KUNIT_EXPECT_EQ(test, dcp_bl_prepare(&state, true, &first), -EALREADY);
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 36, false, false), 0);
	KUNIT_EXPECT_EQ(test, dcp_bl_prepare(&state, false, &first), -ENODATA);
	KUNIT_EXPECT_TRUE(test, state.dirty);
	KUNIT_EXPECT_FALSE(test, state.in_flight);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &first), 0);
	KUNIT_EXPECT_EQ(test, dcp_bl_prepare(&state, true, &second), -EBUSY);
	KUNIT_EXPECT_EQ(test, state.actual, 140U);
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 400, false, false), 0);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, first.sequence, true));
	KUNIT_EXPECT_EQ(test, state.actual, 36U);
	KUNIT_EXPECT_EQ(test, state.target, 400U);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &second), 0);
	KUNIT_EXPECT_EQ(test, second.nits, 400U);
	KUNIT_EXPECT_FALSE(test, dcp_bl_complete(&state, first.sequence, true));
	KUNIT_EXPECT_TRUE(test, state.in_flight);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, second.sequence, true));
	KUNIT_EXPECT_EQ(test, state.actual, 400U);
}

static void backlight_reject_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	struct dcp_backlight_present present;

	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, true, 140, false, 0), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 400, false, false), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, false));
	KUNIT_EXPECT_EQ(test, state.actual, 140U);
	KUNIT_EXPECT_TRUE(test, state.dirty);
	KUNIT_EXPECT_FALSE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 0U);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_EXPECT_EQ(test, state.target, 400U);
	KUNIT_EXPECT_EQ(test, state.actual, 0U);
}

static void backlight_inflight_dpms_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	struct dcp_backlight_present present;

	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 600, true, 140, false, 0), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, true), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_EXPECT_TRUE(test, state.dirty);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 0U);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, true), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 140U);
}

static void backlight_resend_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	struct dcp_backlight_present present;

	/* Nothing is re-sent before Linux controls the level. */
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 525, true, 140, false, 0), 0);
	KUNIT_EXPECT_FALSE(test, dcp_bl_resend(&state));
	KUNIT_EXPECT_FALSE(test, state.dirty);

	state.controlled = true;
	KUNIT_EXPECT_TRUE(test, dcp_bl_resend(&state));
	KUNIT_EXPECT_FALSE(test, dcp_bl_resend(&state));
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 140U);
	/* The present in flight already carries the level. */
	KUNIT_EXPECT_FALSE(test, dcp_bl_resend(&state));
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_EXPECT_FALSE(test, state.dirty);

	/* A blanked panel is re-sent dark, keeping the restore target. */
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_EXPECT_TRUE(test, dcp_bl_resend(&state));
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 0U);
	KUNIT_EXPECT_EQ(test, state.target, 140U);
}

static void backlight_fallback_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	struct dcp_backlight_present present;

	/* A registration default is reported, never presented on its own. */
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 525, false, 0, true, 262), 0);
	KUNIT_EXPECT_FALSE(test, state.level_known);
	state.controlled = true;
	KUNIT_EXPECT_FALSE(test, dcp_bl_resend(&state));
	KUNIT_EXPECT_FALSE(test, state.dirty);
	KUNIT_EXPECT_EQ(test, dcp_bl_prepare(&state, true, &present), -EALREADY);

	/* A rejected userspace level does not become known. */
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 300, false, false), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 300U);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, false));
	KUNIT_EXPECT_FALSE(test, state.level_known);

	/* Once a present carrying it completes, the level is re-sendable. */
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_EXPECT_TRUE(test, state.level_known);
	KUNIT_EXPECT_TRUE(test, dcp_bl_resend(&state));

	/* DPMS has to light the panel at some level: wake uses the default. */
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 525, false, 0, true, 262), 0);
	state.controlled = true;
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, false), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 0U);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_ASSERT_EQ(test, dcp_bl_dpms(&state, true), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 262U);
}

static void backlight_fallback_same_level_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	struct dcp_backlight_present present;

	/* A userspace write of the reported default is still presented. */
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 525, false, 0, true, 262), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 262, false, false), 0);
	KUNIT_EXPECT_TRUE(test, state.dirty);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 262U);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, true));
	KUNIT_EXPECT_TRUE(test, state.level_known);

	/* Once the level is known, repeating it presents nothing. */
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 262, false, false), 0);
	KUNIT_EXPECT_FALSE(test, state.dirty);
	KUNIT_EXPECT_EQ(test, dcp_bl_prepare(&state, true, &present), -EALREADY);

	/* A rejected present leaves it unknown, so the same write is retried. */
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 525, false, 0, true, 262), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 262, false, false), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_TRUE(test, dcp_bl_complete(&state, present.sequence, false));
	KUNIT_EXPECT_FALSE(test, state.level_known);
	state.dirty = false;
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 262, false, false), 0);
	KUNIT_EXPECT_TRUE(test, state.dirty);

	/* An inherited level is known from the start. */
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 525, true, 180, false, 0), 0);
	KUNIT_ASSERT_EQ(test, dcp_bl_request(&state, 180, false, false), 0);
	KUNIT_EXPECT_FALSE(test, state.dirty);
}

static void backlight_seed_known_test(struct kunit *test)
{
	struct dcp_backlight_state state;
	struct dcp_backlight_present present;

	/* A loader sample seeded over a registration default is known. */
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 525, false, 0, true, 262), 0);
	KUNIT_EXPECT_FALSE(test, state.level_known);
	KUNIT_EXPECT_TRUE(test, dcp_bl_seed(&state, 180));
	KUNIT_EXPECT_TRUE(test, state.level_known);
	state.controlled = true;
	KUNIT_EXPECT_TRUE(test, dcp_bl_resend(&state));
	KUNIT_ASSERT_EQ(test, dcp_bl_prepare(&state, true, &present), 0);
	KUNIT_EXPECT_EQ(test, present.nits, 180U);

	/* A refused sample leaves the default unknown. */
	KUNIT_ASSERT_EQ(test, dcp_bl_init(&state, 525, false, 0, true, 262), 0);
	KUNIT_EXPECT_FALSE(test, dcp_bl_seed(&state, 526));
	KUNIT_EXPECT_FALSE(test, state.level_known);
	state.controlled = true;
	KUNIT_EXPECT_FALSE(test, dcp_bl_seed(&state, 180));
	KUNIT_EXPECT_FALSE(test, state.level_known);
	KUNIT_EXPECT_FALSE(test, dcp_bl_resend(&state));
}

static struct kunit_case backlight_cases[] = {
	KUNIT_CASE(backlight_millinits_takeover_test),
	KUNIT_CASE(backlight_takeover_test),
	KUNIT_CASE(backlight_range_test),
	KUNIT_CASE(backlight_dpms_cycles_test),
	KUNIT_CASE(backlight_blanked_target_test),
	KUNIT_CASE(backlight_suspend_test),
	KUNIT_CASE(backlight_surface_and_busy_test),
	KUNIT_CASE(backlight_reject_test),
	KUNIT_CASE(backlight_inflight_dpms_test),
	KUNIT_CASE(backlight_resend_test),
	KUNIT_CASE(backlight_fallback_test),
	KUNIT_CASE(backlight_fallback_same_level_test),
	KUNIT_CASE(backlight_seed_known_test),
	{}
};

static struct kunit_suite backlight_suite = {
	.name = "apple-dcp-backlight",
	.test_cases = backlight_cases,
};

kunit_test_suite(backlight_suite);

MODULE_LICENSE("Dual MIT/GPL");

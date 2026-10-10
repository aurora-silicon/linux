// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "iomfb-state.h"

static void neo_dcp_mode_success_test(struct kunit *test)
{
	struct neo_dcp_mode_state state = { };
	bool connected = true;

	spin_lock_init(&state.lock);
	neo_dcp_mode_begin(&state);
	KUNIT_EXPECT_FALSE(test, state.valid);
	KUNIT_EXPECT_EQ(test, neo_dcp_mode_finish(&state, true, &connected), 0U);
	KUNIT_EXPECT_TRUE(test, state.valid);
	KUNIT_EXPECT_FALSE(test, state.changing);
}

static void neo_dcp_mode_failure_test(struct kunit *test)
{
	struct neo_dcp_mode_state state = { .valid = true };
	bool connected = true;

	spin_lock_init(&state.lock);
	neo_dcp_mode_begin(&state);
	neo_dcp_mode_finish(&state, false, &connected);
	KUNIT_EXPECT_FALSE(test, state.valid);
	KUNIT_EXPECT_FALSE(test, state.changing);
}

static void neo_dcp_mode_deferred_disconnect_test(struct kunit *test)
{
	struct neo_dcp_mode_state state = { };
	bool connected = true;

	spin_lock_init(&state.lock);
	neo_dcp_mode_begin(&state);
	KUNIT_EXPECT_EQ(test, neo_dcp_mode_hotplug(&state, false, &connected), 0U);
	KUNIT_EXPECT_TRUE(test, connected);
	KUNIT_EXPECT_EQ(test, neo_dcp_mode_finish(&state, true, &connected),
			(unsigned int)(DCP_HOTPLUG_VBLANK | DCP_HOTPLUG_NOTIFY));
	KUNIT_EXPECT_FALSE(test, connected);
	KUNIT_EXPECT_FALSE(test, state.valid);
	KUNIT_EXPECT_FALSE(test, state.hotplug_pending);
}

static void neo_dcp_mode_deferred_connect_test(struct kunit *test)
{
	struct neo_dcp_mode_state state = { };
	bool connected = false;

	spin_lock_init(&state.lock);
	neo_dcp_mode_begin(&state);
	neo_dcp_mode_hotplug(&state, true, &connected);
	KUNIT_EXPECT_EQ(test, neo_dcp_mode_finish(&state, true, &connected),
			(unsigned int)DCP_HOTPLUG_NOTIFY);
	KUNIT_EXPECT_TRUE(test, connected);
	KUNIT_EXPECT_FALSE(test, state.valid);
	/* A recovery modeset without another link change can now succeed. */
	neo_dcp_mode_begin(&state);
	neo_dcp_mode_finish(&state, true, &connected);
	KUNIT_EXPECT_TRUE(test, state.valid);
}

static void neo_dcp_mode_transient_disconnect_test(struct kunit *test)
{
	struct neo_dcp_mode_state state = { };
	bool connected = true;

	spin_lock_init(&state.lock);
	neo_dcp_mode_begin(&state);
	neo_dcp_mode_hotplug(&state, false, &connected);
	neo_dcp_mode_hotplug(&state, true, &connected);
	KUNIT_EXPECT_EQ(test, neo_dcp_mode_finish(&state, true, &connected),
			(unsigned int)DCP_HOTPLUG_NOTIFY);
	KUNIT_EXPECT_TRUE(test, connected);
	KUNIT_EXPECT_FALSE(test, state.valid);
}

static void neo_dcp_mode_invalidation_order_test(struct kunit *test)
{
	struct neo_dcp_mode_state state = { };
	bool connected = true;

	spin_lock_init(&state.lock);
	/* Invalidation just before completion must win. */
	neo_dcp_mode_begin(&state);
	neo_dcp_mode_invalidate(&state);
	neo_dcp_mode_finish(&state, true, &connected);
	KUNIT_EXPECT_FALSE(test, state.valid);
	/* So must invalidation just after completion. */
	neo_dcp_mode_begin(&state);
	neo_dcp_mode_finish(&state, true, &connected);
	neo_dcp_mode_invalidate(&state);
	KUNIT_EXPECT_FALSE(test, state.valid);
}

static void neo_dcp_mode_disconnected_test(struct kunit *test)
{
	struct neo_dcp_mode_state state = { };
	bool connected = false;

	spin_lock_init(&state.lock);
	neo_dcp_mode_begin(&state);
	neo_dcp_mode_finish(&state, true, &connected);
	KUNIT_EXPECT_FALSE(test, state.valid);
}

static struct kunit_case neo_dcp_mode_cases[] = {
	KUNIT_CASE(neo_dcp_mode_success_test),
	KUNIT_CASE(neo_dcp_mode_failure_test),
	KUNIT_CASE(neo_dcp_mode_deferred_disconnect_test),
	KUNIT_CASE(neo_dcp_mode_deferred_connect_test),
	KUNIT_CASE(neo_dcp_mode_transient_disconnect_test),
	KUNIT_CASE(neo_dcp_mode_invalidation_order_test),
	KUNIT_CASE(neo_dcp_mode_disconnected_test),
	{ }
};

static struct kunit_suite neo_dcp_mode_suite = {
	.name = "apple-iomfb-mode-state",
	.test_cases = neo_dcp_mode_cases,
};

kunit_test_suite(neo_dcp_mode_suite);
MODULE_LICENSE("Dual MIT/GPL");

// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>

#include "dcp-hdcp.h"

static const unsigned long T0 = 1000;

static struct dcp_hdcp_link bound(void)
{
	struct dcp_hdcp_link l = { .relay = true, .sep = true };

	return l;
}

static void hdcp_property_needs_all(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_attach_property(true, true, true));
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_attach_property(false, true, true));
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_attach_property(true, false, true));
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_attach_property(true, true, false));
}

static void hdcp_unbound_declines(struct kunit *test)
{
	struct dcp_hdcp_link l = { };

	KUNIT_EXPECT_EQ(test, dcp_hdcp_request(&l, true, 0, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_open(&l, 1, T0));
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_DESIRED);
	l.relay = true;
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_open(&l, 1, T0));
	/* Both sides bound: ask DCP for a session. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_bind(&l, true, true, T0), DCP_HDCP_KICK);
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 1, T0));
}

static void hdcp_undesired_declines(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();

	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_UNDESIRED);
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_open(&l, 1, T0));
	KUNIT_EXPECT_EQ(test, dcp_hdcp_request(&l, true, 0, T0), DCP_HDCP_KICK);
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 1, T0));
	/* One session at a time. */
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_open(&l, 1, T0));
}

static void hdcp_session_enables(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();

	dcp_hdcp_request(&l, true, 1, T0);
	KUNIT_ASSERT_TRUE(test, dcp_hdcp_open(&l, 1, T0));
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_DESIRED);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_done(&l, 1, 0, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_ENABLED);
	KUNIT_EXPECT_EQ(test, l.type, 1);

	/* Userspace turns it off: teardown, UNDESIRED. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_request(&l, false, 1, T0),
			DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_UNDESIRED);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_request(&l, false, 1, T0), DCP_HDCP_NONE);
}

static void hdcp_type_change_reauths(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();

	dcp_hdcp_request(&l, true, 0, T0);
	dcp_hdcp_open(&l, 1, T0);
	dcp_hdcp_auth_done(&l, 1, 0, T0);
	/* Same type again: nothing. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_request(&l, true, 0, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_ENABLED);
	/* Type 1 now: the running session goes, no backoff. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_request(&l, true, 1, T0),
			DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_DESIRED);
	KUNIT_EXPECT_EQ(test, l.failures, 0);
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 1, T0));
	KUNIT_EXPECT_EQ(test, l.type, 1);
}

static void hdcp_failure_backs_off(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();
	unsigned long t = T0;
	unsigned int i;

	dcp_hdcp_request(&l, true, 0, t);
	dcp_hdcp_open(&l, 1, t);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_done(&l, 1, -EACCES, t),
			DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_DESIRED);
	/* DCP's next open is declined until the backoff is over. */
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_open(&l, 1, t + 1));
	t += msecs_to_jiffies(DCP_HDCP_RETRY_MS);
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 1, t));

	/* Doubling, capped. */
	for (i = 2; i < 12; i++) {
		dcp_hdcp_auth_done(&l, 1, -EACCES, t);
		KUNIT_EXPECT_EQ(test, l.failures, i);
		t = l.retry_at;
		KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 1, t));
	}
	KUNIT_EXPECT_EQ(test, dcp_hdcp_backoff(20),
			msecs_to_jiffies(DCP_HDCP_RETRY_MAX_MS));
	KUNIT_EXPECT_EQ(test, dcp_hdcp_backoff(2),
			msecs_to_jiffies(2 * DCP_HDCP_RETRY_MS));
	/* A success resets it. */
	dcp_hdcp_auth_done(&l, 1, 0, t);
	KUNIT_EXPECT_EQ(test, l.failures, 0);
}

static void hdcp_close_mid_auth(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();

	/* DCP gives up on an attempt (a receiver timeout): a failure. */
	dcp_hdcp_request(&l, true, 0, T0);
	dcp_hdcp_open(&l, 1, T0);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_closed(&l, 1, T0), DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, l.failures, 1);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_DESIRED);
	/* The attempt's late result changes nothing. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_done(&l, 1, 0, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_DESIRED);

	/* Closing an enabled session (re-authentication) is no failure. */
	l = bound();
	dcp_hdcp_request(&l, true, 0, T0);
	dcp_hdcp_open(&l, 1, T0);
	dcp_hdcp_auth_done(&l, 1, 0, T0);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_closed(&l, 1, T0), DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, l.failures, 0);
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 1, T0));
	/* A close for another generation is ignored. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_closed(&l, 7, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_AUTH);
}

static void hdcp_link_change_drops_enabled(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();

	dcp_hdcp_request(&l, true, 0, T0);
	dcp_hdcp_open(&l, 1, T0);
	dcp_hdcp_auth_done(&l, 1, 0, T0);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_ENABLED);

	/* Same generation: nothing. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_link_event(&l, 1, T0), DCP_HDCP_NONE);
	/* Hotplug, DPMS, resume: back to DESIRED, no backoff. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_link_event(&l, 2, T0), DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_DESIRED);
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 2, T0));
	/* A result from generation 1 is late. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_done(&l, 1, 0, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_AUTH);
	/* A link change clears an earlier backoff. */
	dcp_hdcp_auth_done(&l, 2, -EACCES, T0);
	KUNIT_EXPECT_EQ(test, l.failures, 1);
	dcp_hdcp_link_event(&l, 3, T0);
	KUNIT_EXPECT_EQ(test, l.failures, 0);
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 3, T0));
}

static void hdcp_unbind_drops(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();

	dcp_hdcp_request(&l, true, 0, T0);
	dcp_hdcp_open(&l, 1, T0);
	dcp_hdcp_auth_done(&l, 1, 0, T0);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_bind(&l, true, false, T0),
			DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_DESIRED);
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_open(&l, 1, T0));
	KUNIT_EXPECT_EQ(test, dcp_hdcp_bind(&l, true, true, T0), DCP_HDCP_KICK);
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 1, T0));
}

static void hdcp_sep_busy_holds(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();
	unsigned long hold = msecs_to_jiffies(DCP_HDCP_RETRY_MS);

	dcp_hdcp_request(&l, true, 0, T0);
	dcp_hdcp_open(&l, 1, T0);
	/* The SEP is busy: DESIRED, no failure counted, a short hold. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_done(&l, 1, -EAGAIN, T0),
			DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, l.failures, 0);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_DESIRED);
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_open(&l, 1, T0 + hold - 1));
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_open(&l, 1, T0 + hold));
	dcp_hdcp_auth_done(&l, 1, 0, T0 + hold);
	KUNIT_EXPECT_FALSE(test, l.hold);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_ENABLED);
}

static struct kunit_case dcp_hdcp_cases[] = {
	KUNIT_CASE(hdcp_property_needs_all),
	KUNIT_CASE(hdcp_unbound_declines),
	KUNIT_CASE(hdcp_undesired_declines),
	KUNIT_CASE(hdcp_session_enables),
	KUNIT_CASE(hdcp_type_change_reauths),
	KUNIT_CASE(hdcp_failure_backs_off),
	KUNIT_CASE(hdcp_close_mid_auth),
	KUNIT_CASE(hdcp_link_change_drops_enabled),
	KUNIT_CASE(hdcp_unbind_drops),
	KUNIT_CASE(hdcp_sep_busy_holds),
	{ }
};

static struct kunit_suite dcp_hdcp_suite = {
	.name = "apple-dcp-hdcp",
	.test_cases = dcp_hdcp_cases,
};
kunit_test_suite(dcp_hdcp_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple DCP HDCP policy");
MODULE_LICENSE("Dual MIT/GPL");

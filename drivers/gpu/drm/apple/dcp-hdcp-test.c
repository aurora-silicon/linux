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

/* Runs steps OPEN..TYPE on @gen; returns the action after the last one. */
static enum dcp_hdcp_action run_to_wait(struct kunit *test,
					struct dcp_hdcp_link *l, u32 gen,
					unsigned long now)
{
	enum dcp_hdcp_action a = DCP_HDCP_NONE;
	int s;

	for (s = DCP_HDCP_STEP_OPEN; s < DCP_HDCP_STEP_ENCRYPT; s++) {
		KUNIT_EXPECT_EQ(test, l->step, s);
		a = dcp_hdcp_step_done(l, gen, s, true, now);
		KUNIT_EXPECT_EQ(test, l->state, DCP_HDCP_AUTH);
		KUNIT_EXPECT_EQ(test, dcp_hdcp_property(l),
				DRM_MODE_CONTENT_PROTECTION_DESIRED);
	}
	return a;
}

static void hdcp_no_property_without_both_sides(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_attach_property(true, true, true));
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_attach_property(false, true, true));
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_attach_property(true, false, true));
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_attach_property(true, true, false));
}

static void hdcp_unbound_never_starts(struct kunit *test)
{
	struct dcp_hdcp_link l = { };

	dcp_hdcp_request(&l, true, T0);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_DESIRED);
	l.relay = true;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, T0), DCP_HDCP_NONE);
}

static void hdcp_full_session_enables(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();
	unsigned long wait = msecs_to_jiffies(HDCP_2_2_DELAY_BEFORE_ENCRYPTION_EN);

	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_UNDESIRED);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, T0), DCP_HDCP_NONE);
	dcp_hdcp_request(&l, true, T0);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, T0), DCP_HDCP_RUN);
	KUNIT_EXPECT_EQ(test, run_to_wait(test, &l, 1, T0), DCP_HDCP_WAIT);
	KUNIT_EXPECT_EQ(test, l.encrypt_at, T0 + wait);

	/* Not before the 200 ms. */
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_may_encrypt(&l, 1, T0 + wait - 1));
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_may_encrypt(&l, 2, T0 + wait));
	KUNIT_EXPECT_TRUE(test, dcp_hdcp_may_encrypt(&l, 1, T0 + wait));
	KUNIT_EXPECT_EQ(test, dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_ENCRYPT,
						 true, T0 + wait),
			DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_ENABLED);

	/* Userspace turns it off: teardown, UNDESIRED. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_request(&l, false, T0 + wait),
			DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_UNDESIRED);
}

static void hdcp_failure_backs_off(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();
	unsigned long t = T0;
	unsigned int i;

	dcp_hdcp_request(&l, true, t);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, t), DCP_HDCP_RUN);
	dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_OPEN, true, t);
	dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_AKE_INIT, true, t);
	/* The certificate does not check out. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_CERT,
						 false, t),
			DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_DESIRED);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, t + 1), DCP_HDCP_NONE);
	t += msecs_to_jiffies(DCP_HDCP_RETRY_MS);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, t), DCP_HDCP_RUN);

	/* Doubling, capped. */
	for (i = 2; i < 12; i++) {
		dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_OPEN, false, t);
		KUNIT_EXPECT_EQ(test, l.failures, i);
		t = l.retry_at;
		KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, t), DCP_HDCP_RUN);
	}
	KUNIT_EXPECT_EQ(test, dcp_hdcp_backoff(20),
			msecs_to_jiffies(DCP_HDCP_RETRY_MAX_MS));
	KUNIT_EXPECT_EQ(test, dcp_hdcp_backoff(2),
			msecs_to_jiffies(2 * DCP_HDCP_RETRY_MS));
}

static void hdcp_link_change_drops_enabled(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();
	unsigned long wait = msecs_to_jiffies(HDCP_2_2_DELAY_BEFORE_ENCRYPTION_EN);

	dcp_hdcp_request(&l, true, T0);
	dcp_hdcp_start(&l, 1, T0);
	run_to_wait(test, &l, 1, T0);
	dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_ENCRYPT, true, T0 + wait);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_ENABLED);

	/* Same generation: nothing. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_link_event(&l, 1, T0), DCP_HDCP_NONE);
	/* A new one: back to DESIRED, no backoff. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_link_event(&l, 2, T0), DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_property(&l),
			DRM_MODE_CONTENT_PROTECTION_DESIRED);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 2, T0), DCP_HDCP_RUN);
}

static void hdcp_late_reports_are_ignored(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();

	dcp_hdcp_request(&l, true, T0);
	dcp_hdcp_start(&l, 1, T0);
	dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_OPEN, true, T0);
	dcp_hdcp_link_event(&l, 2, T0);
	dcp_hdcp_start(&l, 2, T0);

	/* Generation 1's AKE_Init result, success or failure, changes nothing. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_AKE_INIT,
						 true, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_OPEN,
						 false, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, l.step, DCP_HDCP_STEP_OPEN);
	KUNIT_EXPECT_EQ(test, l.failures, 0);
	/* Nor does a report for a step other than the running one. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_step_done(&l, 2, DCP_HDCP_STEP_SKE,
						 true, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, l.step, DCP_HDCP_STEP_OPEN);
	/* Skipping the wait is impossible: ENCRYPT is not the running step. */
	KUNIT_EXPECT_FALSE(test, dcp_hdcp_may_encrypt(&l, 2, T0 + HZ));
}

static void hdcp_unbind_drops(struct kunit *test)
{
	struct dcp_hdcp_link l = bound();
	unsigned long wait = msecs_to_jiffies(HDCP_2_2_DELAY_BEFORE_ENCRYPTION_EN);

	dcp_hdcp_request(&l, true, T0);
	dcp_hdcp_start(&l, 1, T0);
	run_to_wait(test, &l, 1, T0);
	dcp_hdcp_step_done(&l, 1, DCP_HDCP_STEP_ENCRYPT, true, T0 + wait);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_bind(&l, true, false, T0),
			DCP_HDCP_TEARDOWN);
	KUNIT_EXPECT_EQ(test, l.state, DCP_HDCP_DESIRED);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_bind(&l, true, true, T0), DCP_HDCP_NONE);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_start(&l, 1, T0), DCP_HDCP_RUN);
}

static struct kunit_case dcp_hdcp_cases[] = {
	KUNIT_CASE(hdcp_no_property_without_both_sides),
	KUNIT_CASE(hdcp_unbound_never_starts),
	KUNIT_CASE(hdcp_full_session_enables),
	KUNIT_CASE(hdcp_failure_backs_off),
	KUNIT_CASE(hdcp_link_change_drops_enabled),
	KUNIT_CASE(hdcp_late_reports_are_ignored),
	KUNIT_CASE(hdcp_unbind_drops),
	{ }
};

static struct kunit_suite dcp_hdcp_suite = {
	.name = "apple-dcp-hdcp",
	.test_cases = dcp_hdcp_cases,
};
kunit_test_suite(dcp_hdcp_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple DCP HDCP policy");
MODULE_LICENSE("Dual MIT/GPL");

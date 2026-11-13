// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Behaviour of the software camera indicator state. The same file builds as
 * a KUnit suite in the kernel and as a plain program (-DDCP_IND_USERSPACE):
 *   gcc -DDCP_IND_USERSPACE -I. -o ind-test dcp-indicator-test.c && ./ind-test
 */
#ifdef DCP_IND_USERSPACE
#include "tests/kunit-shim.h"
#else
#include <kunit/test.h>
#include <linux/module.h>
#endif

#include "dcp-indicator.h"

static void indicator_starts_off(struct kunit *test)
{
	struct dcp_indicator ind = {};

	KUNIT_EXPECT_EQ(test, ind.state, DCP_IND_OFF);
}

/* Confirmed only after a present that carried the square, never before. */
static void indicator_confirmed_only_after_present(struct kunit *test)
{
	struct dcp_indicator ind = {};

	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	KUNIT_EXPECT_EQ(test, ind.state, DCP_IND_PENDING);
	/* A present without the square decides nothing. */
	dcp_ind_present_done(&ind, false);
	KUNIT_EXPECT_EQ(test, ind.state, DCP_IND_PENDING);
	dcp_ind_present_done(&ind, true);
	KUNIT_EXPECT_EQ(test, ind.state, DCP_IND_LIT);
}

/* Presents never start an indicator that nobody asked for. */
static void indicator_idle_ignores_presents(struct kunit *test)
{
	struct dcp_indicator ind = {};

	dcp_ind_present_done(&ind, true);
	KUNIT_EXPECT_EQ(test, ind.state, DCP_IND_OFF);
	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	dcp_ind_present_done(&ind, true);
	dcp_ind_end(&ind);
	dcp_ind_present_done(&ind, true);
	KUNIT_EXPECT_EQ(test, ind.state, DCP_IND_OFF);
}

/* One owner at a time; ending frees it for the next stream. */
static void indicator_single_owner(struct kunit *test)
{
	struct dcp_indicator ind = {};

	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	dcp_ind_present_done(&ind, true);
	KUNIT_EXPECT_EQ(test, dcp_ind_begin(&ind), -EBUSY);
	KUNIT_EXPECT_EQ(test, ind.state, DCP_IND_LIT);
	dcp_ind_end(&ind);
	KUNIT_EXPECT_EQ(test, dcp_ind_begin(&ind), 0);
}

/*
 * A DRM unbind drops the plane while the camera may still stream: the request
 * is kept and asked for again. A request that was ended, or never made, is not.
 */
static void indicator_resumes_only_a_kept_request(struct kunit *test)
{
	struct dcp_indicator ind = {};

	KUNIT_EXPECT_FALSE(test, dcp_ind_wants_resume(&ind));
	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	/* On the panel or pending, nothing to resume. */
	KUNIT_EXPECT_FALSE(test, dcp_ind_wants_resume(&ind));
	dcp_ind_present_done(&ind, true);
	KUNIT_EXPECT_FALSE(test, dcp_ind_wants_resume(&ind));
	dcp_ind_suspend(&ind);
	KUNIT_EXPECT_EQ(test, ind.state, DCP_IND_OFF);
	KUNIT_EXPECT_TRUE(test, dcp_ind_wants_resume(&ind));
	/* Asking again is a new request, and a stop ends it for good. */
	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	KUNIT_EXPECT_FALSE(test, dcp_ind_wants_resume(&ind));
	dcp_ind_end(&ind);
	KUNIT_EXPECT_FALSE(test, dcp_ind_wants_resume(&ind));
	dcp_ind_suspend(&ind);
	KUNIT_EXPECT_FALSE(test, dcp_ind_wants_resume(&ind));
}

/* Stands in for the DRM commit and the work queue; the test drives the retry by hand. */
struct fake_drm {
	int results[4];
	int commits, queued;
	bool last_on;
};

static int fake_commit(void *ctx, bool on)
{
	struct fake_drm *d = ctx;

	d->last_on = on;
	return d->results[d->commits++];
}

static void fake_queue(void *ctx)
{
	((struct fake_drm *)ctx)->queued++;
}

static const struct dcp_ind_ops fake_ops = { fake_commit, fake_queue };

/*
 * A flip still pending refuses the nonblocking commit with -EBUSY, on or off.
 * The request stands, so the commit is queued again until it lands, and the
 * camera never waits for it. Any other error is final and returned.
 */
static void indicator_refused_commit_is_retried(struct kunit *test)
{
	struct dcp_indicator ind = {};
	struct fake_drm d = { .results = { -EBUSY, -EBUSY, 0 } };

	/* Off: the square must still come off. */
	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	dcp_ind_end(&ind);
	KUNIT_EXPECT_EQ(test, dcp_ind_apply(&ind, false, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.queued, 1);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.queued, 2);
	KUNIT_EXPECT_FALSE(test, d.last_on);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.commits, 3);
	/* Landed: nothing left to retry. */
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.commits, 3);
	KUNIT_EXPECT_EQ(test, d.queued, 2);

	/* On: the request stands, so the square is asked for again. */
	d = (struct fake_drm){ .results = { -EBUSY, 0 } };
	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	KUNIT_EXPECT_EQ(test, dcp_ind_apply(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.queued, 1);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_TRUE(test, d.last_on);
	KUNIT_EXPECT_EQ(test, d.commits, 2);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.commits, 2);
}

static void indicator_retry_ends_with_the_request(struct kunit *test)
{
	struct dcp_indicator ind = {};
	struct fake_drm d = { .results = { -EBUSY, -EIO, 0 } };

	/* Any other error is final, and ends the retry. */
	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	KUNIT_EXPECT_EQ(test, dcp_ind_apply(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), -EIO);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.commits, 2);

	/* A request that ends (stopped, withdrawn) takes its on retry with it. */
	d = (struct fake_drm){ .results = { -EBUSY, 0 } };
	dcp_ind_end(&ind);
	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	KUNIT_EXPECT_EQ(test, dcp_ind_apply(&ind, true, &fake_ops, &d), 0);
	dcp_ind_end(&ind);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.commits, 1);

	/* A new request supersedes an off retry; losing the plane drops any retry. */
	d = (struct fake_drm){ .results = { -EBUSY, -EBUSY, 0 } };
	KUNIT_EXPECT_EQ(test, dcp_ind_apply(&ind, false, &fake_ops, &d), 0);
	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.commits, 1);
	KUNIT_EXPECT_EQ(test, dcp_ind_apply(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, false, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.commits, 2);
	dcp_ind_suspend(&ind);
	KUNIT_EXPECT_EQ(test, dcp_ind_retry(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.commits, 2);
}

/*
 * Userspace never gets the reserved plane: a clear is put back, anything else
 * is refused so a compositor sees it in its TEST_ONLY commit and falls back.
 * Arguments: reserved, kernel commit, unchanged, CRTC going off, has fb,
 * has crtc, own fb.
 */
static void indicator_plane_reservation(struct kunit *test)
{
	/* An ordinary plane is never touched. */
	KUNIT_EXPECT_EQ(test, dcp_ind_plane_policy(false, false, false, false, true, true, false),
			DCP_IND_PLANE_PASS);
	/* The kernel's own commit may do anything. */
	KUNIT_EXPECT_EQ(test, dcp_ind_plane_policy(true, true, false, false, true, true, false),
			DCP_IND_PLANE_PASS);
	/* Userspace: carried unchanged, or disabled with its CRTC, is left alone. */
	KUNIT_EXPECT_EQ(test, dcp_ind_plane_policy(true, false, true, false, true, true, false),
			DCP_IND_PLANE_PASS);
	KUNIT_EXPECT_EQ(test, dcp_ind_plane_policy(true, false, false, true, false, false, false),
			DCP_IND_PLANE_PASS);
	/* A blind clear of every plane (console restore) still succeeds, undone. */
	KUNIT_EXPECT_EQ(test, dcp_ind_plane_policy(true, false, false, false, false, false, false),
			DCP_IND_PLANE_RESTORE);
	/*
	 * The kernel's own framebuffer, saved by suspend and committed back by
	 * resume, is not userspace: it must not fail the resume. It is undone.
	 */
	KUNIT_EXPECT_EQ(test, dcp_ind_plane_policy(true, false, false, false, true, true, true),
			DCP_IND_PLANE_RESTORE);
	/* Any other userspace assignment (own fb, a move, a property) is refused. */
	KUNIT_EXPECT_EQ(test, dcp_ind_plane_policy(true, false, false, false, true, true, false),
			DCP_IND_PLANE_REJECT);
	/* An fb without a CRTC is no clear: it is refused, never put back. */
	KUNIT_EXPECT_EQ(test, dcp_ind_plane_policy(true, false, false, false, true, false, false),
			DCP_IND_PLANE_REJECT);
}

/*
 * The panel is not ready (suspended, or the CRTC not yet active): a request made
 * then is kept and asked for again when the display resumes, not dropped.
 */
static void indicator_survives_a_display_that_is_not_ready(struct kunit *test)
{
	struct dcp_indicator ind = {};
	struct fake_drm d = { .results = { -ENODEV, -ENODEV } };

	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	KUNIT_EXPECT_EQ(test, dcp_ind_apply(&ind, true, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, ind.state, DCP_IND_OFF);
	KUNIT_EXPECT_TRUE(test, ind.wanted);
	KUNIT_EXPECT_TRUE(test, dcp_ind_wants_resume(&ind));
	KUNIT_EXPECT_EQ(test, d.queued, 0);

	/* Nothing to take off a panel that is not lit. */
	dcp_ind_end(&ind);
	KUNIT_EXPECT_EQ(test, dcp_ind_apply(&ind, false, &fake_ops, &d), 0);
	KUNIT_EXPECT_EQ(test, d.queued, 0);
	KUNIT_EXPECT_FALSE(test, dcp_ind_wants_resume(&ind));
}

/*
 * Lit when the system suspends: resume puts the plane back disabled, so the
 * display resuming makes the square OFF again and the kept request asks anew.
 */
static void indicator_lit_across_suspend_asks_again(struct kunit *test)
{
	struct dcp_indicator ind = {};

	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	dcp_ind_present_done(&ind, true);
	KUNIT_ASSERT_EQ(test, ind.state, DCP_IND_LIT);
	KUNIT_EXPECT_FALSE(test, dcp_ind_wants_resume(&ind));
	dcp_ind_display_resumed(&ind);
	KUNIT_EXPECT_TRUE(test, dcp_ind_wants_resume(&ind));
	/* Stopped during the suspend: nothing to ask for. */
	KUNIT_ASSERT_EQ(test, dcp_ind_begin(&ind), 0);
	dcp_ind_end(&ind);
	dcp_ind_display_resumed(&ind);
	KUNIT_EXPECT_FALSE(test, dcp_ind_wants_resume(&ind));
}

static struct kunit_case indicator_cases[] = {
	KUNIT_CASE(indicator_starts_off),
	KUNIT_CASE(indicator_confirmed_only_after_present),
	KUNIT_CASE(indicator_idle_ignores_presents),
	KUNIT_CASE(indicator_single_owner),
	KUNIT_CASE(indicator_resumes_only_a_kept_request),
	KUNIT_CASE(indicator_refused_commit_is_retried),
	KUNIT_CASE(indicator_retry_ends_with_the_request),
	KUNIT_CASE(indicator_survives_a_display_that_is_not_ready),
	KUNIT_CASE(indicator_lit_across_suspend_asks_again),
	KUNIT_CASE(indicator_plane_reservation),
	{}
};

static struct kunit_suite indicator_suite = {
	.name = "apple-dcp-indicator",
	.test_cases = indicator_cases,
};

#ifdef DCP_IND_USERSPACE
KUNIT_SHIM_MAIN(indicator_suite)
#else
kunit_test_suite(indicator_suite);
MODULE_DESCRIPTION("Software camera indicator state tests");
MODULE_LICENSE("Dual MIT/GPL");
#endif

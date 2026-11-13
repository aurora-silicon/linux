// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Behaviour of the software camera indicator state. The same file builds as
 * a KUnit suite in the kernel and as a plain program (-DDCP_IND_USERSPACE).
 */
#ifdef DCP_IND_USERSPACE
#include "kunit-shim.h"
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

/* A userspace change to the reserved plane is put back, never honoured. */
static void indicator_plane_reservation(struct kunit *test)
{
	/* An ordinary plane is never touched. */
	KUNIT_EXPECT_FALSE(test, dcp_ind_plane_must_restore(false, false, false, false));
	/* The kernel's own commit may do anything. */
	KUNIT_EXPECT_FALSE(test, dcp_ind_plane_must_restore(true, true, false, false));
	/* Userspace: carried unchanged, or disabled with its CRTC, is left alone. */
	KUNIT_EXPECT_FALSE(test, dcp_ind_plane_must_restore(true, false, true, false));
	KUNIT_EXPECT_FALSE(test, dcp_ind_plane_must_restore(true, false, false, true));
	/* Any other userspace change (own fb, FB_ID=0, CRTC_ID=0, move) is undone. */
	KUNIT_EXPECT_TRUE(test, dcp_ind_plane_must_restore(true, false, false, false));
}

static struct kunit_case indicator_cases[] = {
	KUNIT_CASE(indicator_starts_off),
	KUNIT_CASE(indicator_confirmed_only_after_present),
	KUNIT_CASE(indicator_idle_ignores_presents),
	KUNIT_CASE(indicator_single_owner),
	KUNIT_CASE(indicator_resumes_only_a_kept_request),
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

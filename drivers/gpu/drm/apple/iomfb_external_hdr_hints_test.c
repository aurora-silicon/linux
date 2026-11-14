// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>

#include "iomfb.h"

/* SDR colour mode: parameter 3 = 1.0 and parameter 6 = 1.0, both 16.16. */
static void external_hdr_hints_sdr(struct kunit *test)
{
	struct iomfb_property hints[2];

	iomfb_external_hdr_hint_values(false, hints);
	KUNIT_EXPECT_EQ(test, hints[0].id, 3u);
	KUNIT_EXPECT_EQ(test, hints[0].value, 1u << 16);
	KUNIT_EXPECT_EQ(test, hints[1].id, 6u);
	KUNIT_EXPECT_EQ(test, hints[1].value, 1u << 16);
}

/* PQ (HDR) colour mode: parameter 3 stays 1.0, parameter 6 rises to 2.0. */
static void external_hdr_hints_pq(struct kunit *test)
{
	struct iomfb_property hints[2];

	iomfb_external_hdr_hint_values(true, hints);
	KUNIT_EXPECT_EQ(test, hints[0].id, 3u);
	KUNIT_EXPECT_EQ(test, hints[0].value, 1u << 16);
	KUNIT_EXPECT_EQ(test, hints[1].id, 6u);
	KUNIT_EXPECT_EQ(test, hints[1].value, 2u << 16);
}

static struct kunit_case external_hdr_hints_cases[] = {
	KUNIT_CASE(external_hdr_hints_sdr),
	KUNIT_CASE(external_hdr_hints_pq),
	{}
};

static struct kunit_suite external_hdr_hints_suite = {
	.name = "iomfb_external_hdr_hints",
	.test_cases = external_hdr_hints_cases,
};
kunit_test_suite(external_hdr_hints_suite);

MODULE_LICENSE("Dual MIT/GPL");

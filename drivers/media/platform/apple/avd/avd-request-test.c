// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>

#include "avd-request.h"

static void avd_early_completion_test(struct kunit *test)
{
	struct avd_request req;

	avd_request_begin(&req, 1);
	req.vp = 4;
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x104), AVD_EVENT_NONE);
	req.end_sent = true;
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x105), AVD_EVENT_NONE);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x1000), AVD_EVENT_NONE);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x104), AVD_EVENT_KICK_PP);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x104), AVD_EVENT_NONE);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x1000), AVD_EVENT_NONE);
	KUNIT_EXPECT_TRUE(test, req.pending);
	KUNIT_EXPECT_EQ(test, req.state, AVD_JOB_RUNNING);
	KUNIT_EXPECT_TRUE(test, avd_request_submitted(&req, false, true));
	KUNIT_EXPECT_FALSE(test, req.failed);
	KUNIT_EXPECT_FALSE(test, avd_request_timeout(&req, 1));
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x1000), AVD_EVENT_NONE);
}

static void avd_timeout_race_test(struct kunit *test)
{
	struct avd_request req;

	avd_request_begin(&req, 7);
	req.end_sent = true;
	KUNIT_EXPECT_FALSE(test, avd_request_submitted(&req, false, true));
	KUNIT_EXPECT_FALSE(test, avd_request_timeout(&req, 6));
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x100), AVD_EVENT_KICK_PP);
	KUNIT_EXPECT_TRUE(test, avd_request_timeout(&req, 7));
	KUNIT_EXPECT_TRUE(test, req.failed);
	KUNIT_EXPECT_TRUE(test, req.reset_needed);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x1000), AVD_EVENT_NONE);
	KUNIT_EXPECT_FALSE(test, avd_request_timeout(&req, 7));

	avd_request_begin(&req, 8);
	req.end_sent = true;
	KUNIT_EXPECT_FALSE(test, avd_request_submitted(&req, false, true));
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x100), AVD_EVENT_KICK_PP);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x1000), AVD_EVENT_FINISH);
	KUNIT_EXPECT_FALSE(test, avd_request_timeout(&req, 8));
	KUNIT_EXPECT_FALSE(test, req.failed);
}

static void avd_submission_failure_test(struct kunit *test)
{
	struct avd_request req;

	/* A validation/allocation failure must not reset another context. */
	avd_request_begin(&req, 1);
	KUNIT_EXPECT_TRUE(test, avd_request_submitted(&req, true, false));
	KUNIT_EXPECT_TRUE(test, req.failed);
	KUNIT_EXPECT_FALSE(test, req.reset_needed);

	/* A partially submitted frame must be reset before FIFO reuse. */
	avd_request_begin(&req, 2);
	KUNIT_EXPECT_TRUE(test, avd_request_submitted(&req, true, true));
	KUNIT_EXPECT_TRUE(test, req.reset_needed);

	/* A late PP word cannot turn a submission-time error into success. */
	avd_request_begin(&req, 3);
	req.end_sent = true;
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x100), AVD_EVENT_KICK_PP);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0), AVD_EVENT_NONE);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&req, 0x1000), AVD_EVENT_NONE);
	KUNIT_EXPECT_TRUE(test, avd_request_submitted(&req, false, true));
	KUNIT_EXPECT_TRUE(test, req.failed);
	KUNIT_EXPECT_TRUE(test, req.reset_needed);
}

static void avd_independent_contexts_test(struct kunit *test)
{
	struct avd_request hardware, preparing;

	avd_request_begin(&hardware, 10);
	hardware.end_sent = true;
	KUNIT_EXPECT_FALSE(test, avd_request_submitted(&hardware, false, true));
	avd_request_begin(&preparing, 11);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&hardware, 0x100), AVD_EVENT_KICK_PP);
	KUNIT_EXPECT_EQ(test, preparing.state, AVD_JOB_RUNNING);
	KUNIT_EXPECT_EQ(test, avd_request_irq(&hardware, 0x1000), AVD_EVENT_FINISH);
	KUNIT_EXPECT_FALSE(test, preparing.pending);
	KUNIT_EXPECT_FALSE(test, avd_request_timeout(&hardware, 10));
}

static struct kunit_case avd_request_cases[] = {
	KUNIT_CASE(avd_early_completion_test),
	KUNIT_CASE(avd_timeout_race_test),
	KUNIT_CASE(avd_submission_failure_test),
	KUNIT_CASE(avd_independent_contexts_test),
	{}
};

static struct kunit_suite avd_request_suite = {
	.name = "apple-avd-request",
	.test_cases = avd_request_cases,
};

kunit_test_suite(avd_request_suite);

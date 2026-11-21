// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "dcp-peer-v13_5.h"

static void t602x_no_peer_reply(struct kunit *test)
{
	struct dcp_display_hints_v13_5 request;
	u8 expected[sizeof(request) + 1], response[sizeof(expected)];
	unsigned int enabled;

	for (enabled = 0; enabled <= 1; enabled++) {
		memset(&request, 0x5a, sizeof(request));
		request.hints[0] = enabled;
		memset(response, 0xcc, sizeof(response));

		/* Firmware owns the flags, timing and padding, even on disable. */
		memcpy(expected, &request, sizeof(request));
		memset(expected + 0x60, 0, 20);
		memset(expected + 0x80, 0, 4);
		expected[sizeof(request)] = 0xcc;
		KUNIT_ASSERT_EQ(test, dcp_no_peer_hints_v13_5(&request, response,
							      sizeof(response)), 0);
		KUNIT_EXPECT_MEMEQ(test, response, expected, sizeof(expected));
	}
}

static void t602x_short_reply(struct kunit *test)
{
	struct dcp_display_hints_v13_5 request = {};
	u8 response[sizeof(request) - 1];

	memset(response, 0xcc, sizeof(response));
	KUNIT_EXPECT_EQ(test, dcp_no_peer_hints_v13_5(&request, response,
						      sizeof(response)), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(response, 0xcc, sizeof(response)), NULL);
}

static struct kunit_case dptx_hints_cases[] = {
	KUNIT_CASE(t602x_no_peer_reply),
	KUNIT_CASE(t602x_short_reply),
	{}
};

static struct kunit_suite dptx_hints_suite = {
	.name = "apple-dptx-hints",
	.test_cases = dptx_hints_cases,
};

kunit_test_suite(dptx_hints_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Tests for Apple DPTX display allocation replies");

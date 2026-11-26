// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>
#include <drm/display/drm_dp.h>

#include "dcp-hdcp-wire.h"

static void wire_read_encode(struct kunit *test)
{
	u8 body[DCP_HDCP_REG_ARGS_LEN + 522];

	memset(body, 0xff, sizeof(body));
	KUNIT_ASSERT_EQ(test, dcp_hdcp_reg_read_encode(body, sizeof(body),
						       DP_HDCP_2_2_REG_CERT_RX_OFFSET,
						       522, DCP_HDCP_REG_TIMEOUT_MS),
			(int)sizeof(body));
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body), 0x6900b);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 16), 522);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 32), 500);
	KUNIT_EXPECT_EQ(test, body[4], 0);
	KUNIT_EXPECT_EQ(test, body[sizeof(body) - 1], 0);

	/* No room, nothing, too much. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_read_encode(body, sizeof(body) - 1,
						       0x6900b, 522, 500), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_read_encode(body, sizeof(body),
						       0x69000, 0, 500), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_read_encode(body, sizeof(body),
						       0x69000, DCP_HDCP_REG_MAX + 1,
						       500), -EINVAL);
}

static void wire_read_check(struct kunit *test)
{
	u8 body[DCP_HDCP_REG_ARGS_LEN + 32];

	dcp_hdcp_reg_read_encode(body, sizeof(body), DP_HDCP_2_2_REG_HPRIME_OFFSET,
				 32, 500);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_read_check(body, sizeof(body),
						      DP_HDCP_2_2_REG_HPRIME_OFFSET,
						      32), 0);
	/* Truncated: the data is not all there. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_read_check(body, sizeof(body) - 1,
						      DP_HDCP_2_2_REG_HPRIME_OFFSET,
						      32), -EPROTO);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_read_check(body, 8,
						      DP_HDCP_2_2_REG_HPRIME_OFFSET,
						      32), -EPROTO);
	/* An answer for another register or another length. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_read_check(body, sizeof(body),
						      DP_HDCP_2_2_REG_LPRIME_OFFSET,
						      32), -EPROTO);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_read_check(body, sizeof(body),
						      DP_HDCP_2_2_REG_HPRIME_OFFSET,
						      16), -EPROTO);
	/* An oversized ask is refused before any compare. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_read_check(body, SIZE_MAX,
						      DP_HDCP_2_2_REG_HPRIME_OFFSET,
						      DCP_HDCP_REG_MAX + 1), -EPROTO);
}

static void wire_write(struct kunit *test)
{
	u8 body[DCP_HDCP_REG_ARGS_LEN + 8];
	u8 rtx[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	KUNIT_ASSERT_EQ(test, dcp_hdcp_reg_write_encode(body, sizeof(body),
							DP_HDCP_2_2_REG_RTX_OFFSET,
							rtx, sizeof(rtx)),
			(int)sizeof(body));
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 16), 0x69000);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 32), 8);
	/* A write carries the AUX timeout too, at +48. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 48), DCP_HDCP_REG_TIMEOUT_MS);
	KUNIT_EXPECT_MEMEQ(test, body + DCP_HDCP_REG_ARGS_LEN, rtx, sizeof(rtx));
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_write_check(body, sizeof(body),
						       0x69000, 8), 0);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_write_check(body, 63, 0x69000, 8),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_write_check(body, sizeof(body),
						       0x69008, 8), -EPROTO);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_reg_write_encode(body, sizeof(body) - 1,
							0x69000, rtx, 8), -EINVAL);
}

static void wire_auth_reply(struct kunit *test)
{
	struct dcp_hdcp_word in = { .value = cpu_to_le32(0xffffffff) };
	struct dcp_hdcp_word out;
	u8 small[8];

	/* The open: the AP's answer says whether it takes the session. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_reply(0, DCP_HDCP_AUTH_OPEN, &in,
						  sizeof(in), &out, sizeof(out),
						  true), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(out.value), 1);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_reply(0, DCP_HDCP_AUTH_OPEN, &in,
						  sizeof(in), &out, sizeof(out),
						  false), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(out.value), 0);
	/* Others come back as they were. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_reply(0, DCP_HDCP_AUTH_CONFIG, &in,
						  sizeof(in), &out, sizeof(out),
						  false), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(out.value), 0xffffffff);
	/* A short open answer, another group, an unknown call. */
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_reply(0, DCP_HDCP_AUTH_OPEN, &in,
						  sizeof(in), small, sizeof(small),
						  true), -EPROTO);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_reply(1, DCP_HDCP_AUTH_OPEN, &in,
						  sizeof(in), &out, sizeof(out),
						  true), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_auth_reply(0, 0x55, &in, sizeof(in),
						  &out, sizeof(out), true),
			-EOPNOTSUPP);
}

static void wire_report(struct kunit *test)
{
	struct dcp_hdcp_report r;
	u8 *b = (u8 *)&r;

	dcp_hdcp_report_encode(&r, DCP_HDCP_PROGRESS_LIST_VERIFIED, true, true,
			       false);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 64), 0x78);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 16), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 48), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 32), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 0), 0);
	dcp_hdcp_report_encode(&r, DCP_HDCP_PROGRESS_ENCRYPTED, true, false, true);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 64), 0x74);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 32), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 0), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 48), 0);
}

static struct kunit_case dcp_hdcp_wire_cases[] = {
	KUNIT_CASE(wire_read_encode),
	KUNIT_CASE(wire_read_check),
	KUNIT_CASE(wire_write),
	KUNIT_CASE(wire_auth_reply),
	KUNIT_CASE(wire_report),
	{ }
};

static struct kunit_suite dcp_hdcp_wire_suite = {
	.name = "apple-dcp-hdcp-wire",
	.test_cases = dcp_hdcp_wire_cases,
};
kunit_test_suite(dcp_hdcp_wire_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple DCP HDCP service layouts");
MODULE_LICENSE("Dual MIT/GPL");

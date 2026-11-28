// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the HDCP shim: argument checks, registration rules, the
 * flat layouts handed to the SEP driver, and that outputs are zeroed on
 * failure. Included from hdcp_shim.c. A stand-in SEP driver records each step.
 */
#include <kunit/test.h>

static struct {
	u32 channel;
	u32 step;
	u8 in[256];
	size_t in_len;
	size_t out_len;
	int ret;
	u8 fill;
	int calls;
	int closes;
} fake;

static bool fake_available(void *context)
{
	return true;
}

static int fake_step(void *context, u32 channel, u32 step, const u8 *in,
		     size_t in_len, u8 *out, size_t out_len)
{
	fake.calls++;
	fake.channel = channel;
	fake.step = step;
	fake.in_len = in_len;
	fake.out_len = out_len;
	if (in_len <= sizeof(fake.in) && in_len)
		memcpy(fake.in, in, in_len);
	if (out_len)
		memset(out, fake.fill, out_len);
	return fake.ret;
}

static void fake_close(void *context, u32 channel)
{
	fake.closes++;
	fake.channel = channel;
}

static const struct sep_hdcp_ops fake_ops = {
	.available = fake_available,
	.step = fake_step,
	.close = fake_close,
};

static int fake_context;

static int hdcp_shim_init(struct kunit *test)
{
	memset(&fake, 0, sizeof(fake));
	/* A real SEP driver owns the registration: leave it alone. */
	if (sep_hdcp_register(&fake_context, &fake_ops))
		kunit_skip(test, "a SEP driver is registered");
	return 0;
}

static void hdcp_shim_exit(struct kunit *test)
{
	sep_hdcp_unregister(&fake_context);
}

static void hdcp_shim_registration_rules(struct kunit *test)
{
	static int other_context;
	struct sep_hdcp_ops incomplete = fake_ops;

	KUNIT_EXPECT_EQ(test, sep_hdcp_register(&other_context, &fake_ops),
			-EBUSY);
	KUNIT_EXPECT_EQ(test, sep_hdcp_register(NULL, &fake_ops), -EINVAL);
	incomplete.close = NULL;
	KUNIT_EXPECT_EQ(test, sep_hdcp_register(&other_context, &incomplete),
			-EINVAL);
	/* Unregistering the wrong context must leave the active one intact. */
	sep_hdcp_unregister(&other_context);
	KUNIT_EXPECT_TRUE(test, apple_sep_hdcp_available());
}

static void hdcp_shim_unregistered_is_enodev(struct kunit *test)
{
	u8 rtx[APPLE_SEP_HDCP_RTX_LEN];

	sep_hdcp_unregister(&fake_context);
	memset(rtx, 0xee, sizeof(rtx));
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_open(3, rtx), -ENODEV);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(rtx, 0, sizeof(rtx)));
	KUNIT_EXPECT_FALSE(test, apple_sep_hdcp_available());
	apple_sep_hdcp_close(3);
	KUNIT_EXPECT_EQ(test, fake.calls + fake.closes, 0);
}

static void hdcp_shim_ake_cert_unpacks_both_answers(struct kunit *test)
{
	static u8 cert[APPLE_SEP_HDCP_CERT_LEN];
	u8 rrx[APPLE_SEP_HDCP_RRX_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	u8 ekpub[APPLE_SEP_HDCP_EKPUB_KM_LEN];
	struct apple_sep_hdcp_stored_km km;
	bool stored;

	/* The stand-in fills the whole output: flag 1 means a stored km. */
	fake.fill = 1;
	KUNIT_ASSERT_EQ(test, apple_sep_hdcp_ake_cert(7, cert, rrx, &stored,
						      &km, ekpub), 0);
	KUNIT_EXPECT_EQ(test, fake.step, (u32)SEP_HDCP_STEP_AKE_CERT);
	KUNIT_EXPECT_EQ(test, fake.channel, 7u);
	KUNIT_EXPECT_EQ(test, fake.out_len,
			(size_t)(1 + APPLE_SEP_HDCP_EKPUB_KM_LEN));
	KUNIT_EXPECT_TRUE(test, stored);
	KUNIT_EXPECT_EQ(test, km.ekh_km[0], 1);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(ekpub, 0, sizeof(ekpub)));

	fake.fill = 0;
	KUNIT_ASSERT_EQ(test, apple_sep_hdcp_ake_cert(7, cert, rrx, &stored,
						      &km, ekpub), 0);
	KUNIT_EXPECT_FALSE(test, stored);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(&km, 0, sizeof(km)));

	fake.fill = 0x77;
	fake.ret = -EACCES;
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_ake_cert(7, cert, rrx, &stored,
						      &km, ekpub), -EACCES);
	KUNIT_EXPECT_FALSE(test, stored);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(&km, 0, sizeof(km)));
	KUNIT_EXPECT_TRUE(test, !memchr_inv(ekpub, 0, sizeof(ekpub)));
}

static void hdcp_shim_receiver_list_layout_and_bounds(struct kunit *test)
{
	u8 ids[2 * APPLE_SEP_HDCP_RECEIVER_ID_LEN] = {
		0x10, 0x11, 0x12, 0x13, 0x14, 0x20, 0x21, 0x22, 0x23, 0x24
	};
	u8 rxinfo[2] = { 0x02, 0x20 }, seq[3] = { 0, 0, 1 };
	u8 vp[APPLE_SEP_HDCP_V_PRIME_LEN], v[APPLE_SEP_HDCP_V_LEN];

	memset(vp, 0xab, sizeof(vp));
	KUNIT_ASSERT_EQ(test, apple_sep_hdcp_repeater_list(1, ids, 2, rxinfo,
							   seq, vp, v), 0);
	KUNIT_EXPECT_EQ(test, fake.in_len, (size_t)(10 + 2 + 3 + 16));
	KUNIT_EXPECT_EQ(test, fake.in[5], 0x20);
	KUNIT_EXPECT_EQ(test, fake.in[10], 0x02);
	KUNIT_EXPECT_EQ(test, fake.in[14], 0x01);
	KUNIT_EXPECT_EQ(test, fake.in[15], 0xab);

	fake.calls = 0;
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_repeater_list(1, ids, 0, rxinfo,
							   seq, vp, v), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_repeater_list(1, ids, 32, rxinfo,
							   seq, vp, v), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_repeater_list(1, NULL, 1, rxinfo,
							   seq, vp, v), -EINVAL);
	KUNIT_EXPECT_EQ(test, fake.calls, 0);
}

static void hdcp_shim_stream_layout_and_type(struct kunit *test)
{
	u8 seq[3] = { 0, 0, 2 }, mp[APPLE_SEP_HDCP_M_PRIME_LEN] = { 0x5a };

	KUNIT_ASSERT_EQ(test, apple_sep_hdcp_repeater_stream(2, 0, 1, seq, mp),
			0);
	KUNIT_EXPECT_EQ(test, fake.step, (u32)SEP_HDCP_STEP_STREAM);
	KUNIT_EXPECT_EQ(test, fake.in_len, (size_t)37);
	KUNIT_EXPECT_EQ(test, fake.in[0], 0);
	KUNIT_EXPECT_EQ(test, fake.in[1], 1);
	KUNIT_EXPECT_EQ(test, fake.in[4], 2);
	KUNIT_EXPECT_EQ(test, fake.in[5], 0x5a);
	fake.calls = 0;
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_repeater_stream(2, 0, 2, seq, mp),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, fake.calls, 0);
}

static void hdcp_shim_errors_zero_outputs(struct kunit *test)
{
	u8 rn[APPLE_SEP_HDCP_RN_LEN], eks[APPLE_SEP_HDCP_EDKEY_KS_LEN];
	u8 riv[APPLE_SEP_HDCP_RIV_LEN];

	fake.fill = 0x99;
	fake.ret = -EIO;
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_lc_init(0, rn), -EIO);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(rn, 0, sizeof(rn)));
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_ske(0, eks, riv), -EIO);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(eks, 0, sizeof(eks)));
	KUNIT_EXPECT_TRUE(test, !memchr_inv(riv, 0, sizeof(riv)));
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_open(0, NULL), -EINVAL);

	fake.ret = 0;
	KUNIT_EXPECT_EQ(test, apple_sep_hdcp_set_encryption(4, false), 0);
	KUNIT_EXPECT_EQ(test, fake.step, (u32)SEP_HDCP_STEP_ENCRYPTION_OFF);
	apple_sep_hdcp_close(4);
	KUNIT_EXPECT_EQ(test, fake.closes, 1);
	KUNIT_EXPECT_TRUE(test, apple_sep_hdcp_available());
}

static struct kunit_case hdcp_shim_test_cases[] = {
	KUNIT_CASE(hdcp_shim_registration_rules),
	KUNIT_CASE(hdcp_shim_unregistered_is_enodev),
	KUNIT_CASE(hdcp_shim_ake_cert_unpacks_both_answers),
	KUNIT_CASE(hdcp_shim_receiver_list_layout_and_bounds),
	KUNIT_CASE(hdcp_shim_stream_layout_and_type),
	KUNIT_CASE(hdcp_shim_errors_zero_outputs),
	{}
};

static struct kunit_suite hdcp_shim_test_suite = {
	.name = "apple_sep_hdcp_shim",
	.init = hdcp_shim_init,
	.exit = hdcp_shim_exit,
	.test_cases = hdcp_shim_test_cases,
};

kunit_test_suite(hdcp_shim_test_suite);

// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple Stockholm NCI identity parsing and the read-only
 * Secure Element probe allowlist
 *
 * The responses are the ones the J616s and the J700 controllers returned.
 *
 * Copyright The Aurora Silicon Contributors
 */

#include <kunit/test.h>

static const u8 stockholm_test_reset_rsp[] = { 0x00, 0x10, 0x01 };

static const u8 stockholm_test_init_j616s[] = {
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x00,
	0xff, 0x02, 0x00, 0x04, 0x60, 0x01, 0x01, 0x41,
};

static const u8 stockholm_test_init_j700[] = {
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x00,
	0xff, 0x02, 0x00, 0x04, 0x60, 0x01, 0x01, 0x37,
};

/* A reader-class response: frame and ISO-DEP RF interfaces. */
static const u8 stockholm_test_init_rf[] = {
	0x00, 0x03, 0x1e, 0x03, 0x00, 0x02, 0x01, 0x02, 0x01, 0x00,
	0x02, 0xff, 0x00, 0x01, 0x04, 0x11, 0x22, 0x33, 0x44,
};

static void stockholm_test_reset(struct kunit *test)
{
	struct stockholm_ident id = {};
	int ret;

	ret = stockholm_parse_reset_rsp(stockholm_test_reset_rsp,
					sizeof(stockholm_test_reset_rsp), &id);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, id.nci_version, 0x10);
}

static void stockholm_test_reset_errors(struct kunit *test)
{
	static const u8 failed[] = { 0x03, 0x10, 0x01 };
	static const u8 nci2[] = { 0x00 };
	struct stockholm_ident id = {};

	KUNIT_EXPECT_EQ(test, stockholm_parse_reset_rsp(failed, sizeof(failed), &id),
			-EIO);
	KUNIT_EXPECT_EQ(test, stockholm_parse_reset_rsp(nci2, sizeof(nci2), &id),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, stockholm_parse_reset_rsp(nci2, 0, &id), -EIO);
}

static void stockholm_test_init_macs(struct kunit *test)
{
	struct stockholm_ident id = {};
	int ret;

	ret = stockholm_parse_init_rsp(stockholm_test_init_j616s,
				       sizeof(stockholm_test_init_j616s), &id);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, id.rf_interfaces, 0);
	KUNIT_EXPECT_EQ(test, id.manufacturer, 0x04);
	KUNIT_EXPECT_EQ(test, id.firmware, 0x41010160);

	ret = stockholm_parse_init_rsp(stockholm_test_init_j700,
				       sizeof(stockholm_test_init_j700), &id);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, id.rf_interfaces, 0);
	KUNIT_EXPECT_EQ(test, id.firmware, 0x37010160);
}

static void stockholm_test_init_rf_list(struct kunit *test)
{
	struct stockholm_ident id = {};
	int ret;

	ret = stockholm_parse_init_rsp(stockholm_test_init_rf,
				       sizeof(stockholm_test_init_rf), &id);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, id.rf_interfaces, 2);
	KUNIT_EXPECT_EQ(test, id.manufacturer, 0x04);
	KUNIT_EXPECT_EQ(test, id.firmware, 0x44332211);
}

static void stockholm_test_init_errors(struct kunit *test)
{
	u8 rsp[sizeof(stockholm_test_init_j616s)];
	struct stockholm_ident id = {};
	int len, ret;

	/* Every truncation, down to an empty payload, is rejected. */
	for (len = 0; len < (int)sizeof(stockholm_test_init_j616s); len++) {
		ret = stockholm_parse_init_rsp(stockholm_test_init_j616s, len, &id);
		KUNIT_EXPECT_EQ(test, ret, -EIO);
	}

	memcpy(rsp, stockholm_test_init_j616s, sizeof(rsp));
	rsp[0] = 0x03;
	KUNIT_EXPECT_EQ(test, stockholm_parse_init_rsp(rsp, sizeof(rsp), &id), -EIO);

	/* An RF interface count that runs past the payload. */
	memcpy(rsp, stockholm_test_init_j616s, sizeof(rsp));
	rsp[STOCKHOLM_INIT_RSP_RF_COUNT] = 0xff;
	KUNIT_EXPECT_EQ(test, stockholm_parse_init_rsp(rsp, sizeof(rsp), &id), -EIO);
}

/* Every command the probe sends passes the allowlist, unchanged. */
static void stockholm_test_se_table(struct kunit *test)
{
	const struct stockholm_se_apdu *cmd;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(stockholm_se_apdus); i++) {
		cmd = &stockholm_se_apdus[i];
		KUNIT_EXPECT_TRUE_MSG(test,
				      stockholm_se_apdu_allowed(cmd->apdu,
								cmd->len),
				      "%s", cmd->name);
		KUNIT_EXPECT_LE(test, cmd->len, sizeof(cmd->apdu));
	}
}

/*
 * GlobalPlatform authentication, content and key management, and anything
 * else that writes or could lock the card, is refused.
 */
static void stockholm_test_se_refused(struct kunit *test)
{
	static const u8 refused[][13] = {
		/* INITIALIZE UPDATE, EXTERNAL AUTHENTICATE */
		{ 0x80, 0x50, 0x00, 0x00, 0x08, 1, 2, 3, 4, 5, 6, 7, 8 },
		{ 0x84, 0x82, 0x00, 0x00, 0x08, 1, 2, 3, 4, 5, 6, 7, 8 },
		/* INSTALL, DELETE, LOAD, PUT KEY, SET STATUS, STORE DATA */
		{ 0x80, 0xe6, 0x0c, 0x00, 0x05, 1, 2, 3, 4, 5 },
		{ 0x80, 0xe4, 0x00, 0x00, 0x05, 0x4f, 3, 1, 2, 3 },
		{ 0x80, 0xe8, 0x00, 0x00, 0x05, 1, 2, 3, 4, 5 },
		{ 0x80, 0xd8, 0x00, 0x81, 0x05, 1, 2, 3, 4, 5 },
		{ 0x80, 0xf0, 0x80, 0x07, 0x05, 1, 2, 3, 4, 5 },
		{ 0x80, 0xe2, 0x80, 0x00, 0x05, 1, 2, 3, 4, 5 },
		/* PUT DATA, VERIFY PIN, CHANGE REFERENCE DATA, GET STATUS */
		{ 0x80, 0xda, 0x00, 0x66, 0x05, 1, 2, 3, 4, 5 },
		{ 0x00, 0x20, 0x00, 0x80, 0x05, 1, 2, 3, 4, 5 },
		{ 0x00, 0x24, 0x00, 0x80, 0x05, 1, 2, 3, 4, 5 },
		{ 0x80, 0xf2, 0x80, 0x00, 0x05, 0x4f, 0x00, 0x00, 0x00, 0x00 },
		/* MANAGE CHANNEL, GET CHALLENGE */
		{ 0x00, 0x70, 0x00, 0x00, 0x01, 0x00 },
		{ 0x00, 0x84, 0x00, 0x00, 0x08 },
	};
	static const u8 lens[] = { 13, 13, 10, 10, 10, 10, 10, 10, 10, 10, 10,
				   10, 6, 5 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(refused); i++)
		KUNIT_EXPECT_FALSE_MSG(test,
				       stockholm_se_apdu_allowed(refused[i],
								 lens[i]),
				       "INS %02x", refused[i][1]);
}

/* The allowed instructions only in their exact read-only form. */
static void stockholm_test_se_variants(struct kunit *test)
{
	/* SELECT with a logical channel, secure messaging, by file ID, next */
	static const u8 sel_chan[] = { 0x01, 0xa4, 0x04, 0x00, 0x00 };
	static const u8 sel_sm[] = { 0x0c, 0xa4, 0x04, 0x00, 0x00 };
	static const u8 sel_fid[] = { 0x00, 0xa4, 0x00, 0x00, 0x00 };
	static const u8 sel_next[] = { 0x00, 0xa4, 0x04, 0x02, 0x00 };
	/* SELECT whose Lc does not match the length, or names too short */
	static const u8 sel_lc[] = { 0x00, 0xa4, 0x04, 0x00, 0x07, 0xa0, 0x00,
				     0x00, 0x00, 0x03, 0x10, 0x10 };
	static const u8 sel_short[] = { 0x00, 0xa4, 0x04, 0x00, 0x02, 0xa0,
					0x00, 0x00 };
	/* GET DATA of another tag, with secure messaging, with data */
	static const u8 gd_tag[] = { 0x80, 0xca, 0x00, 0x45, 0x00 };
	static const u8 gd_sm[] = { 0x84, 0xca, 0x9f, 0x7f, 0x00 };
	static const u8 gd_data[] = { 0x80, 0xca, 0x9f, 0x7f, 0x01, 0x00, 0x00 };
	/* GET RESPONSE with parameters */
	static const u8 gr_p1[] = { 0x00, 0xc0, 0x01, 0x00, 0x10 };
	static const u8 ok_gr[] = { 0x00, 0xc0, 0x00, 0x00, 0x10 };

	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(sel_chan, 5));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(sel_sm, 5));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(sel_fid, 5));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(sel_next, 5));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(sel_lc,
							   sizeof(sel_lc)));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(sel_short,
							   sizeof(sel_short)));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(gd_tag, 5));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(gd_sm, 5));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(gd_data,
							   sizeof(gd_data)));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(gr_p1, 5));
	KUNIT_EXPECT_FALSE(test, stockholm_se_apdu_allowed(ok_gr, 4));
	KUNIT_EXPECT_TRUE(test, stockholm_se_apdu_allowed(ok_gr, 5));
}

/* NFCEE_DISCOVER_NTF protocol lists, including truncated ones. */
static void stockholm_test_se_nfcee(struct kunit *test)
{
	static const u8 apdu_hci[] = { 0xc0, 0x01, 0x02, 0x00, 0x01, 0x00 };
	static const u8 hci_only[] = { 0x01, 0x00, 0x01, 0x01, 0x00 };
	static const u8 cut[] = { 0xc0, 0x01, 0x03, 0x01 };
	/* What the J700 reported: the HCI network, disabled, HCI access only. */
	static const u8 j700[] = { 0x01, 0x01, 0x01, 0x01, 0x00 };

	const u8 apdu = NCI_NFCEE_INTERFACE_APDU;
	const u8 hci = NCI_NFCEE_INTERFACE_HCI_ACCESS;

	KUNIT_EXPECT_TRUE(test,
			  stockholm_se_nfcee_has(apdu_hci, sizeof(apdu_hci), apdu));
	KUNIT_EXPECT_FALSE(test,
			   stockholm_se_nfcee_has(hci_only, sizeof(hci_only), apdu));
	KUNIT_EXPECT_TRUE(test,
			  stockholm_se_nfcee_has(hci_only, sizeof(hci_only), hci));
	KUNIT_EXPECT_FALSE(test, stockholm_se_nfcee_has(cut, sizeof(cut), apdu));
	KUNIT_EXPECT_FALSE(test, stockholm_se_nfcee_has(cut, 2, hci));
	KUNIT_EXPECT_FALSE(test, stockholm_se_nfcee_has(j700, sizeof(j700), apdu));
	KUNIT_EXPECT_TRUE(test, stockholm_se_nfcee_has(j700, sizeof(j700), hci));
}

static struct kunit_case stockholm_test_cases[] = {
	KUNIT_CASE(stockholm_test_reset),
	KUNIT_CASE(stockholm_test_reset_errors),
	KUNIT_CASE(stockholm_test_init_macs),
	KUNIT_CASE(stockholm_test_init_rf_list),
	KUNIT_CASE(stockholm_test_init_errors),
	KUNIT_CASE(stockholm_test_se_table),
	KUNIT_CASE(stockholm_test_se_refused),
	KUNIT_CASE(stockholm_test_se_variants),
	KUNIT_CASE(stockholm_test_se_nfcee),
	{}
};

static struct kunit_suite stockholm_test_suite = {
	.name = "apple-stockholm",
	.test_cases = stockholm_test_cases,
};

kunit_test_suite(stockholm_test_suite);

// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple Stockholm NCI identity parsing
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

static struct kunit_case stockholm_test_cases[] = {
	KUNIT_CASE(stockholm_test_reset),
	KUNIT_CASE(stockholm_test_reset_errors),
	KUNIT_CASE(stockholm_test_init_macs),
	KUNIT_CASE(stockholm_test_init_rf_list),
	KUNIT_CASE(stockholm_test_init_errors),
	{}
};

static struct kunit_suite stockholm_test_suite = {
	.name = "apple-stockholm",
	.test_cases = stockholm_test_cases,
};

kunit_test_suite(stockholm_test_suite);

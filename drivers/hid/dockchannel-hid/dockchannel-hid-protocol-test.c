// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * KUnit tests for the DockChannel HID MTP power commands
 */
#include <kunit/test.h>
#include <linux/module.h>

#include "dockchannel-hid-protocol.h"

static void dchid_pm1_command_test(struct kunit *test)
{
	const u8 expected[DCHID_PM1_CMD_SIZE] = {
		DCHID_CMD_RESET_INTERFACE, DCHID_POWER_METHOD_1, 3,
		DCHID_POWER_STATE_ON,
	};
	u8 cmd[DCHID_PM1_CMD_SIZE];

	dchid_pm1_command(3, DCHID_POWER_STATE_ON, cmd);

	KUNIT_EXPECT_MEMEQ(test, cmd, expected, sizeof(expected));
}

static void dchid_pm2_will_change_test(struct kunit *test)
{
	const u8 expected[DCHID_PM2_CMD_SIZE] = {
		DCHID_CMD_RESET_INTERFACE, DCHID_POWER_METHOD_2, 0x5a,
		DCHID_POWER_STATE_ON, 0, 0, 0, 0, 0,
	};
	u8 cmd[DCHID_PM2_CMD_SIZE];

	memset(cmd, 0xff, sizeof(cmd));
	dchid_pm2_command(0x5a, DCHID_POWER_STATE_ON, false, cmd);

	KUNIT_EXPECT_MEMEQ(test, cmd, expected, sizeof(expected));
}

static void dchid_pm2_has_changed_test(struct kunit *test)
{
	const u8 expected[DCHID_PM2_CMD_SIZE] = {
		DCHID_CMD_RESET_INTERFACE, DCHID_POWER_METHOD_2, 0xff,
		DCHID_POWER_STATE_ON, 1, 0, 0, 0, 0,
	};
	u8 cmd[DCHID_PM2_CMD_SIZE];

	memset(cmd, 0xff, sizeof(cmd));
	dchid_pm2_command(0xff, DCHID_POWER_STATE_ON, true, cmd);

	KUNIT_EXPECT_MEMEQ(test, cmd, expected, sizeof(expected));
}

static void dchid_pm2_power_off_test(struct kunit *test)
{
	const u8 expected[DCHID_PM2_CMD_SIZE] = {
		DCHID_CMD_RESET_INTERFACE, DCHID_POWER_METHOD_2, 1,
		DCHID_POWER_STATE_OFF, 1, 0, 0, 0, 0,
	};
	u8 cmd[DCHID_PM2_CMD_SIZE];

	memset(cmd, 0xff, sizeof(cmd));
	dchid_pm2_command(1, DCHID_POWER_STATE_OFF, true, cmd);

	KUNIT_EXPECT_MEMEQ(test, cmd, expected, sizeof(expected));
}

static void dchid_pm1_fallback_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  dchid_pm1_needs_pm2_fallback(-EIO,
						       DCHID_ERR_BAD_ARGUMENT,
						       false, false, true));
	KUNIT_EXPECT_FALSE(test,
			   dchid_pm1_needs_pm2_fallback(-EIO,
							DCHID_ERR_BAD_ARGUMENT,
							true, false, true));
	KUNIT_EXPECT_FALSE(test,
			   dchid_pm1_needs_pm2_fallback(-EIO,
							DCHID_ERR_BAD_ARGUMENT,
							false, true, true));
	KUNIT_EXPECT_FALSE(test,
			   dchid_pm1_needs_pm2_fallback(-EIO,
							DCHID_ERR_BAD_ARGUMENT,
							false, false, false));
	KUNIT_EXPECT_FALSE(test,
			   dchid_pm1_needs_pm2_fallback(-ETIMEDOUT,
							DCHID_ERR_BAD_ARGUMENT,
							false, false, true));
	KUNIT_EXPECT_FALSE(test,
			   dchid_pm1_needs_pm2_fallback(-EIO, 0xe00002bc,
							false, false, true));
	KUNIT_EXPECT_FALSE(test,
			   dchid_pm1_needs_pm2_fallback(0,
							DCHID_ERR_BAD_ARGUMENT,
							false, false, true));
}

/* A TP26 data frame: header, one record of @id carrying the 32-bit @word. */
static size_t dchid_tp26_frame(u8 *buf, const u8 *id, u32 word, u16 len)
{
	size_t pos = DCHID_TP26_HEADER_SIZE;

	memset(buf, 0, DCHID_TP26_HEADER_SIZE);
	buf[0] = 0x5a;
	buf[1] = 0x01;
	buf[2] = 2;
	buf[pos] = 0x02;
	memcpy(buf + pos + 1, id, 16);
	buf[pos + 17] = 0x02;
	put_unaligned_le16(len, buf + pos + 20);
	put_unaligned_le32(word, buf + pos + DCHID_TP26_RECORD_HDR);
	return pos + DCHID_TP26_RECORD_HDR + 4;
}

static void dchid_tp26_clicked_test(struct kunit *test)
{
	static const u8 contacts_id[16] = {
		0x91, 0x62, 0x0e, 0x00, 0x8a, 0x5d, 0x81, 0xd5,
		0x7e, 0xde, 0x54, 0xa8, 0x63, 0x1f, 0xdf, 0x17,
	};
	u8 buf[64];
	size_t n;

	n = dchid_tp26_frame(buf, dchid_tp26_button_id, 1, 4);
	KUNIT_EXPECT_TRUE(test, dchid_tp26_clicked(buf, n));

	n = dchid_tp26_frame(buf, dchid_tp26_button_id, 0, 4);
	KUNIT_EXPECT_FALSE(test, dchid_tp26_clicked(buf, n));

	/* Another stream with the bit set is not a click */
	n = dchid_tp26_frame(buf, contacts_id, 1, 4);
	KUNIT_EXPECT_FALSE(test, dchid_tp26_clicked(buf, n));

	/* A record that runs past the frame */
	n = dchid_tp26_frame(buf, dchid_tp26_button_id, 1, 40);
	KUNIT_EXPECT_FALSE(test, dchid_tp26_clicked(buf, n));

	/* Not a data frame, or not TP26 at all */
	n = dchid_tp26_frame(buf, dchid_tp26_button_id, 1, 4);
	buf[2] = 1;
	KUNIT_EXPECT_FALSE(test, dchid_tp26_clicked(buf, n));
	buf[2] = 2;
	buf[0] = 0x75;
	KUNIT_EXPECT_FALSE(test, dchid_tp26_clicked(buf, n));

	/* Too short for the header */
	KUNIT_EXPECT_FALSE(test, dchid_tp26_clicked(buf, DCHID_TP26_HEADER_SIZE - 1));
}

static struct kunit_case dchid_protocol_test_cases[] = {
	KUNIT_CASE(dchid_pm1_command_test),
	KUNIT_CASE(dchid_pm2_will_change_test),
	KUNIT_CASE(dchid_pm2_has_changed_test),
	KUNIT_CASE(dchid_pm2_power_off_test),
	KUNIT_CASE(dchid_pm1_fallback_test),
	KUNIT_CASE(dchid_tp26_clicked_test),
	{}
};

static struct kunit_suite dchid_protocol_test_suite = {
	.name = "dockchannel-hid-protocol",
	.test_cases = dchid_protocol_test_cases,
};

kunit_test_suite(dchid_protocol_test_suite);

MODULE_DESCRIPTION("Apple DockChannel HID protocol KUnit tests");
MODULE_LICENSE("Dual MIT/GPL");

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

static struct kunit_case dchid_protocol_test_cases[] = {
	KUNIT_CASE(dchid_pm1_command_test),
	KUNIT_CASE(dchid_pm2_will_change_test),
	KUNIT_CASE(dchid_pm2_has_changed_test),
	KUNIT_CASE(dchid_pm2_power_off_test),
	{}
};

static struct kunit_suite dchid_protocol_test_suite = {
	.name = "dockchannel-hid-protocol",
	.test_cases = dchid_protocol_test_cases,
};

kunit_test_suite(dchid_protocol_test_suite);

MODULE_DESCRIPTION("Apple DockChannel HID protocol KUnit tests");
MODULE_LICENSE("Dual MIT/GPL");

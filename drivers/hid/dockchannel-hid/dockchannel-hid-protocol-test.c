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

static void dchid_ring_refusal_keeps_fifo_test(struct kunit *test)
{
	/* A node with an MMIO FIFO receive path of its own may fall back to it. */
	KUNIT_EXPECT_TRUE(test, dchid_ring_refused(-ETIMEDOUT, true));
}

static void dchid_ring_refusal_fails_start_without_fallback_test(struct kunit *test)
{
	/* The T6041 ring is its only receive path: a refusal must fail the start. */
	KUNIT_EXPECT_FALSE(test, dchid_ring_refused(-ETIMEDOUT, false));
}

static void dchid_ring_success_is_not_a_refusal_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, dchid_ring_refused(0, true));
}

static void dchid_ring_doorbell_any_iface_test(struct kunit *test)
{
	/* The firmware may address the doorbell to the interface whose frame is queued. */
	KUNIT_EXPECT_TRUE(test, dchid_notify_wants_drain(true, 0, 0));
	KUNIT_EXPECT_TRUE(test, dchid_notify_wants_drain(true, 2, 0));
	KUNIT_EXPECT_TRUE(test, dchid_notify_wants_drain(true, 7, 0));
	/* A frame with a payload is not a doorbell, and ring frames never are. */
	KUNIT_EXPECT_FALSE(test, dchid_notify_wants_drain(true, 2, 4));
	KUNIT_EXPECT_FALSE(test, dchid_notify_wants_drain(false, 0, 0));
}

static void dchid_ring_empty_doorbell_rechecked_test(struct kunit *test)
{
	/* A doorbell that finds nothing is looked at once more, without sleeping. */
	KUNIT_EXPECT_TRUE(test, dchid_ring_retry_needed(0, false, 0));
	KUNIT_EXPECT_FALSE(test, dchid_ring_retry_needed(0, false, 1));
	KUNIT_EXPECT_EQ(test, dchid_ring_recheck_delay_us(false), 0);
}

static void dchid_ring_partial_frame_rechecked_test(struct kunit *test)
{
	int tries;

	/* Frames drained but the next one is still being written: wait and look again. */
	for (tries = 0; tries < DCHID_RING_RECHECKS; tries++)
		KUNIT_EXPECT_TRUE(test, dchid_ring_retry_needed(3, true, tries));
	KUNIT_EXPECT_FALSE(test, dchid_ring_retry_needed(3, true, DCHID_RING_RECHECKS));
	KUNIT_EXPECT_EQ(test, dchid_ring_recheck_delay_us(true), DCHID_RING_RECHECK_US);
	/* A clean full drain needs no retry. */
	KUNIT_EXPECT_FALSE(test, dchid_ring_retry_needed(3, false, 0));
}

static void dchid_ring_failed_prepare_is_not_a_refusal_test(struct kunit *test)
{
	/* CMD_REGISTER_RING was never sent, so the ring is not refused: stay unregistered and retry. */
	KUNIT_EXPECT_FALSE(test, dchid_ring_stays_on_fifo(false, -ETIMEDOUT, true));
}

static void dchid_ring_refused_register_stays_on_fifo_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, dchid_ring_stays_on_fifo(true, -ETIMEDOUT, true));
	KUNIT_EXPECT_FALSE(test, dchid_ring_stays_on_fifo(true, -ETIMEDOUT, false));
	KUNIT_EXPECT_FALSE(test, dchid_ring_stays_on_fifo(true, 0, true));
}

static struct kunit_case dchid_protocol_test_cases[] = {
	KUNIT_CASE(dchid_pm1_command_test),
	KUNIT_CASE(dchid_pm2_will_change_test),
	KUNIT_CASE(dchid_pm2_has_changed_test),
	KUNIT_CASE(dchid_pm2_power_off_test),
	KUNIT_CASE(dchid_ring_refusal_keeps_fifo_test),
	KUNIT_CASE(dchid_ring_refusal_fails_start_without_fallback_test),
	KUNIT_CASE(dchid_ring_success_is_not_a_refusal_test),
	KUNIT_CASE(dchid_ring_failed_prepare_is_not_a_refusal_test),
	KUNIT_CASE(dchid_ring_refused_register_stays_on_fifo_test),
	KUNIT_CASE(dchid_ring_doorbell_any_iface_test),
	KUNIT_CASE(dchid_ring_empty_doorbell_rechecked_test),
	KUNIT_CASE(dchid_ring_partial_frame_rechecked_test),
	{}
};

static struct kunit_suite dchid_protocol_test_suite = {
	.name = "dockchannel-hid-protocol",
	.test_cases = dchid_protocol_test_cases,
};

kunit_test_suite(dchid_protocol_test_suite);

MODULE_DESCRIPTION("Apple DockChannel HID protocol KUnit tests");
MODULE_LICENSE("Dual MIT/GPL");

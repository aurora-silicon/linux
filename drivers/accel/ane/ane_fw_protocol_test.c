// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "ane_fw_protocol.h"

#define TEST_IPC_SIZE 0x10000
#define TEST_IPC_DVA 0x20000000000ULL
#define TEST_TABLE_OFF 0x1000

static void *ane_test_table(struct kunit *test)
{
	static const char * const names[] = {
		"TERMINAL", "IO", "DEBUG", "BUF_H2T", "BUF_T2H",
		"SHAREDMALLOC", "IO_T2H", "DATA_CHAIN_H2T",
	};
	void *ipc = kunit_kzalloc(test, TEST_IPC_SIZE, GFP_KERNEL);
	unsigned int i;

	if (!ipc)
		return NULL;
	for (i = 0; i < ANE_FW_CHANNELS; i++) {
		u8 *d = ipc + TEST_TABLE_OFF + i * ANE_FW_DESCRIPTOR_SIZE;

		strscpy((char *)d, names[i], 64);
		put_unaligned_le32(i == 0 ? 2 : (i == 4 || i == 5 || i == 6), d + 0x40);
		put_unaligned_le32(31 - i, d + 0x44);
		put_unaligned_le64(i + 1, d + 0x48);
		put_unaligned_le64(TEST_IPC_DVA + 0x2000 + i * 0x400, d + 0x50);
	}
	return ipc;
}

static int ane_test_channels_parse(void *ipc, struct ane_fw_channel *channels)
{
	return ane_fw_channels_parse(ipc + TEST_TABLE_OFF, TEST_IPC_DVA, TEST_IPC_SIZE,
				     TEST_IPC_DVA + TEST_TABLE_OFF, channels);
}

static void ane_dynamic_channels_test(struct kunit *test)
{
	struct ane_fw_channel channels[ANE_FW_CHANNELS];
	u8 *ipc = ane_test_table(test);
	u8 *io;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, ipc);
	KUNIT_ASSERT_EQ(test, ane_test_channels_parse(ipc, channels), 0);
	KUNIT_EXPECT_EQ(test, channels[1].bit, 30U);
	KUNIT_EXPECT_EQ(test, channels[1].slots, 2U);
	KUNIT_EXPECT_EQ(test, channels[1].offset, 0x2400ULL);
	{
		u8 saved[ANE_FW_DESCRIPTOR_SIZE];
		u8 *first = ipc + TEST_TABLE_OFF + ANE_FW_DESCRIPTOR_SIZE;
		u8 *last = ipc + TEST_TABLE_OFF + 7 * ANE_FW_DESCRIPTOR_SIZE;

		memcpy(saved, first, sizeof(saved));
		memcpy(first, last, sizeof(saved));
		memcpy(last, saved, sizeof(saved));
		KUNIT_ASSERT_EQ(test, ane_test_channels_parse(ipc, channels), 0);
		KUNIT_EXPECT_EQ(test, channels[1].offset, 0x2400ULL);
	}
	memset(ipc + 0x2000, 0xa5, 0x2000);
	ane_fw_channels_host_init(ipc, channels);
	io = ipc + channels[1].offset;
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(io), 1ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(io + 8), 0ULL);
	KUNIT_EXPECT_EQ(test, io[24], (u8)0xa5);
	for (i = 0; i < 24; i++)
		KUNIT_EXPECT_EQ(test, ipc[0x2000 + i], (u8)0xa5);
}

static void ane_channel_snapshot_test(struct kunit *test)
{
	struct ane_fw_channel channels[ANE_FW_CHANNELS];
	u8 *ipc = ane_test_table(test);
	u8 *snapshot = kunit_kmalloc(test, ANE_FW_TABLE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ipc);
	KUNIT_ASSERT_NOT_NULL(test, snapshot);
	memcpy(snapshot, ipc + TEST_TABLE_OFF, ANE_FW_TABLE_SIZE);
	put_unaligned_le64(0, ipc + TEST_TABLE_OFF + ANE_FW_DESCRIPTOR_SIZE + 0x48);
	ret = ane_fw_channels_parse(snapshot, TEST_IPC_DVA, TEST_IPC_SIZE,
				    TEST_IPC_DVA + TEST_TABLE_OFF, channels);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, channels[1].slots, 2U);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
}

static void ane_channel_rejections_test(struct kunit *test)
{
	struct ane_fw_channel channels[ANE_FW_CHANNELS];
	u8 *ipc = ane_test_table(test);
	u8 *d;

	KUNIT_ASSERT_NOT_NULL(test, ipc);
	d = ipc + TEST_TABLE_OFF + ANE_FW_DESCRIPTOR_SIZE;
	put_unaligned_le64(0, d + 0x48);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
	put_unaligned_le64(0x401, d + 0x48);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
	put_unaligned_le64(2, d + 0x48);
	put_unaligned_le64(TEST_IPC_DVA + TEST_TABLE_OFF, d + 0x50);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
	put_unaligned_le64(TEST_IPC_DVA + 0x2000, d + 0x50);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
	put_unaligned_le64(TEST_IPC_DVA + TEST_IPC_SIZE - 64, d + 0x50);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
	put_unaligned_le64(TEST_IPC_DVA + 0x2400, d + 0x50);
	put_unaligned_le32(32, d + 0x44);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
	put_unaligned_le32(30, d + 0x44);
	put_unaligned_le32(1, d + 0x40);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
	put_unaligned_le32(0, d + 0x40);
	strscpy((char *)d, "TERMINAL", 64);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
	memset(d, 0x41, 64);
	KUNIT_EXPECT_EQ(test, ane_test_channels_parse(ipc, channels), -EPROTO);
	KUNIT_EXPECT_FALSE(test, ane_fw_contains(16, 64, U64_MAX, 32));
	KUNIT_EXPECT_FALSE(test, ane_fw_contains(16, 64, 16, 65));
}

static void ane_reply_status_test(struct kunit *test)
{
	u8 command[16] = {};
	const u64 dva = 0x30000000000;

	put_unaligned_le16(3, command + 4);
	put_unaligned_le32(ANE_FW_TICK_RATE, command + 8);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva, 16, 16, dva, 16, 3,
						  command, true), -EAGAIN);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 16, 0, dva, 16, 3,
						  command, true), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_config_status(command, sizeof(command)), 0);
	put_unaligned_le16(1, command + 6);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 16, 0, dva, 16, 3,
						  command, true), -EREMOTEIO);
	put_unaligned_le16(0xffff, command + 6);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 16, 0, dva, 16, 3,
						  command, true), -EREMOTEIO);
	put_unaligned_le16(0, command + 6);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 15, 0, dva, 16, 3,
						  command, true), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 16, 1, dva, 16, 3,
						  command, true), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 16, 0, dva, 16, 4,
						  command, true), -EPROTO);
	put_unaligned_le32(1, command);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 16, 0, dva, 16, 3,
						  command, true), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 16, 0, dva, 16, 3,
						  command, false), 0);
	put_unaligned_le32(1, command + 8);
	KUNIT_EXPECT_EQ(test, ane_fw_config_status(command, sizeof(command)), -EPROTO);
}

static void ane_finished_event_test(struct kunit *test)
{
	u8 event[0x28] = {};

	put_unaligned_le32(0x300, event + 4);
	put_unaligned_le64(42, event + 8);
	put_unaligned_le32(3, event + 0x10);
	put_unaligned_le32(4, event + 0x14);
	KUNIT_EXPECT_EQ(test, ane_fw_finished_event(event, sizeof(event), 42, 3, 4), 0);
	put_unaligned_le32(1, event + 0x1c);
	KUNIT_EXPECT_EQ(test, ane_fw_finished_event(event, sizeof(event), 42, 3, 4), 1);
	KUNIT_EXPECT_EQ(test, ane_fw_finished_event(event, sizeof(event), 41, 3, 4), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_finished_event(event, sizeof(event), 42, 3, 5), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_finished_event(event, 0x20, 42, 3, 4), -EPROTO);
	put_unaligned_le32(2, event + 0x1c);
	KUNIT_EXPECT_EQ(test, ane_fw_finished_event(event, sizeof(event), 42, 3, 4), -EPROTO);
}

struct ane_test_tx {
	u64 words[16];
	unsigned int count;
};

static int ane_test_send(void *ctx, u64 word0, u64 word1)
{
	struct ane_test_tx *tx = ctx;

	if (word1 || tx->count == ARRAY_SIZE(tx->words))
		return -EOVERFLOW;
	tx->words[tx->count++] = word0;
	return 0;
}

static void ane_rtk_order_test(struct kunit *test)
{
	struct ane_fw_rtk rtk = {};
	struct ane_test_tx tx = {};

	KUNIT_ASSERT_EQ(test, ane_fw_rtk_receive(&rtk, ANE_FW_RTK_TYPE(1) |
						 (13 << 16) | 11, 0, &tx, ane_test_send), 0);
	KUNIT_EXPECT_EQ(test, tx.words[0], ANE_FW_RTK_TYPE(2) | (12 << 16) | 12);
	KUNIT_ASSERT_EQ(test, ane_fw_rtk_receive(&rtk, ANE_FW_RTK_TYPE(8) |
						 BIT(2) | BIT(8), 0, &tx, ane_test_send), 0);
	KUNIT_EXPECT_EQ(test, tx.words[1], ANE_FW_RTK_TYPE(8) | 1);
	KUNIT_ASSERT_EQ(test, ane_fw_rtk_receive(&rtk, ANE_FW_RTK_TYPE(8) |
						 BIT_ULL(32) | ANE_FW_RTK_LAST | BIT(0) | BIT(5), 0,
						 &tx, ane_test_send), 0);
	KUNIT_EXPECT_EQ(test, rtk.phase, ANE_FW_RTK_IOP_ACK);
	KUNIT_EXPECT_EQ(test, tx.count, 6U);
	KUNIT_EXPECT_EQ(test, tx.words[2], ANE_FW_RTK_TYPE(8) | BIT_ULL(32) |
		ANE_FW_RTK_LAST);
	KUNIT_EXPECT_EQ(test, tx.words[3], ANE_FW_RTK_TYPE(5) | (2ULL << 32) | 2);
	KUNIT_EXPECT_EQ(test, tx.words[4], ANE_FW_RTK_TYPE(5) | (8ULL << 32) | 2);
	KUNIT_ASSERT_EQ(test, ane_fw_rtk_receive(&rtk, ANE_FW_RTK_TYPE(7) | 0x20,
						 0, &tx, ane_test_send), 0);
	KUNIT_EXPECT_EQ(test, tx.words[6], ANE_FW_RTK_TYPE(0xb) | 0x20);
	KUNIT_ASSERT_EQ(test, ane_fw_rtk_receive(&rtk, ANE_FW_RTK_TYPE(0xb) | 0x20,
						 0, &tx, ane_test_send), 0);
	KUNIT_EXPECT_EQ(test, rtk.phase, ANE_FW_RTK_READY);
	KUNIT_EXPECT_EQ(test, tx.words[7], ANE_FW_RTK_TYPE(5) | (32ULL << 32) | 2);
	KUNIT_EXPECT_EQ(test, tx.words[8], ANE_FW_RTK_TYPE(5) | (37ULL << 32) | 2);
}

static void ane_rtk_rejections_test(struct kunit *test)
{
	struct ane_fw_rtk rtk = {};
	struct ane_test_tx tx = {};
	u64 hello = ANE_FW_RTK_TYPE(1) | (12 << 16) | 12;
	int ret;

	KUNIT_EXPECT_EQ(test, ane_fw_rtk_receive(&rtk, ANE_FW_RTK_TYPE(1) |
						 (2 << 16) | 1, 0, &tx, ane_test_send), -EPROTO);
	KUNIT_EXPECT_EQ(test, tx.count, 0U);
	ret = ane_fw_rtk_receive(&rtk, hello, 2, &tx, ane_test_send);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, tx.count, 0U);
	KUNIT_EXPECT_EQ(test, ane_fw_rtk_receive(&rtk, ANE_FW_RTK_TYPE(7) | 0x20,
						 0, &tx, ane_test_send), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_rtk_receive(&rtk, ANE_FW_RTK_TYPE(0xff),
						 0, &tx, ane_test_send), -EOPNOTSUPP);
}

static struct kunit_case ane_protocol_cases[] = {
	KUNIT_CASE(ane_dynamic_channels_test),
	KUNIT_CASE(ane_channel_snapshot_test),
	KUNIT_CASE(ane_channel_rejections_test),
	KUNIT_CASE(ane_reply_status_test),
	KUNIT_CASE(ane_finished_event_test),
	KUNIT_CASE(ane_rtk_order_test),
	KUNIT_CASE(ane_rtk_rejections_test),
	{}
};

static struct kunit_suite ane_protocol_suite = {
	.name = "ane-fw-protocol",
	.test_cases = ane_protocol_cases,
};

kunit_test_suite(ane_protocol_suite);
MODULE_DESCRIPTION("KUnit tests for ANE 27.x channel and RTKit contracts");
MODULE_LICENSE("Dual MIT/GPL");

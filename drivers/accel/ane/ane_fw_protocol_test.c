// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "ane_fw_protocol.h"
#include "ane_h16_protocol.h"
#include "ane_t6021_boot.h"

static void ane_h16_id_table_test(struct kunit *test)
{
	static const struct ane_h16_command_info expected[] = {
		{ 0x200, 0x1c0, 0x1c0, 1, 1, false },
		{ 0x201, 0xc, 0xc, 1, 1, false },
		{ 0x202, 0x258, 0x258, 1, 1, false },
		{ 0x203, 0x10, 0x10, 1, 1, false },
		{ 0x20c, 0xca8, 0xcd8, 0x18, 7, false },
		{ 0x2d, 0xc, 0xc, 1, 7, false },
		{ 0x300, 0x60, 0x60, 1, 6, true },
	};
	u8 *command = kunit_kzalloc(test, 0xcd8, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, command);
	for (i = 0; i < ARRAY_SIZE(expected); i++) {
		const struct ane_h16_command_info *e = &expected[i];
		const struct ane_h16_command_info *info = ane_h16_command_info(e->id);
		size_t length;
		int ret;

		KUNIT_ASSERT_NOT_NULL(test, info);
		KUNIT_EXPECT_EQ(test, info->min_size, e->min_size);
		KUNIT_EXPECT_EQ(test, info->max_size, e->max_size);
		KUNIT_EXPECT_EQ(test, info->size_step, e->size_step);
		KUNIT_EXPECT_EQ(test, info->channel, e->channel);
		KUNIT_EXPECT_EQ(test, info->from_firmware, e->from_firmware);
		if (e->from_firmware) {
			ret = ane_h16_command_prepare(command, e->min_size, e->id, e->channel);
			KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
			continue;
		}
		for (length = e->min_size; length <= e->max_size; length += e->size_step) {
			ret = ane_h16_command_prepare(command, length, e->id, e->channel);
			KUNIT_EXPECT_EQ(test, ret, 0);
		}
		ret = ane_h16_command_prepare(command, e->min_size - 1, e->id, e->channel);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
		ret = ane_h16_command_prepare(command, e->max_size + 1, e->id, e->channel);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
		ret = ane_h16_command_prepare(command, e->min_size, e->id, e->channel ^ 1);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	}
	KUNIT_EXPECT_PTR_EQ(test, ane_h16_command_info(0x204), NULL);
	KUNIT_EXPECT_PTR_EQ(test, ane_h16_command_info(0x301), NULL);
	KUNIT_EXPECT_EQ(test, ane_h16_command_prepare(command, 0xca8, 0x204, 7),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ane_h16_command_prepare(command, 0xca9, 0x20c, 7), -EINVAL);
	/* The H14 CREATE_PROCESS size must never be sent to H16. */
	KUNIT_EXPECT_EQ(test, ane_h16_command_prepare(command, 0x10, 0x202, 1), -EINVAL);
}

static void ane_h16_envelope_test(struct kunit *test)
{
	u8 command[0xc];
	const u64 dva = 0x30000000000;

	memset(command, 0xa5, sizeof(command));
	put_unaligned_le32(1, command + 8);
	KUNIT_ASSERT_EQ(test, ane_h16_command_prepare(command, sizeof(command), 0x2d, 7), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(command), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 4), (u16)0x2d);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 6), (u16)0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(command + 8), 1U);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 0xc, 0, dva, 0xc,
						  0x2d, command, true), 0);
	/* Test the upper byte too: acknowledgment is u16, not u8. */
	put_unaligned_le16(0x100, command + 6);
	KUNIT_EXPECT_EQ(test, ane_fw_reply_status(dva | 1, 0xc, 0, dva, 0xc,
						  0x2d, command, true), -EREMOTEIO);
	put_unaligned_le32(0, command + 8);
	KUNIT_ASSERT_EQ(test, ane_h16_command_prepare(command, sizeof(command), 0x2d, 7), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 6), (u16)0);
	put_unaligned_le32(2, command + 8);
	KUNIT_EXPECT_EQ(test, ane_h16_command_prepare(command, sizeof(command), 0x2d, 7),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, ane_h16_command_prepare(NULL, sizeof(command), 0x2d, 7),
			-EINVAL);
}

static void ane_h16_report_test(struct kunit *test)
{
	u8 event[0x60] = {};
	unsigned int n;

	put_unaligned_le16(0x300, event + 4);
	for (n = 0; n < 4; n++) {
		put_unaligned_le32(0xfe00 | n, event);
		KUNIT_EXPECT_EQ(test, ane_h16_event_report_status(event, sizeof(event), 6), 0);
		/* A report can never satisfy the legacy completion predicate. */
		KUNIT_EXPECT_EQ(test, ane_fw_finished_event(event, sizeof(event), 0, 0, 0),
				-EPROTO);
	}
	KUNIT_EXPECT_EQ(test, ane_h16_event_report_status(event, sizeof(event), 7), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_event_report_status(event, 0x28, 6), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_event_report_status(event, 0xc, 6), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_event_report_status(NULL, 0x60, 6), -EPROTO);
	put_unaligned_le32(0xfe04, event);
	KUNIT_EXPECT_EQ(test, ane_h16_event_report_status(event, sizeof(event), 6), 0);
	put_unaligned_le32(0xfe00, event);
	put_unaligned_le16(0x301, event + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_event_report_status(event, sizeof(event), 6), -EPROTO);
	put_unaligned_le16(0x300, event + 4);
	put_unaligned_le16(0x100, event + 6);
	KUNIT_EXPECT_EQ(test, ane_h16_event_report_status(event, sizeof(event), 6), -EPROTO);
}

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
	KUNIT_EXPECT_EQ(test, channels[1].index, 1U);
	{
		u8 saved[ANE_FW_DESCRIPTOR_SIZE];
		u8 *first = ipc + TEST_TABLE_OFF + ANE_FW_DESCRIPTOR_SIZE;
		u8 *last = ipc + TEST_TABLE_OFF + 7 * ANE_FW_DESCRIPTOR_SIZE;

		memcpy(saved, first, sizeof(saved));
		memcpy(first, last, sizeof(saved));
		memcpy(last, saved, sizeof(saved));
		KUNIT_ASSERT_EQ(test, ane_test_channels_parse(ipc, channels), 0);
		KUNIT_EXPECT_EQ(test, channels[1].offset, 0x2400ULL);
		KUNIT_EXPECT_EQ(test, channels[1].index, 7U);
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

/* The H14 IPC size the native 27.2 firmware asks for, and its table DVA. */
#define TEST_H14_IPC_SIZE 0xc441
#define TEST_H14_IPC_DVA 0xff7b0000ULL

/* A 27.x table at the base of an H14-sized IPC surface. */
static u8 *ane_test_h14_table(struct kunit *test)
{
	static const u32 types[ANE_FW_CHANNELS] = { 2, 0, 0, 0, 1, 1, 1, 0 };
	u8 *ipc = kunit_kzalloc(test, TEST_H14_IPC_SIZE, GFP_KERNEL);
	unsigned int i;

	if (!ipc)
		return NULL;
	for (i = 0; i < ANE_FW_CHANNELS; i++) {
		u8 *d = ipc + i * ANE_FW_DESCRIPTOR_SIZE;

		strscpy((char *)d, ane_fw_channel_name(i), 64);
		put_unaligned_le32(types[i], d + 0x40);
		put_unaligned_le32(i, d + 0x44);
		put_unaligned_le64(0x10, d + 0x48);
		put_unaligned_le64(TEST_H14_IPC_DVA + ANE_FW_TABLE_SIZE + i * 0x400,
				   d + 0x50);
	}
	return ipc;
}

/* The 13.5 layout, as the 13.5 firmware writes it at the IPC base. */
static u8 *ane_test_static_table(struct kunit *test, u64 dva)
{
	u8 *ipc = kunit_kzalloc(test, ANE_T6021_CHMAN_TOTAL, GFP_KERNEL);
	unsigned int i;

	if (!ipc)
		return NULL;
	for (i = 0; i < ANE_T6021_CHMAN_COUNT; i++) {
		const struct ane_t6021_chman_static *s = &ane_t6021_chman_layout[i];
		struct ane_t6021_chman_desc *d = (void *)ipc + i * ANE_FW_DESCRIPTOR_SIZE;

		strscpy(d->name, s->name, sizeof(d->name));
		d->type = s->type;
		d->bit = s->bit;
		d->size = s->size;
		d->ring = dva + s->off;
	}
	return ipc;
}

static void ane_h14_layout_select_test(struct kunit *test)
{
	struct ane_fw_channel channels[ANE_FW_CHANNELS];
	u8 *snapshot = kunit_kzalloc(test, ANE_FW_TABLE_SIZE, GFP_KERNEL);
	u8 *ipc = ane_test_h14_table(test);
	u8 *legacy = ane_test_static_table(test, TEST_H14_IPC_DVA);
	u64 offset = U64_MAX;

	KUNIT_ASSERT_NOT_NULL(test, snapshot);
	KUNIT_ASSERT_NOT_NULL(test, ipc);
	KUNIT_ASSERT_NOT_NULL(test, legacy);
	/* The firmware's own IPC request selects the layout. */
	KUNIT_EXPECT_TRUE(test, ane_t6021_chman_published(TEST_H14_IPC_SIZE));
	KUNIT_EXPECT_TRUE(test, ane_t6021_chman_published(0x4000));
	KUNIT_EXPECT_FALSE(test, ane_t6021_chman_published(ANE_T6021_CHMAN_TOTAL));
	KUNIT_EXPECT_FALSE(test, ane_t6021_chman_published(ANE_T6021_CHMAN_TOTAL + 1));

	/* 27.x: the table at the DONE address inside the requested surface. */
	KUNIT_ASSERT_EQ(test, ane_fw_channels_snapshot(ipc, TEST_H14_IPC_DVA,
						       TEST_H14_IPC_SIZE,
						       TEST_H14_IPC_DVA, snapshot,
						       &offset, channels), 0);
	KUNIT_EXPECT_EQ(test, offset, 0ULL);
	KUNIT_EXPECT_EQ(test, memcmp(snapshot, ipc, ANE_FW_TABLE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, channels[1].offset, (u64)ANE_FW_TABLE_SIZE + 0x400);
	KUNIT_EXPECT_EQ(test, channels[1].bit, 1U);
	KUNIT_EXPECT_EQ(test, channels[5].type, 1U);
	KUNIT_EXPECT_EQ(test, channels[7].slots, 0x10U);
	/* The same table is not the 13.5 layout. */
	KUNIT_EXPECT_NE(test, ane_t6021_chman_check((const void *)ipc,
						    TEST_H14_IPC_DVA), 0U);

	/* 13.5: a full-size request keeps the static layout. */
	KUNIT_EXPECT_EQ(test, ane_t6021_chman_check((const void *)legacy,
						    TEST_H14_IPC_DVA), 0U);
	KUNIT_EXPECT_TRUE(test, ane_t6021_chman_host_init(legacy, ANE_T6021_CHMAN_TOTAL,
							  TEST_H14_IPC_DVA));
	/* The static layout never fits a 27.x-sized request. */
	KUNIT_EXPECT_FALSE(test, ane_t6021_chman_host_init(legacy, TEST_H14_IPC_SIZE,
							   TEST_H14_IPC_DVA));
}

static void ane_h14_table_rejections_test(struct kunit *test)
{
	struct ane_fw_channel channels[ANE_FW_CHANNELS] = {};
	u8 *snapshot = kunit_kzalloc(test, ANE_FW_TABLE_SIZE, GFP_KERNEL);
	u8 *ipc = ane_test_h14_table(test);
	u8 *d;
	u64 offset = 0;

	KUNIT_ASSERT_NOT_NULL(test, snapshot);
	KUNIT_ASSERT_NOT_NULL(test, ipc);
	/* DONE address outside the surface, or a table crossing its end. */
	KUNIT_EXPECT_EQ(test, ane_fw_channels_snapshot(ipc, TEST_H14_IPC_DVA,
						       TEST_H14_IPC_SIZE, 0, snapshot,
						       &offset, channels), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_channels_snapshot(ipc, TEST_H14_IPC_DVA,
						       TEST_H14_IPC_SIZE,
						       TEST_H14_IPC_DVA + TEST_H14_IPC_SIZE -
						       ANE_FW_TABLE_SIZE + 1,
						       snapshot, &offset, channels),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_channels_snapshot(NULL, TEST_H14_IPC_DVA,
						       TEST_H14_IPC_SIZE,
						       TEST_H14_IPC_DVA, snapshot,
						       &offset, channels), -EPROTO);
	/* A ring past the requested size. */
	d = ipc + 7 * ANE_FW_DESCRIPTOR_SIZE;
	put_unaligned_le64(TEST_H14_IPC_DVA + 0xc400, d + 0x50);
	KUNIT_EXPECT_EQ(test, ane_fw_channels_snapshot(ipc, TEST_H14_IPC_DVA,
						       TEST_H14_IPC_SIZE,
						       TEST_H14_IPC_DVA, snapshot,
						       &offset, channels), -EPROTO);
	put_unaligned_le64(TEST_H14_IPC_DVA + ANE_FW_TABLE_SIZE + 7 * 0x400, d + 0x50);
	/* A duplicate name, then an unknown one. */
	strscpy((char *)d, "IO", 64);
	KUNIT_EXPECT_EQ(test, ane_fw_channels_snapshot(ipc, TEST_H14_IPC_DVA,
						       TEST_H14_IPC_SIZE,
						       TEST_H14_IPC_DVA, snapshot,
						       &offset, channels), -EPROTO);
	strscpy((char *)d, "IPC", 64);
	KUNIT_EXPECT_EQ(test, ane_fw_channels_snapshot(ipc, TEST_H14_IPC_DVA,
						       TEST_H14_IPC_SIZE,
						       TEST_H14_IPC_DVA, snapshot,
						       &offset, channels), -EPROTO);
	/* A rejected table leaves the caller's channels untouched. */
	KUNIT_EXPECT_EQ(test, channels[1].slots, 0U);
}

static void ane_fw_host_command_test(struct kunit *test)
{
	static const struct { u16 id, size; } startup[] = {
		{ 0x4, 12 }, { 0x0, 12 }, { 0x3, 16 }, { 0x22, 100 },
	};
	u8 *command = kunit_kzalloc(test, 0x1c0, GFP_KERNEL);
	unsigned int i;
	u16 id, size;

	KUNIT_ASSERT_NOT_NULL(test, command);
	for (i = 0; ane_fw_startup_command(i, &id, &size); i++) {
		KUNIT_ASSERT_LT(test, i, (unsigned int)ARRAY_SIZE(startup));
		KUNIT_EXPECT_EQ(test, id, startup[i].id);
		KUNIT_EXPECT_EQ(test, size, startup[i].size);
		memset(command, 0xff, 0x1c0);
		KUNIT_EXPECT_EQ(test, ane_fw_host_command_prepare(command, size, id,
								  true, 9), 0);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(command), 0U);
		KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 4), id);
		KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 6), 0);
		KUNIT_EXPECT_EQ(test, ane_fw_host_command_prepare(command, size + 1, id,
								  true, 1), -EINVAL);
		KUNIT_EXPECT_EQ(test, ane_fw_host_command_prepare(command, size, id,
								  false, 1), -EINVAL);
	}
	KUNIT_EXPECT_EQ(test, i, (unsigned int)ARRAY_SIZE(startup));
	/* 13.5-only and mode commands never go out in the 27.x format. */
	KUNIT_EXPECT_EQ(test, ane_fw_host_command_prepare(command, 0x18, 0x1f,
							  true, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ane_fw_host_command_prepare(command, 0x18, 0x27,
							  true, 1), -EOPNOTSUPP);
	/* Program commands keep the H16 shape and ring checks. */
	KUNIT_EXPECT_EQ(test, ane_fw_host_command_prepare(command, 0x1c0, 0x200,
							  true, 1), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_host_command_prepare(command, 0x1c0, 0x200,
							  true, 7), -EINVAL);
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
	KUNIT_CASE(ane_h16_id_table_test),
	KUNIT_CASE(ane_h16_envelope_test),
	KUNIT_CASE(ane_h16_report_test),
	KUNIT_CASE(ane_dynamic_channels_test),
	KUNIT_CASE(ane_channel_snapshot_test),
	KUNIT_CASE(ane_channel_rejections_test),
	KUNIT_CASE(ane_h14_layout_select_test),
	KUNIT_CASE(ane_h14_table_rejections_test),
	KUNIT_CASE(ane_fw_host_command_test),
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

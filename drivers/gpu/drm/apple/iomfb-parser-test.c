// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by iomfb.c to exercise the receive dispatcher itself. */
#include <kunit/device.h>
#include <kunit/test.h>

static int iomfb_parser_test_init(struct kunit *test)
{
	struct apple_dcp *dcp;

	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;
	dcp->dev = kunit_device_register(test, "iomfb-parser");
	if (IS_ERR(dcp->dev))
		return PTR_ERR(dcp->dev);
	dcp->fw_compat = DCP_FIRMWARE_H17P;
	test->priv = dcp;
	return dcp_cb_shadow_alloc(dcp);
}

static void iomfb_tag_test(struct kunit *test)
{
	char tag[] = "411D";
	unsigned int i;

	KUNIT_EXPECT_EQ(test, dcp_parse_tag(tag), 114);
	for (i = 0; i < sizeof(tag) - 1; i++) {
		char saved = tag[i];

		tag[i] = 0xff;
		KUNIT_EXPECT_EQ(test, dcp_parse_tag(tag), -EINVAL);
		tag[i] = saved;
	}
	tag[0] = '/';
	KUNIT_EXPECT_EQ(test, dcp_parse_tag(tag), -EINVAL);
	tag[0] = ':';
	KUNIT_EXPECT_EQ(test, dcp_parse_tag(tag), -EINVAL);
}

static void iomfb_short_callback_test(struct kunit *test)
{
	struct apple_dcp *dcp = test->priv;
	unsigned int len;

	/* A NULL record must never be read before the envelope is checked. */
	for (len = 0; len < sizeof(struct dcp_packet_header); len++) {
		dcp->crashed = false;
		dcpep_handle_cb(dcp, DCP_CONTEXT_CB, NULL, len, 0);
		KUNIT_EXPECT_TRUE(test, dcp->crashed);
		KUNIT_EXPECT_EQ(test, dcp->ch_cb.depth, 0);
	}
}

static void iomfb_callback_envelope_test(struct kunit *test)
{
	struct apple_dcp *dcp = test->priv;
	static const struct {
		u32 length;
		u16 offset;
		u8 depth;
	} cases[] = {
		{ sizeof(struct dcp_packet_header), 0x8000, 0 },
		{ sizeof(struct dcp_packet_header), 0x7ff8, 0 },
		{ U32_MAX, 0, 0 },
		{ sizeof(struct dcp_packet_header), 0, DCP_MAX_CALL_DEPTH },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		dcp->crashed = false;
		dcp->ch_cb.depth = cases[i].depth;
		dcpep_handle_cb(dcp, DCP_CONTEXT_CB, NULL, cases[i].length, cases[i].offset);
		KUNIT_EXPECT_TRUE(test, dcp->crashed);
		KUNIT_EXPECT_EQ(test, dcp->ch_cb.depth, cases[i].depth);
	}
	dcp->crashed = false;
	dcpep_handle_cb(dcp, DCP_NUM_CONTEXTS, NULL, sizeof(struct dcp_packet_header), 0);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
}

static void iomfb_callback_lengths_test(struct kunit *test)
{
	struct apple_dcp *dcp = test->priv;
	struct dcp_packet_header record = { .tag = { '4', '1', '1', 'D' } };
	static const u32 lengths[][2] = {
		{ U32_MAX, 1 }, { 1, U32_MAX }, { 1, 0 }, { 0, 1 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lengths); i++) {
		record.in_len = lengths[i][0];
		record.out_len = lengths[i][1];
		dcp->crashed = false;
		dcpep_handle_cb(dcp, DCP_CONTEXT_CB, &record, sizeof(record), 0);
		KUNIT_EXPECT_TRUE(test, dcp->crashed);
		KUNIT_EXPECT_EQ(test, dcp->ch_cb.depth, 0);
	}
	memcpy(record.tag, "/11D", 4);
	record.in_len = 0;
	record.out_len = 0;
	dcp->crashed = false;
	dcpep_handle_cb(dcp, DCP_CONTEXT_CB, &record, sizeof(record), 0);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
}

static bool iomfb_parser_callback(struct apple_dcp *dcp, int tag, void *out, void *in)
{
	put_unaligned_le32(0x12345678, out);
	return false;
}

static void iomfb_callback_size_test(struct kunit *test)
{
	struct apple_dcp *dcp = test->priv;
	iomfb_cb_handler *handlers;
	struct dcp_packet_header *record;
	size_t size = sizeof(*record) + 0x1044 + 0x1004;

	handlers = kunit_kcalloc(test, IOMFB_MAX_CB, sizeof(*handlers), GFP_KERNEL);
	record = kunit_kzalloc(test, size + 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, handlers);
	KUNIT_ASSERT_NOT_NULL(test, record);
	handlers[114] = iomfb_parser_callback;
	dcp->cb_handlers = handlers;
	memcpy(record->tag, "411D", 4);
	record->in_len = 0x1044;
	record->out_len = 0x1008;
	dcpep_handle_cb(dcp, DCP_CONTEXT_CB, record, size + 4, 0);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
	KUNIT_EXPECT_EQ(test, dcp->ch_cb.depth, 0);
	dcp->crashed = false;
	record->out_len = 0x1004;
	dcpep_handle_cb(dcp, DCP_CONTEXT_CB, record, size, 0);
	KUNIT_EXPECT_FALSE(test, dcp->crashed);
	KUNIT_EXPECT_EQ(test, dcp->ch_cb.depth, 1);
	/* A deferred reply is built in the private copy ... */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(dcp->ch_cb.output[0]), 0x12345678U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)(record + 1) + 0x1044), 0U);
	/* ... and reaches the shared record when the callback is acked. */
	dcp_cb_reply_commit(&dcp->ch_cb, dcp_pop_depth(&dcp->ch_cb.depth));
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)(record + 1) + 0x1044), 0x12345678U);
}

static struct dcp_packet_header *iomfb_race_record;

/* Firmware rewrites the shared record while the handler runs. */
static bool iomfb_race_callback(struct apple_dcp *dcp, int tag, void *out, void *in)
{
	struct dcp_packet_header *hdr = in - sizeof(*hdr);

	iomfb_race_record->in_len = 0x7ffffff0;
	iomfb_race_record->out_len = 0x7ffffff0;
	put_unaligned_le32(0xffffffff, iomfb_race_record + 1);
	/* The handler sees only the copy, which still holds the checked values. */
	if (hdr->in_len != 8 || hdr->out_len != 4 || get_unaligned_le32(in) != 0x11111111)
		return false;
	put_unaligned_le32(0x12345678, out);
	return false;
}

static void iomfb_callback_copy_once_test(struct kunit *test)
{
	struct apple_dcp *dcp = test->priv;
	iomfb_cb_handler *handlers;
	struct dcp_packet_header *record;
	size_t size = sizeof(*record) + 8 + 4;
	u8 *canary;

	handlers = kunit_kcalloc(test, IOMFB_MAX_CB, sizeof(*handlers), GFP_KERNEL);
	record = kunit_kzalloc(test, size + 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, handlers);
	KUNIT_ASSERT_NOT_NULL(test, record);
	handlers[114] = iomfb_race_callback;
	dcp->cb_handlers = handlers;
	dcp->fw_compat = DCP_FIRMWARE_V_12_3;
	memcpy(record->tag, "411D", 4);
	record->in_len = 8;
	record->out_len = 4;
	put_unaligned_le32(0x11111111, record + 1);
	put_unaligned_le32(0xdeadbeef, (u8 *)(record + 1) + 8);
	canary = (u8 *)record + size;
	memset(canary, 0xa5, 64);
	iomfb_race_record = record;

	dcpep_handle_cb(dcp, DCP_CONTEXT_CB, record, size, 0x100);
	KUNIT_ASSERT_FALSE(test, dcp->crashed);
	KUNIT_ASSERT_EQ(test, dcp->ch_cb.depth, 1);
	dcp_cb_reply_commit(&dcp->ch_cb, dcp_pop_depth(&dcp->ch_cb.depth));
	/* The reply lands where the checked lengths put it, and nowhere else. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)(record + 1) + 8), 0x12345678U);
	KUNIT_EXPECT_NULL(test, memchr_inv(canary, 0xa5, 64));
}

static void iomfb_legacy_callback_lengths_test(struct kunit *test)
{
	struct apple_dcp *dcp = test->priv;
	struct dcp_packet_header record = { .tag = { '4', '1', '1', 'D' } };
	static const u32 lengths[][2] = {
		{ U32_MAX, 1 }, { 1, U32_MAX }, { 1, 0 }, { 0, 1 }, { 0x7ffffff0, 0 },
	};
	unsigned int i;

	/* Firmware without a size table still gets the envelope and length checks. */
	dcp->fw_compat = DCP_FIRMWARE_V_12_3;
	for (i = 0; i < ARRAY_SIZE(lengths); i++) {
		record.in_len = lengths[i][0];
		record.out_len = lengths[i][1];
		dcp->crashed = false;
		dcpep_handle_cb(dcp, DCP_CONTEXT_CB, &record, sizeof(record), 0);
		KUNIT_EXPECT_TRUE(test, dcp->crashed);
		KUNIT_EXPECT_EQ(test, dcp->ch_cb.depth, 0);
	}
	/* Command contexts carry no callbacks and have no private window. */
	record.in_len = 0;
	record.out_len = 0;
	dcp->crashed = false;
	dcpep_handle_cb(dcp, DCP_CONTEXT_CMD, &record, sizeof(record), 0);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
}

static bool iomfb_deferred_callback(struct apple_dcp *dcp, int tag, void *out, void *in)
{
	return false;
}

static void iomfb_push_deferred_callback(struct kunit *test, struct apple_dcp *dcp,
					 struct dcp_packet_header *record)
{
	iomfb_cb_handler *handlers;

	handlers = kunit_kcalloc(test, IOMFB_MAX_CB, sizeof(*handlers), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, handlers);
	handlers[114] = iomfb_deferred_callback;
	dcp->cb_handlers = handlers;
	memcpy(record->tag, "411D", 4);
	record->in_len = 0;
	record->out_len = 4;
	dcpep_handle_cb(dcp, DCP_CONTEXT_CB, record, sizeof(*record) + 4, 0);
	KUNIT_ASSERT_FALSE(test, dcp->crashed);
	KUNIT_ASSERT_EQ(test, dcp->ch_cb.depth, 1);
}

/* An ack from the firmware must complete a command, never a pending callback. */
static void iomfb_ack_pending_callback_test(struct kunit *test)
{
	struct apple_dcp *dcp = test->priv;
	struct dcp_packet_header *record;

	record = kunit_kzalloc(test, sizeof(*record) + 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, record);
	dcp->fw_compat = DCP_FIRMWARE_V_12_3;
	iomfb_push_deferred_callback(test, dcp, record);
	/* A command that once ran at this depth left its header behind. */
	dcp->ch_cb.header[0].in_len = 0x100;
	dcpep_handle_ack(dcp, DCP_CONTEXT_CB);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
	KUNIT_EXPECT_EQ(test, dcp->ch_cb.depth, 1);
}

/* dcp_ack() answers only a pending firmware callback. */
static void iomfb_ack_without_callback_test(struct kunit *test)
{
	struct apple_dcp *dcp = test->priv;
	static const struct dcp_method_entry call = {
		.name = "test", .tag = { 'A', '0', '0', '0' },
	};

	dcp->quiescing = true;	/* nothing is sent to a mailbox */
	dcp->fw_compat = DCP_FIRMWARE_V_12_3;
	dcp->shmem = kunit_kzalloc(test, 0x10000, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp->shmem);

	/* An empty stack must not wrap. */
	dcp_ack(dcp, DCP_CONTEXT_CB);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
	KUNIT_EXPECT_EQ(test, dcp->ch_cb.depth, 0);

	/* Nor may our own command be popped as if it were a callback. */
	dcp->crashed = false;
	dcp_push_context(dcp, DCP_CONTEXT_CB, &call, 4, 4, dcp->shmem, NULL, NULL);
	KUNIT_ASSERT_FALSE(test, dcp->crashed);
	dcp_ack(dcp, DCP_CONTEXT_CB);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
	KUNIT_EXPECT_EQ(test, dcp->ch_cb.depth, 1);
}

/* Every firmware bounds the command stack before it writes a frame. */
static void iomfb_command_depth_test(struct kunit *test)
{
	static const struct dcp_method_entry call = {
		.name = "test", .tag = { 'A', '0', '0', '0' },
	};
	static const enum dcp_firmware_version fws[] = {
		DCP_FIRMWARE_V_12_3, DCP_FIRMWARE_V_13_5, DCP_FIRMWARE_V_26_6_DESKTOP,
	};
	struct apple_dcp *dcp = test->priv;
	unsigned int i;

	dcp->shmem = kunit_kzalloc(test, 0x10000, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp->shmem);
	dcp->quiescing = true;
	for (i = 0; i < ARRAY_SIZE(fws); i++) {
		dcp->fw_compat = fws[i];
		dcp->crashed = false;
		memset(&dcp->ch_cmd, 0, sizeof(dcp->ch_cmd));
		dcp->ch_cmd.depth = DCP_MAX_CALL_DEPTH;
		dcp_push_context(dcp, DCP_CONTEXT_CMD, &call, 4, 4, &i, NULL, NULL);
		KUNIT_EXPECT_TRUE(test, dcp->crashed);
		KUNIT_EXPECT_EQ(test, dcp->ch_cmd.depth, DCP_MAX_CALL_DEPTH);
		/* A frame that does not fit the transmit window is refused too. */
		dcp->crashed = false;
		dcp->ch_cmd.depth = 0;
		dcp_push_context(dcp, DCP_CONTEXT_CMD, &call, 0x8000, 0, dcp->shmem, NULL, NULL);
		KUNIT_EXPECT_TRUE(test, dcp->crashed);
		KUNIT_EXPECT_EQ(test, dcp->ch_cmd.depth, 0);
		/* A command that fits is pushed and marked as a command frame. */
		dcp->crashed = false;
		dcp_push_context(dcp, DCP_CONTEXT_CMD, &call, 4, 4, &i, NULL, NULL);
		KUNIT_EXPECT_FALSE(test, dcp->crashed);
		KUNIT_EXPECT_EQ(test, dcp->ch_cmd.depth, 1);
		KUNIT_EXPECT_TRUE(test, dcp->ch_cmd.command[0]);
	}
}

/* Bytes past a record read as zero, not as what an earlier record left there. */
static void iomfb_callback_tail_test(struct kunit *test)
{
	struct apple_dcp *dcp = test->priv;
	struct dcp_packet_header *record;

	record = kunit_kzalloc(test, sizeof(*record) + 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, record);
	dcp->fw_compat = DCP_FIRMWARE_V_12_3;
	memset(dcp->ch_cb.cb_shadow, 0x5a, DCP_CB_SHADOW_SIZE);
	iomfb_push_deferred_callback(test, dcp, record);
	KUNIT_EXPECT_NULL(test, memchr_inv(dcp->ch_cb.cb_shadow + sizeof(*record) + 4, 0,
					   DCP_CB_TAIL));
}

static struct kunit_case iomfb_parser_cases[] = {
	KUNIT_CASE(iomfb_tag_test),
	KUNIT_CASE(iomfb_short_callback_test),
	KUNIT_CASE(iomfb_callback_envelope_test),
	KUNIT_CASE(iomfb_callback_lengths_test),
	KUNIT_CASE(iomfb_callback_size_test),
	KUNIT_CASE(iomfb_callback_copy_once_test),
	KUNIT_CASE(iomfb_legacy_callback_lengths_test),
	KUNIT_CASE(iomfb_ack_pending_callback_test),
	KUNIT_CASE(iomfb_ack_without_callback_test),
	KUNIT_CASE(iomfb_command_depth_test),
	KUNIT_CASE(iomfb_callback_tail_test),
	{}
};

static struct kunit_suite iomfb_parser_suite = {
	.name = "apple-iomfb-parser",
	.init = iomfb_parser_test_init,
	.test_cases = iomfb_parser_cases,
};

kunit_test_suite(iomfb_parser_suite);

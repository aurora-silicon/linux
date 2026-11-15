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
	return 0;
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
	KUNIT_EXPECT_EQ(test, get_unaligned_le32((u8 *)(record + 1) + 0x1044), 0x12345678U);
}

static struct kunit_case iomfb_parser_cases[] = {
	KUNIT_CASE(iomfb_tag_test),
	KUNIT_CASE(iomfb_short_callback_test),
	KUNIT_CASE(iomfb_callback_envelope_test),
	KUNIT_CASE(iomfb_callback_lengths_test),
	KUNIT_CASE(iomfb_callback_size_test),
	{}
};

static struct kunit_suite iomfb_parser_suite = {
	.name = "apple-iomfb-parser",
	.init = iomfb_parser_test_init,
	.test_cases = iomfb_parser_cases,
};

kunit_test_suite(iomfb_parser_suite);

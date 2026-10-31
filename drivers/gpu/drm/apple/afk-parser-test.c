// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by afk.c to exercise the EPIC receive handlers. */
#include <kunit/device.h>
#include <kunit/test.h>

static int afk_parser_test_init(struct kunit *test)
{
	struct apple_dcp_afkep *ep;
	struct apple_dcp *dcp;

	ep = kunit_kzalloc(test, sizeof(*ep), GFP_KERNEL);
	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	if (!ep || !dcp)
		return -ENOMEM;
	dcp->dev = kunit_device_register(test, "afk-parser");
	if (IS_ERR(dcp->dev))
		return PTR_ERR(dcp->dev);
	dcp->fw_compat = DCP_FIRMWARE_H17P;
	ep->dcp = dcp;
	ep->num_channels = 1;
	ep->services[0].channel = 1;
	ep->services[0].enabled = true;
	spin_lock_init(&ep->services[0].lock);
	test->priv = ep;
	return 0;
}

static void afk_compact_header_test(struct kunit *test)
{
	u8 record[sizeof(struct epic_hdr) + sizeof(struct epic_sub_hdr_h17p)] = {};
	unsigned int len;

	for (len = 0; len < sizeof(record); len++)
		KUNIT_EXPECT_FALSE(test, afk_validate_h17p_header(NULL, len));
	put_unaligned_le32(sizeof(record) - 8, record + 4);
	KUNIT_EXPECT_TRUE(test, afk_validate_h17p_header(record, sizeof(record)));
	KUNIT_EXPECT_FALSE(test, afk_validate_h17p_header(record, sizeof(record) + 1));
	put_unaligned_le32(U32_MAX, record + 4);
	KUNIT_EXPECT_FALSE(test, afk_validate_h17p_header(record, sizeof(record)));
}

static void afk_short_epic_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	unsigned int len;

	for (len = 0; len < sizeof(struct epic_hdr) + sizeof(struct epic_sub_hdr_h17p); len++)
		afk_recv_handle(ep, 0, EPIC_TYPE_NOTIFY, NULL, len);
	for (len = 0; len < 32; len++)
		afk_recv_handle_init(ep, 2, NULL, len);
	KUNIT_EXPECT_EQ(test, ep->num_channels, 1U);
}

static void afk_short_reply_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	struct epic_cmd_info *pending = &ep->services[0].cmds[0];
	unsigned int len;

	set_bit(0, ep->services[0].cmd_map);
	pending->tag = 0x200;
	for (len = 0; len < sizeof(struct epic_cmd); len++) {
		afk_recv_handle_reply(ep, 1, 0x200, NULL, len);
		KUNIT_EXPECT_FALSE(test, pending->done);
	}
}

static void afk_reply_index_test(struct kunit *test)
{
	struct apple_dcp_afkep *ep = test->priv;
	struct epic_cmd_info *pending = &ep->services[0].cmds[0];
	struct epic_cmd reply = { .retcode = cpu_to_le32(7) };

	set_bit(0, ep->services[0].cmd_map);
	pending->tag = 0x200;
	afk_recv_handle_reply(ep, 1, MAX_PENDING_CMDS, &reply, sizeof(reply));
	KUNIT_EXPECT_FALSE(test, pending->done);
	afk_recv_handle_reply(ep, 1, 0x2ff, &reply, sizeof(reply));
	KUNIT_EXPECT_FALSE(test, pending->done);
	afk_recv_handle_reply(ep, 1, 0x300, &reply, sizeof(reply));
	KUNIT_EXPECT_FALSE(test, pending->done);
	afk_recv_handle_reply(ep, 2, 0x200, NULL, sizeof(reply));
	KUNIT_EXPECT_FALSE(test, pending->done);
	afk_recv_handle_reply(ep, 1, 0x200, &reply, sizeof(reply));
	KUNIT_EXPECT_TRUE(test, pending->done);
	KUNIT_EXPECT_EQ(test, pending->retcode, 7U);
	reply.retcode = cpu_to_le32(9);
	afk_recv_handle_reply(ep, 1, 0x200, &reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test, pending->retcode, 7U);
}

static void afk_serialized_report_test(struct kunit *test)
{
	u8 dictionary[8] = {};
	unsigned int length;

	put_unaligned_le32(0xd3, dictionary);
	put_unaligned_le32(1U << 24, dictionary + 4);
	for (length = 0; length < sizeof(dictionary); length++)
		KUNIT_EXPECT_FALSE(test, afk_compact_report_dictionary(dictionary, length));
	KUNIT_EXPECT_TRUE(test, afk_compact_report_dictionary(dictionary, sizeof(dictionary)));
	put_unaligned_le32(0, dictionary);
	KUNIT_EXPECT_FALSE(test, afk_compact_report_dictionary(dictionary, sizeof(dictionary)));
}

static void afk_compact_pinned_slot_test(struct kunit *test)
{
	static const struct apple_epic_service_ops ops = {
		.name = "DCPAVSimpleVideoInterface",
		.reusable = true,
	};
	struct apple_dcp_afkep *ep = test->priv;
	struct apple_epic_service *old = &ep->services[0];
	struct apple_epic_service *next;

	ep->endpoint = AV_ENDPOINT;
	old->ops = &ops;
	old->unit = 0;
	strscpy(old->name, ops.name, sizeof(old->name));
	old->users = 1;
	next = afk_compact_slot(ep, &ops, ops.name, 0);
	KUNIT_EXPECT_PTR_NE(test, next, old);
	KUNIT_EXPECT_EQ(test, old->channel, 1U);
	KUNIT_EXPECT_TRUE(test, old->torndown);
	/* Once the caller releases it, a retired slot is reusable. */
	old->users = 0;
	KUNIT_EXPECT_PTR_EQ(test, afk_compact_slot(ep, &ops, ops.name, 0), old);
}

static struct kunit_case afk_parser_cases[] = {
	KUNIT_CASE(afk_compact_pinned_slot_test),
	KUNIT_CASE(afk_serialized_report_test),
	KUNIT_CASE(afk_compact_header_test),
	KUNIT_CASE(afk_short_epic_test),
	KUNIT_CASE(afk_short_reply_test),
	KUNIT_CASE(afk_reply_index_test),
	{}
};

static struct kunit_suite afk_parser_suite = {
	.name = "apple-epic-parser",
	.init = afk_parser_test_init,
	.test_cases = afk_parser_cases,
};

kunit_test_suite(afk_parser_suite);

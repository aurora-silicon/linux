// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by dockchannel-hid.c to exercise the MTP receive handlers. */
#include <kunit/device.h>
#include <kunit/test.h>

static int dchid_parser_test_init(struct kunit *test)
{
	struct dockchannel_hid *dchid;
	struct dchid_iface *iface;

	dchid = kunit_kzalloc(test, sizeof(*dchid), GFP_KERNEL);
	iface = kunit_kzalloc(test, sizeof(*iface), GFP_KERNEL);
	if (!dchid || !iface)
		return -ENOMEM;
	dchid->dev = kunit_device_register(test, "dchid-parser");
	if (IS_ERR(dchid->dev))
		return PTR_ERR(dchid->dev);
	iface->dchid = dchid;
	iface->name = "test";
	iface->out_report = 0x10;
	iface->out_flags = 0x80;
	iface->tx_seq = 7;
	init_completion(&iface->out_complete);
	dchid->ifaces[1] = iface;
	test->priv = iface;
	return 0;
}

static void dchid_short_ack_test(struct kunit *test)
{
	struct dchid_iface *iface = test->priv;
	struct dchid_hdr hdr = {};

	for (hdr.length = 0; hdr.length < sizeof(struct dchid_subhdr); hdr.length++) {
		dchid_handle_ack(iface, &hdr, NULL);
		KUNIT_EXPECT_FALSE(test, completion_done(&iface->out_complete));
		KUNIT_EXPECT_EQ(test, iface->out_report, 0x10);
	}
}

static void dchid_ack_length_test(struct kunit *test)
{
	struct dchid_iface *iface = test->priv;
	struct dchid_hdr hdr = { .length = sizeof(struct dchid_subhdr), .seq = 7 };
	struct dchid_subhdr sub = { .flags = 0x80 };

	/* A report ID is required even when no response body was requested. */
	dchid_handle_ack(iface, &hdr, &sub);
	KUNIT_EXPECT_FALSE(test, completion_done(&iface->out_complete));
	sub.length = U16_MAX;
	dchid_handle_ack(iface, &hdr, &sub);
	KUNIT_EXPECT_FALSE(test, completion_done(&iface->out_complete));
	KUNIT_EXPECT_EQ(test, iface->out_report, 0x10);
}

static void dchid_ack_identity_test(struct kunit *test)
{
	struct dchid_iface *iface = test->priv;
	struct dchid_hdr hdr = { .length = sizeof(struct dchid_subhdr) + 1, .seq = 7 };
	struct {
		struct dchid_subhdr sub;
		u8 id;
	} __packed ack = { .sub = { .flags = 0x80, .length = 1 }, .id = 0x11 };

	dchid_handle_ack(iface, &hdr, &ack);
	KUNIT_EXPECT_FALSE(test, completion_done(&iface->out_complete));
	ack.id = 0x10;
	hdr.seq++;
	dchid_handle_ack(iface, &hdr, &ack);
	KUNIT_EXPECT_FALSE(test, completion_done(&iface->out_complete));
	hdr.seq--;
	ack.sub.flags++;
	dchid_handle_ack(iface, &hdr, &ack);
	KUNIT_EXPECT_FALSE(test, completion_done(&iface->out_complete));
	ack.sub.flags--;
	dchid_handle_ack(iface, &hdr, &ack);
	KUNIT_EXPECT_TRUE(test, completion_done(&iface->out_complete));
	KUNIT_EXPECT_EQ(test, iface->out_report, -1);
	KUNIT_EXPECT_EQ(test, iface->resp_size, 1U);
}

static void dchid_ack_copy_test(struct kunit *test)
{
	struct dchid_iface *iface = test->priv;
	struct dchid_hdr hdr = { .length = sizeof(struct dchid_subhdr) + 5, .seq = 7 };
	struct {
		struct dchid_subhdr sub;
		u8 data[5];
	} __packed ack = { .sub = { .flags = 0x80, .length = 5 },
			  .data = { 0x10, 1, 2, 3, 4 } };
	u8 response[4] = { 0xa5, 0xa5, 0xa5, 0xa5 };

	iface->resp_buf = response + 1;
	iface->resp_size = 2;
	dchid_handle_ack(iface, &hdr, &ack);
	KUNIT_EXPECT_TRUE(test, completion_done(&iface->out_complete));
	KUNIT_EXPECT_EQ(test, iface->resp_size, 3U);
	KUNIT_EXPECT_MEMEQ(test, response, ((u8[]){ 0xa5, 1, 2, 0xa5 }), sizeof(response));
}

static void dchid_init_bounds_test(struct kunit *test)
{
	struct dchid_iface *iface = test->priv;
	struct dockchannel_hid *dchid = iface->dchid;
	struct {
		struct dchid_init_hdr hdr;
		struct dchid_init_block_hdr block;
	} __packed msg = { .hdr = { .iface = 1, .more_packets = 1 } };
	unsigned int len;

	for (len = 0; len < sizeof(msg.hdr); len++)
		dchid_handle_init(dchid, NULL, len);
	msg.hdr.iface = MAX_INTERFACES;
	dchid_handle_init(dchid, &msg, sizeof(msg));
	msg.hdr.iface = U8_MAX;
	dchid_handle_init(dchid, &msg, sizeof(msg));
	msg.hdr.iface = 1;
	msg.block.type = INIT_HID_DESCRIPTOR;
	msg.block.length = U16_MAX;
	dchid_handle_init(dchid, &msg, sizeof(msg));
	KUNIT_EXPECT_PTR_EQ(test, iface->hid_desc, NULL);
	msg.block.type = INIT_GPIO_REQUEST;
	msg.block.length = 0;
	dchid_handle_init(dchid, &msg, sizeof(msg));
	KUNIT_EXPECT_FALSE(test, iface->gpio_announced);
	KUNIT_EXPECT_PTR_EQ(test, dchid->ifaces[1], iface);
}

static void dchid_short_input_test(struct kunit *test)
{
	struct dchid_iface *iface = test->priv;
	struct dchid_work *work;
	unsigned int len;

	/* Any attempted HID delivery would dereference the absent HID device. */
	iface->open = true;
	for (len = 0; len < sizeof(struct dchid_subhdr); len++) {
		work = kzalloc(struct_size(work, data, len), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, work);
		work->iface = iface;
		work->hdr.iface = 1;
		work->hdr.length = len;
		dchid_packet_work(&work->work);
	}
	work = kzalloc(struct_size(work, data, sizeof(struct dchid_subhdr)), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, work);
	work->iface = iface;
	work->hdr.iface = 1;
	work->hdr.length = sizeof(struct dchid_subhdr);
	((struct dchid_subhdr *)work->data)->length = U16_MAX;
	dchid_packet_work(&work->work);
}

static void dchid_init_descriptor_test(struct kunit *test)
{
	struct dchid_iface *iface = test->priv;
	struct {
		struct dchid_init_hdr hdr;
		struct dchid_init_block_hdr block;
		u8 descriptor[4];
	} __packed msg = {
		.hdr = { .iface = 1, .more_packets = 1 },
		.block = { .type = INIT_HID_DESCRIPTOR, .length = 4 },
		.descriptor = { 0x05, 0x01, 0x09, 0x06 },
	};

	dchid_handle_init(iface->dchid, &msg, sizeof(msg));
	KUNIT_ASSERT_NOT_NULL(test, iface->hid_desc);
	KUNIT_EXPECT_EQ(test, iface->hid_desc_len, sizeof(msg.descriptor));
	KUNIT_EXPECT_MEMEQ(test, iface->hid_desc, msg.descriptor, sizeof(msg.descriptor));
}

static void dchid_packet_checksum_test(struct kunit *test)
{
	struct dchid_hdr hdr = { .hdr_len = sizeof(hdr), .length = 8 };
	u8 body[12] = {};
	u32 checksum;

	put_unaligned_le32(U32_MAX - dchid_checksum(&hdr, sizeof(hdr)), body + 8);
	KUNIT_EXPECT_TRUE(test, dchid_verify_checksum(&hdr, body, &checksum));
	KUNIT_EXPECT_EQ(test, checksum, U32_MAX);
	body[0] ^= 1;
	KUNIT_EXPECT_FALSE(test, dchid_verify_checksum(&hdr, body, &checksum));
}

static void dchid_unaligned_checksum_test(struct kunit *test)
{
	struct dchid_hdr hdr = { .hdr_len = sizeof(hdr) };
	u8 body[16] = {};
	u32 checksum;

	for (hdr.length = 9; hdr.length < 12; hdr.length++) {
		put_unaligned_le32(U32_MAX - dchid_checksum(&hdr, sizeof(hdr)), body + 8);
		memset(body + 12, 0xa5, 4);
		KUNIT_EXPECT_FALSE(test, dchid_verify_checksum(&hdr, body, &checksum));
	}
}

/* A ring whose next frame straddles the end of the data area. */
static void *dchid_test_ring(struct kunit *test, u32 consumer, u32 producer)
{
	struct dockchannel_hid *dchid = ((struct dchid_iface *)test->priv)->dchid;
	u32 *indices;

	dchid->ring = kunit_kzalloc(test, DCHID_RING_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dchid->ring);
	indices = dchid->ring;
	indices[0] = producer;
	indices[1] = consumer;
	dchid->ring_read = consumer;
	dchid->ring_registered = true;
	dchid->use_ring = true;
	return dchid->ring + DCHID_RING_HEADER_SIZE;
}

static void dchid_ring_wrap_test(struct kunit *test)
{
	struct dchid_iface *iface = test->priv;
	const u32 capacity = DCHID_RING_SIZE - DCHID_RING_HEADER_SIZE;
	const u32 consumer = capacity - 12;
	struct {
		struct dchid_hdr hdr;
		struct dchid_subhdr sub;
		u8 id;
		u8 pad[3];
		__le32 checksum;
	} __packed frame = {
		.hdr = { .hdr_len = sizeof(struct dchid_hdr), .channel = DCHID_CHANNEL_CMD,
			 .length = sizeof(struct dchid_subhdr) + 4, .seq = 7, .iface = 1 },
		.sub = { .flags = 0x80, .length = 1 },
		.id = 0x10,
	};
	u8 *data;

	/* A complete, checksummed acknowledgment of the pending command. */
	frame.checksum = cpu_to_le32(U32_MAX - dchid_checksum(&frame, sizeof(frame) - 4));
	data = dchid_test_ring(test, consumer, sizeof(frame) - 12);
	memcpy(data + consumer, &frame, 12);
	memcpy(data, (u8 *)&frame + 12, sizeof(frame) - 12);

	KUNIT_EXPECT_EQ(test, dchid_drain_ring(iface->dchid), 0);
	/* The frame arrived whole: its acknowledgment matched the pending command. */
	KUNIT_EXPECT_TRUE(test, completion_done(&iface->out_complete));
	KUNIT_EXPECT_EQ(test, iface->dchid->ring_read, sizeof(frame) - 12);
	KUNIT_EXPECT_EQ(test, ((u32 *)iface->dchid->ring)[1], sizeof(frame) - 12);
}

static void dchid_ring_bad_header_test(struct kunit *test)
{
	struct dchid_iface *iface = test->priv;
	struct dchid_hdr hdr = { .hdr_len = sizeof(hdr) + 1, .length = 4 };
	u8 *data = dchid_test_ring(test, 0, 16);

	memcpy(data, &hdr, sizeof(hdr));
	KUNIT_EXPECT_EQ(test, dchid_drain_ring(iface->dchid), -EPROTO);
	KUNIT_EXPECT_EQ(test, iface->dchid->ring_read, 0U);
}

static struct kunit_case dchid_parser_cases[] = {
	KUNIT_CASE(dchid_unaligned_checksum_test),
	KUNIT_CASE(dchid_short_ack_test),
	KUNIT_CASE(dchid_ack_length_test),
	KUNIT_CASE(dchid_ack_identity_test),
	KUNIT_CASE(dchid_ack_copy_test),
	KUNIT_CASE(dchid_init_bounds_test),
	KUNIT_CASE(dchid_short_input_test),
	KUNIT_CASE(dchid_init_descriptor_test),
	KUNIT_CASE(dchid_packet_checksum_test),
	KUNIT_CASE(dchid_ring_wrap_test),
	KUNIT_CASE(dchid_ring_bad_header_test),
	{}
};

static struct kunit_suite dchid_parser_suite = {
	.name = "dockchannel-hid-parser",
	.init = dchid_parser_test_init,
	.test_cases = dchid_parser_cases,
};

kunit_test_suite(dchid_parser_suite);

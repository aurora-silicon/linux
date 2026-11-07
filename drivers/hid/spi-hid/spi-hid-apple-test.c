// SPDX-License-Identifier: GPL-2.0
/* Fault injection for the Apple SPI HID command/response path. */
#include <kunit/device.h>
#include <kunit/test.h>

static const u8 spihid_test_dimensions[] = {
	0xd9, 0x10, 0x27, 0, 0, 0x70, 0x17, 0, 0,
	0x78, 0xec, 0x48, 0xf4, 0x88, 0x13, 0xb8, 0x0b,
};

struct spihid_test {
	struct spihid_apple *spihid;
	struct kunit *test;
	unsigned int writes;
	unsigned int bad_status;
	unsigned int corrupt_replies;
	int spi_error;
	bool stale_first;
};

static void spihid_test_reply(struct spihid_test *ctx, u8 device, u8 id, u8 report, bool corrupt)
{
	struct spihid_apple *spihid = ctx->spihid;
	struct spihid_transfer_packet *pkt = (void *)spihid->rx_buf;
	struct spihid_msg_hdr *hdr = (void *)pkt->data;
	size_t len = sizeof(spihid_test_dimensions);

	memset(pkt, 0, sizeof(*pkt));
	pkt->flags = SPIHID_WRITE_PACKET;
	pkt->device = device;
	pkt->length = cpu_to_le16(sizeof(*hdr) + len + 2);
	hdr->unknown0 = 0x32;
	hdr->unknown1 = report;
	hdr->id = id;
	hdr->length = cpu_to_le16(len);
	memcpy(pkt->data + sizeof(*hdr), spihid_test_dimensions, len);
	put_unaligned_le16(crc16(0, pkt->data, sizeof(*hdr) + len),
			   pkt->data + sizeof(*hdr) + len);
	pkt->crc16 = cpu_to_le16(crc16(0, (u8 *)pkt,
				       offsetof(struct spihid_transfer_packet, crc16)));
	if (corrupt)
		pkt->crc16 ^= cpu_to_le16(1);
	spihid_process_read(spihid);
}

static int spihid_test_transfer(struct spi_controller *ctlr,
				struct spi_message *msg)
{
	struct spihid_test *ctx = *(struct spihid_test **)spi_controller_get_devdata(ctlr);
	struct spihid_apple *spihid = ctx->spihid;
	struct spihid_transfer_packet *pkt = (void *)spihid->tx_buf;
	struct spihid_msg_hdr *hdr = (void *)pkt->data;

	ctx->writes++;
	if (ctx->spi_error) {
		msg->status = ctx->spi_error;
		spi_finalize_current_message(ctlr);
		return 0;
	}

	if (ctx->writes <= ctx->bad_status) {
		static const u8 bad[] = { 0x9c, 0x17, 0xe5, 0xf8 };

		memcpy(spihid->status_buf, bad, sizeof(bad));
	} else {
		memcpy(spihid->status_buf, spi_hid_apple_status_ok,
		       sizeof(spi_hid_apple_status_ok));
		if (hdr->unknown0 == 0x32) {
			if (ctx->stale_first) {
				spihid_test_reply(ctx, SPIHID_DEVICE_ID_KBD,
						  hdr->id, hdr->unknown1, false);
				KUNIT_EXPECT_EQ(ctx->test, spihid->tp.reply_len, 0);
				KUNIT_EXPECT_EQ(ctx->test, spihid->kbd.reply_len, 0);
				spihid_test_reply(ctx, pkt->device, hdr->id - 1,
						  hdr->unknown1, false);
				KUNIT_EXPECT_EQ(ctx->test, spihid->tp.reply_len, 0);
				spihid_test_reply(ctx, pkt->device, hdr->id,
						  hdr->unknown1 + 1, false);
				KUNIT_EXPECT_EQ(ctx->test, spihid->tp.reply_len, 0);
			}
			/* Complete before spi_sync returns, like an early IRQ. */
			spihid_test_reply(ctx, pkt->device, hdr->id, hdr->unknown1,
					  ctx->writes <= ctx->corrupt_replies);
		}
	}
	msg->status = 0;
	msg->actual_length = sizeof(*pkt) + sizeof(spi_hid_apple_status_ok);
	spi_finalize_current_message(ctlr);
	return 0;
}

static void spihid_test_unregister_controller(void *data)
{
	spi_unregister_controller(data);
}

static void spihid_test_unregister_device(void *data)
{
	spi_unregister_device(data);
}

static int spihid_test_init(struct kunit *test)
{
	struct spi_board_info info = {
		.modalias = "spihid-kunit",
		.max_speed_hz = 1000000,
	};
	struct spi_controller *ctlr;
	struct spihid_test *ctx;
	struct spihid_apple *spihid;
	struct device *parent;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	spihid = kunit_kzalloc(test, sizeof(*spihid), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, spihid);
	ctx->spihid = spihid;
	ctx->test = test;
	test->priv = ctx;

	parent = kunit_device_register(test, "spihid-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, parent);
	ctlr = spi_alloc_host(parent, sizeof(ctx));
	KUNIT_ASSERT_NOT_NULL(test, ctlr);
	*(struct spihid_test **)spi_controller_get_devdata(ctlr) = ctx;
	ctlr->transfer_one_message = spihid_test_transfer;
	ret = spi_register_controller(ctlr);
	if (ret) {
		spi_controller_put(ctlr);
		return ret;
	}
	ret = kunit_add_action_or_reset(test, spihid_test_unregister_controller, ctlr);
	KUNIT_ASSERT_EQ(test, ret, 0);
	spihid->spidev = spi_new_device(ctlr, &info);
	KUNIT_ASSERT_NOT_NULL(test, spihid->spidev);
	ret = kunit_add_action_or_reset(test, spihid_test_unregister_device, spihid->spidev);
	KUNIT_ASSERT_EQ(test, ret, 0);

	spihid->rx_buf = kunit_kzalloc(test, SPIHID_MAX_INPUT_REPORT_SIZE, GFP_KERNEL);
	spihid->report.buf = kunit_kzalloc(test, SPIHID_MAX_INPUT_REPORT_SIZE, GFP_KERNEL);
	spihid->tx_buf = kunit_kzalloc(test, sizeof(struct spihid_transfer_packet), GFP_KERNEL);
	spihid->status_buf = kunit_kzalloc(test, sizeof(spi_hid_apple_status_ok), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, spihid->rx_buf);
	KUNIT_ASSERT_NOT_NULL(test, spihid->report.buf);
	KUNIT_ASSERT_NOT_NULL(test, spihid->tx_buf);
	KUNIT_ASSERT_NOT_NULL(test, spihid->status_buf);
	mutex_init(&spihid->tx_lock);
	mutex_init(&spihid->get_report_lock);
	spin_lock_init(&spihid->reply_lock);
	init_waitqueue_head(&spihid->wait);
	spihid_set_power(spihid, SPIHID_PWR_ON);
	spihid->tp.id = SPIHID_DEVICE_ID_TP;
	spihid_apple_setup_spi_msgs(spihid);
	return 0;
}

static void spihid_test_bad_status(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	u8 buf[32];
	int ret;

	ctx->bad_status = SPIHID_GET_REPORT_ATTEMPTS;
	memset(buf, 0xa5, sizeof(buf));
	ret = spihid_get_report(ctx->spihid, &ctx->spihid->tp, 0xd9, buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	KUNIT_EXPECT_EQ(test, ctx->writes, SPIHID_GET_REPORT_ATTEMPTS);
	KUNIT_EXPECT_EQ(test, buf[0], 0xa5);
	KUNIT_EXPECT_FALSE(test, ctx->spihid->tp.reply_pending);
	/* A rejected attempt must not leave a waiter for a late reply. */
	spihid_test_reply(ctx, SPIHID_DEVICE_ID_TP, ctx->spihid->msg_id - 1, 0xd9, false);
	KUNIT_EXPECT_EQ(test, ctx->spihid->tp.reply_len, 0);
}

static void spihid_test_later_get_bad_status(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	struct spihid_apple *spihid = ctx->spihid;
	u8 mt[] = { 0x02, 0x01 };
	u8 buf[32];
	int ret;

	/* Earlier successful traffic must not prevent later GET recovery. */
	spihid->msg_id = 9;
	ret = spihid_get_report(spihid, &spihid->tp, 0xd9, buf, sizeof(buf));
	KUNIT_ASSERT_EQ(test, ret, sizeof(spihid_test_dimensions));
	KUNIT_ASSERT_EQ(test, ctx->writes, 1);

	/* First input open sends the MT SET immediately before dimensions. */
	ret = spihid_apple_request(spihid, SPIHID_DEVICE_ID_TP, 0x52,
				   mt[0], 0, 2, mt, sizeof(mt), NULL);
	KUNIT_ASSERT_EQ(test, ret, sizeof(mt));
	KUNIT_ASSERT_EQ(test, ctx->writes, 2);

	/* Reproduce report d9, id 11, status 9c 17 e5 f8, then succeed. */
	ctx->bad_status = 3;
	ctx->stale_first = true;
	memset(buf, 0xa5, sizeof(buf));
	ret = spihid_get_report(spihid, &spihid->tp, 0xd9, buf, sizeof(buf));
	KUNIT_ASSERT_EQ(test, ret, sizeof(spihid_test_dimensions));
	KUNIT_EXPECT_EQ(test, ctx->writes, 4);
	KUNIT_EXPECT_EQ(test, spihid->msg_id, 13);
	KUNIT_EXPECT_MEMEQ(test, buf, spihid_test_dimensions, ret);
	KUNIT_EXPECT_EQ(test, buf[ret], 0xa5);
	KUNIT_EXPECT_FALSE(test, spihid->tp.reply_pending);
}

static void spihid_test_crc_timeout_retry(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	u8 buf[32];
	int ret;

	ctx->corrupt_replies = 1;
	ret = spihid_get_report(ctx->spihid, &ctx->spihid->tp, 0xd9, buf, sizeof(buf));
	KUNIT_ASSERT_EQ(test, ret, sizeof(spihid_test_dimensions));
	KUNIT_EXPECT_EQ(test, ctx->writes, 2);
	KUNIT_EXPECT_MEMEQ(test, buf, spihid_test_dimensions, ret);
}

static void spihid_test_stale_reply(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	u8 buf[32];
	int ret;

	ctx->corrupt_replies = 1;
	ctx->stale_first = true;
	ret = spihid_get_report(ctx->spihid, &ctx->spihid->tp, 0xd9, buf, sizeof(buf));
	KUNIT_ASSERT_EQ(test, ret, sizeof(spihid_test_dimensions));
	KUNIT_EXPECT_EQ(test, ctx->writes, 2);
	KUNIT_EXPECT_MEMEQ(test, buf, spihid_test_dimensions, ret);
}

static void spihid_test_retry_limit(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	u8 buf[32];
	int ret;

	ctx->corrupt_replies = SPIHID_GET_REPORT_ATTEMPTS;
	ret = spihid_get_report(ctx->spihid, &ctx->spihid->tp, 0xd9, buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ctx->writes, SPIHID_GET_REPORT_ATTEMPTS);
	KUNIT_EXPECT_FALSE(test, ctx->spihid->tp.reply_pending);
	/* A reply arriving after timeout must not publish stale data. */
	spihid_test_reply(ctx, SPIHID_DEVICE_ID_TP, ctx->spihid->msg_id - 1, 0xd9, false);
	KUNIT_EXPECT_EQ(test, ctx->spihid->tp.reply_len, 0);
}

static void spihid_test_short_buffer(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	u8 buf[32];
	int ret;

	memset(buf, 0xa5, sizeof(buf));
	ret = spihid_get_report(ctx->spihid, &ctx->spihid->tp, 0xd9, buf, 4);
	KUNIT_EXPECT_EQ(test, ret, 4);
	KUNIT_EXPECT_MEMEQ(test, buf, spihid_test_dimensions, 4);
	KUNIT_EXPECT_EQ(test, buf[4], 0xa5);
	KUNIT_EXPECT_EQ(test, buf[7], 0xa5);

	/* A larger destination must not cause a source overread either. */
	ret = spihid_get_report(ctx->spihid, &ctx->spihid->tp, 0xd9, buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, ret, sizeof(spihid_test_dimensions));
	KUNIT_EXPECT_MEMEQ(test, buf, spihid_test_dimensions, sizeof(spihid_test_dimensions));
	KUNIT_EXPECT_EQ(test, buf[sizeof(spihid_test_dimensions)], 0xa5);
}

static void spihid_test_mixed_retry_limit(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	u8 buf[32];
	int ret;

	/* Status rejection and lost replies share one bounded budget. */
	ctx->bad_status = 1;
	ctx->corrupt_replies = SPIHID_GET_REPORT_ATTEMPTS;
	memset(buf, 0xa5, sizeof(buf));
	ret = spihid_get_report(ctx->spihid, &ctx->spihid->tp, 0xd9, buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ctx->writes, SPIHID_GET_REPORT_ATTEMPTS);
	KUNIT_EXPECT_EQ(test, buf[0], 0xa5);
	KUNIT_EXPECT_FALSE(test, ctx->spihid->tp.reply_pending);
}

static void spihid_test_spi_error_no_retry(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	static const int errors[] = { -EIO, -EPROTO };
	u8 buf[32];
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(errors); i++) {
		ctx->writes = 0;
		ctx->spi_error = errors[i];
		memset(buf, 0xa5, sizeof(buf));
		ret = spihid_get_report(ctx->spihid, &ctx->spihid->tp, 0xd9,
					buf, sizeof(buf));
		KUNIT_EXPECT_EQ(test, ret, errors[i]);
		KUNIT_EXPECT_EQ(test, ctx->writes, 1);
		KUNIT_EXPECT_EQ(test, buf[0], 0xa5);
		KUNIT_EXPECT_FALSE(test, ctx->spihid->tp.reply_pending);
	}
}

static void spihid_test_no_set_retry(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	u8 buf[] = { 0x21, 0 };
	int ret;

	ctx->bad_status = 1;
	ret = spihid_apple_request(ctx->spihid, SPIHID_DEVICE_ID_AUX,
				   0x52, buf[0], 0, 2, buf, sizeof(buf), NULL);
	KUNIT_EXPECT_EQ(test, ret, sizeof(buf));
	KUNIT_EXPECT_EQ(test, ctx->writes, 1);
}

static struct kunit_case spihid_test_cases[] = {
	KUNIT_CASE(spihid_test_bad_status),
	KUNIT_CASE(spihid_test_later_get_bad_status),
	KUNIT_CASE(spihid_test_crc_timeout_retry),
	KUNIT_CASE(spihid_test_stale_reply),
	KUNIT_CASE(spihid_test_retry_limit),
	KUNIT_CASE(spihid_test_short_buffer),
	KUNIT_CASE(spihid_test_mixed_retry_limit),
	KUNIT_CASE(spihid_test_spi_error_no_retry),
	KUNIT_CASE(spihid_test_no_set_retry),
	{}
};

static struct kunit_suite spihid_test_suite = {
	.name = "spi-hid-apple",
	.init = spihid_test_init,
	.test_cases = spihid_test_cases,
};

kunit_test_suite(spihid_test_suite);

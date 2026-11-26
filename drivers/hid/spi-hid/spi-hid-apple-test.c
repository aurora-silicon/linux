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

static void spihid_test_fragment(struct spihid_apple *spihid, u16 offset,
				 u16 remain)
{
	struct spihid_transfer_packet *pkt = (void *)spihid->rx_buf;

	memset(pkt, 0, sizeof(*pkt));
	pkt->flags = SPIHID_READ_PACKET;
	pkt->device = SPIHID_DEVICE_ID_TP;
	pkt->offset = cpu_to_le16(offset);
	pkt->remain = cpu_to_le16(remain);
	pkt->length = cpu_to_le16(sizeof(pkt->data));
	memset(pkt->data, 0x5a, sizeof(pkt->data));
	pkt->crc16 = cpu_to_le16(crc16(0, (u8 *)pkt,
				       offsetof(struct spihid_transfer_packet, crc16)));
	spihid_process_read(spihid);
}

/* A fragmented message longer than the reassembly buffer is dropped. */
static void spihid_test_oversized_message(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	struct spihid_apple *spihid = ctx->spihid;
	const size_t frag = sizeof(((struct spihid_transfer_packet *)0)->data);
	const size_t total = SPIHID_MAX_INPUT_REPORT_SIZE + 2 * frag;
	const size_t guard = 2 * frag;
	size_t offset, i;
	u8 *buf;

	/* Room past the real buffer size, so an overrun is visible. */
	buf = kunit_kzalloc(test, SPIHID_MAX_INPUT_REPORT_SIZE + guard,
			    GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf + SPIHID_MAX_INPUT_REPORT_SIZE, 0xc3, guard);
	spihid->report.buf = buf;

	/* Every fragment but the last, so the message never completes. */
	for (offset = 0; offset + frag < total; offset += frag) {
		spihid_test_fragment(spihid, offset, total - offset - frag);
		KUNIT_EXPECT_LE(test, spihid->report.length,
				SPIHID_MAX_INPUT_REPORT_SIZE);
		KUNIT_EXPECT_LE(test, spihid->report.offset,
				SPIHID_MAX_INPUT_REPORT_SIZE);
	}

	for (i = 0; i < guard; i++)
		if (buf[SPIHID_MAX_INPUT_REPORT_SIZE + i] != 0xc3)
			break;
	KUNIT_EXPECT_EQ(test, i, guard);
}

/* A message that fills the buffer exactly is still reassembled. */
static void spihid_test_max_message(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	struct spihid_apple *spihid = ctx->spihid;
	const size_t frag = sizeof(((struct spihid_transfer_packet *)0)->data);
	const size_t total = SPIHID_MAX_INPUT_REPORT_SIZE;
	size_t offset;

	for (offset = 0; offset + frag < total; offset += frag) {
		spihid_test_fragment(spihid, offset, total - offset - frag);
		KUNIT_EXPECT_EQ(test, spihid->report.offset, offset + frag);
		KUNIT_EXPECT_EQ(test, spihid->report.length, total);
	}
}

/* The management interface has no descriptor buffer. */
static void spihid_test_mngt_report_desc(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	u8 desc[16] = { 0x05, 0x01 };

	KUNIT_EXPECT_NULL(test, ctx->spihid->mngt.hid_desc);
	KUNIT_EXPECT_FALSE(test,
			   spihid_process_iface_hid_report_desc(ctx->spihid,
								SPIHID_DEVICE_ID_MNGT,
								desc, sizeof(desc)));
	KUNIT_EXPECT_EQ(test, ctx->spihid->mngt.hid_desc_len, 0);
}

/*
 * A transport that logs what the core asks of it. Its disable_irq() stands
 * for the real one, which returns only after the threaded handler has run.
 */
struct spihid_test_ops {
	struct spihid_apple_ops ops;
	struct spihid_test *ctx;
	struct spi_device *spi;
	char log[64];
	size_t log_len;
	bool boot;		/* enable_irq() delivers the boot message */
	int irq_enabled;
	bool hid_at_irq_off;	/* a HID device still existed at disable_irq() */
	int power_at_irq_off;
};

#define to_spihid_test_ops(o) container_of(o, struct spihid_test_ops, ops)

static void spihid_test_log(struct spihid_test_ops *t, const char *what)
{
	t->log_len += scnprintf(t->log + t->log_len, sizeof(t->log) - t->log_len,
				"%s ", what);
}

static int spihid_test_power_on(struct spihid_apple_ops *ops)
{
	spihid_test_log(to_spihid_test_ops(ops), "on");
	return 0;
}

static int spihid_test_power_off(struct spihid_apple_ops *ops)
{
	spihid_test_log(to_spihid_test_ops(ops), "off");
	return 0;
}

static int spihid_test_enable_irq(struct spihid_apple_ops *ops)
{
	struct spihid_test_ops *t = to_spihid_test_ops(ops);
	struct spihid_apple *spihid = spi_get_drvdata(t->spi);

	spihid_test_log(t, "irq-on");
	t->irq_enabled++;
	if (t->boot) {
		/* The fake controller now answers this state's transfers. */
		t->ctx->spihid = spihid;
		spihid->status_booted = true;
		wake_up_interruptible(&spihid->wait);
	}
	return 0;
}

static int spihid_test_disable_irq(struct spihid_apple_ops *ops)
{
	struct spihid_test_ops *t = to_spihid_test_ops(ops);
	struct spihid_apple *spihid = spi_get_drvdata(t->spi);

	spihid_test_log(t, "irq-off");
	t->irq_enabled--;
	t->hid_at_irq_off = spihid->kbd.hid || spihid->tp.hid || spihid->aux.hid;
	t->power_at_irq_off = atomic_read(&spihid->power);
	return 0;
}

static struct spihid_test_ops *spihid_test_ops_init(struct kunit *test,
						    struct spi_device *spi)
{
	struct spihid_test_ops *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->ctx = test->priv;
	t->spi = spi;
	t->ops.power_on = spihid_test_power_on;
	t->ops.power_off = spihid_test_power_off;
	t->ops.enable_irq = spihid_test_enable_irq;
	t->ops.disable_irq = spihid_test_disable_irq;
	return t;
}

/*
 * The IRQ is requested before the core's device-managed state, so devres
 * frees that state first when probe fails. A probe that fails after it
 * enabled the IRQ must have stopped the handler and powered the controller
 * off by the time it returns.
 */
static void spihid_test_probe_fails(struct kunit *test, bool boot)
{
	struct spihid_test *ctx = test->priv;
	struct spihid_apple *orig = ctx->spihid;
	struct spi_device *spi = orig->spidev;
	struct spihid_test_ops *t = spihid_test_ops_init(test, spi);
	void *group;
	int ret;

	t->boot = boot;
	group = devres_open_group(&spi->dev, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, group);

	ret = spihid_apple_core_probe(spi, &t->ops);

	KUNIT_EXPECT_EQ(test, ret, -ENODEV);
	KUNIT_EXPECT_STREQ(test, t->log, "on irq-on irq-off off ");
	KUNIT_EXPECT_EQ(test, t->irq_enabled, 0);
	KUNIT_EXPECT_EQ(test, t->power_at_irq_off, SPIHID_PWR_OFF);

	ctx->spihid = orig;
	spi_set_drvdata(spi, orig);
	devres_release_group(&spi->dev, group);
}

/* No boot message: probe gives up waiting for the device to boot. */
static void spihid_test_probe_boot_timeout(struct kunit *test)
{
	spihid_test_probe_fails(test, false);
}

/* Booted, but the device information never comes. */
static void spihid_test_probe_info_timeout(struct kunit *test)
{
	spihid_test_probe_fails(test, true);
}

/* Remove stops the IRQ, and with it report delivery, before the HID devices go. */
static void spihid_test_remove_irq_first(struct kunit *test)
{
	struct spihid_test *ctx = test->priv;
	struct spihid_apple *spihid = ctx->spihid;
	struct spihid_test_ops *t = spihid_test_ops_init(test, spihid->spidev);

	spihid->ops = &t->ops;
	spi_set_drvdata(spihid->spidev, spihid);
	spihid->kbd.hid = hid_allocate_device();
	KUNIT_ASSERT_FALSE(test, IS_ERR(spihid->kbd.hid));
	spihid->tp.hid = hid_allocate_device();
	KUNIT_ASSERT_FALSE(test, IS_ERR(spihid->tp.hid));

	spihid_apple_core_remove(spihid->spidev);

	KUNIT_EXPECT_STREQ(test, t->log, "irq-off off ");
	KUNIT_EXPECT_TRUE(test, t->hid_at_irq_off);
	KUNIT_EXPECT_EQ(test, t->power_at_irq_off, SPIHID_PWR_OFF);
	KUNIT_EXPECT_NULL(test, spihid->kbd.hid);
	KUNIT_EXPECT_NULL(test, spihid->tp.hid);
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
	KUNIT_CASE(spihid_test_oversized_message),
	KUNIT_CASE(spihid_test_max_message),
	KUNIT_CASE(spihid_test_mngt_report_desc),
	KUNIT_CASE(spihid_test_probe_boot_timeout),
	KUNIT_CASE(spihid_test_probe_info_timeout),
	KUNIT_CASE(spihid_test_remove_irq_first),
	{}
};

static struct kunit_suite spihid_test_suite = {
	.name = "spi-hid-apple",
	.init = spihid_test_init,
	.test_cases = spihid_test_cases,
};

kunit_test_suite(spihid_test_suite);

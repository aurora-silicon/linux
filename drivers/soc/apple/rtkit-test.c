// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple RTKit IPC library. Included by rtkit.c.
 *
 * The tests drive the receive handlers with messages a co-processor would
 * send and check the replies. apple_rtkit_send_message() is redirected to a
 * static stub just before the mailbox write, so its crashed and running
 * checks still run; shared buffers come from a fake shmem_setup_ep. No
 * mailbox, IOMMU or MMIO is touched.
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

#define RTK_TEST_MAX_SENT 32
#define RTK_TEST_IOVA_BASE 0x10000000

struct rtk_test_msg {
	u8 ep;
	u64 msg;
};

struct rtk_test_ctx {
	struct kunit *test;
	struct apple_rtkit *rtk;
	struct apple_rtkit_ops ops;

	struct rtk_test_msg sent[RTK_TEST_MAX_SENT];
	unsigned int n_sent;

	unsigned int n_setup;
	u8 setup_ep;
	struct apple_rtkit_shmem setup_req;
	int setup_ret;
	dma_addr_t next_iova;
	/* Written to each new buffer, as a log the firmware already wrote. */
	const void *prefill;
	size_t prefill_len;

	unsigned int n_crashed;
	size_t crashed_size;
	u32 crashed_fourcc;

	unsigned int n_syslog;
	char syslog_ctx[24];
	char syslog_msg[64];

	unsigned int n_recv;
	u8 recv_ep;
	u64 recv_msg;
};

static int rtk_test_send(struct apple_rtkit *rtk, u8 ep, u64 message,
			 struct completion *completion, bool atomic)
{
	struct rtk_test_ctx *ctx = rtk->cookie;

	if (ctx->n_sent < RTK_TEST_MAX_SENT) {
		ctx->sent[ctx->n_sent].ep = ep;
		ctx->sent[ctx->n_sent].msg = message;
	}
	ctx->n_sent++;
	return 0;
}

static int rtk_test_shmem_setup(void *cookie, u8 ep, struct apple_rtkit_shmem *bfr)
{
	struct rtk_test_ctx *ctx = cookie;

	ctx->n_setup++;
	ctx->setup_ep = ep;
	ctx->setup_req = *bfr;
	if (ctx->setup_ret)
		return ctx->setup_ret;

	bfr->buffer = kunit_kzalloc(ctx->test, bfr->size, GFP_KERNEL);
	if (!bfr->buffer)
		return -ENOMEM;
	if (ctx->prefill)
		memcpy(bfr->buffer, ctx->prefill, min(ctx->prefill_len, bfr->size));
	if (bfr->iova) {
		/* A buffer the co-processor placed at its own address. */
		bfr->is_mapped = true;
	} else {
		bfr->iova = ctx->next_iova;
		ctx->next_iova += SZ_1M;
	}
	return 0;
}

static void rtk_test_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
}

static void rtk_test_crashed(void *cookie, const void *crashlog, size_t size)
{
	struct rtk_test_ctx *ctx = cookie;

	ctx->n_crashed++;
	ctx->crashed_size = size;
	ctx->crashed_fourcc = 0;
	if (crashlog && size >= sizeof(u32))
		memcpy(&ctx->crashed_fourcc, crashlog, sizeof(u32));
}

static bool rtk_test_syslog_is_debug(void *cookie, const char *context,
				     const char *message)
{
	struct rtk_test_ctx *ctx = cookie;

	ctx->n_syslog++;
	strscpy(ctx->syslog_ctx, context, sizeof(ctx->syslog_ctx));
	strscpy(ctx->syslog_msg, message, sizeof(ctx->syslog_msg));
	return true;
}

static void rtk_test_recv(void *cookie, u8 endpoint, u64 message)
{
	struct rtk_test_ctx *ctx = cookie;

	ctx->n_recv++;
	ctx->recv_ep = endpoint;
	ctx->recv_msg = message;
}

static int rtk_test_init(struct kunit *test)
{
	struct rtk_test_ctx *ctx;
	struct apple_rtkit *rtk;
	struct device *dev;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	rtk = kunit_kzalloc(test, sizeof(*rtk), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rtk);
	dev = kunit_device_register(test, "apple-rtkit-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	ctx->test = test;
	ctx->rtk = rtk;
	ctx->next_iova = RTK_TEST_IOVA_BASE;
	ctx->ops.crashed = rtk_test_crashed;
	ctx->ops.recv_message = rtk_test_recv;
	ctx->ops.shmem_setup_ep = rtk_test_shmem_setup;
	ctx->ops.shmem_destroy = rtk_test_shmem_destroy;
	ctx->ops.syslog_is_debug = rtk_test_syslog_is_debug;

	rtk->dev = dev;
	rtk->cookie = ctx;
	rtk->ops = &ctx->ops;
	rtk->handshake_timeout_ms = APPLE_RTKIT_HANDSHAKE_TIMEOUT_MS;
	init_completion(&rtk->epmap_completion);
	init_completion(&rtk->coreanalytics_completion);
	init_completion(&rtk->iop_pwr_ack_completion);
	init_completion(&rtk->ap_pwr_ack_completion);
	init_completion(&rtk->strict_idle_done);
	INIT_KFIFO(rtk->sync_ack_fifo);
	INIT_KFIFO(rtk->m5_saved.deferred);
	INIT_DELAYED_WORK(&rtk->crashlog_late_work, apple_rtkit_crashlog_late);
	rtk->wq = alloc_ordered_workqueue("apple-rtkit-test", 0);
	KUNIT_ASSERT_NOT_NULL(test, rtk->wq);

	kunit_activate_static_stub(test, apple_rtkit_send_message, rtk_test_send);
	test->priv = ctx;
	return 0;
}

static void rtk_test_exit(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	cancel_delayed_work_sync(&rtk->crashlog_late_work);
	destroy_workqueue(rtk->wq);
	kfree(rtk->syslog_msg_buffer);
}

static u64 rtk_test_mgmt(u8 type, u64 payload)
{
	return FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, type) | payload;
}

static u64 rtk_test_hello(unsigned int min_ver, unsigned int max_ver)
{
	return rtk_test_mgmt(APPLE_RTKIT_MGMT_HELLO,
			     FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MINVER, min_ver) |
			     FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MAXVER, max_ver));
}

static u64 rtk_test_epmap(u32 base, u32 bitmap, bool last)
{
	return rtk_test_mgmt(APPLE_RTKIT_MGMT_EPMAP,
			     FIELD_PREP(APPLE_RTKIT_MGMT_EPMAP_BASE, base) |
			     FIELD_PREP(APPLE_RTKIT_MGMT_EPMAP_BITMAP, bitmap) |
			     (last ? APPLE_RTKIT_MGMT_EPMAP_LAST : 0));
}

static u64 rtk_test_startep(u8 ep)
{
	return rtk_test_mgmt(APPLE_RTKIT_MGMT_STARTEP,
			     FIELD_PREP(APPLE_RTKIT_MGMT_STARTEP_EP, ep) |
			     APPLE_RTKIT_MGMT_STARTEP_FLAG);
}

static u64 rtk_test_buffer_request(u8 type, size_t size, u64 iova)
{
	return FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, type) |
	       FIELD_PREP(APPLE_RTKIT_BUFFER_REQUEST_SIZE, size >> 12) |
	       FIELD_PREP(APPLE_RTKIT_BUFFER_REQUEST_IOVA, iova);
}

static void rtk_test_expect_sent(struct kunit *test, unsigned int i, u8 ep, u64 msg)
{
	struct rtk_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_LT(test, i, ctx->n_sent);
	KUNIT_ASSERT_LT(test, i, RTK_TEST_MAX_SENT);
	KUNIT_EXPECT_EQ(test, ctx->sent[i].ep, ep);
	KUNIT_EXPECT_EQ(test, ctx->sent[i].msg, msg);
}

static void rtk_test_hello_accepts_supported(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 reply12 = rtk_test_mgmt(APPLE_RTKIT_MGMT_HELLO_REPLY,
				    FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MINVER, 12) |
				    FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MAXVER, 12));

	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, rtk_test_hello(11, 12), false, 0);
	KUNIT_EXPECT_EQ(test, rtk->version, 12);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_MGMT, reply12);
	KUNIT_EXPECT_FALSE(test, completion_done(&rtk->epmap_completion));

	/* A newer remote maximum settles on the highest version supported here. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, rtk_test_hello(11, 14), false, 0);
	rtk_test_expect_sent(test, 1, APPLE_RTKIT_EP_MGMT, reply12);

	/* Both ends of the supported range are accepted alone. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, rtk_test_hello(12, 12), false, 0);
	rtk_test_expect_sent(test, 2, APPLE_RTKIT_EP_MGMT, reply12);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, rtk_test_hello(1, 11), false, 0);
	KUNIT_EXPECT_EQ(test, rtk->version, 11);
	rtk_test_expect_sent(test, 3, APPLE_RTKIT_EP_MGMT,
			     rtk_test_mgmt(APPLE_RTKIT_MGMT_HELLO_REPLY,
					   FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MINVER, 11) |
					   FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MAXVER, 11)));
	KUNIT_EXPECT_EQ(test, rtk->boot_result, 0);
	KUNIT_EXPECT_FALSE(test, completion_done(&rtk->epmap_completion));
}

static void rtk_test_hello_rejects_unsupported(struct kunit *test)
{
	static const struct { unsigned int min, max; } bad[] = {
		{ 13, 14 },	/* minimum too new */
		{ 9, 10 },	/* maximum too old */
	};
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	int i;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		reinit_completion(&rtk->epmap_completion);
		rtk->boot_result = 0;
		rtk->version = 0;
		apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
					rtk_test_hello(bad[i].min, bad[i].max), false, 0);
		KUNIT_EXPECT_EQ_MSG(test, rtk->boot_result, -EINVAL, "range %u..%u",
				    bad[i].min, bad[i].max);
		KUNIT_EXPECT_TRUE(test, completion_done(&rtk->epmap_completion));
		KUNIT_EXPECT_EQ(test, rtk->version, 0);
	}
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 0);
}

static void rtk_test_hello_client_version(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	ctx->ops.protocol_version = 11;
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, rtk_test_hello(11, 12), false, 0);
	KUNIT_EXPECT_EQ(test, rtk->version, 11);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_MGMT,
			     rtk_test_mgmt(APPLE_RTKIT_MGMT_HELLO_REPLY,
					   FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MINVER, 11) |
					   FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MAXVER, 11)));

	/* The client's exact version outside the remote range aborts the boot. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, rtk_test_hello(12, 12), false, 0);
	KUNIT_EXPECT_EQ(test, rtk->boot_result, -EINVAL);
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->epmap_completion));
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 1);
}

static void rtk_test_epmap_starts_system_endpoints(struct kunit *test)
{
	static const u8 started[] = { 1, 2, 3, 4, 8, 0xa };
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	/* mgmt, crashlog, syslog, debug, ioreport, 5 (unknown), 7, oslog, tracekit */
	u32 bitmap = BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) |
		     BIT(7) | BIT(8) | BIT(0xa);
	int i;

	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, rtk_test_epmap(0, bitmap, true),
				false, 0);

	for (i = 0; i < 32; i++)
		KUNIT_EXPECT_EQ_MSG(test, apple_rtkit_has_endpoint(rtk, i),
				    !!(bitmap & BIT(i)), "endpoint %d", i);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1 + ARRAY_SIZE(started));
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_MGMT,
			     rtk_test_mgmt(APPLE_RTKIT_MGMT_EPMAP_REPLY,
					   APPLE_RTKIT_MGMT_EPMAP_LAST));
	for (i = 0; i < ARRAY_SIZE(started); i++) {
		rtk_test_expect_sent(test, 1 + i, APPLE_RTKIT_EP_MGMT,
				     rtk_test_startep(started[i]));
		KUNIT_EXPECT_TRUE(test, apple_rtkit_endpoint_started(rtk, started[i]));
	}
	/* Neither management, an unknown system endpoint nor CoreAnalytics. */
	KUNIT_EXPECT_FALSE(test, apple_rtkit_endpoint_started(rtk, 0));
	KUNIT_EXPECT_FALSE(test, apple_rtkit_endpoint_started(rtk, 5));
	KUNIT_EXPECT_FALSE(test, apple_rtkit_endpoint_started(rtk, 7));
	KUNIT_EXPECT_EQ(test, rtk->boot_result, 0);
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->epmap_completion));
}

static void rtk_test_epmap_multipart(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
				rtk_test_epmap(0, BIT(0) | BIT(2), false), false, 0);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_MGMT,
			     rtk_test_mgmt(APPLE_RTKIT_MGMT_EPMAP_REPLY,
					   APPLE_RTKIT_MGMT_EPMAP_REPLY_MORE));
	KUNIT_EXPECT_FALSE(test, completion_done(&rtk->epmap_completion));
	KUNIT_EXPECT_FALSE(test, apple_rtkit_endpoint_started(rtk, 2));

	/* The second part: endpoints 0x20 and 0x37 are application endpoints. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
				rtk_test_epmap(1, BIT(0) | BIT(0x17), true), false, 0);
	KUNIT_EXPECT_TRUE(test, apple_rtkit_has_endpoint(rtk, 0x20));
	KUNIT_EXPECT_TRUE(test, apple_rtkit_has_endpoint(rtk, 0x37));
	KUNIT_EXPECT_FALSE(test, apple_rtkit_has_endpoint(rtk, 1));
	KUNIT_EXPECT_FALSE(test, apple_rtkit_has_endpoint(rtk, 0x17));
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 3);
	rtk_test_expect_sent(test, 1, APPLE_RTKIT_EP_MGMT,
			     rtk_test_mgmt(APPLE_RTKIT_MGMT_EPMAP_REPLY,
					   FIELD_PREP(APPLE_RTKIT_MGMT_EPMAP_BASE, 1) |
					   APPLE_RTKIT_MGMT_EPMAP_LAST));
	/* Only system endpoints are started by RTKit itself. */
	rtk_test_expect_sent(test, 2, APPLE_RTKIT_EP_MGMT, rtk_test_startep(2));
	KUNIT_EXPECT_FALSE(test, apple_rtkit_endpoint_started(rtk, 0x20));
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->epmap_completion));
}

static void rtk_test_epmap_flags(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	ctx->ops.flags = APPLE_RTKIT_NEEDS_COREANALYTICS | APPLE_RTKIT_EARLY_CRASHLOG;
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
				rtk_test_epmap(0, BIT(0) | BIT(1) | BIT(7), true), false, 0);

	/* The crash log is started before the map is acknowledged, and only once. */
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 3);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_MGMT, rtk_test_startep(1));
	rtk_test_expect_sent(test, 1, APPLE_RTKIT_EP_MGMT,
			     rtk_test_mgmt(APPLE_RTKIT_MGMT_EPMAP_REPLY,
					   APPLE_RTKIT_MGMT_EPMAP_LAST));
	rtk_test_expect_sent(test, 2, APPLE_RTKIT_EP_MGMT, rtk_test_startep(7));
	KUNIT_EXPECT_TRUE(test, apple_rtkit_endpoint_started(rtk, 7));
}

static void rtk_test_epmap_retained(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	rtk->retained_wake = true;
	set_bit(1, rtk->started);
	set_bit(2, rtk->started);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
				rtk_test_epmap(0, BIT(0) | BIT(1) | BIT(2) | BIT(4), true),
				false, 0);
	/* The started endpoints are echoed and nothing is started again. */
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_MGMT,
			     rtk_test_mgmt(APPLE_RTKIT_MGMT_EPMAP_REPLY,
					   BIT(1) | BIT(2) | APPLE_RTKIT_MGMT_EPMAP_LAST));
	KUNIT_EXPECT_EQ(test, rtk->boot_result, 0);
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->epmap_completion));

	/* A started endpoint missing from the map fails the wake. */
	reinit_completion(&rtk->epmap_completion);
	set_bit(4 + 32, rtk->started);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
				rtk_test_epmap(1, BIT(0), true), false, 0);
	KUNIT_EXPECT_EQ(test, rtk->boot_result, -EPROTO);
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->epmap_completion));
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 1);
}

static void rtk_test_power_acks(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	KUNIT_EXPECT_FALSE(test, apple_rtkit_is_running(rtk));
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
				rtk_test_mgmt(APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK,
					      APPLE_RTKIT_PWR_STATE_ON), false, 0);
	KUNIT_EXPECT_EQ(test, rtk->iop_power_state, APPLE_RTKIT_PWR_STATE_ON);
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->iop_pwr_ack_completion));
	KUNIT_EXPECT_FALSE(test, completion_done(&rtk->ap_pwr_ack_completion));
	KUNIT_EXPECT_FALSE(test, apple_rtkit_is_running(rtk));

	/* Bits above the 16-bit state field are not part of the state. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
				rtk_test_mgmt(APPLE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK,
					      BIT_ULL(40) | APPLE_RTKIT_PWR_STATE_ON),
				false, 0);
	KUNIT_EXPECT_EQ(test, rtk->ap_power_state, APPLE_RTKIT_PWR_STATE_ON);
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->ap_pwr_ack_completion));
	KUNIT_EXPECT_TRUE(test, apple_rtkit_is_running(rtk));

	rtk->crashed = true;
	KUNIT_EXPECT_FALSE(test, apple_rtkit_is_running(rtk));
	KUNIT_EXPECT_TRUE(test, apple_rtkit_is_crashed(rtk));
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 0);
}

static void rtk_test_retained_quiesced(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 quiesced = rtk_test_mgmt(APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK,
				     APPLE_RTKIT_PWR_STATE_QUIESCED);

	rtk->retained_wake = true;
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, quiesced, false, 0);
	/* One ON request, and still waiting for ON. */
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_MGMT,
			     rtk_test_mgmt(APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE,
					   APPLE_RTKIT_PWR_STATE_ON));
	KUNIT_EXPECT_FALSE(test, completion_done(&rtk->iop_pwr_ack_completion));

	/* A second QUIESCED is not asked again: it fails the wake. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, quiesced, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 1);
	KUNIT_EXPECT_EQ(test, rtk->boot_result, -EPROTO);
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->iop_pwr_ack_completion));
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->epmap_completion));
}

static void rtk_test_idle_notify(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	rtk->strict_idle_waiting = true;
	rtk->iop_power_state = APPLE_RTKIT_PWR_STATE_IDLE;
	rtk->ap_power_state = APPLE_RTKIT_PWR_STATE_IDLE;
	/* A nonzero argument is not the idle notification. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
				rtk_test_mgmt(APPLE_RTKIT_MGMT_IDLE_NOTIFY, 1), false, 0);
	KUNIT_EXPECT_FALSE(test, completion_done(&rtk->strict_idle_done));
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT,
				rtk_test_mgmt(APPLE_RTKIT_MGMT_IDLE_NOTIFY, 0), false, 0);
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->strict_idle_done));

	/* Unknown management messages are dropped without a reply. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_MGMT, rtk_test_mgmt(0x3f, 0), false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 0);
}

static void rtk_test_send_checks(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	set_bit(0x20, rtk->endpoints);
	/* Application endpoints need a running co-processor. */
	KUNIT_EXPECT_EQ(test, apple_rtkit_send_message(rtk, 0x20, 0x1234, NULL, false),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, apple_rtkit_start_ep(rtk, 0x20), -EINVAL);
	/* An undiscovered endpoint cannot be started. */
	KUNIT_EXPECT_EQ(test, apple_rtkit_start_ep(rtk, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 0);

	rtk->iop_power_state = APPLE_RTKIT_PWR_STATE_ON;
	rtk->ap_power_state = APPLE_RTKIT_PWR_STATE_ON;
	KUNIT_EXPECT_EQ(test, apple_rtkit_send_message(rtk, 0x20, 0x1234, NULL, false), 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_start_ep(rtk, 0x20), 0);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 2);
	rtk_test_expect_sent(test, 0, 0x20, 0x1234);
	rtk_test_expect_sent(test, 1, APPLE_RTKIT_EP_MGMT, rtk_test_startep(0x20));
	KUNIT_EXPECT_TRUE(test, apple_rtkit_endpoint_started(rtk, 0x20));

	/* Nothing is sent to a crashed co-processor, not even management. */
	rtk->crashed = true;
	KUNIT_EXPECT_EQ(test, apple_rtkit_send_message(rtk, 0, 0, NULL, false), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 2);
}

static void rtk_test_app_endpoint_dispatch(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	apple_rtkit_rx_dispatch(rtk, 0x37, 0xdeadbeefcafeull, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_recv, 1);
	KUNIT_EXPECT_EQ(test, ctx->recv_ep, 0x37);
	KUNIT_EXPECT_EQ(test, ctx->recv_msg, 0xdeadbeefcafeull);

	/* Nothing is delivered once teardown has started. */
	rtk->shutting_down = true;
	apple_rtkit_rx_dispatch(rtk, 0x37, 1, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_recv, 1);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 0);
}

static void rtk_test_buffer_reply(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 req = rtk_test_buffer_request(APPLE_RTKIT_BUFFER_REQUEST, SZ_8K, 0);

	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_IOREPORT, req, false, 0);
	KUNIT_ASSERT_EQ(test, ctx->n_setup, 1);
	KUNIT_EXPECT_EQ(test, ctx->setup_ep, APPLE_RTKIT_EP_IOREPORT);
	KUNIT_EXPECT_EQ(test, ctx->setup_req.size, SZ_8K);
	KUNIT_EXPECT_EQ(test, ctx->setup_req.iova, 0);
	KUNIT_EXPECT_EQ(test, rtk->ioreport_buffer.size, SZ_8K);
	KUNIT_EXPECT_EQ(test, rtk->ioreport_buffer.iova, RTK_TEST_IOVA_BASE);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_IOREPORT,
			     rtk_test_buffer_request(APPLE_RTKIT_BUFFER_REQUEST, SZ_8K,
						     RTK_TEST_IOVA_BASE));

	/* An identical request is answered again without a new buffer. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_IOREPORT, req, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_setup, 1);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 2);
	KUNIT_EXPECT_EQ(test, ctx->sent[1].msg, ctx->sent[0].msg);

	/* A changed geometry is refused while the buffer is live. */
	req = rtk_test_buffer_request(1, SZ_16K, 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_common_rx_get_buffer(rtk, &rtk->ioreport_buffer,
							       APPLE_RTKIT_EP_IOREPORT, req),
			-EBUSY);
	KUNIT_EXPECT_EQ(test, rtk->ioreport_buffer.size, SZ_8K);
	KUNIT_EXPECT_EQ(test, ctx->n_setup, 1);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 2);

	/* ioreport's two unknown notifications are echoed, others dropped. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_IOREPORT,
				FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, 0x8) | 0x55, false, 0);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_IOREPORT,
				FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, 0xc), false, 0);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_IOREPORT,
				FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, 0x9), false, 0);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 4);
	rtk_test_expect_sent(test, 2, APPLE_RTKIT_EP_IOREPORT,
			     FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, 0x8) | 0x55);
	rtk_test_expect_sent(test, 3, APPLE_RTKIT_EP_IOREPORT,
			     FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, 0xc));
}

static void rtk_test_buffer_request_invalid(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 req;

	/* A zero size is never set up. */
	req = rtk_test_buffer_request(1, 0, 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_common_rx_get_buffer(rtk, &rtk->ioreport_buffer,
							       APPLE_RTKIT_EP_IOREPORT, req),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->n_setup, 0);

	/* A failed setup leaves no buffer behind and sends no reply. */
	ctx->setup_ret = -ENOMEM;
	req = rtk_test_buffer_request(1, SZ_4K, 0);
	KUNIT_EXPECT_EQ(test, apple_rtkit_common_rx_get_buffer(rtk, &rtk->syslog_buffer,
							       APPLE_RTKIT_EP_SYSLOG, req),
			-ENOMEM);
	KUNIT_EXPECT_EQ(test, rtk->syslog_buffer.size, 0);
	ctx->setup_ret = 0;

	/* Without a setup callback, a placed buffer cannot be mapped. */
	ctx->ops.shmem_setup_ep = NULL;
	req = rtk_test_buffer_request(1, SZ_4K, 0x8000);
	KUNIT_EXPECT_EQ(test, apple_rtkit_common_rx_get_buffer(rtk, &rtk->ioreport_buffer,
							       APPLE_RTKIT_EP_IOREPORT, req),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, rtk->ioreport_buffer.size, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 0);
}

static void rtk_test_placed_buffers(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 req = rtk_test_buffer_request(1, SZ_16K, 0xabc000);

	/* A placed buffer is acknowledged with its request, unchanged. */
	ctx->ops.flags = APPLE_RTKIT_COPROC_PLACES_BUFFERS;
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_IOREPORT, req, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->setup_req.iova, 0xabc000);
	KUNIT_EXPECT_EQ(test, ctx->setup_req.size, SZ_16K);
	KUNIT_EXPECT_TRUE(test, rtk->ioreport_buffer.is_mapped);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_IOREPORT, req);

	/* A firmware-owned log buffer is taken as it is, unmapped and unanswered. */
	ctx->ops.flags = APPLE_RTKIT_FIRMWARE_OWNED_LOGS;
	req = rtk_test_buffer_request(1, SZ_8K, 0xdef000);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG, req, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_setup, 1);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 1);
	KUNIT_EXPECT_TRUE(test, rtk->syslog_buffer.is_mapped);
	KUNIT_EXPECT_NULL(test, rtk->syslog_buffer.buffer);
	KUNIT_EXPECT_EQ(test, rtk->syslog_buffer.iova, 0xdef000);
	KUNIT_EXPECT_EQ(test, rtk->syslog_buffer.size, SZ_8K);

	/* Its records are still acknowledged, without being read. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG,
				FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_SYSLOG_LOG),
				false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 2);
	KUNIT_EXPECT_EQ(test, ctx->n_syslog, 0);
}

static void rtk_test_oslog_buffer_request(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 req = FIELD_PREP(APPLE_RTKIT_OSLOG_TYPE, APPLE_RTKIT_OSLOG_BUFFER_REQUEST) |
		  FIELD_PREP(APPLE_RTKIT_OSLOG_SIZE, SZ_16K);

	/* oslog carries its size unshifted and its IOVA in 4 KiB pages. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_OSLOG, req, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->setup_ep, APPLE_RTKIT_EP_OSLOG);
	KUNIT_EXPECT_EQ(test, ctx->setup_req.size, SZ_16K);
	KUNIT_EXPECT_EQ(test, ctx->setup_req.iova, 0);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_OSLOG,
			     FIELD_PREP(APPLE_RTKIT_OSLOG_TYPE, APPLE_RTKIT_OSLOG_BUFFER_REQUEST) |
			     FIELD_PREP(APPLE_RTKIT_OSLOG_SIZE, SZ_16K) |
			     FIELD_PREP(APPLE_RTKIT_OSLOG_IOVA, RTK_TEST_IOVA_BASE >> 12));

	apple_rtkit_free_buffer(rtk, &rtk->oslog_buffer);
	req = FIELD_PREP(APPLE_RTKIT_OSLOG_TYPE, APPLE_RTKIT_OSLOG_BUFFER_REQUEST) |
	      FIELD_PREP(APPLE_RTKIT_OSLOG_SIZE, SZ_8K) |
	      FIELD_PREP(APPLE_RTKIT_OSLOG_IOVA, 0x12345);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_OSLOG, req, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->setup_req.size, SZ_8K);
	KUNIT_EXPECT_EQ(test, ctx->setup_req.iova, 0x12345000);
	KUNIT_EXPECT_EQ(test, rtk->oslog_buffer.iova, 0x12345000);
}

/* Builds a syslog ring of four entries with a 0x40-byte message each. */
static void rtk_test_syslog_setup(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	size_t entry_size = 0x20 + 0x40;
	u8 *entry;

	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG,
				rtk_test_buffer_request(1, SZ_8K, 0), false, 0);
	KUNIT_ASSERT_NOT_NULL(test, rtk->syslog_buffer.buffer);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG,
				FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_SYSLOG_INIT) |
				FIELD_PREP(APPLE_RTKIT_SYSLOG_N_ENTRIES, 4) |
				FIELD_PREP(APPLE_RTKIT_SYSLOG_MSG_SIZE, 0x40), false, 0);
	KUNIT_ASSERT_NOT_NULL(test, rtk->syslog_msg_buffer);
	KUNIT_ASSERT_EQ(test, rtk->syslog_n_entries, 4);
	KUNIT_ASSERT_EQ(test, rtk->syslog_msg_size, 0x40);

	entry = rtk->syslog_buffer.buffer + 1 * entry_size;
	strscpy(entry + 8, "ctx", 24);
	strscpy(entry + 8 + 24, "hello rtkit \r\n", 0x40);
	/* Entry 4 is past the ring the firmware described, but inside the buffer. */
	entry = rtk->syslog_buffer.buffer + 4 * entry_size;
	strscpy(entry + 8, "stale", 24);
	strscpy(entry + 8 + 24, "not a record", 0x40);
	ctx->n_sent = 0;
}

static void rtk_test_syslog_log(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 log1 = FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_SYSLOG_LOG) | 1;

	rtk_test_syslog_setup(test);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG, log1, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_syslog, 1);
	KUNIT_EXPECT_STREQ(test, ctx->syslog_ctx, "ctx");
	/* Trailing whitespace is cropped. */
	KUNIT_EXPECT_STREQ(test, ctx->syslog_msg, "hello rtkit");
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_SYSLOG, log1);
}

static void rtk_test_syslog_index_range(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 log4 = FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_SYSLOG_LOG) | 4;
	u64 log_ff = FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_SYSLOG_LOG) | 0xff;

	rtk_test_syslog_setup(test);
	/* Records past the ring are not read, but still acknowledged. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG, log4, false, 0);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG, log_ff, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_syslog, 0);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 2);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_SYSLOG, log4);
	rtk_test_expect_sent(test, 1, APPLE_RTKIT_EP_SYSLOG, log_ff);
}

static void rtk_test_syslog_no_geometry(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 log0 = FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_SYSLOG_LOG);

	rtk_test_syslog_setup(test);
	/* A zero-sized INIT drops the message buffer rather than kzalloc(0). */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG,
				FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_SYSLOG_INIT) |
				FIELD_PREP(APPLE_RTKIT_SYSLOG_N_ENTRIES, 4), false, 0);
	KUNIT_EXPECT_NULL(test, rtk->syslog_msg_buffer);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG, log0, false, 0);

	/* An inherited session cannot read the ring either. */
	rtk->syslog_inherited = true;
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_SYSLOG, log0, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_syslog, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 2);
}

static void rtk_test_kdebug(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 get = FIELD_PREP(APPLE_RTKIT_KDEBUG_TYPE, APPLE_RTKIT_KDEBUG_GET_BUFFER);

	rtk->sync_syslog_ack = true;
	/* No entries, and more than the cap, are refused before any setup. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_DEBUG, get, false, 0);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_DEBUG,
				get | FIELD_PREP(APPLE_RTKIT_KDEBUG_DATA,
						 APPLE_RTKIT_KDEBUG_BUF0_MAX /
						 APPLE_RTKIT_KDEBUG_ENTRY_SIZE + 1), false, 0);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_DEBUG,
				get | APPLE_RTKIT_KDEBUG_DATA, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_setup, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 0);

	/* 16 entries round up to one aligned buffer, plus the fixed second one. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_DEBUG,
				get | FIELD_PREP(APPLE_RTKIT_KDEBUG_DATA, 16), false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_setup, 2);
	KUNIT_EXPECT_EQ(test, rtk->kdebug_buffer0.size, APPLE_RTKIT_KDEBUG_BUF_ALIGN);
	KUNIT_EXPECT_EQ(test, rtk->kdebug_buffer1.size, APPLE_RTKIT_KDEBUG_BUF1_SIZE);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 2);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_DEBUG,
			     FIELD_PREP(APPLE_RTKIT_KDEBUG_TYPE, APPLE_RTKIT_KDEBUG_SET_BUFFER0) |
			     RTK_TEST_IOVA_BASE);
	rtk_test_expect_sent(test, 1, APPLE_RTKIT_EP_DEBUG,
			     FIELD_PREP(APPLE_RTKIT_KDEBUG_TYPE, APPLE_RTKIT_KDEBUG_SET_BUFFER1) |
			     (RTK_TEST_IOVA_BASE + SZ_1M));

	/* A repeated request of the same size reuses both buffers. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_DEBUG,
				get | FIELD_PREP(APPLE_RTKIT_KDEBUG_DATA, 16), false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_setup, 2);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 4);
}

static void rtk_test_coreanalytics_encoding(struct kunit *test)
{
	u64 reply;

	KUNIT_EXPECT_EQ(test, apple_rtkit_coreanalytics_size(APPLE_RTKIT_CA_UNIT_4K |
							     FIELD_PREP(APPLE_RTKIT_CA_COUNT, 3)),
			0x3000);
	KUNIT_EXPECT_EQ(test, apple_rtkit_coreanalytics_size(APPLE_RTKIT_CA_UNIT_1M |
							     FIELD_PREP(APPLE_RTKIT_CA_COUNT, 2)),
			0x200000);
	/* Without a unit there is no size. */
	KUNIT_EXPECT_EQ(test, apple_rtkit_coreanalytics_size(FIELD_PREP(APPLE_RTKIT_CA_COUNT, 2)),
			0);

	KUNIT_ASSERT_EQ(test, apple_rtkit_coreanalytics_reply(0x3000, 0x40000, &reply), 0);
	KUNIT_EXPECT_EQ(test, reply,
			FIELD_PREP(APPLE_RTKIT_CA_TYPE, APPLE_RTKIT_CA_BUFFER_REPLY) |
			FIELD_PREP(APPLE_RTKIT_CA_COUNT, 3) | APPLE_RTKIT_CA_UNIT_4K | 0x40000);
	/* A partial unit rounds up; a megabyte or more counts in megabytes. */
	KUNIT_ASSERT_EQ(test, apple_rtkit_coreanalytics_reply(0x2001, 0, &reply), 0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_RTKIT_CA_COUNT, reply), 3);
	KUNIT_ASSERT_EQ(test, apple_rtkit_coreanalytics_reply(0x180000, 0, &reply), 0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(APPLE_RTKIT_CA_COUNT, reply), 2);
	KUNIT_EXPECT_TRUE(test, reply & APPLE_RTKIT_CA_UNIT_1M);
	KUNIT_EXPECT_FALSE(test, reply & APPLE_RTKIT_CA_UNIT_4K);

	KUNIT_EXPECT_EQ(test, apple_rtkit_coreanalytics_reply(0, 0, &reply), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_rtkit_coreanalytics_reply(0x1000, BIT_ULL(44), &reply),
			-EINVAL);
	/* 256 MiB needs a count of 256, which the 8-bit field cannot hold. */
	KUNIT_EXPECT_EQ(test, apple_rtkit_coreanalytics_reply(SZ_256M, 0, &reply), -EINVAL);
}

static void rtk_test_coreanalytics_rx(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 req = FIELD_PREP(APPLE_RTKIT_CA_TYPE, APPLE_RTKIT_CA_BUFFER_REQUEST) |
		  APPLE_RTKIT_CA_UNIT_4K | FIELD_PREP(APPLE_RTKIT_CA_COUNT, 4);
	u64 event = FIELD_PREP(APPLE_RTKIT_CA_TYPE, APPLE_RTKIT_CA_EVENT) |
		    FIELD_PREP(APPLE_RTKIT_CA_EVENT_ID, 0x1234) | 0xff;

	/* A client that asks for CoreAnalytics, as the PMP does. */
	ctx->ops.flags = APPLE_RTKIT_NEEDS_COREANALYTICS;
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_COREANALYTICS, req, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->setup_ep, APPLE_RTKIT_EP_COREANALYTICS);
	KUNIT_EXPECT_EQ(test, rtk->coreanalytics_buffer.size, 0x4000);
	KUNIT_EXPECT_TRUE(test, completion_done(&rtk->coreanalytics_completion));
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_COREANALYTICS,
			     FIELD_PREP(APPLE_RTKIT_CA_TYPE, APPLE_RTKIT_CA_BUFFER_REPLY) |
			     APPLE_RTKIT_CA_UNIT_4K | FIELD_PREP(APPLE_RTKIT_CA_COUNT, 4) |
			     RTK_TEST_IOVA_BASE);

	/* A duplicate request never replaces the live buffer. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_COREANALYTICS, req, false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_setup, 1);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 1);

	/* An event is acknowledged with its ID only. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_COREANALYTICS, event, false, 0);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 2);
	rtk_test_expect_sent(test, 1, APPLE_RTKIT_EP_COREANALYTICS,
			     FIELD_PREP(APPLE_RTKIT_CA_TYPE, APPLE_RTKIT_CA_EVENT_ACK) |
			     FIELD_PREP(APPLE_RTKIT_CA_EVENT_ID, 0x1234));
}

static void rtk_test_crashlog_handshake_then_crash(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	u64 req = rtk_test_buffer_request(APPLE_RTKIT_CRASHLOG_CRASH, SZ_4K, 0);
	struct apple_rtkit_crashlog_header *hdr;

	set_bit(APPLE_RTKIT_EP_CRASHLOG, rtk->endpoints);
	/* In a handshaked session the first message is the buffer request. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_CRASHLOG, req, false, 0);
	KUNIT_EXPECT_FALSE(test, rtk->crashed);
	KUNIT_EXPECT_EQ(test, ctx->n_crashed, 0);
	KUNIT_EXPECT_EQ(test, rtk->crashlog_buffer.size, SZ_4K);
	KUNIT_ASSERT_EQ(test, ctx->n_sent, 1);
	rtk_test_expect_sent(test, 0, APPLE_RTKIT_EP_CRASHLOG,
			     rtk_test_buffer_request(APPLE_RTKIT_BUFFER_REQUEST, SZ_4K,
						     RTK_TEST_IOVA_BASE));

	/* The firmware writes a log, then reports the crash. */
	hdr = rtk->crashlog_buffer.buffer;
	hdr->fourcc = APPLE_RTKIT_CRASHLOG_HEADER_FOURCC;
	hdr->size = sizeof(*hdr);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_CRASHLOG, req, false, 0);
	KUNIT_EXPECT_TRUE(test, rtk->crashed);
	KUNIT_EXPECT_EQ(test, ctx->n_crashed, 1);
	KUNIT_EXPECT_EQ(test, ctx->crashed_size, SZ_4K);
	KUNIT_EXPECT_EQ(test, ctx->crashed_fourcc, APPLE_RTKIT_CRASHLOG_HEADER_FOURCC);

	/* Unknown crashlog messages are ignored; a crashed IOP gets no messages. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_CRASHLOG,
				FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, 2), false, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_crashed, 1);
	KUNIT_EXPECT_EQ(test, apple_rtkit_send_message(rtk, APPLE_RTKIT_EP_MGMT, 0, NULL, false),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 1);
}

static void rtk_test_crashlog_report_only(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	struct apple_rtkit_crashlog_header log = {
		.fourcc = APPLE_RTKIT_CRASHLOG_HEADER_FOURCC,
		.size = sizeof(log),
	};

	/*
	 * An undiscovered crashlog endpoint only reports crashes; the client's
	 * setup maps the buffer, which already holds the log.
	 */
	ctx->prefill = &log;
	ctx->prefill_len = sizeof(log);
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_CRASHLOG,
				rtk_test_buffer_request(1, SZ_4K, 0x7000), false, 0);
	KUNIT_EXPECT_TRUE(test, rtk->crashed);
	KUNIT_EXPECT_EQ(test, ctx->n_crashed, 1);
	KUNIT_EXPECT_EQ(test, ctx->crashed_size, SZ_4K);
	KUNIT_EXPECT_EQ(test, ctx->crashed_fourcc, APPLE_RTKIT_CRASHLOG_HEADER_FOURCC);
}

static void rtk_test_crashlog_unreadable(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	/* A buffer holding no log yet: the crash is reported without one. */
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_CRASHLOG,
				rtk_test_buffer_request(1, SZ_4K, 0x7000), false, 0);
	KUNIT_EXPECT_TRUE(test, rtk->crashed);
	KUNIT_EXPECT_EQ(test, ctx->n_crashed, 1);
	KUNIT_EXPECT_EQ(test, ctx->crashed_size, 0);
	/* ...and the log is looked for again later. */
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&rtk->crashlog_late_work));
	cancel_delayed_work_sync(&rtk->crashlog_late_work);
}

static void rtk_test_crashlog_no_mapping(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;

	/* No setup callback and no IOMMU domain: the crash is still reported. */
	ctx->ops.shmem_setup_ep = NULL;
	apple_rtkit_rx_dispatch(rtk, APPLE_RTKIT_EP_CRASHLOG,
				rtk_test_buffer_request(1, SZ_4K, 0x7000), false, 0);
	KUNIT_EXPECT_TRUE(test, rtk->crashed);
	KUNIT_EXPECT_EQ(test, ctx->n_crashed, 1);
	KUNIT_EXPECT_EQ(test, ctx->crashed_size, 0);
	KUNIT_EXPECT_EQ(test, rtk->crashlog_buffer.size, 0);
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 0);
}

static void rtk_test_crashlog_header_valid(struct kunit *test)
{
	struct rtk_test_ctx *ctx = test->priv;
	struct apple_rtkit *rtk = ctx->rtk;
	struct apple_rtkit_crashlog_header *hdr;
	u32 *words;

	rtk->crashlog_buffer.size = 0x100;
	rtk->crashlog_buffer.buffer = kunit_kzalloc(test, 0x100, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rtk->crashlog_buffer.buffer);
	hdr = rtk->crashlog_buffer.buffer;

	KUNIT_EXPECT_FALSE(test, apple_rtkit_crashlog_header_valid(rtk));
	hdr->fourcc = APPLE_RTKIT_CRASHLOG_HEADER_FOURCC;
	hdr->size = 0x100;
	KUNIT_EXPECT_TRUE(test, apple_rtkit_crashlog_header_valid(rtk));
	hdr->size = 0x101;
	KUNIT_EXPECT_FALSE(test, apple_rtkit_crashlog_header_valid(rtk));
	hdr->size = sizeof(*hdr) - 1;
	KUNIT_EXPECT_FALSE(test, apple_rtkit_crashlog_header_valid(rtk));

	/* A container is valid when its current record leads to a log. */
	words = rtk->crashlog_buffer.buffer;
	memset(words, 0, 0x100);
	words[0] = APPLE_RTKIT_CRASHLOG_CONTAINER_FOURCC;
	words[1] = 0x200;	/* version 2 */
	words[2] = 0x100;	/* size */
	words[3] = 0x41;	/* current id */
	words[4] = 1;		/* one record */
	/* record 0 at 0x14: id (u64), unknown (u64), offset (u32) */
	words[5] = 0x41;
	words[9] = 0x80;
	hdr = rtk->crashlog_buffer.buffer + 0x80;
	hdr->fourcc = APPLE_RTKIT_CRASHLOG_HEADER_FOURCC;
	hdr->size = 0x80;
	KUNIT_EXPECT_TRUE(test, apple_rtkit_crashlog_header_valid(rtk));
	words[3] = 0x42;
	KUNIT_EXPECT_FALSE(test, apple_rtkit_crashlog_header_valid(rtk));

	/* Too small for a header at all. */
	rtk->crashlog_buffer.size = sizeof(*hdr) - 1;
	KUNIT_EXPECT_FALSE(test, apple_rtkit_crashlog_header_valid(rtk));
	KUNIT_EXPECT_EQ(test, ctx->n_sent, 0);
}

static struct kunit_case apple_rtkit_test_cases[] = {
	KUNIT_CASE(rtk_test_hello_accepts_supported),
	KUNIT_CASE(rtk_test_hello_rejects_unsupported),
	KUNIT_CASE(rtk_test_hello_client_version),
	KUNIT_CASE(rtk_test_epmap_starts_system_endpoints),
	KUNIT_CASE(rtk_test_epmap_multipart),
	KUNIT_CASE(rtk_test_epmap_flags),
	KUNIT_CASE(rtk_test_epmap_retained),
	KUNIT_CASE(rtk_test_power_acks),
	KUNIT_CASE(rtk_test_retained_quiesced),
	KUNIT_CASE(rtk_test_idle_notify),
	KUNIT_CASE(rtk_test_send_checks),
	KUNIT_CASE(rtk_test_app_endpoint_dispatch),
	KUNIT_CASE(rtk_test_buffer_reply),
	KUNIT_CASE(rtk_test_buffer_request_invalid),
	KUNIT_CASE(rtk_test_placed_buffers),
	KUNIT_CASE(rtk_test_oslog_buffer_request),
	KUNIT_CASE(rtk_test_syslog_log),
	KUNIT_CASE(rtk_test_syslog_index_range),
	KUNIT_CASE(rtk_test_syslog_no_geometry),
	KUNIT_CASE(rtk_test_kdebug),
	KUNIT_CASE(rtk_test_coreanalytics_encoding),
	KUNIT_CASE(rtk_test_coreanalytics_rx),
	KUNIT_CASE(rtk_test_crashlog_handshake_then_crash),
	KUNIT_CASE(rtk_test_crashlog_report_only),
	KUNIT_CASE(rtk_test_crashlog_unreadable),
	KUNIT_CASE(rtk_test_crashlog_no_mapping),
	KUNIT_CASE(rtk_test_crashlog_header_valid),
	{}
};

static struct kunit_suite apple_rtkit_test_suite = {
	.name = "apple-rtkit",
	.init = rtk_test_init,
	.exit = rtk_test_exit,
	.test_cases = apple_rtkit_test_cases,
};

kunit_test_suite(apple_rtkit_test_suite);

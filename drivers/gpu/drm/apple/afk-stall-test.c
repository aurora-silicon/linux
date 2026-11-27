// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by afk.c: a legacy EPIC endpoint whose firmware stops answering,
 * through the real send, timeout and reply paths. The rings are host memory
 * and the doorbell is a stub, so nothing reads the queued commands.
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/dma-mapping.h>

#include "dptxep.h"

#define AFK_STALL_STRIDE	64
/* The ring size firmware 13.5 gives its DPTX endpoint: 16 KiB less three strides. */
#define AFK_STALL_RING		(SZ_16K - 3 * AFK_STALL_STRIDE)
/* Timeouts of a single jiffy: nothing answers in time anyway. */
#define AFK_STALL_TIMEOUT_MS	1

struct afk_stall_test {
	struct apple_dcp_afkep ep;
	struct apple_dcp dcp;
	unsigned int doorbells;
	int doorbell_ret;
};

static const struct apple_epic_service_ops afk_stall_ops = {
	.name = "dcpdptx-port-epic",
};

static int afk_stall_doorbell(struct apple_dcp_afkep *ep, u64 message)
{
	struct afk_stall_test *t = container_of(ep, struct afk_stall_test, ep);

	t->doorbells++;
	return t->doorbell_ret;
}

static int afk_stall_ring(struct kunit *test, struct afk_ringbuffer *bfr)
{
	u8 *hdr = kunit_kzalloc(test, 3 * AFK_STALL_STRIDE + AFK_STALL_RING, GFP_KERNEL);

	if (!hdr)
		return -ENOMEM;
	bfr->rptr = (__le32 *)(hdr + AFK_STALL_STRIDE);
	bfr->wptr = (__le32 *)(hdr + 2 * AFK_STALL_STRIDE);
	bfr->buf = hdr + 3 * AFK_STALL_STRIDE;
	bfr->bufsz = AFK_STALL_RING;
	bfr->stride = AFK_STALL_STRIDE;
	bfr->ready = true;
	return 0;
}

static int afk_stall_test_init(struct kunit *test)
{
	struct apple_epic_service *service;
	struct afk_stall_test *t;
	struct device *dev;
	int ret;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	dev = kunit_device_register(test, "afk-stall");
	if (IS_ERR(dev))
		return PTR_ERR(dev);
	/* The command buffers are coherent DMA, as on the display processor. */
	dev->coherent_dma_mask = DMA_BIT_MASK(64);
	dev->dma_mask = &dev->coherent_dma_mask;
	t->dcp.dev = dev;
	t->dcp.fw_compat = DCP_FIRMWARE_V_13_5;
	afk_ep_init_state(&t->ep);
	t->ep.dcp = &t->dcp;
	t->ep.endpoint = 0x2a;
	ret = afk_stall_ring(test, &t->ep.txbfr) ?: afk_stall_ring(test, &t->ep.rxbfr);
	if (ret)
		return ret;
	t->ep.num_channels = 1;
	service = &t->ep.services[0];
	service->ep = &t->ep;
	service->ops = &afk_stall_ops;
	service->channel = 1;
	service->enabled = true;
	test->priv = t;
	kunit_activate_static_stub(test, afk_send, afk_stall_doorbell);
	return 0;
}

static unsigned int afk_stall_held(struct apple_epic_service *service)
{
	return bitmap_weight(service->cmd_map, MAX_PENDING_CMDS);
}

static u32 afk_stall_unread(struct apple_dcp_afkep *ep)
{
	return le32_to_cpu(*ep->txbfr.wptr) - le32_to_cpu(*ep->txbfr.rptr);
}

/* The firmware answers every command still waiting: the test frees their buffers. */
static void afk_stall_answer_all(struct afk_stall_test *t)
{
	struct apple_epic_service *service = &t->ep.services[0];
	struct epic_cmd reply = {};
	unsigned int i;

	for_each_set_bit(i, service->cmd_map, MAX_PENDING_CMDS)
		afk_recv_handle_reply(&t->ep, service->channel, service->cmds[i].tag,
				      &reply, sizeof(reply));
}

/*
 * The SAFE state of a port whose firmware stopped answering is retried, and
 * each retry lowers HPD again: one request waits in the queue, however often.
 */
static void afk_stall_hpd_low_repeats_test(struct kunit *test)
{
	struct afk_stall_test *t = test->priv;
	struct apple_epic_service *service = &t->ep.services[0];
	u32 unread;
	int i;

	KUNIT_EXPECT_EQ(test, dptxport_set_hpd_timeout(service, false, AFK_STALL_TIMEOUT_MS),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->doorbells, 1U);
	KUNIT_EXPECT_EQ(test, afk_stall_held(service), 1U);
	unread = afk_stall_unread(&t->ep);
	KUNIT_EXPECT_GT(test, unread, 0U);

	/* More retries than there are slots: nothing more is queued. */
	for (i = 0; i < 2 * MAX_PENDING_CMDS; i++)
		KUNIT_EXPECT_EQ(test,
				dptxport_set_hpd_timeout(service, false, AFK_STALL_TIMEOUT_MS),
				-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->doorbells, 1U);
	KUNIT_EXPECT_EQ(test, afk_stall_held(service), 1U);
	KUNIT_EXPECT_EQ(test, afk_stall_unread(&t->ep), unread);

	/* The next display's HPD still gets a slot, and its unplug another. */
	KUNIT_EXPECT_EQ(test, dptxport_set_hpd_timeout(service, true, AFK_STALL_TIMEOUT_MS),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, dptxport_set_hpd_timeout(service, false, AFK_STALL_TIMEOUT_MS),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->doorbells, 3U);
	KUNIT_EXPECT_EQ(test, afk_stall_held(service), 3U);
	KUNIT_EXPECT_EQ(test, afk_stall_unread(&t->ep), 3 * unread);

	afk_stall_answer_all(t);
	KUNIT_EXPECT_EQ(test, afk_stall_held(service), 0U);
}

/* A late answer frees its slot, and the same request is then sent again. */
static void afk_stall_answered_resends_test(struct kunit *test)
{
	struct afk_stall_test *t = test->priv;
	struct apple_epic_service *service = &t->ep.services[0];
	struct epic_cmd reply = {};

	KUNIT_EXPECT_EQ(test, dptxport_set_hpd_timeout(service, false, AFK_STALL_TIMEOUT_MS),
			-ETIMEDOUT);
	KUNIT_ASSERT_EQ(test, afk_stall_held(service), 1U);
	KUNIT_EXPECT_TRUE(test, service->stall_reported);
	afk_recv_handle_reply(&t->ep, service->channel, service->last_tag, &reply,
			      sizeof(reply));
	KUNIT_EXPECT_EQ(test, afk_stall_held(service), 0U);
	KUNIT_EXPECT_FALSE(test, service->stall_reported);

	KUNIT_EXPECT_EQ(test, dptxport_set_hpd_timeout(service, false, AFK_STALL_TIMEOUT_MS),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->doorbells, 2U);
	KUNIT_EXPECT_EQ(test, afk_stall_held(service), 1U);
	afk_stall_answer_all(t);
}

/* Only the call queued last, with the same input, and only once it timed out. */
static void afk_stall_unanswered_match_test(struct kunit *test)
{
	struct afk_stall_test *t = test->priv;
	struct apple_epic_service *service = &t->ep.services[0];
	u8 data[20] = {}, other[20] = { [16] = 1 };

	KUNIT_EXPECT_FALSE(test, afk_service_call_unanswered(service, 8, 8, data, sizeof(data)));
	KUNIT_EXPECT_EQ(test, afk_service_call_timeout(service, 8, 8, data, sizeof(data), 12,
						       NULL, 0, 12, AFK_STALL_TIMEOUT_MS),
			-ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, afk_service_call_unanswered(service, 8, 8, data, sizeof(data)));
	KUNIT_EXPECT_FALSE(test, afk_service_call_unanswered(service, 8, 8, other, sizeof(other)));
	KUNIT_EXPECT_FALSE(test, afk_service_call_unanswered(service, 8, 7, data, sizeof(data)));
	KUNIT_EXPECT_FALSE(test, afk_service_call_unanswered(service, 0, 8, data, sizeof(data)));

	/* Another call queued behind it: the copy would no longer be redundant. */
	KUNIT_EXPECT_EQ(test, afk_service_call_timeout(service, 0, 6, NULL, 0, 16, NULL, 0, 16,
						       AFK_STALL_TIMEOUT_MS),
			-ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, afk_service_call_unanswered(service, 8, 8, data, sizeof(data)));
	KUNIT_EXPECT_TRUE(test, afk_service_call_unanswered(service, 0, 6, NULL, 0));

	/* Compact EPIC keeps its timed-out commands elsewhere. */
	t->ep.compact = true;
	KUNIT_EXPECT_FALSE(test, afk_service_call_unanswered(service, 0, 6, NULL, 0));
	t->ep.compact = false;
	afk_stall_answer_all(t);
	KUNIT_EXPECT_FALSE(test, afk_service_call_unanswered(service, 0, 6, NULL, 0));
}

/*
 * A doorbell the mailbox did not deliver leaves its entry in the ring for the
 * next one: the command keeps its slot until answered, and the loss is noted.
 */
static void afk_stall_lost_doorbell_test(struct kunit *test)
{
	struct afk_stall_test *t = test->priv;
	struct apple_epic_service *service = &t->ep.services[0];

	t->doorbell_ret = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, dptxport_set_hpd_timeout(service, true, AFK_STALL_TIMEOUT_MS),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->ep.doorbells_lost, 1U);
	KUNIT_EXPECT_EQ(test, t->ep.doorbell_error, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, afk_stall_held(service), 1U);
	KUNIT_EXPECT_GT(test, afk_stall_unread(&t->ep), 0U);

	t->doorbell_ret = 0;
	KUNIT_EXPECT_EQ(test, dptxport_set_hpd_timeout(service, false, AFK_STALL_TIMEOUT_MS),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->ep.doorbells_lost, 0U);
	KUNIT_EXPECT_EQ(test, afk_stall_held(service), 2U);
	afk_stall_answer_all(t);
}

static struct kunit_case afk_stall_cases[] = {
	KUNIT_CASE(afk_stall_hpd_low_repeats_test),
	KUNIT_CASE(afk_stall_answered_resends_test),
	KUNIT_CASE(afk_stall_unanswered_match_test),
	KUNIT_CASE(afk_stall_lost_doorbell_test),
	{}
};

static struct kunit_suite afk_stall_suite = {
	.name = "apple-afk-stall",
	.init = afk_stall_test_init,
	.test_cases = afk_stall_cases,
};

kunit_test_suite(afk_stall_suite);

// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by apple-sio.c: the checks sio_recv_msg() applies to the tag and
 * the channel a firmware message names, and when an ended transfer is freed.
 */
#include <kunit/device.h>
#include <kunit/test-bug.h>
#include <kunit/test.h>

static u64 sio_test_msg(u8 type, u8 tag, u8 sioep)
{
	return FIELD_PREP(SIOMSG_TYPE, type) | FIELD_PREP(SIOMSG_TAG, tag) |
	       FIELD_PREP(SIOMSG_EP, sioep);
}

/*
 * The firmware's ACKs, NACKs and reports name a tag and a channel. Only a
 * tag the host has outstanding is completed; tag 0, tags past the table and
 * channels past the device's are dropped without a WARN.
 */
static void sio_recv_msg_checks_tag_and_channel_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio;
	struct device *dev;

	dev = kunit_device_register(test, "apple-sio-msg-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	sio = kunit_kzalloc(test, struct_size(sio, channels, 1), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sio);
	sio->dev = dev;
	sio->nchannels = 1;
	spin_lock_init(&sio->tags.lock);
	/* As allocated, before the probe sets anything up. */
	init_completion(&sio->tags.completions[3]);
	init_completion(&sio->tags.completions[4]);

	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, 0, 0));
	KUNIT_EXPECT_EQ(test, sio->tags.completions[0].done, 0);
	KUNIT_EXPECT_FALSE(test, sio->tags.acked[0]);

	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, 17, 0));
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_NACK, 63, 0));
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_REPORT, 1, 0xff));

	/* Tag 4 was never handed out. */
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, 4, 0));
	KUNIT_EXPECT_EQ(test, sio->tags.completions[4].done, 0);
	KUNIT_EXPECT_FALSE(test, sio->tags.acked[4]);

	/* An outstanding tag is still completed. */
	set_bit(3, sio->tags.allocated);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, 3, 0));
	KUNIT_EXPECT_EQ(test, sio->tags.completions[3].done, 1);
	KUNIT_EXPECT_TRUE(test, sio->tags.acked[3]);

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

struct sio_test_ack {
	struct sio_chan *chan;
	unsigned int calls;
	bool ok;
};

static void sio_test_ack_callback(struct sio_chan *chan, void *cookie, bool ok)
{
	struct sio_test_ack *ack = cookie;

	ack->chan = chan;
	ack->ok = ok;
	ack->calls++;
}

static struct sio_data *sio_test_alloc(struct kunit *test, int nchannels)
{
	struct sio_data *sio;
	struct device *dev;
	int i;

	dev = kunit_device_register(test, "apple-sio-tag-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	sio = kunit_kzalloc(test, struct_size(sio, channels, nchannels), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sio);
	sio->dev = dev;
	sio->nchannels = nchannels;
	sio_init_tags(sio);
	for (i = 0; i < nchannels; i++) {
		struct sio_chan *chan = &sio->channels[i];

		chan->host = sio;
		chan->no = i;
		spin_lock_init(&chan->vc.lock);
		INIT_LIST_HEAD(&chan->retired);
		INIT_LIST_HEAD(&chan->dead);
		init_waitqueue_head(&chan->drained);
		mutex_init(&chan->sync_lock);
		init_waitqueue_head(&chan->sync_wait);
		INIT_WORK(&chan->terminate_wq, sio_terminate_work);
	}
	return sio;
}

/* The tag _sio_send_siomsg() binds to a request; a callback makes it atomic. */
static int sio_test_send(struct sio_data *sio, u8 type, u8 ep, sio_ack_callback callback,
			 void *cookie)
{
	unsigned long generation;

	return sio_alloc_tag(sio, FIELD_PREP(SIOMSG_TYPE, type) | FIELD_PREP(SIOMSG_EP, ep),
			     !!callback, callback, cookie, &generation);
}

/* sio_call()'s end, its wait having seen the reply if one came */
static int sio_test_call_end(struct sio_data *sio, int tag)
{
	return sio_call_result(sio, tag, try_wait_for_completion(&sio->tags.completions[tag]));
}

static bool sio_test_answered(struct sio_data *sio, int tag)
{
	return completion_done(&sio->tags.completions[tag]);
}

/*
 * A reply naming another endpoint than its tag's request is dropped: an
 * ISSUE's callback does not run, a call is not answered, and the tags stay
 * with their requests until their own endpoint's reply.
 */
static void sio_reply_endpoint_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc(test, 2);
	struct sio_test_ack ack = {};
	int issue, call, setup;

	issue = sio_test_send(sio, MSG_ISSUE, 0, sio_test_ack_callback, &ack);
	call = sio_test_send(sio, MSG_CONFIGURE, 1, NULL, NULL);
	/* A parameter's key gives the endpoint byte: 0x30d is endpoint 3 */
	setup = sio_test_send(sio, MSG_SETUP, 0x30d >> 8, NULL, NULL);
	KUNIT_ASSERT_GT(test, issue, 0);
	KUNIT_ASSERT_GT(test, call, 0);
	KUNIT_ASSERT_GT(test, setup, 0);

	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, issue, 1));
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_NACK, issue, 1));
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, call, 0));
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_NACK, call, 0));
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, setup, 0));
	KUNIT_EXPECT_EQ(test, ack.calls, 0);
	KUNIT_EXPECT_FALSE(test, sio_test_answered(sio, call));
	KUNIT_EXPECT_FALSE(test, sio_test_answered(sio, setup));
	KUNIT_EXPECT_TRUE(test, sio_tag_outstanding(sio, issue));
	KUNIT_EXPECT_TRUE(test, sio_tag_outstanding(sio, call));
	KUNIT_EXPECT_TRUE(test, sio_tag_outstanding(sio, setup));

	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, issue, 0));
	KUNIT_EXPECT_EQ(test, ack.calls, 1);
	KUNIT_EXPECT_PTR_EQ(test, ack.chan, &sio->channels[0]);
	KUNIT_EXPECT_TRUE(test, ack.ok);
	KUNIT_EXPECT_FALSE(test, sio_tag_outstanding(sio, issue));

	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_NACK, call, 1));
	KUNIT_EXPECT_EQ(test, sio_test_call_end(sio, call), 0);
	KUNIT_EXPECT_FALSE(test, sio_tag_outstanding(sio, call));
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, setup, 3));
	KUNIT_EXPECT_EQ(test, sio_test_call_end(sio, setup), 1);
	KUNIT_EXPECT_FALSE(test, sio_tag_outstanding(sio, setup));

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/*
 * The ISSUE accounting of a channel is not touched by an ACK that names it
 * for another channel's ISSUE, in the old accounting with the same DMA
 * cookie on both channels, nor in the ring accounting.
 */
static void sio_issue_ack_other_channel_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc(test, 2);
	struct sio_chan *a = &sio->channels[0], *b = &sio->channels[1];
	struct sio_tx *ta, *tb;
	int tag;

	ta = kunit_kzalloc(test, struct_size(ta, siodesc, 1), GFP_KERNEL);
	tb = kunit_kzalloc(test, struct_size(tb, siodesc, 1), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ta);
	KUNIT_ASSERT_NOT_NULL(test, tb);
	ta->nperiods = 1;
	tb->nperiods = 1;

	/* Old accounting; one descriptor short of the most in flight, none issued after */
	ta->vd.tx.cookie = 2;
	tb->vd.tx.cookie = 2;
	ta->ninflight = SIO_MAX_NINFLIGHT - 1;
	tb->ninflight = SIO_MAX_NINFLIGHT - 1;
	a->current_tx = ta;
	b->current_tx = tb;
	a->pending = 1;
	b->pending = 1;
	tag = sio_test_send(sio, MSG_ISSUE, a->no, sio_handle_issue_ack,
			    (void *)(uintptr_t)ta->vd.tx.cookie);
	KUNIT_ASSERT_GT(test, tag, 0);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, tag, b->no));
	KUNIT_EXPECT_EQ(test, b->pending, 1);
	KUNIT_EXPECT_EQ(test, tb->ninflight, SIO_MAX_NINFLIGHT - 1);
	KUNIT_EXPECT_EQ(test, a->pending, 1);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, tag, a->no));
	KUNIT_EXPECT_EQ(test, a->pending, 0);
	KUNIT_EXPECT_EQ(test, ta->ninflight, SIO_MAX_NINFLIGHT);
	KUNIT_EXPECT_EQ(test, b->pending, 1);
	KUNIT_EXPECT_EQ(test, tb->ninflight, SIO_MAX_NINFLIGHT - 1);

	/* Ring accounting: each channel waits for the ACK of its one ISSUE */
	a->current_tx = NULL;
	b->current_tx = NULL;
	ta->ninflight = 0;
	tb->ninflight = 0;
	a->ring = true;
	b->ring = true;
	a->issued[0] = ta;
	b->issued[0] = tb;
	a->nissued = 1;
	b->nissued = 1;
	a->issuing = true;
	b->issuing = true;
	tag = sio_test_send(sio, MSG_ISSUE, a->no, sio_handle_issue_ack, NULL);
	KUNIT_ASSERT_GT(test, tag, 0);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, tag, b->no));
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_NACK, tag, b->no));
	KUNIT_EXPECT_TRUE(test, b->issuing);
	KUNIT_EXPECT_EQ(test, b->nissued, 1);
	KUNIT_EXPECT_EQ(test, tb->ninflight, 0);
	KUNIT_EXPECT_TRUE(test, a->issuing);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, tag, a->no));
	KUNIT_EXPECT_FALSE(test, a->issuing);
	KUNIT_EXPECT_EQ(test, ta->ninflight, 1);
	KUNIT_EXPECT_TRUE(test, b->issuing);
	KUNIT_EXPECT_EQ(test, tb->ninflight, 0);

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/* STARTED answers only a START, an ACK any other request, a NACK any. */
static void sio_reply_kind_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc(test, 1);
	struct sio_test_ack ack = {};
	u64 started = FIELD_PREP(SIOMSG_DATA, 9);
	int start, call, issue;

	start = sio_test_send(sio, MSG_START, 0, NULL, NULL);
	call = sio_test_send(sio, MSG_SETUP, 0, NULL, NULL);
	issue = sio_test_send(sio, MSG_ISSUE, 0, sio_test_ack_callback, &ack);
	KUNIT_ASSERT_GT(test, start, 0);
	KUNIT_ASSERT_GT(test, call, 0);
	KUNIT_ASSERT_GT(test, issue, 0);

	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_STARTED, call, 0) | started);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_STARTED, issue, 0) | started);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, start, 0));
	KUNIT_EXPECT_EQ(test, sio->protocol, 0);
	KUNIT_EXPECT_EQ(test, ack.calls, 0);
	KUNIT_EXPECT_FALSE(test, sio_test_answered(sio, start));
	KUNIT_EXPECT_FALSE(test, sio_test_answered(sio, call));

	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_STARTED, start, 0) | started);
	KUNIT_EXPECT_EQ(test, sio->protocol, 9);
	KUNIT_EXPECT_EQ(test, sio_test_call_end(sio, start), 1);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, call, 0));
	KUNIT_EXPECT_EQ(test, sio_test_call_end(sio, call), 1);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_NACK, issue, 0));
	KUNIT_EXPECT_EQ(test, ack.calls, 1);
	KUNIT_EXPECT_FALSE(test, ack.ok);

	start = sio_test_send(sio, MSG_START, 0, NULL, NULL);
	KUNIT_ASSERT_GT(test, start, 0);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_NACK, start, 0));
	KUNIT_EXPECT_EQ(test, sio_test_call_end(sio, start), 0);
	KUNIT_EXPECT_EQ(test, sio->protocol, 9);

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/*
 * A call that timed out keeps its tag, through two rounds of the tag space.
 * Its late reply, to the same endpoint as the requests that own tags by
 * then, answers none of them and frees the tag. A reply after the timeout
 * that the call sees before it gives up still answers it.
 */
static void sio_late_reply_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc(test, 2);
	struct sio_test_ack ack = {};
	int late, tag, call, issue, i;

	late = sio_test_send(sio, MSG_CONFIGURE, 1, NULL, NULL);
	KUNIT_ASSERT_GT(test, late, 0);
	KUNIT_EXPECT_EQ(test, sio_call_result(sio, late, false), -ETIME);
	KUNIT_EXPECT_TRUE(test, sio_tag_outstanding(sio, late));
	KUNIT_EXPECT_EQ(test, sio->tags.nstale, 1U);

	for (i = 0; i < 2 * SIO_USABLE_TAGS; i++) {
		tag = sio_test_send(sio, MSG_CONFIGURE, 1, NULL, NULL);
		KUNIT_ASSERT_GT(test, tag, 0);
		KUNIT_EXPECT_NE(test, tag, late);
		sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, tag, 1));
		KUNIT_EXPECT_EQ(test, sio_test_call_end(sio, tag), 1);
	}

	call = sio_test_send(sio, MSG_CONFIGURE, 1, NULL, NULL);
	issue = sio_test_send(sio, MSG_ISSUE, 1, sio_test_ack_callback, &ack);
	KUNIT_ASSERT_GT(test, call, 0);
	KUNIT_ASSERT_GT(test, issue, 0);
	/* Of another endpoint, it stays held */
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, late, 0));
	KUNIT_EXPECT_TRUE(test, sio_tag_outstanding(sio, late));
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_NACK, late, 1));
	KUNIT_EXPECT_FALSE(test, sio_tag_outstanding(sio, late));
	KUNIT_EXPECT_EQ(test, sio->tags.nstale, 0U);
	KUNIT_EXPECT_FALSE(test, sio_test_answered(sio, call));
	KUNIT_EXPECT_EQ(test, ack.calls, 0);
	KUNIT_EXPECT_TRUE(test, sio_tag_outstanding(sio, call));
	KUNIT_EXPECT_TRUE(test, sio_tag_outstanding(sio, issue));
	/* A second reply for it names a tag no longer outstanding */
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, late, 1));
	KUNIT_EXPECT_FALSE(test, sio_test_answered(sio, call));
	KUNIT_EXPECT_EQ(test, ack.calls, 0);

	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, call, 1));
	KUNIT_EXPECT_EQ(test, sio_test_call_end(sio, call), 1);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, issue, 1));
	KUNIT_EXPECT_EQ(test, ack.calls, 1);

	/* Answered after the timeout, before the call gives up */
	tag = sio_test_send(sio, MSG_TERMINATE, 0, NULL, NULL);
	KUNIT_ASSERT_GT(test, tag, 0);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, tag, 0));
	KUNIT_EXPECT_EQ(test, sio_call_result(sio, tag, false), 1);
	KUNIT_EXPECT_FALSE(test, sio_tag_outstanding(sio, tag));
	KUNIT_EXPECT_EQ(test, sio->tags.nstale, 0U);

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/*
 * Calls are refused once SIO_MAX_STALE tags are held, and ISSUEs still get
 * the other tags. A late reply makes room again, and a firmware reset frees
 * every held tag.
 */
static void sio_stale_tags_bounded_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc(test, 1);
	int stale[SIO_MAX_STALE], issue[SIO_USABLE_TAGS - SIO_MAX_STALE];
	struct sio_test_ack ack = {};
	unsigned int i;
	int tag;

	for (i = 0; i < SIO_MAX_STALE; i++) {
		stale[i] = sio_test_send(sio, MSG_TERMINATE, 0, NULL, NULL);
		KUNIT_ASSERT_GT(test, stale[i], 0);
		KUNIT_EXPECT_EQ(test, sio_call_result(sio, stale[i], false), -ETIME);
	}
	KUNIT_EXPECT_EQ(test, sio_test_send(sio, MSG_TERMINATE, 0, NULL, NULL), -EBUSY);
	for (i = 0; i < ARRAY_SIZE(issue); i++) {
		issue[i] = sio_test_send(sio, MSG_ISSUE, 0, sio_test_ack_callback, &ack);
		KUNIT_EXPECT_GT(test, issue[i], 0);
	}
	KUNIT_EXPECT_EQ(test, sio_test_send(sio, MSG_ISSUE, 0, sio_test_ack_callback, &ack),
			-EBUSY);
	for (i = 0; i < ARRAY_SIZE(issue); i++)
		sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, issue[i], 0));
	KUNIT_EXPECT_EQ(test, ack.calls, ARRAY_SIZE(issue));
	KUNIT_EXPECT_EQ(test, sio_test_send(sio, MSG_TERMINATE, 0, NULL, NULL), -EBUSY);

	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, stale[0], 0));
	KUNIT_EXPECT_EQ(test, sio->tags.nstale, SIO_MAX_STALE - 1U);
	tag = sio_test_send(sio, MSG_TERMINATE, 0, NULL, NULL);
	KUNIT_ASSERT_GT(test, tag, 0);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, tag, 0));
	KUNIT_EXPECT_EQ(test, sio_test_call_end(sio, tag), 1);

	sio_release_stale(sio);
	KUNIT_EXPECT_EQ(test, sio->tags.nstale, 0U);
	for (i = 0; i < SIO_MAX_STALE; i++)
		KUNIT_EXPECT_FALSE(test, sio_tag_outstanding(sio, stale[i]));
	tag = sio_test_send(sio, MSG_TERMINATE, 0, NULL, NULL);
	KUNIT_EXPECT_GT(test, tag, 0);

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/*
 * A request whose send failed gives back its tag, unless a reply freed it
 * before and it went to another request since.
 */
static void sio_cancel_tag_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc(test, 1);
	unsigned long first, second;
	struct sio_test_ack ack = {};
	int tag, again;

	tag = sio_alloc_tag(sio, FIELD_PREP(SIOMSG_TYPE, MSG_ISSUE), true,
			    sio_test_ack_callback, &ack, &first);
	KUNIT_ASSERT_GT(test, tag, 0);
	sio_recv_msg(sio, EP_SIO, sio_test_msg(MSG_ACK, tag, 0));
	KUNIT_EXPECT_EQ(test, ack.calls, 1);
	/* The allocator hands out the same tag next */
	sio->tags.last_tag = tag - 1;
	again = sio_alloc_tag(sio, FIELD_PREP(SIOMSG_TYPE, MSG_CONFIGURE), false, NULL, NULL,
			      &second);
	KUNIT_ASSERT_EQ(test, again, tag);
	sio_cancel_tag(sio, tag, first);
	KUNIT_EXPECT_TRUE(test, sio_tag_outstanding(sio, tag));
	sio_cancel_tag(sio, again, second);
	KUNIT_EXPECT_FALSE(test, sio_tag_outstanding(sio, tag));

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

static struct kunit_case sio_msg_test_cases[] = {
	KUNIT_CASE(sio_recv_msg_checks_tag_and_channel_test),
	KUNIT_CASE(sio_reply_endpoint_test),
	KUNIT_CASE(sio_issue_ack_other_channel_test),
	KUNIT_CASE(sio_reply_kind_test),
	KUNIT_CASE(sio_late_reply_test),
	KUNIT_CASE(sio_stale_tags_bounded_test),
	KUNIT_CASE(sio_cancel_tag_test),
	{}
};

static struct kunit_suite sio_msg_test_suite = {
	.name = "apple-sio-msg",
	.test_cases = sio_msg_test_cases,
};

/* A channel with descriptor slots, as the probe sets them up */
static struct sio_data *sio_test_alloc_dma(struct kunit *test, int nchannels)
{
	struct sio_data *sio = sio_test_alloc(test, nchannels);
	int i;

	sio->hw = &sio_hw_generic;
	sio->shmem = kunit_kzalloc(test, 56 + SIO_NO_DESC_SLOTS * sizeof(struct sio_coproc_desc),
				   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sio->shmem);
	sio->shmem_desc_base = sio->shmem + 56;
	sio->desc_allocated = kunit_kcalloc(test, BITS_TO_LONGS(SIO_NO_DESC_SLOTS),
					    sizeof(unsigned long), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sio->desc_allocated);
	INIT_LIST_HEAD(&sio->dma.channels);
	for (i = 0; i < nchannels; i++) {
		sio->channels[i].vc.desc_free = sio_tx_free;
		vchan_init(&sio->channels[i].vc, &sio->dma);
	}
	return sio;
}

/* The channel's current transfer, ended by sio_terminate_all() with n descriptors in flight */
static struct sio_tx *sio_test_ended_tx(struct kunit *test, struct sio_chan *chan, int n)
{
	struct sio_tx *tx = sio_alloc_tx(chan, 0x10000, 4 * 0x400, 0x400, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, tx);
	tx->vd.tx.cookie = 2;
	tx->ninflight = n;
	tx->terminated = true;
	chan->current_tx = tx;
	return tx;
}

static int sio_test_slot(struct sio_data *sio, struct sio_tx *tx, int i)
{
	return tx->siodesc[i] - sio->shmem_desc_base;
}

/* The firmware's answers to TERMINATE */
struct sio_test_terminate {
	unsigned int nacks;	/* refused first */
	int ret;		/* then answered so */
	unsigned int calls;
	unsigned int report_at;	/* at this call, the firmware reports ... */
	unsigned int reports;	/* ... so many descriptors */
};

static int sio_test_terminate(struct sio_chan *chan)
{
	struct sio_test_terminate *t = kunit_get_current_test()->priv;
	unsigned int i;

	t->calls++;
	if (t->calls == t->report_at)
		for (i = 0; i < t->reports; i++)
			sio_process_report(chan);
	return t->calls <= t->nacks ? 0 : t->ret;
}

/*
 * A TERMINATE not answered while descriptors are in flight does not free the
 * transfer: it keeps its descriptors and the channel starts no other transfer.
 * sio_synchronize() does not return until the firmware stopped it: it sends
 * TERMINATE again through refusals until one is acknowledged.
 */
static void sio_terminate_unconfirmed_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc_dma(test, 1);
	struct sio_chan *chan = &sio->channels[0];
	struct sio_test_terminate t = {};
	struct dma_async_tx_descriptor *next_desc;
	struct sio_tx *tx, *next;
	unsigned long flags;
	LIST_HEAD(to_free);
	int slot, i;

	test->priv = &t;
	tx = sio_test_ended_tx(test, chan, 2);
	slot = sio_test_slot(sio, tx, 0);

	t.ret = -ETIME;
	sio_end_tx(chan, tx, sio_test_terminate);
	KUNIT_EXPECT_EQ(test, t.calls, SIO_TERMINATE_TRIES);
	KUNIT_EXPECT_TRUE(test, chan->quarantined);
	KUNIT_EXPECT_PTR_EQ(test, chan->current_tx, tx);
	for (i = 0; i < tx->nperiods; i++)
		KUNIT_EXPECT_TRUE(test, test_bit(sio_test_slot(sio, tx, i), sio->desc_allocated));

	/* The slots are not handed out again, and a queued transfer does not start. */
	next = sio_alloc_tx(chan, 0x20000, 4 * 0x400, 0x400, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, next);
	for (i = 0; i < next->nperiods; i++)
		KUNIT_EXPECT_GE(test, sio_test_slot(sio, next, i), slot + tx->nperiods);
	next_desc = vchan_tx_prep(&chan->vc, &next->vd, 0);
	dmaengine_submit(next_desc);
	sio_issue_pending(&chan->vc.chan);
	KUNIT_EXPECT_PTR_EQ(test, chan->current_tx, tx);
	/* As sio_terminate_all() drops it */
	spin_lock_irqsave(&chan->vc.lock, flags);
	vchan_get_all_descriptors(&chan->vc, &to_free);
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	vchan_dma_desc_free_list(&chan->vc, &to_free);

	/* Refused three times, over two rounds, then acknowledged */
	t = (struct sio_test_terminate){ .nacks = 3, .ret = 1 };
	sio_stop_held(chan, sio_test_terminate, false);
	KUNIT_EXPECT_EQ(test, t.calls, 4U);
	KUNIT_EXPECT_FALSE(test, chan->quarantined);
	KUNIT_EXPECT_NULL(test, chan->current_tx);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_FALSE(test, test_bit(slot + i, sio->desc_allocated));

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/*
 * A held transfer is also freed once the firmware reported its descriptors,
 * though it refuses TERMINATE, or once it crashed and issues nothing more.
 */
static void sio_terminate_held_stopped_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc_dma(test, 1);
	struct sio_chan *chan = &sio->channels[0];
	struct sio_test_terminate t = {};
	struct sio_tx *tx;
	int slot;

	test->priv = &t;
	tx = sio_test_ended_tx(test, chan, 2);
	slot = sio_test_slot(sio, tx, 0);
	chan->quarantined = true;
	t = (struct sio_test_terminate){ .ret = 0, .report_at = 2, .reports = 2 };
	sio_stop_held(chan, sio_test_terminate, false);
	KUNIT_EXPECT_EQ(test, t.calls, 2U);
	KUNIT_EXPECT_NULL(test, chan->current_tx);
	KUNIT_EXPECT_FALSE(test, chan->quarantined);
	KUNIT_EXPECT_FALSE(test, test_bit(slot, sio->desc_allocated));

	tx = sio_test_ended_tx(test, chan, 2);
	chan->quarantined = true;
	t = (struct sio_test_terminate){ .ret = -ESHUTDOWN };
	sio_stop_held(chan, sio_test_terminate, false);
	KUNIT_EXPECT_EQ(test, t.calls, 1U);
	KUNIT_EXPECT_NULL(test, chan->current_tx);
	KUNIT_EXPECT_FALSE(test, chan->quarantined);

	/* Not held: nothing to stop */
	t = (struct sio_test_terminate){ .ret = 0 };
	sio_stop_held(chan, sio_test_terminate, false);
	KUNIT_EXPECT_EQ(test, t.calls, 0U);

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/*
 * The ring accounting: sio_synchronize() keeps the transfers retired with
 * descriptors in flight until the channel is stopped, sending TERMINATE again
 * through refusals, and frees them after.
 */
static void sio_terminate_ring_retired_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc_dma(test, 1);
	struct sio_chan *chan = &sio->channels[0];
	struct sio_test_terminate t = { .nacks = 2, .ret = 1 };
	struct sio_tx *tx;
	int slot;

	test->priv = &t;
	tx = sio_test_ended_tx(test, chan, 2);
	slot = sio_test_slot(sio, tx, 0);
	chan->current_tx = NULL;
	chan->ring = true;
	chan->issued[0] = tx;
	chan->issued[1] = tx;
	chan->nissued = 2;
	tx->retired = true;
	list_add_tail(&tx->vd.node, &chan->retired);

	sio_wait_retired(chan, sio_test_terminate, false);
	KUNIT_EXPECT_EQ(test, t.calls, 3U);
	KUNIT_EXPECT_TRUE(test, list_empty(&chan->retired));
	KUNIT_EXPECT_EQ(test, chan->nissued, 0U);
	KUNIT_EXPECT_TRUE(test, test_bit(slot, sio->desc_allocated));
	sio_free_dead(chan);
	KUNIT_EXPECT_FALSE(test, test_bit(slot, sio->desc_allocated));

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/* With nothing in flight, a TERMINATE refused or not answered still frees the transfer. */
static void sio_terminate_drained_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc_dma(test, 1);
	struct sio_chan *chan = &sio->channels[0];
	struct sio_test_terminate t = { .ret = 0 };
	struct sio_tx *tx;
	int slot;

	test->priv = &t;
	tx = sio_test_ended_tx(test, chan, 0);
	slot = sio_test_slot(sio, tx, 0);
	sio_end_tx(chan, tx, sio_test_terminate);
	KUNIT_EXPECT_EQ(test, t.calls, 1U);
	KUNIT_EXPECT_FALSE(test, chan->quarantined);
	KUNIT_EXPECT_NULL(test, chan->current_tx);
	KUNIT_EXPECT_FALSE(test, test_bit(slot, sio->desc_allocated));

	t.calls = 0;
	t.ret = -ETIME;
	tx = sio_test_ended_tx(test, chan, 0);
	sio_end_tx(chan, tx, sio_test_terminate);
	KUNIT_EXPECT_EQ(test, t.calls, 1U);
	KUNIT_EXPECT_NULL(test, chan->current_tx);

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/*
 * The old accounting: an ISSUE acknowledged after the transfer ended is in
 * flight until its report, and an ended transfer's client is not called back.
 */
static void sio_terminate_late_issue_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc_dma(test, 1);
	struct sio_chan *chan = &sio->channels[0];
	struct sio_tx *tx;

	tx = sio_test_ended_tx(test, chan, 1);
	chan->pending = 1;
	KUNIT_EXPECT_FALSE(test, sio_tx_drained(chan, tx));

	sio_process_report(chan);
	KUNIT_EXPECT_EQ(test, tx->ninflight, 0);
	KUNIT_EXPECT_NULL(test, chan->vc.cyclic);
	KUNIT_EXPECT_FALSE(test, sio_tx_drained(chan, tx));

	sio_handle_issue_ack(chan, (void *)(uintptr_t)tx->vd.tx.cookie, true);
	KUNIT_EXPECT_EQ(test, chan->pending, 0U);
	KUNIT_EXPECT_EQ(test, tx->nlate, 1);
	KUNIT_EXPECT_EQ(test, tx->ninflight, 0);
	KUNIT_EXPECT_FALSE(test, sio_tx_drained(chan, tx));

	sio_process_report(chan);
	KUNIT_EXPECT_EQ(test, tx->nlate, 0);
	KUNIT_EXPECT_NULL(test, chan->vc.cyclic);
	KUNIT_EXPECT_TRUE(test, sio_tx_drained(chan, tx));

	chan->current_tx = NULL;
	sio_tx_free(&tx->vd);

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/* A held transfer whose ring the firmware also holds: two descriptors issued */
static struct sio_tx *sio_test_held_tx(struct kunit *test, struct sio_chan *chan)
{
	struct sio_tx *tx = sio_test_ended_tx(test, chan, 2);

	chan->quarantined = true;
	return tx;
}

/*
 * Bounded, for a caller that keeps the buffers (a display's unplug, a system
 * sleep): a channel the firmware does not stop is tried once, and its held
 * or retired transfers stay, the channel busy. Unbounded, it is freed once
 * the firmware stops it.
 */
static void sio_sync_bounded_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc_dma(test, 1);
	struct sio_chan *chan = &sio->channels[0];
	/* Acknowledged once the bounded tries are over: only an unbounded one gets there */
	struct sio_test_terminate t = { .nacks = SIO_TERMINATE_TRIES, .ret = 1 };
	struct sio_tx *tx;

	test->priv = &t;
	tx = sio_test_held_tx(test, chan);
	KUNIT_EXPECT_EQ(test, sio_sync(chan, sio_test_terminate, true), -EBUSY);
	KUNIT_EXPECT_EQ(test, t.calls, SIO_TERMINATE_TRIES);
	KUNIT_EXPECT_PTR_EQ(test, chan->current_tx, tx);
	KUNIT_EXPECT_TRUE(test, chan->quarantined);
	KUNIT_EXPECT_TRUE(test, sio_chan_busy(chan));

	t = (struct sio_test_terminate){ .ret = 1 };
	KUNIT_EXPECT_EQ(test, sio_sync(chan, sio_test_terminate, false), 0);
	KUNIT_EXPECT_NULL(test, chan->current_tx);
	KUNIT_EXPECT_FALSE(test, sio_chan_busy(chan));

	/* The ring accounting: a retired transfer stays retired. */
	tx = sio_test_ended_tx(test, chan, 2);
	chan->current_tx = NULL;
	chan->ring = true;
	chan->issued[0] = tx;
	chan->issued[1] = tx;
	chan->nissued = 2;
	tx->retired = true;
	list_add_tail(&tx->vd.node, &chan->retired);
	t = (struct sio_test_terminate){ .nacks = 1, .ret = 1 };
	KUNIT_EXPECT_EQ(test, sio_sync(chan, sio_test_terminate, true), -EBUSY);
	KUNIT_EXPECT_EQ(test, t.calls, 1U);
	KUNIT_EXPECT_FALSE(test, list_empty(&chan->retired));
	KUNIT_EXPECT_EQ(test, chan->nissued, 2U);

	t = (struct sio_test_terminate){ .ret = 1 };
	KUNIT_EXPECT_EQ(test, sio_sync(chan, sio_test_terminate, false), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&chan->retired));

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/*
 * While a synchronize waits for a channel that does not stop, a bounded one
 * fails after the time a channel that stops takes, leaving the channel as it
 * is: the caller does not wait on the firmware with its own locks held.
 */
static void sio_sync_lock_bounded_test(struct kunit *test)
{
	bool tainted = test_taint(TAINT_WARN);
	struct sio_data *sio = sio_test_alloc_dma(test, 1);
	struct sio_chan *chan = &sio->channels[0];
	struct sio_test_terminate t = { .ret = 1 };
	unsigned long start;
	struct sio_tx *tx;

	test->priv = &t;
	tx = sio_test_held_tx(test, chan);
	mutex_lock(&chan->sync_lock);
	start = jiffies;
	KUNIT_EXPECT_EQ(test, sio_sync(chan, sio_test_terminate, true), -EBUSY);
	KUNIT_EXPECT_GE(test, jiffies - start, msecs_to_jiffies(SIO_SYNC_LOCK_TIMEOUT_MS));
	KUNIT_EXPECT_EQ(test, t.calls, 0U);
	KUNIT_EXPECT_PTR_EQ(test, chan->current_tx, tx);
	sio_sync_unlock(chan);

	KUNIT_EXPECT_EQ(test, sio_sync(chan, sio_test_terminate, true), 0);
	KUNIT_EXPECT_EQ(test, t.calls, 1U);
	KUNIT_EXPECT_NULL(test, chan->current_tx);

	if (!tainted)
		KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));
}

/* sio_synchronize() is bounded from a system sleep's prepare to its end. */
static void sio_sleep_notifier_test(struct kunit *test)
{
	struct sio_data *sio = sio_test_alloc_dma(test, 1);
	static const unsigned long prepare[] = {
		PM_SUSPEND_PREPARE, PM_HIBERNATION_PREPARE, PM_RESTORE_PREPARE,
	};
	static const unsigned long post[] = {
		PM_POST_SUSPEND, PM_POST_HIBERNATION, PM_POST_RESTORE,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(prepare); i++) {
		KUNIT_EXPECT_FALSE(test, READ_ONCE(sio->sleeping));
		sio_pm_notify(&sio->pm_nb, prepare[i], NULL);
		KUNIT_EXPECT_TRUE(test, READ_ONCE(sio->sleeping));
		sio_pm_notify(&sio->pm_nb, post[i], NULL);
		KUNIT_EXPECT_FALSE(test, READ_ONCE(sio->sleeping));
	}
}

static struct kunit_case sio_terminate_test_cases[] = {
	KUNIT_CASE_SLOW(sio_terminate_unconfirmed_test),
	KUNIT_CASE_SLOW(sio_terminate_held_stopped_test),
	KUNIT_CASE_SLOW(sio_terminate_ring_retired_test),
	KUNIT_CASE(sio_terminate_drained_test),
	KUNIT_CASE(sio_terminate_late_issue_test),
	KUNIT_CASE_SLOW(sio_sync_bounded_test),
	KUNIT_CASE_SLOW(sio_sync_lock_bounded_test),
	KUNIT_CASE(sio_sleep_notifier_test),
	{}
};

static struct kunit_suite sio_terminate_test_suite = {
	.name = "apple-sio-terminate",
	.test_cases = sio_terminate_test_cases,
};

kunit_test_suites(&sio_msg_test_suite, &sio_terminate_test_suite);

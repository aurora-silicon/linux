// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by apple-sio.c: the checks sio_recv_msg() applies to the tag and
 * the channel a firmware message names.
 */
#include <kunit/device.h>
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

kunit_test_suite(sio_msg_test_suite);

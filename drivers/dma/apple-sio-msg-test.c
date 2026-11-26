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

static struct kunit_case sio_msg_test_cases[] = {
	KUNIT_CASE(sio_recv_msg_checks_tag_and_channel_test),
	{}
};

static struct kunit_suite sio_msg_test_suite = {
	.name = "apple-sio-msg",
	.test_cases = sio_msg_test_cases,
};

kunit_test_suite(sio_msg_test_suite);

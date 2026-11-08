// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the WoWLAN command bytes and the wake-reason event, which
 * the firmware sends and the driver treats as untrusted.
 */
#include <kunit/test.h>
#include <linux/unaligned.h>

#include "wow.h"

static void wow_expect_zero(struct kunit *test, const u8 *p, size_t from, size_t to)
{
	size_t i;

	for (i = from; i < to; i++)
		KUNIT_EXPECT_EQ_MSG(test, p[i], 0, "byte %zu", i);
}

static void mt7932_wow_triggers_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mt7932_wow_triggers(false, false), 0);
	KUNIT_EXPECT_EQ(test, mt7932_wow_triggers(true, false), 0x01);
	KUNIT_EXPECT_EQ(test, mt7932_wow_triggers(false, true), 0x14);
	KUNIT_EXPECT_EQ(test, mt7932_wow_triggers(true, true), 0x15);
}

static void mt7932_wow_param_test(struct kunit *test)
{
	u8 p[MT7932_WOW_PARAM_LEN + 1];

	memset(p, 0xa5, sizeof(p));
	mt7932_wow_param(p, MT7932_WOW_START, mt7932_wow_triggers(true, true));
	KUNIT_EXPECT_EQ(test, p[0], 1);
	KUNIT_EXPECT_EQ(test, p[1], 0x15);
	/* Filter: drop everything for the host except a magic packet. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(p + 2), 0x0003);
	wow_expect_zero(test, p, 4, 8);
	/* First wake-HIF record: PCIe, in-band, no GPIO pin or level. */
	KUNIT_EXPECT_EQ(test, p[8], 2);
	wow_expect_zero(test, p, 9, MT7932_WOW_PARAM_LEN);
	/* Never writes past the 228-byte block. */
	KUNIT_EXPECT_EQ(test, p[MT7932_WOW_PARAM_LEN], 0xa5);

	/* A STOP never carries triggers, whatever the caller passes. */
	mt7932_wow_param(p, MT7932_WOW_STOP, 0xff);
	KUNIT_EXPECT_EQ(test, p[0], 2);
	KUNIT_EXPECT_EQ(test, p[1], 0);
	KUNIT_EXPECT_EQ(test, p[8], 2);
	wow_expect_zero(test, p, 2, 8);
	wow_expect_zero(test, p, 9, MT7932_WOW_PARAM_LEN);
}

/* The SET travels in the ordinary runtime envelope, with a reply requested. */
static void mt7932_wow_envelope_test(struct kunit *test)
{
	u8 p[MT7932_WOW_PARAM_LEN], out[MT7932_ENVELOPE + MT7932_WOW_PARAM_LEN];
	int total;

	mt7932_wow_param(p, MT7932_WOW_START, MT7932_WOW_MAGIC);
	total = mt7932_envelope(out, sizeof(out), MT7932_CMD_SET_WOWLAN, 0, 7, true, true,
				true, p, sizeof(p));
	KUNIT_ASSERT_EQ(test, total, MT7932_ENVELOPE + MT7932_WOW_PARAM_LEN);
	KUNIT_EXPECT_EQ(test, out[36], 0x4a);
	KUNIT_EXPECT_EQ(test, out[38], 1);
	KUNIT_EXPECT_EQ(test, out[39], 7);
	KUNIT_EXPECT_EQ(test, out[43], 1);
	KUNIT_EXPECT_EQ(test, out[MT7932_ENVELOPE], 1);
	KUNIT_EXPECT_EQ(test, out[MT7932_ENVELOPE + 1], 1);
	KUNIT_EXPECT_EQ(test, out[MT7932_ENVELOPE + 8], 2);
}

static void mt7932_wow_ack_test(struct kunit *test)
{
	u8 r[48] = {};

	KUNIT_EXPECT_EQ(test, mt7932_wow_ack(r, 31), -EPROTO);
	r[28] = 0x4a;
	KUNIT_EXPECT_EQ(test, mt7932_wow_ack(r, 40), 0);
	/* Generic result for the SET: status 0 accepts, nonzero rejects. */
	r[28] = MT7932_EVENT_CMD_RESULT;
	r[36] = MT7932_CMD_SET_WOWLAN;
	KUNIT_EXPECT_EQ(test, mt7932_wow_ack(r, 40), 0);
	r[37] = 3;
	KUNIT_EXPECT_EQ(test, mt7932_wow_ack(r, 40), -EREMOTEIO);
	/* A result naming another command, or too short to name one, has no status for us. */
	KUNIT_EXPECT_EQ(test, mt7932_wow_ack(r, 37), 0);
	r[36] = 0x05;
	KUNIT_EXPECT_EQ(test, mt7932_wow_ack(r, 40), 0);
}

static int wow_event(u8 *packet, size_t size, u8 eid, size_t payload, struct mt7932_event *e)
{
	size_t total = MT7932_EVENT_PAYLOAD + payload;

	memset(packet, 0, size);
	put_unaligned_le32((7U << 27) | total, packet);
	put_unaligned_le16(total - 24, packet + 24);
	packet[28] = eid;
	return mt7932_event_parse(packet, total, e);
}

static void mt7932_wow_reason_test(struct kunit *test)
{
	struct mt7932_event e;
	u8 packet[64];

	KUNIT_ASSERT_EQ(test, wow_event(packet, sizeof(packet), 0x62, 4, &e), 0);
	KUNIT_EXPECT_EQ(test, mt7932_wow_reason(&e), MT7932_WOW_REASON_MAGIC);
	packet[36] = MT7932_WOW_REASON_DISCONNECT;
	KUNIT_EXPECT_EQ(test, mt7932_wow_reason(&e), MT7932_WOW_REASON_DISCONNECT);
	/* Other public reasons pass through for a generic report. */
	packet[36] = 3;
	KUNIT_EXPECT_EQ(test, mt7932_wow_reason(&e), 3);

	/* One reason byte is enough; trailing extensions are ignored. */
	KUNIT_ASSERT_EQ(test, wow_event(packet, sizeof(packet), 0x62, 1, &e), 0);
	KUNIT_EXPECT_EQ(test, mt7932_wow_reason(&e), 0);
	KUNIT_ASSERT_EQ(test, wow_event(packet, sizeof(packet), 0x62, 20, &e), 0);
	packet[36] = 8;
	KUNIT_EXPECT_EQ(test, mt7932_wow_reason(&e), 8);

	/* A header without a reason byte is rejected before it is read. */
	KUNIT_ASSERT_EQ(test, wow_event(packet, sizeof(packet), 0x62, 0, &e), 0);
	KUNIT_EXPECT_EQ(test, mt7932_wow_reason(&e), -EMSGSIZE);

	/* Another event is never a wake reason. */
	KUNIT_ASSERT_EQ(test, wow_event(packet, sizeof(packet), 0x11, 4, &e), 0);
	KUNIT_EXPECT_EQ(test, mt7932_wow_reason(&e), -ENOMSG);
}

static struct kunit_case mt7932_wow_test_cases[] = {
	KUNIT_CASE(mt7932_wow_triggers_test),
	KUNIT_CASE(mt7932_wow_param_test),
	KUNIT_CASE(mt7932_wow_envelope_test),
	KUNIT_CASE(mt7932_wow_ack_test),
	KUNIT_CASE(mt7932_wow_reason_test),
	{}
};

static struct kunit_suite mt7932_wow_test_suite = {
	.name = "mt7932-wow",
	.test_cases = mt7932_wow_test_cases,
};
kunit_test_suite(mt7932_wow_test_suite);

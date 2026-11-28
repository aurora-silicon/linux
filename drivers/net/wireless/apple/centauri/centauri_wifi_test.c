// SPDX-License-Identifier: GPL-2.0-only
/* Included by centauri_wifi.c to exercise the statistics reply handler. */
#include <kunit/test.h>

/* A reply to command 0xb0001 carrying one TLV of @length bytes. */
static void centauri_stats_reply(struct centauri_wifi *wifi, u8 *packet,
				 u16 size, u16 tag, u16 length)
{
	memset(packet, 0, size);
	put_unaligned_le16(tag, packet + 24);
	put_unaligned_le16(length, packet + 26);
	record_stats_reply(wifi, packet, size, 0);
}

static void centauri_station_stats_len_test(struct kunit *test)
{
	struct centauri_wifi *wifi = kunit_kzalloc(test, sizeof(*wifi), GFP_KERNEL);
	u8 *packet = kunit_kzalloc(test, BUFFER_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, wifi);
	KUNIT_ASSERT_NOT_NULL(test, packet);

	centauri_stats_reply(wifi, packet, 28 + 752, 0xf928, 752);
	KUNIT_ASSERT_TRUE(test, wifi->station_stats_valid);
	KUNIT_EXPECT_EQ(test, wifi->station_stats_len, 752);

	/* A later reply with another record leaves the counters as they were. */
	centauri_stats_reply(wifi, packet, 28, 0xf921, U16_MAX);
	KUNIT_EXPECT_EQ(test, wifi->stats_length, U16_MAX);
	KUNIT_EXPECT_TRUE(test, wifi->station_stats_valid);
	KUNIT_EXPECT_EQ(test, wifi->station_stats_len, 752);
	KUNIT_EXPECT_LE(test, wifi->station_stats_len, sizeof(wifi->station_stats));

	centauri_stats_reply(wifi, packet, 28 + 888, 0xf928, 888);
	KUNIT_EXPECT_EQ(test, wifi->station_stats_len, 888);

	/* An unsupported counter record length is not recorded. */
	centauri_stats_reply(wifi, packet, 28 + 900, 0xf928, 900);
	KUNIT_EXPECT_EQ(test, wifi->station_stats_len, 888);
}

static struct kunit_case centauri_wifi_cases[] = {
	KUNIT_CASE(centauri_station_stats_len_test),
	{}
};

static struct kunit_suite centauri_wifi_suite = {
	.name = "apple-centauri-wifi",
	.test_cases = centauri_wifi_cases,
};

kunit_test_suite(centauri_wifi_suite);

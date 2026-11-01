// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "mt7932.h"

static void mt_move_report_expect(struct kunit *test, bool move, bool cancelled,
				  int ret, bool result, bool disconnect,
				  bool landed, u16 reason)
{
	struct mt7932_connect_report report =
		mt7932_connect_report(move, cancelled, ret,
				      WLAN_REASON_DISASSOC_STA_HAS_LEFT);

	KUNIT_EXPECT_EQ(test, report.result, result);
	KUNIT_EXPECT_EQ(test, report.disconnect, disconnect);
	KUNIT_EXPECT_EQ(test, report.landed, landed);
	if (disconnect)
		KUNIT_EXPECT_EQ(test, report.reason, reason);
}

/* An ordinary connect: one connect result, or only a disconnect if cancelled. */
static void mt_connect_report_plain_test(struct kunit *test)
{
	mt_move_report_expect(test, false, false, 0, true, false, false, 0);
	mt_move_report_expect(test, false, false, -ETIMEDOUT, true, false, false, 0);
	mt_move_report_expect(test, false, false, -ECONNREFUSED, true, false, false, 0);
	mt_move_report_expect(test, false, false, -EIO, true, false, false, 0);
	mt_move_report_expect(test, false, true, -ECANCELED, false, true, false,
			      WLAN_REASON_DISASSOC_STA_HAS_LEFT);
}

/* A move that lands takes over the connection through its connect result.
 * One that fails or is cancelled reports the failed result, which is what
 * iwd waits for, and then ends the connection it replaced.
 */
static void mt_connect_report_move_test(struct kunit *test)
{
	mt_move_report_expect(test, true, false, 0, true, false, true, 0);
	mt_move_report_expect(test, true, false, -ETIMEDOUT, true, true, false,
			      WLAN_REASON_DEAUTH_LEAVING);
	mt_move_report_expect(test, true, false, -ECONNRESET, true, true, false,
			      WLAN_REASON_DEAUTH_LEAVING);
	mt_move_report_expect(test, true, false, -EIO, true, true, false,
			      WLAN_REASON_DEAUTH_LEAVING);
	/* A regulatory pass cancels a join without marking it cancelled. */
	mt_move_report_expect(test, true, false, -ECANCELED, true, true, false,
			      WLAN_REASON_DEAUTH_LEAVING);
	mt_move_report_expect(test, true, true, -ECANCELED, true, true, false,
			      WLAN_REASON_DISASSOC_STA_HAS_LEFT);
}

/* Whatever the outcome, cfg80211 hears of it, at most one disconnect follows
 * and only a successful move marks the new connection.
 */
static void mt_connect_report_complete_test(struct kunit *test)
{
	static const int results[] = {
		0, -ETIMEDOUT, -ECONNREFUSED, -ECONNRESET, -EPROTO, -ECANCELED,
		-EIO, -ESHUTDOWN,
	};
	struct mt7932_connect_report report;
	unsigned int i, flags;

	for (i = 0; i < ARRAY_SIZE(results); i++) {
		for (flags = 0; flags < 4; flags++) {
			bool move = flags & 1, cancelled = flags & 2;

			/* A cancel always leaves an error. */
			if (cancelled && !results[i])
				continue;
			report = mt7932_connect_report(move, cancelled, results[i],
						       WLAN_REASON_UNSPECIFIED);
			KUNIT_EXPECT_TRUE(test, report.result || report.disconnect);
			KUNIT_EXPECT_EQ(test, report.landed, move && !results[i]);
			if (report.landed)
				KUNIT_EXPECT_FALSE(test, report.disconnect);
			/* A connect result for a move, whether it failed or not. */
			if (move)
				KUNIT_EXPECT_TRUE(test, report.result);
			/* Only a successful connect leaves a connection behind. */
			if (report.result && results[i])
				KUNIT_EXPECT_EQ(test, report.disconnect, move);
		}
	}
}

/* A connection a move made reports a local end as from the AP. */
static void mt_disconnect_local_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, mt7932_disconnect_local(true, false));
	KUNIT_EXPECT_FALSE(test, mt7932_disconnect_local(true, true));
	KUNIT_EXPECT_FALSE(test, mt7932_disconnect_local(false, false));
	KUNIT_EXPECT_FALSE(test, mt7932_disconnect_local(false, true));
}

static struct kunit_case mt_move_test_cases[] = {
	KUNIT_CASE(mt_connect_report_plain_test),
	KUNIT_CASE(mt_connect_report_move_test),
	KUNIT_CASE(mt_connect_report_complete_test),
	KUNIT_CASE(mt_disconnect_local_test),
	{}
};

static struct kunit_suite mt_move_test_suite = {
	.name = "mt7932-move",
	.test_cases = mt_move_test_cases,
};

kunit_test_suite(mt_move_test_suite);

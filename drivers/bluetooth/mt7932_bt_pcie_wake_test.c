// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for how mt7932_bt_pcie classifies received events while the
 * system suspends with wakeup armed. Built as part of mt7932_bt_pcie.c,
 * so the tests reach its static helpers.
 */
#include <kunit/test.h>

static void bt7932_wake_event_bredr_test(struct kunit *test)
{
	/* Connection Request: bdaddr, class of device, link type. */
	static const u8 request[] = { HCI_EV_CONN_REQUEST, 10,
				      1, 2, 3, 4, 5, 6, 0x40, 0x25, 0x00, 0x01 };
	static const u8 complete[] = { HCI_EV_CONN_COMPLETE, 11, 0x00, 0x01, 0x00,
				       1, 2, 3, 4, 5, 6, 0x01, 0x00 };
	static const u8 sync[] = { HCI_EV_SYNC_CONN_COMPLETE, 1, 0x00 };

	KUNIT_EXPECT_EQ(test, bt7932_wake_event(request, sizeof(request)), BT7932_WAKE_REMOTE);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(complete, sizeof(complete)), BT7932_WAKE_REMOTE);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(sync, sizeof(sync)), BT7932_WAKE_REMOTE);
}

static void bt7932_wake_event_le_test(struct kunit *test)
{
	static const u8 adv[] = { HCI_EV_LE_META, 2, HCI_EV_LE_ADVERTISING_REPORT, 1 };
	static const u8 direct[] = { HCI_EV_LE_META, 1, HCI_EV_LE_DIRECT_ADV_REPORT };
	static const u8 ext[] = { HCI_EV_LE_META, 1, HCI_EV_LE_EXT_ADV_REPORT };
	/* LE Connection Complete and Read Remote Features Complete. */
	static const u8 conn[] = { HCI_EV_LE_META, 1, HCI_EV_LE_CONN_COMPLETE };
	static const u8 features[] = { HCI_EV_LE_META, 1, 0x04 };
	static const u8 no_subevent[] = { HCI_EV_LE_META, 0 };

	KUNIT_EXPECT_EQ(test, bt7932_wake_event(adv, sizeof(adv)), BT7932_WAKE_ADVERTISING);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(direct, sizeof(direct)), BT7932_WAKE_ADVERTISING);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(ext, sizeof(ext)), BT7932_WAKE_ADVERTISING);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(conn, sizeof(conn)), BT7932_WAKE_NONE);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(features, sizeof(features)), BT7932_WAKE_NONE);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(no_subevent, sizeof(no_subevent)),
			BT7932_WAKE_NONE);
}

/* The replies to the commands that set up suspend must never end it. */
static void bt7932_wake_event_replies_test(struct kunit *test)
{
	/* Command Complete for LE Set Scan Enable, Command Status. */
	static const u8 cc[] = { HCI_EV_CMD_COMPLETE, 4, 0x01, 0x0c, 0x20, 0x00 };
	static const u8 cs[] = { HCI_EV_CMD_STATUS, 4, 0x00, 0x01, 0x0d, 0x20 };
	static const u8 disconn[] = { HCI_EV_DISCONN_COMPLETE, 4, 0x00, 0x01, 0x00, 0x15 };
	/* Vendor events, including the MediaTek vendor channel reply. */
	static const u8 vendor[] = { HCI_EV_VENDOR, 1, 0x00 };
	static const u8 mtk[] = { 0xe4, 5, 0x02, 0x01, 0x01, 0x00, 0x00 };

	KUNIT_EXPECT_EQ(test, bt7932_wake_event(cc, sizeof(cc)), BT7932_WAKE_NONE);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(cs, sizeof(cs)), BT7932_WAKE_NONE);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(disconn, sizeof(disconn)), BT7932_WAKE_NONE);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(vendor, sizeof(vendor)), BT7932_WAKE_NONE);
	KUNIT_EXPECT_EQ(test, bt7932_wake_event(mtk, sizeof(mtk)), BT7932_WAKE_NONE);
}

/* A Hardware Error ends the sleep so that recovery runs awake. */
static void bt7932_wake_event_error_test(struct kunit *test)
{
	static const u8 error[] = { HCI_EV_HARDWARE_ERROR, 1, 0x00 };

	KUNIT_EXPECT_EQ(test, bt7932_wake_event(error, sizeof(error)), BT7932_WAKE_ERROR);
}

/* The wake log names the event and the remote address it carries. */
static void bt7932_wake_name_bredr_test(struct kunit *test)
{
	static const u8 request[] = { HCI_EV_CONN_REQUEST, 10,
				      1, 2, 3, 4, 5, 6, 0x40, 0x25, 0x00, 0x01 };
	static const u8 complete[] = { HCI_EV_CONN_COMPLETE, 11, 0x00, 0x01, 0x00,
				       1, 2, 3, 4, 5, 6, 0x01, 0x00 };
	static const u8 sync_short[] = { HCI_EV_SYNC_CONN_COMPLETE, 1, 0x00 };
	static const u8 error[] = { HCI_EV_HARDWARE_ERROR, 1, 0x00 };
	const u8 *addr;

	KUNIT_EXPECT_STREQ(test, bt7932_wake_name(request, sizeof(request), &addr),
			   "connection request");
	KUNIT_EXPECT_PTR_EQ(test, addr, &request[2]);
	KUNIT_EXPECT_STREQ(test, bt7932_wake_name(complete, sizeof(complete), &addr),
			   "connection complete");
	KUNIT_EXPECT_PTR_EQ(test, addr, &complete[5]);
	bt7932_wake_name(sync_short, sizeof(sync_short), &addr);
	KUNIT_EXPECT_NULL(test, addr);
	KUNIT_EXPECT_STREQ(test, bt7932_wake_name(error, sizeof(error), &addr), "hardware error");
	KUNIT_EXPECT_NULL(test, addr);
}

static void bt7932_wake_name_le_test(struct kunit *test)
{
	/* Subevent, one report, event type, address type, address, data length, RSSI. */
	static const u8 adv[] = { HCI_EV_LE_META, 12, HCI_EV_LE_ADVERTISING_REPORT, 1,
				  0x00, 0x01, 1, 2, 3, 4, 5, 6, 0x00, 0xc0 };
	/* The extended report's event type takes two bytes. */
	static const u8 ext[] = { HCI_EV_LE_META, 11, HCI_EV_LE_EXT_ADV_REPORT, 1,
				  0x13, 0x00, 0x01, 1, 2, 3, 4, 5, 6 };
	static const u8 adv_short[] = { HCI_EV_LE_META, 2, HCI_EV_LE_ADVERTISING_REPORT, 1 };
	const u8 *addr;

	KUNIT_EXPECT_STREQ(test, bt7932_wake_name(adv, sizeof(adv), &addr),
			   "LE advertising report");
	KUNIT_EXPECT_PTR_EQ(test, addr, &adv[6]);
	KUNIT_EXPECT_STREQ(test, bt7932_wake_name(ext, sizeof(ext), &addr),
			   "LE extended advertising report");
	KUNIT_EXPECT_PTR_EQ(test, addr, &ext[7]);
	bt7932_wake_name(adv_short, sizeof(adv_short), &addr);
	KUNIT_EXPECT_NULL(test, addr);
}

static struct kunit_case bt7932_wake_test_cases[] = {
	KUNIT_CASE(bt7932_wake_event_bredr_test),
	KUNIT_CASE(bt7932_wake_event_le_test),
	KUNIT_CASE(bt7932_wake_event_replies_test),
	KUNIT_CASE(bt7932_wake_event_error_test),
	KUNIT_CASE(bt7932_wake_name_bredr_test),
	KUNIT_CASE(bt7932_wake_name_le_test),
	{}
};

static struct kunit_suite bt7932_wake_test_suite = {
	.name = "mt7932-bt-wake",
	.test_cases = bt7932_wake_test_cases,
};

kunit_test_suite(bt7932_wake_test_suite);

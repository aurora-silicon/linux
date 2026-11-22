// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/unaligned.h>

#include "mt7932.h"
#include "wow.h"

static bool mt_wow_test_zero(const u8 *p, size_t from, size_t to)
{
	size_t i;

	for (i = from; i < to; i++)
		if (p[i])
			return false;
	return true;
}

static void mt_wow_detect_test(struct kunit *test)
{
	struct cfg80211_wowlan wow = {};

	KUNIT_EXPECT_EQ(test, mt7932_wow_detect(NULL), 0);
	KUNIT_EXPECT_EQ(test, mt7932_wow_detect(&wow), 0);
	wow.magic_pkt = true;
	KUNIT_EXPECT_EQ(test, mt7932_wow_detect(&wow), MT7932_WOW_DETECT_MAGIC);
	wow.magic_pkt = false;
	wow.disconnect = true;
	/* Losing the AP's beacons ends the association as surely as a deauth. */
	KUNIT_EXPECT_EQ(test, mt7932_wow_detect(&wow), 0x14);
	wow.magic_pkt = true;
	KUNIT_EXPECT_EQ(test, mt7932_wow_detect(&wow), 0x15);
}

static void mt_wow_ctrl_test(struct kunit *test)
{
	u8 body[MT7932_WOW_CTRL_LENGTH];

	KUNIT_EXPECT_EQ(test, sizeof(body), (size_t)228);
	memset(body, 0xa5, sizeof(body));
	mt7932_wow_ctrl(body, true, 0x15);
	KUNIT_EXPECT_EQ(test, body[0], MT7932_WOW_START);
	KUNIT_EXPECT_EQ(test, body[1], 0x15);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 2), 0x63);
	KUNIT_EXPECT_EQ(test, body[4], 0);	/* scenario */
	KUNIT_EXPECT_EQ(test, body[5], 1);	/* pins */
	KUNIT_EXPECT_TRUE(test, mt_wow_test_zero(body, 6, 8));
	KUNIT_EXPECT_EQ(test, body[8], 3);	/* GPIO */
	KUNIT_EXPECT_EQ(test, body[9], 13);
	KUNIT_EXPECT_EQ(test, body[10], 5);
	/* Interval, second wake signal, port counts and lists. */
	KUNIT_EXPECT_TRUE(test, mt_wow_test_zero(body, 11, sizeof(body)));

	memset(body, 0xa5, sizeof(body));
	mt7932_wow_ctrl(body, false, 0x15);
	KUNIT_EXPECT_EQ(test, body[0], MT7932_WOW_STOP);
	KUNIT_EXPECT_TRUE(test, mt_wow_test_zero(body, 1, 5));
	KUNIT_EXPECT_EQ(test, body[5], 1);
	KUNIT_EXPECT_EQ(test, body[8], 3);
	KUNIT_EXPECT_EQ(test, body[9], 13);
	KUNIT_EXPECT_EQ(test, body[10], 5);
	KUNIT_EXPECT_TRUE(test, mt_wow_test_zero(body, 11, sizeof(body)));
}

static void mt_wow_suspend_mode_test(struct kunit *test)
{
	u8 body[MT7932_SUSPEND_MODE_LENGTH];

	KUNIT_EXPECT_EQ(test, sizeof(body), (size_t)68);
	memset(body, 0xa5, sizeof(body));
	mt7932_suspend_mode(body, true);
	KUNIT_EXPECT_EQ(test, body[0], 0);	/* BSS */
	KUNIT_EXPECT_EQ(test, body[1], 1);
	KUNIT_EXPECT_EQ(test, body[2], 1);	/* DTIM multiple */
	KUNIT_EXPECT_EQ(test, body[3], 1);	/* WoW suspend */
	KUNIT_EXPECT_TRUE(test, mt_wow_test_zero(body, 4, sizeof(body)));
	mt7932_suspend_mode(body, false);
	KUNIT_EXPECT_EQ(test, body[1], 0);
	KUNIT_EXPECT_EQ(test, body[2], 1);
	KUNIT_EXPECT_EQ(test, body[3], 1);
}

static void mt_wow_config_test(struct kunit *test)
{
	static const u8 addr[ETH_ALEN] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	u8 magic[MT7932_WOW_MAGIC_LENGTH], clear[MT7932_WOW_CLEAR_LENGTH];

	KUNIT_EXPECT_EQ(test, sizeof(magic), (size_t)52);
	KUNIT_EXPECT_EQ(test, sizeof(clear), (size_t)44);
	memset(magic, 0xa5, sizeof(magic));
	mt7932_wow_magic(magic, addr);
	KUNIT_EXPECT_TRUE(test, mt_wow_test_zero(magic, 0, 32));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(magic + 32), 0x06);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(magic + 34), 20);
	KUNIT_EXPECT_EQ(test, magic[36], 0);	/* entry */
	KUNIT_EXPECT_EQ(test, magic[37], 1);	/* enabled */
	KUNIT_EXPECT_EQ(test, magic[38], 1);	/* forward */
	KUNIT_EXPECT_EQ(test, magic[39], 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(magic + 40), 0);
	KUNIT_EXPECT_MEMEQ(test, magic + 42, addr, ETH_ALEN);
	KUNIT_EXPECT_TRUE(test, mt_wow_test_zero(magic, 48, sizeof(magic)));

	memset(clear, 0xa5, sizeof(clear));
	mt7932_wow_clear(clear);
	KUNIT_EXPECT_TRUE(test, mt_wow_test_zero(clear, 0, 32));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(clear + 32), 0x18);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(clear + 34), 12);
	KUNIT_EXPECT_TRUE(test, mt_wow_test_zero(clear, 36, sizeof(clear)));
}

static void mt_wow_reason_test(struct kunit *test)
{
	struct cfg80211_wowlan_wakeup wakeup;
	u8 body[MT7932_WOW_REASON_LENGTH] = {};

	KUNIT_EXPECT_EQ(test, mt7932_wow_reason(body, sizeof(body) - 1), -EINVAL);
	body[0] = 9;
	KUNIT_EXPECT_EQ(test, mt7932_wow_reason(body, sizeof(body)), 9);

	KUNIT_EXPECT_FALSE(test, mt7932_wow_wakeup(MT7932_WOW_REASON_NONE, &wakeup));
	KUNIT_EXPECT_TRUE(test, mt7932_wow_wakeup(MT7932_WOW_REASON_MAGIC, &wakeup));
	KUNIT_EXPECT_TRUE(test, wakeup.magic_pkt);
	KUNIT_EXPECT_FALSE(test, wakeup.disconnect);
	KUNIT_EXPECT_EQ(test, wakeup.pattern_idx, -1);
	KUNIT_EXPECT_TRUE(test, mt7932_wow_wakeup(MT7932_WOW_REASON_DISCONNECT, &wakeup));
	KUNIT_EXPECT_TRUE(test, wakeup.disconnect);
	KUNIT_EXPECT_FALSE(test, wakeup.magic_pkt);
	/* A firmware reason with no cfg80211 trigger is still a wireless wake. */
	KUNIT_EXPECT_TRUE(test, mt7932_wow_wakeup(9, &wakeup));
	KUNIT_EXPECT_FALSE(test, wakeup.magic_pkt);
	KUNIT_EXPECT_FALSE(test, wakeup.disconnect);
	KUNIT_EXPECT_EQ(test, wakeup.pattern_idx, -1);
}

#ifdef CONFIG_PM
static void mt_wow_test_work(struct work_struct *work)
{
}

static void mt_wow_test_device_release(struct device *dev)
{
}

static void mt_wow_test_put(void *data)
{
	put_device(data);
}

static void mt_wow_reason_event_test(struct kunit *test)
{
	struct mt7932 *m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);
	u8 *packet = kunit_kzalloc(test, 36 + MT7932_WOW_REASON_LENGTH, GFP_KERNEL);
	struct mt7932_event event = {
		.eid = MT7932_EID_WOW_REASON,
		.packet = packet,
		.length = 36 + MT7932_WOW_REASON_LENGTH,
	};
	unsigned long flags;

	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_ASSERT_NOT_NULL(test, packet);
	m->pdev = kunit_kzalloc(test, sizeof(*m->pdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m->pdev);
	device_initialize(&m->pdev->dev);
	m->pdev->dev.release = mt_wow_test_device_release;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mt_wow_test_put, &m->pdev->dev), 0);
	KUNIT_ASSERT_EQ(test, dev_set_name(&m->pdev->dev, "mt7932-wow-test"), 0);
	spin_lock_init(&m->response_lock);
	INIT_WORK(&m->wow_work, mt_wow_test_work);
	m->wow_reason = MT7932_WOW_REASON_NONE;
	packet[36] = MT7932_WOW_REASON_MAGIC;

	/* Not a sleep WoW was armed for: ignored. */
	spin_lock_irqsave(&m->response_lock, flags);
	mt_wow_reason_event(m, &event);
	spin_unlock_irqrestore(&m->response_lock, flags);
	flush_work(&m->wow_work);
	KUNIT_EXPECT_EQ(test, m->wow_reason, MT7932_WOW_REASON_NONE);

	/* A short event leaves the wait open. */
	m->wow_report = true;
	event.length--;
	spin_lock_irqsave(&m->response_lock, flags);
	mt_wow_reason_event(m, &event);
	spin_unlock_irqrestore(&m->response_lock, flags);
	KUNIT_EXPECT_TRUE(test, m->wow_report);
	KUNIT_EXPECT_EQ(test, m->wow_reason, MT7932_WOW_REASON_NONE);

	/* One report per sleep. */
	event.length++;
	spin_lock_irqsave(&m->response_lock, flags);
	mt_wow_reason_event(m, &event);
	spin_unlock_irqrestore(&m->response_lock, flags);
	flush_work(&m->wow_work);
	KUNIT_EXPECT_FALSE(test, m->wow_report);
	KUNIT_EXPECT_EQ(test, m->wow_reason, MT7932_WOW_REASON_MAGIC);
	packet[36] = MT7932_WOW_REASON_DISCONNECT;
	spin_lock_irqsave(&m->response_lock, flags);
	mt_wow_reason_event(m, &event);
	spin_unlock_irqrestore(&m->response_lock, flags);
	KUNIT_EXPECT_EQ(test, m->wow_reason, MT7932_WOW_REASON_MAGIC);
}
#endif

static struct kunit_case mt_wow_test_cases[] = {
	KUNIT_CASE(mt_wow_detect_test),
	KUNIT_CASE(mt_wow_ctrl_test),
	KUNIT_CASE(mt_wow_suspend_mode_test),
	KUNIT_CASE(mt_wow_config_test),
	KUNIT_CASE(mt_wow_reason_test),
#ifdef CONFIG_PM
	KUNIT_CASE(mt_wow_reason_event_test),
#endif
	{}
};

static struct kunit_suite mt_wow_test_suite = {
	.name = "mt7932-wow",
	.test_cases = mt_wow_test_cases,
};

kunit_test_suite(mt_wow_test_suite);

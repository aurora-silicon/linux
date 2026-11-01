// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "mt7932.h"

static void mt_rf_test_device_release(struct device *dev)
{
	/* The enclosing synthetic PCI device is owned by KUnit. */
}

static void mt_policy_retry_test_work(struct work_struct *work)
{
	struct mt7932 *m = container_of(work, struct mt7932, startup_work);

	m->reg_attempted++;
}

static void mt_rf_test_cleanup(void *data)
{
	struct mt7932 *m = data;

	cancel_work_sync(&m->startup_work);
	free_netdev(m->netdev);
	put_device(&m->pdev->dev);
}

static int mt_rf_test_init(struct kunit *test)
{
	struct mt7932 *m;
	int ret;

	m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->pdev = kunit_kzalloc(test, sizeof(*m->pdev), GFP_KERNEL);
	if (!m->pdev)
		return -ENOMEM;
	device_initialize(&m->pdev->dev);
	m->pdev->dev.release = mt_rf_test_device_release;
	ret = dev_set_name(&m->pdev->dev, "mt7932-rf-test");
	if (ret) {
		put_device(&m->pdev->dev);
		return ret;
	}
	INIT_WORK(&m->startup_work, mt_policy_retry_test_work);
	m->netdev = alloc_etherdev(sizeof(m));
	if (!m->netdev) {
		put_device(&m->pdev->dev);
		return -ENOMEM;
	}
	ret = kunit_add_action_or_reset(test, mt_rf_test_cleanup, m);
	if (ret)
		return ret;
	*(struct mt7932 **)netdev_priv(m->netdev) = m;
	mutex_init(&m->command_mutex);
	spin_lock_init(&m->response_lock);
	spin_lock_init(&m->data_lock);
	init_completion(&m->cal_response);
	init_completion(&m->assoc_start);
	init_completion(&m->assoc_done);
	init_completion(&m->discovery_done);
	m->interface_registered = true;
	m->connecting = true;
	m->peer_valid = true;
	m->rf_ready = true;
	m->cal_state.active = true;
	netif_carrier_on(m->netdev);
	netif_start_queue(m->netdev);
	test->priv = m;
	return 0;
}

static void mt_rf_test_fail(struct mt7932 *m, int error)
{
	unsigned long flags;

	spin_lock_irqsave(&m->response_lock, flags);
	mt_rf_fail_locked(m, error);
	spin_unlock_irqrestore(&m->response_lock, flags);
}

static void mt_rf_failure_wakes_association_test(struct kunit *test)
{
	struct mt7932 *m = test->priv;

	/* Exercise the data-publisher join, without publishing any DMA. */
	smp_store_release(&m->data_ready, true);
	mt_rf_test_fail(m, -ENOENT);

	KUNIT_EXPECT_TRUE(test, completion_done(&m->cal_response));
	KUNIT_EXPECT_TRUE(test, completion_done(&m->assoc_start));
	KUNIT_EXPECT_TRUE(test, completion_done(&m->assoc_done));
	KUNIT_EXPECT_TRUE(test, completion_done(&m->discovery_done));
	KUNIT_EXPECT_EQ(test, m->connect_error, -ENOENT);
	KUNIT_EXPECT_FALSE(test, mt_rf_allowed(m));
	KUNIT_EXPECT_FALSE(test, netif_carrier_ok(m->netdev));
	KUNIT_EXPECT_TRUE(test, netif_queue_stopped(m->netdev));
	/* An error must not claim that the firmware-owned peer was retired. */
	KUNIT_EXPECT_TRUE(test, m->peer_valid);
}

static void mt_rf_failure_blocks_startup_test(struct kunit *test)
{
	struct mt7932 *m = test->priv;

	mt_rf_test_fail(m, -EPROTO);
	/* No BAR/DMA exists: startup must reject before touching hardware. */
	KUNIT_EXPECT_EQ(test, mt_enable_scan(m), -EPROTO);
	KUNIT_EXPECT_FALSE(test, m->rf_ready);
	KUNIT_EXPECT_FALSE(test, m->data_ready);
}

static void mt_rf_failure_preserves_first_error_test(struct kunit *test)
{
	struct mt7932 *m = test->priv;

	mt_rf_test_fail(m, -ENOENT);
	mt_rf_test_fail(m, -EPROTO);
	KUNIT_EXPECT_EQ(test, m->cal_state.error, -ENOENT);
	KUNIT_EXPECT_EQ(test, m->connect_error, -ENOENT);
	KUNIT_EXPECT_EQ(test, mt_enable_scan(m), -ENOENT);
}

static void mt_cal_band_reply_completion_test(struct kunit *test)
{
	struct mt7932_cal_completion state = {};
	u8 request[16] = {}, reply[20] = {};
	unsigned int band, i, expected;

	reply[3] = 1;
	for (band = 0; band < 2; band++) {
		memset(&state, 0, sizeof(state));
		put_unaligned_le32(band, request + 8);
		expected = band ? 3 : 4;
		KUNIT_ASSERT_EQ(test, mt7932_cal_begin(&state,
			mt7932_cal_request_replies(request)), 0);
		/* The actual D6 completion machine must finish on the final
		 * band-specific reply, not time out waiting for a fourth 5 GHz reply.
		 */
		for (i = 0; i < expected; i++) {
			KUNIT_EXPECT_EQ(test, mt7932_cal_null(&state, reply,
				 sizeof(reply), 12), i + 1 == expected ? 1 : 0);
			KUNIT_EXPECT_EQ(test, state.done, i + 1 == expected);
		}
		KUNIT_EXPECT_FALSE(test, state.active);
	}
}

static void mt_missing_policy_admission_retry_test(struct kunit *test)
{
	static const struct cfg80211_ops ops = {};
	struct mt7932 *m = test->priv;
	struct cfg80211_scan_request scan = { .wdev = &m->wdev };
	struct cfg80211_connect_params connect = {};
	struct wiphy *wiphy;
	unsigned int entry;

	wiphy = wiphy_new(&ops, sizeof(m));
	KUNIT_ASSERT_NOT_NULL(test, wiphy);
	*(struct mt7932 **)wiphy_priv(wiphy) = m;
	/* Missing policy has not touched hardware. Actual frontend entry points
	 * must retry that lookup while keeping ordinary RF admission closed.
	 */
	m->rf_ready = false;
	m->reg_pending = true;
	for (entry = 0; entry < 3; entry++) {
		m->reg_retryable = true;
		m->reg_generation = 7;
		m->reg_attempted = 7;
		switch (entry) {
		case 0:
			KUNIT_EXPECT_EQ(test, mt_net_open(m->netdev), 0);
			break;
		case 1:
			KUNIT_EXPECT_EQ(test, mt_scan(wiphy, &scan), -EOPNOTSUPP);
			break;
		default:
			KUNIT_EXPECT_EQ(test, mt_connect(wiphy, m->netdev, &connect), -EAGAIN);
			break;
		}
		/* Assert the actual frontend queued recovery before checking that
		 * another request cannot consume the same retry a second time.
		 */
		KUNIT_EXPECT_EQ(test, m->reg_generation, 8U);
		KUNIT_EXPECT_FALSE(test, m->reg_retryable);
		mt_retry_missing_policy(m);
		flush_work(&m->startup_work);
		KUNIT_EXPECT_EQ(test, m->reg_generation, 8U);
		KUNIT_EXPECT_EQ(test, m->reg_attempted, 8U);
		KUNIT_EXPECT_FALSE(test, m->reg_retryable);
		KUNIT_EXPECT_TRUE(test, m->reg_pending);
		KUNIT_EXPECT_FALSE(test, mt_rf_allowed(m));
	}
	/* Neither a partial policy SET nor terminal calibration may be replayed. */
	m->reg_retryable = true;
	m->policy_failed = true;
	mt_retry_missing_policy(m);
	flush_work(&m->startup_work);
	KUNIT_EXPECT_EQ(test, m->reg_generation, 8U);
	m->policy_failed = false;
	mt_rf_test_fail(m, -EPROTO);
	KUNIT_EXPECT_EQ(test, mt_net_open(m->netdev), -EPROTO);
	flush_work(&m->startup_work);
	KUNIT_EXPECT_EQ(test, m->reg_generation, 8U);
	wiphy_free(wiphy);
}

struct mt_cal_drain_test_context {
	struct kunit *test;
	unsigned int commands, replies;
};

static int mt_cal_drain_test_send(struct mt7932 *m, const void *data, size_t length)
{
	struct mt_cal_drain_test_context *ctx = m->cal_test_context;
	const u8 *body = data;
	u8 reply[20] = {};
	unsigned long flags;
	int ret = 0;

	KUNIT_EXPECT_EQ(ctx->test, m->cal_state.expected, 3U);
	if (m->cal_state.expected != 3 || length < 20)
		return -EPROTO;
	ctx->commands++;
	/* Firmware completes each logical group after its final fragment. */
	if ((body[2] & 15) + 1 != body[2] >> 4)
		return 0;
	reply[3] = 1;
	spin_lock_irqsave(&m->response_lock, flags);
	ret = mt7932_cal_null(&m->cal_state, reply, sizeof(reply), m->smart_version);
	if (ret > 0)
		complete(&m->cal_response);
	spin_unlock_irqrestore(&m->response_lock, flags);
	ctx->replies++;
	return ret < 0 ? ret : 0;
}

static void mt_startup_band1_queue_drain_test(struct kunit *test)
{
	/* Synthetic open-container fixtures only: no unit calibration assets. */
	static const u16 tags[] = { 0x501, 0x2001, 0x4124, 0x3126 };
	static const u16 bytes[] = { 200, 2160, 20, 1224 };
	struct mt_cal_drain_test_context ctx = { .test = test };
	struct mt7932 *m = test->priv;
	struct firmware oca = {};
	size_t size = 16 + 20 * ARRAY_SIZE(tags), offset;
	u8 *data;
	unsigned int i, j;
	int ret;

	for (i = 0; i < ARRAY_SIZE(tags); i++)
		size += 8 + bytes[i];
	data = kunit_kzalloc(test, size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, "BLOB", 4);
	offset = 16 + 20 * ARRAY_SIZE(tags);
	put_unaligned_be32(offset, data + 4);
	put_unaligned_be16(12, data + 8);
	put_unaligned_be16(ARRAY_SIZE(tags), data + 10);
	for (i = 0; i < ARRAY_SIZE(tags); i++) {
		u8 *entry = data + 16 + 20 * i;
		u32 sum = 0;

		put_unaligned_be16(tags[i], entry);
		put_unaligned_be16(12, entry + 2);
		put_unaligned_be32(offset, entry + 4);
		put_unaligned_be32(8 + bytes[i], entry + 8);
		memcpy(data + offset, entry, 4);
		put_unaligned_be32(8 + bytes[i], data + offset + 4);
		for (j = 0; j < 8 + bytes[i]; j++)
			sum += data[offset + j];
		put_unaligned_be32(sum, entry + 12);
		offset += 8 + bytes[i];
	}
	oca.data = data;
	oca.size = size;
	memset(&m->cal_state, 0, sizeof(m->cal_state));
	m->smart_version = 12;
	m->preload_version = 1;
	m->module_byte = 0x89;
	m->cal_request_count = 1;
	put_unaligned_le32(0x1000, m->cal_requests[0] + 4);
	put_unaligned_le32(1, m->cal_requests[0] + 8);
	put_unaligned_le32(36, m->cal_requests[0] + 12);
	m->cal_test_send = mt_cal_drain_test_send;
	m->cal_test_context = &ctx;
	mutex_lock(&m->command_mutex);
	ret = mt_cal_drain_requests(m, &oca);
	mutex_unlock(&m->command_mutex);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx.commands, 5U);
	KUNIT_EXPECT_EQ(test, ctx.replies, 3U);
	KUNIT_EXPECT_EQ(test, m->cal_request_count, 0U);
	KUNIT_EXPECT_TRUE(test, m->cal_state.done);
	KUNIT_EXPECT_FALSE(test, m->cal_state.active);
	KUNIT_EXPECT_EQ(test, m->cal_state.error, 0);
	m->cal_test_send = NULL;
	m->cal_test_context = NULL;
}

struct mt_scan_retire_test_context {
	struct delayed_work retire;
	struct mt7932 *m;
	bool late;
};

static struct mt_scan_retire_test_context *mt_scan_retire_ctx;

/* The firmware reports the end of the cancelled batch a little later. */
static void mt_scan_retire_test_late(struct work_struct *work)
{
	struct mt_scan_retire_test_context *ctx =
		container_of(to_delayed_work(work), struct mt_scan_retire_test_context, retire);
	struct mt7932 *m = ctx->m;
	unsigned long flags;

	spin_lock_irqsave(&m->response_lock, flags);
	m->retired_scan_seq = 0;
	complete(&m->scan_done);
	spin_unlock_irqrestore(&m->response_lock, flags);
}

/*
 * The scan completion work had already submitted the next batch when the
 * abort arrived: the abort cancels it, and its end is still to come.
 */
static void mt_scan_retire_test_finish(struct work_struct *work)
{
	struct mt_scan_retire_test_context *ctx = mt_scan_retire_ctx;
	struct mt7932 *m = ctx->m;
	unsigned long flags;

	spin_lock_irqsave(&m->response_lock, flags);
	if (m->scan_request && m->scan_aborted) {
		m->scan_request = NULL;
		m->retired_scan_seq = 8;
	}
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (ctx->late)
		schedule_delayed_work(&ctx->retire, msecs_to_jiffies(300));
}

static void mt_scan_quiesce_retirement_test(struct kunit *test)
{
	static const struct cfg80211_ops ops = {};
	struct mt_scan_retire_test_context ctx = {};
	struct cfg80211_scan_request *scan;
	struct mt7932 *m = test->priv;
	unsigned long start;
	unsigned int round;

	scan = kunit_kzalloc(test, sizeof(*scan), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, scan);
	m->wiphy = wiphy_new(&ops, sizeof(m));
	KUNIT_ASSERT_NOT_NULL(test, m->wiphy);
	*(struct mt7932 **)wiphy_priv(m->wiphy) = m;
	init_completion(&m->scan_done);
	INIT_WORK(&m->scan_finish_work, mt_scan_retire_test_finish);
	INIT_DELAYED_WORK(&ctx.retire, mt_scan_retire_test_late);
	ctx.m = m;
	mt_scan_retire_ctx = &ctx;
	for (round = 0; round < 2; round++) {
		ctx.late = !round;
		reinit_completion(&m->scan_done);
		m->scan_request = scan;
		scan->wdev = &m->wdev;
		m->scan_aborted = false;
		m->retired_scan_seq = 0;
		/* The batch the quiesce waits for has just finished. */
		complete(&m->scan_done);
		start = jiffies;
		if (ctx.late) {
			/* A late but genuine end retires the scan: no failure. */
			KUNIT_EXPECT_EQ(test, mt_scan_quiesce(m), 0);
			KUNIT_EXPECT_EQ(test, m->retired_scan_seq, 0);
		} else {
			/* A batch the firmware never ends still fails, after the bound. */
			KUNIT_EXPECT_EQ(test, mt_scan_quiesce(m), -EBUSY);
			KUNIT_EXPECT_EQ(test, m->retired_scan_seq, 8);
			KUNIT_EXPECT_GE(test, jiffies - start, msecs_to_jiffies(1900));
			KUNIT_EXPECT_LE(test, jiffies - start, msecs_to_jiffies(3000));
		}
		cancel_delayed_work_sync(&ctx.retire);
		flush_work(&m->scan_finish_work);
	}
	mt_scan_retire_ctx = NULL;
	wiphy_free(m->wiphy);
	m->wiphy = NULL;
}

/*
 * An RF failure while the connect worker is publishing a new connection
 * must not report the disconnect ahead of the connect: it leaves the report
 * to the worker. (cfg80211 is not called here: the synthetic netdev has no
 * wireless device, so a direct report would crash the test.)
 */
static void mt_rf_failure_during_publication_test(struct kunit *test)
{
	struct mt7932 *m = test->priv;

	m->connecting = false;
	m->connected = true;
	m->connect_publishing = true;
	mt_rf_test_fail(m, -EIO);
	KUNIT_EXPECT_TRUE(test, m->connect_lost);
	KUNIT_EXPECT_FALSE(test, m->connected);
	KUNIT_EXPECT_TRUE(test, m->disconnecting);
	KUNIT_EXPECT_FALSE(test, netif_carrier_ok(m->netdev));
	/* Ownership is uncertain after an RF failure: keep the peer. */
	KUNIT_EXPECT_TRUE(test, m->peer_valid);
	m->connect_lost = false;
	m->connect_publishing = false;
}

/* mt_connect() has marked a move but not yet left the old AP. It reports the
 * end of the connection itself once its admission check sees link_failed, so
 * the RF failure must not report it too, nor queue it behind a publishing
 * worker, which mt_connect() joins first. The synthetic netdev has no wdev:
 * a cfg80211 report here would not survive.
 */
static void mt_rf_failure_at_move_start_test(struct kunit *test)
{
	struct mt7932 *m = test->priv;
	unsigned int publishing;

	for (publishing = 0; publishing < 2; publishing++) {
		m->connecting = false;
		m->connected = true;
		m->disconnecting = false;
		m->link_failed = false;
		m->rf_ready = true;
		m->roam_moving = true;
		m->connect_publishing = publishing;
		netif_carrier_on(m->netdev);
		mt_rf_test_fail(m, -EIO);
		KUNIT_EXPECT_FALSE(test, m->connected);
		KUNIT_EXPECT_TRUE(test, m->disconnecting);
		KUNIT_EXPECT_TRUE(test, m->link_failed);
		KUNIT_EXPECT_FALSE(test, m->connect_lost);
		KUNIT_EXPECT_TRUE(test, m->roam_moving);
		KUNIT_EXPECT_FALSE(test, mt_rf_allowed(m));
		KUNIT_EXPECT_FALSE(test, netif_carrier_ok(m->netdev));
		KUNIT_EXPECT_TRUE(test, m->peer_valid);
	}
	m->roam_moving = false;
	m->connect_publishing = false;
}

static void mt_invalid_association_channel_wakes_both_test(struct kunit *test)
{
	struct mt7932 *m = test->priv;
	u8 packet[72] = {};
	struct mt7932_event event = { .packet = packet, .length = sizeof(packet) };
	unsigned long flags;
	unsigned int stage, invalid;

	put_unaligned_le32(0x4d, packet + 40);
	m->connect_channel = 11;
	for (stage = 0; stage < 2; stage++) {
		for (invalid = 0; invalid < 4; invalid++) {
			memset(packet + 48, 0, 24);
			put_unaligned_le32(1, packet + 60);
			packet[65] = 11;
			event.length = sizeof(packet);
			switch (invalid) {
			case 0:
				/* A truncated channel event must end either wait. */
				event.length = 67;
				break;
			case 1:
				put_unaligned_le32(2, packet + 60);
				break;
			case 2:
				packet[65] = 6;
				break;
			default:
				packet[64] = 0xff;
				break;
			}
			m->connect_error = 0;
			reinit_completion(&m->assoc_start);
			reinit_completion(&m->assoc_done);
			if (stage) {
				/* The worker has consumed its first association start. */
				complete(&m->assoc_start);
				KUNIT_ASSERT_TRUE(test, try_wait_for_completion(&m->assoc_start));
			}
			spin_lock_irqsave(&m->response_lock, flags);
			mt_link_event(m, &event);
			spin_unlock_irqrestore(&m->response_lock, flags);
			KUNIT_EXPECT_EQ(test, m->connect_error, -EPROTO);
			KUNIT_EXPECT_TRUE(test, completion_done(&m->assoc_start));
			KUNIT_EXPECT_TRUE(test, completion_done(&m->assoc_done));
			KUNIT_EXPECT_TRUE(test, m->peer_valid);
			KUNIT_EXPECT_TRUE(test, mt_rf_allowed(m));
		}
	}
}

static void mt_valid_repeated_association_start_test(struct kunit *test)
{
	static const struct cfg80211_ops ops = {};
	struct mt7932 *m = test->priv;
	struct ieee80211_channel channel = {
		.band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11,
	};
	struct ieee80211_supported_band band = {
		.channels = &channel, .n_channels = 1, .ht_cap.ht_supported = true,
	};
	u8 packet[72] = {};
	struct mt7932_event event = { .packet = packet, .length = sizeof(packet) };
	struct wiphy *wiphy = wiphy_new(&ops, 0);
	unsigned long flags;
	unsigned int start;

	KUNIT_ASSERT_NOT_NULL(test, wiphy);
	wiphy->bands[NL80211_BAND_2GHZ] = &band;
	m->wiphy = wiphy;
	m->connect_channel = 11;
	put_unaligned_le32(0x4d, packet + 40);
	put_unaligned_le32(1, packet + 60);
	packet[65] = 11;
	for (start = 0; start < 2; start++) {
		spin_lock_irqsave(&m->response_lock, flags);
		mt_link_event(m, &event);
		spin_unlock_irqrestore(&m->response_lock, flags);
		KUNIT_EXPECT_EQ(test, m->connect_error, 0);
		KUNIT_EXPECT_TRUE(test, try_wait_for_completion(&m->assoc_start));
		KUNIT_EXPECT_FALSE(test, completion_done(&m->assoc_done));
		KUNIT_EXPECT_EQ(test, m->connect_center, (u8)11);
		KUNIT_EXPECT_TRUE(test, m->peer_valid);
		KUNIT_EXPECT_TRUE(test, mt_rf_allowed(m));
	}
	m->wiphy = NULL;
	wiphy_free(wiphy);
}

static void mt_late_association_channel_preserves_terminal_test(struct kunit *test)
{
	struct mt7932 *m = test->priv;
	u8 packet[72] = {};
	struct mt7932_event event = { .packet = packet, .length = sizeof(packet) };
	unsigned long flags;
	unsigned int terminal;

	put_unaligned_le32(0x4d, packet + 40);
	for (terminal = 0; terminal < 3; terminal++) {
		m->connecting = terminal != 0;
		m->disconnecting = terminal == 1;
		m->connect_error = terminal == 2 ? -ECANCELED : 0;
		reinit_completion(&m->assoc_start);
		reinit_completion(&m->assoc_done);
		spin_lock_irqsave(&m->response_lock, flags);
		mt_link_event(m, &event);
		spin_unlock_irqrestore(&m->response_lock, flags);
		KUNIT_EXPECT_EQ(test, m->connect_error, terminal == 2 ? -ECANCELED : 0);
		KUNIT_EXPECT_FALSE(test, completion_done(&m->assoc_start));
		KUNIT_EXPECT_FALSE(test, completion_done(&m->assoc_done));
		KUNIT_EXPECT_TRUE(test, m->peer_valid);
	}
}

static struct kunit_case mt_rf_test_cases[] = {
	KUNIT_CASE(mt_invalid_association_channel_wakes_both_test),
	KUNIT_CASE(mt_valid_repeated_association_start_test),
	KUNIT_CASE(mt_late_association_channel_preserves_terminal_test),
	KUNIT_CASE(mt_startup_band1_queue_drain_test),
	KUNIT_CASE(mt_missing_policy_admission_retry_test),
	KUNIT_CASE(mt_cal_band_reply_completion_test),
	KUNIT_CASE(mt_rf_failure_wakes_association_test),
	KUNIT_CASE(mt_rf_failure_blocks_startup_test),
	KUNIT_CASE(mt_rf_failure_preserves_first_error_test),
	KUNIT_CASE(mt_scan_quiesce_retirement_test),
	KUNIT_CASE(mt_rf_failure_during_publication_test),
	KUNIT_CASE(mt_rf_failure_at_move_start_test),
	{}
};

static struct kunit_suite mt_rf_test_suite = {
	.name = "mt7932-rf-failure",
	.init = mt_rf_test_init,
	.test_cases = mt_rf_test_cases,
};

kunit_test_suite(mt_rf_test_suite);

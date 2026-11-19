// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <kunit/static_stub.h>

#include "mt7932.h"

static const u8 test_mac[ETH_ALEN] = { 0x02, 1, 2, 3, 4, 5 };

static void mt7932_awdl_queue_test(struct kunit *test)
{
	u8 body[32];

	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_queue(body, 0, false, true), 32);
	KUNIT_EXPECT_MEMEQ(test, body, ((u8[]){ 2, 0, 1, 1 }), 4);
	KUNIT_EXPECT_EQ(test, body[14], 1);
	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_queue(body, 0, false, false), 32);
	KUNIT_EXPECT_MEMEQ(test, body, ((u8[]){ 0, 0, 1, 1 }), 4);
	KUNIT_EXPECT_MEMEQ(test, body + 4, ((u8[28]) {}), 28);
	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_queue(body, 7, true, true), 32);
	KUNIT_EXPECT_MEMEQ(test, body, ((u8[]){ 15, 0, 5, 0 }), 4);
	KUNIT_EXPECT_EQ(test, body[14], 1);
	KUNIT_EXPECT_EQ(test, body[15], 7);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 16), 15);
	KUNIT_EXPECT_MEMEQ(test, body + 20,
			   ((u8[]){ 1, 0, 0, 1, 2, 2, 3, 3 }), 8);
	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_queue(body, 7, true, false), 32);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 16), 2);
}

static void mt7932_awdl_active_test(struct kunit *test)
{
	u8 body[12];

	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_bss_active(body, test_mac, 9, true), 12);
	KUNIT_EXPECT_EQ(test, body[0], 1);
	KUNIT_EXPECT_EQ(test, body[1], 1);
	KUNIT_EXPECT_EQ(test, body[2], 5);
	KUNIT_EXPECT_EQ(test, body[3], 1);
	KUNIT_EXPECT_MEMEQ(test, body + 4, test_mac, ETH_ALEN);
	KUNIT_EXPECT_EQ(test, body[10], 9);
	KUNIT_EXPECT_EQ(test, body[11], 1);
	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_bss_active(body, test_mac, 9, false), 12);
	KUNIT_EXPECT_EQ(test, body[1], 0);
	KUNIT_EXPECT_EQ(test, body[3], 1);
	KUNIT_EXPECT_MEMEQ(test, body + 4, ((u8[6]) {}), ETH_ALEN);
}

static void mt7932_awdl_bss_test(struct kunit *test)
{
	u8 body[MT7932_AWDL_BSS_RECORD_LEN];

	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_bss(body, 9, 1, 0x7a, 2), 116);
	KUNIT_EXPECT_MEMEQ(test, body, ((u8[]){ 1, 1, 7, 0 }), 4);
	KUNIT_EXPECT_EQ(test, body[42], 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 44), 0x3fc0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 46), 0x0540);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 50), 0x004b);
	KUNIT_EXPECT_EQ(test, body[55], 0x7a);
	KUNIT_EXPECT_EQ(test, body[58], 9);
	KUNIT_EXPECT_MEMEQ(test, body + 68, ((u8[]){ 1, 2, 44, 0 }), 4);
	KUNIT_EXPECT_EQ(test, body[81], 1);
	KUNIT_EXPECT_EQ(test, body[82], 1);
	KUNIT_EXPECT_EQ(test, body[83], 2);
	KUNIT_EXPECT_EQ(test, body[84], 155);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 86), 0x0540);
	KUNIT_EXPECT_EQ(test, body[88], 2);
	KUNIT_EXPECT_EQ(test, body[89], 2);
	KUNIT_EXPECT_MEMEQ(test, body + 90, ((u8[]){ 4, 1, 0, 2 }), 4);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 96), 0x3ff0);
	KUNIT_EXPECT_MEMEQ(test, body + 104, ((u8[12]) {}), 12);
}

static void mt7932_awdl_sta_test(struct kunit *test)
{
	struct mt7932_awdl_peer_caps caps = {
		.phy = 0x7a, .rates = 0x3fc0, .ht_mcs = 0xff, .mcs32 = 1,
		.rx_mcs = { 0xff, 0xff }, .rx_highest = 867,
		.ht_cap = 0x1234, .ht_ext_cap = 0x5678, .tx_bf_cap = 0x11223344,
		.ampdu = 0x1b, .asel = 2, .rcpi = 80,
		.vht_cap = 0x55667788, .vht_rx_mcs = 0xfffa,
		.vht_rx_highest = 866, .vht_tx_mcs = 0xfffa,
		.vht_tx_highest = 866, .nss = 2, .he = { 0xaa, 0xbb },
	};
	u8 body[MT7932_AWDL_STA_RECORD_LEN];

	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_sta(body, 3, 4, test_mac, &caps,
						     true, true), 200);
	KUNIT_EXPECT_MEMEQ(test, body,
			   ((u8[]){ 3, 0x10, 2, 1, 2, 3, 4, 5 }), 8);
	KUNIT_EXPECT_EQ(test, body[12], 1);
	KUNIT_EXPECT_EQ(test, body[13], 0x7a);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 14), 0x3fc0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 16), 0x0540);
	KUNIT_EXPECT_EQ(test, body[18], 1);
	KUNIT_EXPECT_EQ(test, body[20], 2);
	KUNIT_EXPECT_EQ(test, body[23], 1);
	KUNIT_EXPECT_MEMEQ(test, body + 24, caps.rx_mcs, 10);
	KUNIT_EXPECT_EQ(test, body[51], 1);
	KUNIT_EXPECT_EQ(test, body[54], 4);
	KUNIT_EXPECT_EQ(test, body[55], 0xff);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 56), 0x55667788);
	KUNIT_EXPECT_EQ(test, body[68], 2);
	KUNIT_EXPECT_EQ(test, body[125], 1);
	KUNIT_EXPECT_MEMEQ(test, body + 136, caps.he, 34);
	KUNIT_EXPECT_MEMEQ(test, body + 170, ((u8[30]) {}), 30);
	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_sta(body, 3, 4, test_mac, &caps,
						     false, true), 200);
	KUNIT_EXPECT_EQ(test, body[20], 0);
}

static void mt7932_awdl_tlv_test(struct kunit *test)
{
	u8 body[8] = { 1, 0, 0, 0, 0x44, 0x33, 0x22, 0x11 };
	u8 out[20];

	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_tlv(out, sizeof(out), 0x0e,
						     body, sizeof(body)), 20);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(out), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(out + 2), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(out + 4), 0x0e);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(out + 8), 8);
	KUNIT_EXPECT_MEMEQ(test, out + 12, body, sizeof(body));
}

static void mt7932_awdl_schedule_test(struct kunit *test)
{
	const u8 channels[] = { 149, 6 };
	const u8 classes[] = { 128, 81 };
	u8 body[134];

	KUNIT_ASSERT_EQ(test, mt7932_awdl_build_channel_sequence(body, channels,
							  classes, 2), 134);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body), 1);
	KUNIT_EXPECT_EQ(test, body[4], 4);
	KUNIT_EXPECT_EQ(test, body[5], 2);
	KUNIT_EXPECT_MEMEQ(test, body + 6, channels, 2);
	KUNIT_EXPECT_MEMEQ(test, body + 70, classes, 2);
	KUNIT_EXPECT_MEMEQ(test, body + 72, ((u8[62]) {}), 62);
}

static void mt7932_awdl_data_test(struct kunit *test)
{
	u8 ethernet[64] = { 0 }, wrapped[80];
	size_t length;
	int ret;

	memcpy(ethernet, "\x33\x33\x00\x00\x00\xfb\x02\x01\x02\x03\x04\x05", 12);
	put_unaligned_be16(ETH_P_IPV6, ethernet + 12);
	memset(ethernet + 14, 0xa5, 50);
	ret = mt7932_awdl_encap(wrapped, sizeof(wrapped), ethernet, sizeof(ethernet),
				 0x1234);
	KUNIT_ASSERT_EQ(test, ret, 80);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(wrapped + 12), 66);
	KUNIT_EXPECT_MEMEQ(test, wrapped + 14,
			   ((u8[]){ 0xaa, 0xaa, 3, 0, 0x17, 0xf2,
				      8, 0, 3, 4 }), 10);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(wrapped + 24), 0x1234);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(wrapped + 28), ETH_P_IPV6);
	length = ret;
	KUNIT_ASSERT_EQ(test, mt7932_awdl_decap(wrapped, &length), 0);
	KUNIT_EXPECT_EQ(test, length, sizeof(ethernet));
	KUNIT_EXPECT_MEMEQ(test, wrapped, ethernet, sizeof(ethernet));
}

static void mt7932_awdl_rx_test(struct kunit *test)
{
	u8 packet[24 + ETH_HLEN] = {};
	struct mt7932_rx_frame frame;

	put_unaligned_le32(sizeof(packet) | (2U << 27), packet);
	put_unaligned_le32(4, packet + 4);
	put_unaligned_le32(1 | BIT(13), packet + 8);
	put_unaligned_le32((149U << 8) | BIT(16), packet + 12);
	put_unaligned_be16(ETH_P_IP, packet + 24 + 12);
	KUNIT_ASSERT_EQ(test, mt7932_rx_frame(packet, sizeof(packet), &frame), 0);
	KUNIT_EXPECT_TRUE(test, frame.translated);
	KUNIT_EXPECT_EQ(test, frame.bss, 1);
	KUNIT_EXPECT_EQ(test, frame.wtbl, 4);
	KUNIT_EXPECT_EQ(test, frame.channel, 149);
}

static void mt7932_awdl_event_test(struct kunit *test)
{
	u8 packet[52] = {};
	struct mt7932_event event = {
		.eid = 0xf7, .packet = packet, .length = sizeof(packet),
	};
	struct mt7932_awdl_event_info info;

	put_unaligned_le32(7, packet + 40);
	KUNIT_ASSERT_EQ(test, mt7932_awdl_event_parse(&event, &info), 0);
	KUNIT_EXPECT_EQ(test, info.subtype, 7U);
	KUNIT_EXPECT_PTR_EQ(test, info.data, packet + 44);
	KUNIT_EXPECT_EQ(test, info.length, (size_t)8);
	event.length = 43;
	KUNIT_EXPECT_EQ(test, mt7932_awdl_event_parse(&event, &info), -EMSGSIZE);
}

/* Capture the real AWDL control paths without a PCI device or DMA rings. */
struct mt_awdl_control_test {
	struct mt7932 m;
	unsigned int requests;
	unsigned int fail_request;
	unsigned long station_owned, wtbl_owned, wmm_owned;
	u8 own_mac[2][ETH_ALEN];
	u8 fail_cid;
};

static int mt_awdl_test_request(struct mt7932 *m, u8 cid, bool runtime,
				bool set, bool wait, const void *payload,
				size_t length)
{
	struct kunit *test = kunit_get_current_test();
	struct mt_awdl_control_test *ctx = test->priv;
	const u8 *body = payload;

	ctx->requests++;
	KUNIT_EXPECT_PTR_EQ(test, m, &ctx->m);
	KUNIT_EXPECT_TRUE(test, runtime);
	KUNIT_EXPECT_TRUE(test, set);
	switch (cid) {
	case 0x6b:
		KUNIT_EXPECT_EQ(test, length, (size_t)32);
		if (body[2] == 1) {
			KUNIT_EXPECT_FALSE(test, wait);
			KUNIT_EXPECT_EQ(test, body[3], MT7932_AWDL_BSS_INDEX);
			KUNIT_EXPECT_EQ(test, get_unaligned_le16(body), body[14] ? 2 : 0);
		} else {
			KUNIT_EXPECT_EQ(test, body[2], 5);
			KUNIT_EXPECT_TRUE(test, wait);
			KUNIT_EXPECT_EQ(test, BIT(body[15]) & ctx->station_owned, 0UL);
		}
		KUNIT_EXPECT_EQ(test, body[14], body[0] ? 1 : 0);
		break;
	case 0x11:
		KUNIT_EXPECT_EQ(test, length, (size_t)12);
		KUNIT_EXPECT_EQ(test, body[3], 1);
		KUNIT_EXPECT_EQ(test, body[10], m->awdl.bmc_wtbl);
		KUNIT_EXPECT_EQ(test, BIT(body[10]) & ctx->wtbl_owned, 0UL);
		/* Model the hardware identity write, not just the outer BSS ID. */
		if (body[3] < ARRAY_SIZE(ctx->own_mac))
			memcpy(ctx->own_mac[body[3]], body + 4, ETH_ALEN);
		KUNIT_EXPECT_MEMEQ(test, ctx->own_mac[0], m->mac, ETH_ALEN);
		KUNIT_EXPECT_FALSE(test, wait);
		KUNIT_EXPECT_EQ(test, body[0], MT7932_AWDL_BSS_INDEX);
		break;
	case 0x12:
		KUNIT_EXPECT_EQ(test, length, (size_t)MT7932_AWDL_BSS_RECORD_LEN);
		KUNIT_EXPECT_FALSE(test, wait);
		KUNIT_EXPECT_EQ(test, body[0], MT7932_AWDL_BSS_INDEX);
		KUNIT_EXPECT_EQ(test, body[68], MT7932_AWDL_BSS_INDEX);
		KUNIT_EXPECT_EQ(test, body[58], m->awdl.bmc_wtbl);
		KUNIT_EXPECT_EQ(test, BIT(body[58]) & ctx->wtbl_owned, 0UL);
		KUNIT_EXPECT_EQ(test, body[91], m->awdl.wmm);
		KUNIT_EXPECT_EQ(test, BIT(body[91]) & ctx->wmm_owned, 0UL);
		/* No infrastructure AP station or peer reference in the AWDL BSS. */
		KUNIT_EXPECT_EQ(test, body[48], 0);
		KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 94), 0);
		break;
	case 0x13:
		KUNIT_EXPECT_TRUE(test, wait);
		KUNIT_EXPECT_EQ(test, body[12], MT7932_AWDL_BSS_INDEX);
		KUNIT_EXPECT_EQ(test, BIT(body[0]) & ctx->station_owned, 0UL);
		KUNIT_EXPECT_EQ(test, BIT(body[54]) & ctx->wtbl_owned, 0UL);
		KUNIT_EXPECT_NE(test, body[54], m->awdl.bmc_wtbl);
		break;
	case 0xf7:
		KUNIT_EXPECT_FALSE(test, wait);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 12), 1U);
		break;
	default:
		KUNIT_FAIL(test, "unexpected AWDL CID %02x", cid);
	}
	return cid == ctx->fail_cid || ctx->requests == ctx->fail_request ? -ENOSPC : 0;
}

static void mt_awdl_test_free_wiphy(void *data)
{
	wiphy_free(data);
}

static void mt_awdl_test_free_netdev(void *data)
{
	free_netdev(data);
}

static struct mt_awdl_control_test *mt_awdl_control_init(struct kunit *test)
{
	static const struct cfg80211_ops ops;
	struct mt_awdl_control_test *ctx;
	struct mt7932 *m;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	m = &ctx->m;
	m->wiphy = wiphy_new_nm(&ops, sizeof(m), "mt7932-awdl-test");
	KUNIT_ASSERT_NOT_NULL(test, m->wiphy);
	ret = kunit_add_action_or_reset(test, mt_awdl_test_free_wiphy, m->wiphy);
	KUNIT_ASSERT_EQ(test, ret, 0);
	*(struct mt7932 **)wiphy_priv(m->wiphy) = m;
	m->awdl.netdev = alloc_etherdev(0);
	KUNIT_ASSERT_NOT_NULL(test, m->awdl.netdev);
	ret = kunit_add_action_or_reset(test, mt_awdl_test_free_netdev, m->awdl.netdev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	eth_hw_addr_set(m->awdl.netdev, test_mac);
	m->wdev.iftype = NL80211_IFTYPE_STATION;
	m->awdl.wdev.iftype = NL80211_IFTYPE_STATION;
	mutex_init(&m->command_mutex);
	spin_lock_init(&m->response_lock);
	m->phy_cap[4] = 2;
	/* Synthetic infrastructure ownership must survive all AWDL operations. */
	m->peer_ids.station[0] = 1;
	m->peer_ids.station[1] = 2;
	m->peer_ids.wtbl[0] = 3;
	m->peer_ids.wtbl[1] = 4;
	m->peer_ids.wtbl[2] = 5;
	m->peer_ids.wtbl[3] = 6;
	m->station_alloc = BIT(0) | BIT(1) | BIT(2);
	m->wtbl_alloc = BIT(0) | BIT(3) | BIT(4) | BIT(5) | BIT(6);
	m->wmm_alloc = BIT(0);
	ctx->station_owned = m->station_alloc;
	ctx->wtbl_owned = m->wtbl_alloc;
	ctx->wmm_owned = m->wmm_alloc;
	ether_addr_copy(m->mac, ((u8[]){ 0x02, 6, 7, 8, 9, 10 }));
	ether_addr_copy(ctx->own_mac[0], m->mac);
	test->priv = ctx;
	kunit_activate_static_stub(test, mt_request, mt_awdl_test_request);
	return ctx;
}

static void mt7932_awdl_routing_test(struct kunit *test)
{
	struct mt_awdl_control_test *ctx = mt_awdl_control_init(test);
	struct mt7932 *m = &ctx->m;
	struct wireless_dev other = { .iftype = NL80211_IFTYPE_STATION };
	struct wireless_dev *wdevs[] = { &m->wdev, &other, &m->awdl.wdev };
	unsigned int state, i;

	for (state = 0; state < 4; state++) {
		m->awdl.interface_up = state & 1;
		m->awdl.active = state & 2;
		for (i = 0; i < ARRAY_SIZE(wdevs); i++) {
			int expected = i < 2 ? -EOPNOTSUPP :
				       state == 3 ? 0 : -ENETDOWN;
			int ret;

			KUNIT_EXPECT_EQ(test, mt7932_awdl_station_route(&m->awdl, wdevs[i]),
					expected);
			if (!expected)
				continue;
			/* NULL params catch any access before routing/admission. */
			ret = mt_awdl_add_station(m->wiphy, wdevs[i], test_mac, NULL);
			KUNIT_EXPECT_EQ(test, ret, expected);
			ret = mt_awdl_change_station(m->wiphy, wdevs[i], test_mac, NULL);
			KUNIT_EXPECT_EQ(test, ret, expected);
			ret = mt_awdl_del_station(m->wiphy, wdevs[i], NULL);
			KUNIT_EXPECT_EQ(test, ret, expected);
		}
	}
	KUNIT_EXPECT_EQ(test, ctx->requests, 0U);
}

static void mt7932_awdl_lifecycle_test(struct kunit *test)
{
	struct mt_awdl_control_test *ctx = mt_awdl_control_init(test);
	struct mt7932 *m = &ctx->m;
	unsigned long station = m->station_alloc, wtbl = m->wtbl_alloc;
	struct mt7932_peer_ids ids = m->peer_ids;
	struct mt7932_ring ring = { .head = 7, .tail = 2, .queued = 5 };
	unsigned int cycle, before;

	m->bss_active = true;
	m->connected = true;
	m->data_ready = true;
	m->tx[0] = ring;
	for (cycle = 0; cycle < 3; cycle++) {
		before = ctx->requests;
		KUNIT_ASSERT_EQ(test, mt_awdl_activate(m), 0);
		KUNIT_EXPECT_EQ(test, ctx->requests, before);
		KUNIT_EXPECT_EQ(test, m->wtbl_alloc, wtbl);
		m->awdl.interface_up = true;
		KUNIT_ASSERT_EQ(test, mt_awdl_activate(m), 0);
		KUNIT_EXPECT_TRUE(test, m->awdl.active);
		KUNIT_EXPECT_EQ(test, ctx->requests, before + 6);
		KUNIT_EXPECT_EQ(test, m->station_alloc, station);
		KUNIT_EXPECT_FALSE(test, m->awdl.sync_enabled);
		KUNIT_ASSERT_EQ(test, mt_awdl_activate(m), 0);
		KUNIT_EXPECT_EQ(test, ctx->requests, before + 6);
		m->awdl.interface_up = false;
		KUNIT_ASSERT_EQ(test, mt_awdl_deactivate(m), 0);
		KUNIT_EXPECT_EQ(test, ctx->requests, before + 10);
		KUNIT_EXPECT_FALSE(test, m->awdl.active);
		KUNIT_EXPECT_EQ(test, m->station_alloc, station);
		KUNIT_EXPECT_EQ(test, m->wtbl_alloc, wtbl);
		KUNIT_EXPECT_EQ(test, m->wmm_alloc, BIT(0));
		KUNIT_EXPECT_MEMEQ(test, &m->peer_ids, &ids, sizeof(ids));
		KUNIT_EXPECT_MEMEQ(test, &m->tx[0], &ring, sizeof(ring));
		KUNIT_EXPECT_TRUE(test, m->bss_active);
		KUNIT_EXPECT_TRUE(test, m->connected);
		KUNIT_EXPECT_TRUE(test, m->data_ready);
	}
	/* A failed submission still rolls BSS 1 back without touching BSS 0. */
	ctx->fail_cid = 0x12;
	m->awdl.interface_up = true;
	before = ctx->requests;
	KUNIT_EXPECT_EQ(test, mt_awdl_activate(m), -ENOSPC);
	KUNIT_EXPECT_EQ(test, ctx->requests, before + 4);
	KUNIT_EXPECT_FALSE(test, m->awdl.active);
	KUNIT_EXPECT_EQ(test, m->station_alloc, station);
	KUNIT_EXPECT_EQ(test, m->wtbl_alloc, wtbl);
	KUNIT_EXPECT_EQ(test, m->wmm_alloc, BIT(0));
}

static void mt7932_awdl_peer_routing_test(struct kunit *test)
{
	struct mt_awdl_control_test *ctx = mt_awdl_control_init(test);
	struct mt7932 *m = &ctx->m;
	struct station_parameters params = {};
	struct station_del_parameters del = { .mac = test_mac };
	unsigned long station = m->station_alloc, wtbl = m->wtbl_alloc;

	m->awdl.interface_up = true;
	KUNIT_ASSERT_EQ(test, mt_awdl_activate(m), 0);
	KUNIT_ASSERT_EQ(test, mt_awdl_add_station(m->wiphy, &m->awdl.wdev,
						  test_mac, &params), 0);
	KUNIT_EXPECT_EQ(test, ctx->requests, 8U);
	KUNIT_EXPECT_TRUE(test, m->awdl.peers[0].live);
	KUNIT_ASSERT_EQ(test, mt_awdl_change_station(m->wiphy, &m->awdl.wdev,
						     test_mac, &params), 0);
	KUNIT_EXPECT_EQ(test, ctx->requests, 8U);
	KUNIT_ASSERT_EQ(test, mt_awdl_del_station(m->wiphy, &m->awdl.wdev, &del), 0);
	KUNIT_EXPECT_EQ(test, ctx->requests, 10U);
	KUNIT_EXPECT_EQ(test, m->station_alloc, station);
	m->awdl.interface_up = false;
	KUNIT_ASSERT_EQ(test, mt_awdl_deactivate(m), 0);
	KUNIT_EXPECT_EQ(test, m->wtbl_alloc, wtbl);
}

static void mt7932_awdl_rollback_test(struct kunit *test)
{
	struct mt_awdl_control_test *ctx = mt_awdl_control_init(test);
	struct mt7932 *m = &ctx->m;
	unsigned int step;

	/* Fail each sync-off activation submission, including BSSID/templates. */
	for (step = 1; step <= 6; step++) {
		ctx->fail_request = ctx->requests + step;
		m->awdl.interface_up = true;
		KUNIT_EXPECT_EQ(test, mt_awdl_activate(m), -ENOSPC);
		KUNIT_EXPECT_FALSE(test, m->awdl.active);
		KUNIT_EXPECT_EQ(test, m->station_alloc, ctx->station_owned);
		KUNIT_EXPECT_EQ(test, m->wtbl_alloc, ctx->wtbl_owned);
		KUNIT_EXPECT_EQ(test, m->wmm_alloc, ctx->wmm_owned);
		KUNIT_EXPECT_MEMEQ(test, ctx->own_mac[0], m->mac, ETH_ALEN);
	}
}

static void mt7932_awdl_exhaustion_test(struct kunit *test)
{
	struct mt_awdl_control_test *ctx = mt_awdl_control_init(test);
	struct mt7932 *m = &ctx->m;

	m->awdl.interface_up = true;
	m->wtbl_alloc = GENMASK(13, 0);
	KUNIT_EXPECT_EQ(test, mt_awdl_activate(m), -ENOSPC);
	KUNIT_EXPECT_EQ(test, m->wtbl_alloc, GENMASK(13, 0));
	m->wtbl_alloc = ctx->wtbl_owned;
	m->wmm_alloc = BIT(0) | BIT(1);
	KUNIT_EXPECT_EQ(test, mt_awdl_activate(m), -ENOSPC);
	KUNIT_EXPECT_EQ(test, m->wtbl_alloc, ctx->wtbl_owned);
	KUNIT_EXPECT_EQ(test, m->wmm_alloc, BIT(0) | BIT(1));
	KUNIT_EXPECT_EQ(test, ctx->requests, 0U);
}

static struct kunit_case mt7932_awdl_cases[] = {
	KUNIT_CASE(mt7932_awdl_routing_test),
	KUNIT_CASE(mt7932_awdl_lifecycle_test),
	KUNIT_CASE(mt7932_awdl_rollback_test),
	KUNIT_CASE(mt7932_awdl_exhaustion_test),
	KUNIT_CASE(mt7932_awdl_peer_routing_test),
	KUNIT_CASE(mt7932_awdl_queue_test),
	KUNIT_CASE(mt7932_awdl_active_test),
	KUNIT_CASE(mt7932_awdl_bss_test),
	KUNIT_CASE(mt7932_awdl_sta_test),
	KUNIT_CASE(mt7932_awdl_tlv_test),
	KUNIT_CASE(mt7932_awdl_schedule_test),
	KUNIT_CASE(mt7932_awdl_data_test),
	KUNIT_CASE(mt7932_awdl_rx_test),
	KUNIT_CASE(mt7932_awdl_event_test),
	{}
};

static struct kunit_suite mt7932_awdl_suite = {
	.name = "mt7932-awdl",
	.test_cases = mt7932_awdl_cases,
};

kunit_test_suite(mt7932_awdl_suite);


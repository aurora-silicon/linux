// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/unaligned.h>
#include <net/cfg80211.h>

#include "association.h"
#include "bandwidth.h"
#include "calibration.h"
#include "capabilities.h"
#include "scan.h"

static void mt7932_channel_identity_test(struct kunit *test)
{
	unsigned int i, channel, psc = 0;
	enum nl80211_band band;
	u8 primary;

	KUNIT_EXPECT_EQ(test, mt7932_channel_band(NL80211_BAND_2GHZ, 5), 1U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_band(NL80211_BAND_5GHZ, 5), 0U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_band(NL80211_BAND_6GHZ, 5), 3U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_band(NL80211_BAND_6GHZ, 37), 3U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_band(NL80211_BAND_5GHZ, 149), 2U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_band(NL80211_BAND_6GHZ, 149), 3U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_band(NUM_NL80211_BANDS, 5), 0U);
	for (i = 0; i < MT7932_CHANNELS_6G; i++) {
		channel = mt7932_channel_6g(i);
		KUNIT_EXPECT_EQ(test, channel, 1 + 4 * i);
		KUNIT_EXPECT_EQ(test, mt7932_channel_band(NL80211_BAND_6GHZ, channel), 3U);
		KUNIT_EXPECT_EQ(test, ieee80211_channel_to_frequency(channel, NL80211_BAND_6GHZ),
				5955 + 20 * i);
		KUNIT_ASSERT_TRUE(test, mt7932_channel_from_rx(181 + i, &band, &primary));
		KUNIT_EXPECT_EQ(test, band, NL80211_BAND_6GHZ);
		KUNIT_EXPECT_EQ(test, (unsigned int)primary, channel);
		psc += mt7932_channel_psc(NL80211_BAND_6GHZ, channel);
	}
	KUNIT_EXPECT_EQ(test, psc, 15U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_6g(MT7932_CHANNELS_6G), 0U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_6g(UINT_MAX), 0U);
	KUNIT_EXPECT_FALSE(test, mt7932_channel_psc(NL80211_BAND_2GHZ, 5));
	KUNIT_EXPECT_FALSE(test, mt7932_channel_psc(NL80211_BAND_5GHZ, 149));
	for (i = 0; i < 256; i++) {
		bool valid = i >= 1 && i <= 233 && i % 4 == 1;

		KUNIT_EXPECT_EQ(test, !!mt7932_channel_band(NL80211_BAND_6GHZ, i), valid);
		if (i >= 240)
			KUNIT_EXPECT_FALSE(test, mt7932_channel_from_rx(i, &band, &primary));
	}
}

static void mt7932_channel_legacy_test(struct kunit *test)
{
	unsigned int i;
	enum nl80211_band band;
	u8 channel, center, bw;

	for (i = 1; i <= MT7932_CHANNELS_2G; i++) {
		KUNIT_ASSERT_TRUE(test, mt7932_channel_from_rx(i, &band, &channel));
		KUNIT_EXPECT_EQ(test, band, NL80211_BAND_2GHZ);
		KUNIT_EXPECT_EQ(test, (unsigned int)channel, i);
		KUNIT_ASSERT_TRUE(test, mt7932_channel_layout(band, channel, 0, 0, 2,
							      &center, &bw));
		KUNIT_EXPECT_EQ(test, center, channel);
		KUNIT_EXPECT_EQ(test, bw, 0);
	}
	for (i = 0; i < MT7932_CHANNELS_5G; i++) {
		u8 primary = mt7932_channel_5g(i);

		KUNIT_ASSERT_TRUE(test, mt7932_channel_from_rx(primary, &band, &channel));
		KUNIT_EXPECT_EQ(test, band, NL80211_BAND_5GHZ);
		KUNIT_EXPECT_EQ(test, channel, primary);
	}
	KUNIT_EXPECT_FALSE(test, mt7932_channel_from_rx(0, &band, &channel));
	KUNIT_EXPECT_FALSE(test, mt7932_channel_from_rx(14, &band, &channel));
	KUNIT_EXPECT_FALSE(test, mt7932_channel_from_rx(180, &band, &channel));
	KUNIT_EXPECT_TRUE(test, mt7932_channel_layout(NL80211_BAND_5GHZ, 36, 1, 1, 2,
						      &center, &bw));
	KUNIT_EXPECT_EQ(test, center, 42);
	KUNIT_EXPECT_EQ(test, bw, 2);
	KUNIT_EXPECT_TRUE(test, mt7932_channel_layout(NL80211_BAND_6GHZ, 5, 0, 0, 2,
						      &center, &bw));
}

static void mt7932_band_scan_test(struct kunit *test)
{
	static const u8 channels[] = {5, 37, 149};
	u8 *body;

	body = kunit_kzalloc(test, 0x4d4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, body);
	KUNIT_ASSERT_EQ(test, mt7932_scan_body(body, 0x4d4, 1, NL80211_BAND_6GHZ,
					       channels, ARRAY_SIZE(channels)), 0x4d4);
	KUNIT_EXPECT_EQ(test, body[0xa0], 3);
	KUNIT_EXPECT_EQ(test, body[0xa1], 5);
	KUNIT_EXPECT_EQ(test, body[0xa2], 3);
	KUNIT_EXPECT_EQ(test, body[0xa3], 37);
	mt7932_scan_disconnected_passive(body);
	KUNIT_EXPECT_EQ(test, body[2], 0);
	KUNIT_EXPECT_EQ(test, body[3], 0);
	KUNIT_EXPECT_EQ(test, mt7932_scan_options(body, NULL, 0, NULL, 0), 0);
	KUNIT_EXPECT_LT(test, mt7932_scan_options(body, (const u8 *)"hidden", 6, NULL, 0), 0);
	KUNIT_ASSERT_EQ(test, mt7932_scan_body(body, 0x4d4, 1, NL80211_BAND_6GHZ,
					       channels, 1), 0x4d4);
	KUNIT_EXPECT_EQ(test, mt7932_scan_options(body, (const u8 *)"hidden", 6, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, body[2], 1);
	KUNIT_EXPECT_EQ(test, body[3], 8);
	KUNIT_EXPECT_EQ(test, body[4], 1);
	/* Further directed SSIDs fill the next slots; wildcards take none. */
	KUNIT_EXPECT_EQ(test, mt7932_scan_ssid_slot(body, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, mt7932_scan_ssid_slot(body, (const u8 *)"second", 6), 0);
	KUNIT_EXPECT_EQ(test, body[4], 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 8 + 36), 6);
	KUNIT_EXPECT_EQ(test, memcmp(body + 12 + 36, "second", 6), 0);
	KUNIT_EXPECT_EQ(test, memcmp(body + 12, "hidden", 6), 0);
	KUNIT_EXPECT_EQ(test, mt7932_scan_ssid_slot(body, (const u8 *)"c", 1), 0);
	KUNIT_EXPECT_EQ(test, mt7932_scan_ssid_slot(body, (const u8 *)"d", 1), 0);
	KUNIT_EXPECT_EQ(test, body[4], 4);
	KUNIT_EXPECT_EQ(test, mt7932_scan_ssid_slot(body, (const u8 *)"e", 1), -ENOSPC);
	KUNIT_EXPECT_EQ(test, body[0xa0], 3);
	KUNIT_EXPECT_EQ(test, body[0xa1], 5);
	KUNIT_EXPECT_EQ(test, mt7932_scan_home(body, NL80211_BAND_6GHZ, 5), 0);
	KUNIT_EXPECT_EQ(test, body[0x4cd], 3);
	KUNIT_EXPECT_EQ(test, mt7932_scan_home(body, NL80211_BAND_2GHZ, 5), 0);
	KUNIT_EXPECT_EQ(test, body[0x4cd], 1);
	KUNIT_EXPECT_EQ(test, mt7932_scan_home(body, NUM_NL80211_BANDS, 0), 0);
	KUNIT_EXPECT_EQ(test, body[0x4cd], 0);
	KUNIT_EXPECT_LT(test, mt7932_scan_body(body, 0x4d4, 1, NL80211_BAND_2GHZ,
					       channels, ARRAY_SIZE(channels)), 0);
}

static void mt7932_he_6g_test(struct kunit *test)
{
	struct ieee80211_supported_band band5 = {};
	struct ieee80211_sband_iftype_data data;

	mt7932_band_capabilities(&band5, true);
	mt7932_he_6g_capabilities(&data, &band5);
	KUNIT_EXPECT_TRUE(test, data.he_cap.has_he);
	KUNIT_EXPECT_EQ(test, data.types_mask, BIT(NL80211_IFTYPE_STATION));
	KUNIT_EXPECT_EQ(test, le16_to_cpu(data.he_cap.he_mcs_nss_supp.rx_mcs_80), 0xfffa);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(data.he_6ghz_capa.capa), 0xb8);
	KUNIT_EXPECT_EQ(test, mt7932_channel_6g_center(5, true), 7U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_6g_center(233, false), 0U);
	KUNIT_EXPECT_EQ(test, mt7932_channel_6g_center(225, true), 0U);
}

static void mt7932_scan_6g_policy_test(struct kunit *test)
{
	struct cfg80211_ssid ssids[2] = { {}, { .ssid_len = 6 } };
	struct cfg80211_scan_request request = { .ssids = ssids };
	struct ieee80211_channel channel = {
		.band = NL80211_BAND_6GHZ, .hw_value = 5,
	};
	u64 legacy = BIT_ULL(1) | BIT_ULL(9);
	unsigned int i;

	KUNIT_EXPECT_TRUE(test, mt7932_scan_listen(&request, &channel, legacy));
	request.n_ssids = 1; /* wildcard on a PSC still listens */
	KUNIT_EXPECT_TRUE(test, mt7932_scan_listen(&request, &channel, legacy));
	request.n_ssids = 2;
	KUNIT_EXPECT_FALSE(test, mt7932_scan_listen(&request, &channel, legacy));
	KUNIT_EXPECT_TRUE(test, mt7932_scan_listen(&request, &channel, 0));
	request.flags = NL80211_SCAN_FLAG_COLOCATED_6GHZ;
	/* RNR constraints must not become an unconstrained directed probe,
	 * including a PSC with psc_no_listen and a non-PSC target channel.
	 */
	for (i = 1; i <= 233; i += 4) {
		channel.hw_value = i;
		KUNIT_EXPECT_TRUE(test,
				  mt7932_scan_listen(&request, &channel, ~0ULL));
	}
	request.flags = 0;
	request.n_6ghz_params = 1;
	channel.hw_value = 37;
	KUNIT_EXPECT_TRUE(test, mt7932_scan_listen(&request, &channel, legacy));
	request.n_6ghz_params = 0;
	channel.flags = IEEE80211_CHAN_NO_IR;
	KUNIT_EXPECT_TRUE(test, mt7932_scan_listen(&request, &channel, legacy));
	channel.flags = IEEE80211_CHAN_RADAR;
	KUNIT_EXPECT_TRUE(test, mt7932_scan_listen(&request, &channel, legacy));
	channel.flags = 0;
	channel.band = NL80211_BAND_2GHZ;
	request.flags = NL80211_SCAN_FLAG_COLOCATED_6GHZ;
	KUNIT_EXPECT_FALSE(test, mt7932_scan_listen(&request, &channel, legacy));
}

static void mt7932_scan_6g_golden_test(struct kunit *test)
{
	static const u8 channels[] = { 5, 37, 149 };
	static const u8 expected[0x4d4] = {
		[0] = 1, [6] = 0x12, [7] = 1,
		[0x9a] = 40, [0x9e] = 4, [0x9f] = 3,
		[0xa0] = 3, 5, 3, 37, 3, 149,
		[0xe0] = 21,
		[0xe2] = 0x7f, 0x0a, 1, 0, 8, 0x8c, 1, 0x40, 0, 0, 0, 0,
		0x6b, 7, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		[0x33c] = 40, [0x4b9] = 0x18, [0x4bc] = 45,
		[0x4c2] = 220, [0x4ca] = 233,
	};
	u8 *body = kunit_kmalloc(test, sizeof(expected), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, body);
	KUNIT_ASSERT_EQ(test, mt7932_scan_body(body, sizeof(expected), 1,
					       NL80211_BAND_6GHZ, channels, 3),
			0x4d4);
	KUNIT_EXPECT_EQ(test, mt7932_scan_discovery_timeout(body), 5000U);
	mt7932_scan_disconnected_passive(body);
	KUNIT_EXPECT_MEMEQ(test, body, expected, sizeof(expected));
	for (i = 1; i <= MT7932_SCAN_CHANNELS; i++) {
		body[0x9f] = i;
		KUNIT_EXPECT_EQ(test, mt7932_scan_discovery_timeout(body),
				5000U + 233 * i);
	}
	KUNIT_EXPECT_EQ(test, mt7932_scan_discovery_timeout(body), 12456U);
	KUNIT_EXPECT_GT(test, mt7932_scan_discovery_timeout(body), 32U * 220);
}

/* Association-start group selection does not establish runtime D7's band-2
 * procedures. Refuse before touching the container or a partial send plan.
 */
static void mt7932_cal_6g_runtime_rejected_test(struct kunit *test)
{
	static const u32 upper[] = { 0, 0x1000, 0x1001, U32_MAX };
	struct mt7932_cal_piece pieces[7], untouched[7];
	u8 request[16] = {};
	unsigned int channel, i, tag;
	int ret;

	memset(untouched, 0xa5, sizeof(untouched));
	put_unaligned_le32(2, request + 8);
	for (i = 0; i < ARRAY_SIZE(upper); i++) {
		put_unaligned_le32(upper[i], request + 4);
		for (channel = 1; channel <= 233; channel++) {
			put_unaligned_le32(channel, request + 12);
			memcpy(pieces, untouched, sizeof(pieces));
			tag = 0xffff;
			ret = mt7932_cal_requested(NULL, 0, request, 16, 12, 0x89, pieces, &tag);
			KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
			KUNIT_EXPECT_EQ(test, tag, 0U);
			KUNIT_EXPECT_MEMEQ(test, pieces, untouched, sizeof(pieces));
		}
	}
}

static void mt7932_cal_6g_test(struct kunit *test)
{
	u8 blob[52] = {};
	struct mt7932_cal_piece pieces[2];
	unsigned int i, tag, sum = 0;

	/* Synthetic raw-container metadata, no board calibration data. */
	memcpy(blob, "BLOB", 4);
	put_unaligned_be32(36, blob + 4);
	put_unaligned_be16(12, blob + 8);
	put_unaligned_be16(1, blob + 10);
	put_unaligned_be16(0x2009, blob + 16);
	put_unaligned_be16(12, blob + 18);
	put_unaligned_be32(36, blob + 20);
	put_unaligned_be32(16, blob + 24);
	memcpy(blob + 36, blob + 16, 4);
	put_unaligned_be32(16, blob + 40);
	for (i = 36; i < sizeof(blob); i++)
		sum += blob[i];
	put_unaligned_be32(sum, blob + 28);
	KUNIT_ASSERT_EQ(test, mt7932_cal_association(blob, sizeof(blob), NL80211_BAND_6GHZ,
						     5, pieces, &tag), 2);
	KUNIT_EXPECT_EQ(test, tag, 0x2009U);
	KUNIT_EXPECT_EQ(test, pieces[0].parameter, 0x040a0009U);
	KUNIT_EXPECT_EQ(test, pieces[0].length, 4U);
	KUNIT_EXPECT_EQ(test, pieces[1].fragment, 0x21);
}

static void mt7932_6g_security_test(struct kunit *test)
{
	static const struct mt7932_peer_ids ids = {{1, 2}, {3, 4, 5, 6}};
	static const u8 bssid[] = {2, 0, 0, 0, 0, 1};
	u8 pmk[32] = {}, *body;
	int ret;

	body = kunit_kzalloc(test, 856, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, body);
	ret = mt7932_connect_body(body, 856, (const u8 *)"test", 4, pmk, bssid,
				  NL80211_BAND_6GHZ, 5, 20, 2, &ids, false,
				  (const u8 *)"password", 8, false);
	KUNIT_ASSERT_EQ(test, ret, 856);
	KUNIT_EXPECT_EQ(test, body[1], 8);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 0x44), 10U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 0x48), 0);
	KUNIT_EXPECT_EQ(test, body[0x338], 3);
	KUNIT_EXPECT_EQ(test, body[0x1c4], BIT(1));
	ret = mt7932_connect_body(body, 856, (const u8 *)"test", 4, NULL, bssid,
				  NL80211_BAND_6GHZ, 5, 20, 2, &ids, false, NULL, 0, true);
	KUNIT_ASSERT_EQ(test, ret, 856);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 0x20c), BIT(18));
	ret = mt7932_connect_body(body, 856, (const u8 *)"test", 4, pmk, bssid,
				  NL80211_BAND_6GHZ, 5, 20, 2, &ids, false, NULL, 0, false);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt7932_connect_body(body, 856, (const u8 *)"test", 4, NULL, bssid,
				  NL80211_BAND_6GHZ, 5, 20, 2, &ids, true, NULL, 0, false);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_FALSE(test,
			   mt7932_connect_band_allowed(NL80211_BAND_6GHZ,
						       WLAN_AKM_SUITE_SAE, NL80211_MFP_REQUIRED,
						       NL80211_SAE_PWE_HUNT_AND_PECK));
	KUNIT_EXPECT_TRUE(test,
			  mt7932_connect_band_allowed(NL80211_BAND_6GHZ,
						      WLAN_AKM_SUITE_SAE, NL80211_MFP_REQUIRED,
						      NL80211_SAE_PWE_HASH_TO_ELEMENT));
	KUNIT_EXPECT_FALSE(test,
			   mt7932_connect_band_allowed(NL80211_BAND_6GHZ,
						       WLAN_AKM_SUITE_OWE, NL80211_MFP_OPTIONAL,
						       NL80211_SAE_PWE_UNSPECIFIED));
}

static struct kunit_case mt7932_channel_cases[] = {
	KUNIT_CASE(mt7932_channel_identity_test),
	KUNIT_CASE(mt7932_channel_legacy_test),
	KUNIT_CASE(mt7932_band_scan_test),
	KUNIT_CASE(mt7932_he_6g_test),
	KUNIT_CASE(mt7932_6g_security_test),
	KUNIT_CASE(mt7932_cal_6g_test),
	KUNIT_CASE(mt7932_scan_6g_policy_test),
	KUNIT_CASE(mt7932_scan_6g_golden_test),
	KUNIT_CASE(mt7932_cal_6g_runtime_rejected_test),
	{}
};

static struct kunit_suite mt7932_channel_suite = {
	.name = "mt7932-six-ghz",
	.test_cases = mt7932_channel_cases,
};

kunit_test_suite(mt7932_channel_suite);

MODULE_LICENSE("GPL");

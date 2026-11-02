// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "association.h"
#include "regulatory.h"

/* Standard RSN IE: CCMP, AKM 6, MFPC, empty PMKID list, BIP-CMAC-128. */
static const u8 pmf_rsn[] = {
	48, 26, 1, 0,  0,   15, 172, 4, 1, 0, 0, 15, 172, 4,
	1,  0,	0, 15, 172, 6,	128, 0, 0, 0, 0, 15, 172, 6,
};

static void mt_pmf_admission_test(struct kunit *test)
{
	u8 ies[2 * sizeof(pmf_rsn)], pmk[32] = {};
	struct cfg80211_connect_params request = {
		.crypto = {
			.wpa_versions = NL80211_WPA_VERSION_2,
			.n_ciphers_pairwise = 1,
			.ciphers_pairwise = {WLAN_CIPHER_SUITE_CCMP},
			.cipher_group = WLAN_CIPHER_SUITE_CCMP,
			.n_akm_suites = 1,
			.akm_suites = {WLAN_AKM_SUITE_PSK_SHA256},
			.psk = pmk,
		},
	};
	bool pmf;
	unsigned int i;

	for (i = NL80211_MFP_REQUIRED; i <= NL80211_MFP_OPTIONAL; i++) {
		request.mfp = i;
		KUNIT_EXPECT_EQ(test, mt7932_connect_auth(&request),
				WLAN_AKM_SUITE_PSK_SHA256);
		KUNIT_EXPECT_EQ(test,
				mt7932_connect_admit(pmf_rsn, sizeof(pmf_rsn),
						     WLAN_AKM_SUITE_PSK_SHA256,
						     i, &pmf),
				0);
		KUNIT_EXPECT_TRUE(test, pmf);
	}
	request.mfp = NL80211_MFP_NO;
	KUNIT_EXPECT_LT(test, mt7932_connect_auth(&request), 0);
	request.mfp = NL80211_MFP_REQUIRED;
	request.crypto.wpa_versions = NL80211_WPA_VERSION_3;
	KUNIT_EXPECT_LT(test, mt7932_connect_auth(&request), 0);
	request.crypto.wpa_versions = NL80211_WPA_VERSION_2;
	request.crypto.n_akm_suites = 2;
	request.crypto.akm_suites[0] = WLAN_AKM_SUITE_PSK;
	request.crypto.akm_suites[1] = WLAN_AKM_SUITE_PSK_SHA256;
	KUNIT_EXPECT_LT(test, mt7932_connect_auth(&request), 0);
	KUNIT_EXPECT_LT(test,
			mt7932_connect_admit(pmf_rsn, sizeof(pmf_rsn),
					     WLAN_AKM_SUITE_PSK_SHA256,
					     NL80211_MFP_NO, &pmf),
			0);
	for (i = 0; i < sizeof(pmf_rsn); i++)
		KUNIT_EXPECT_LT(test,
				mt7932_connect_admit(pmf_rsn, i,
						     WLAN_AKM_SUITE_PSK_SHA256,
					NL80211_MFP_REQUIRED, &pmf),
				0);
	memcpy(ies, pmf_rsn, sizeof(pmf_rsn));
	ies[20] = 0; /* no MFPC */
	KUNIT_EXPECT_LT(test,
			mt7932_connect_admit(ies, sizeof(pmf_rsn),
					     WLAN_AKM_SUITE_PSK_SHA256,
					     NL80211_MFP_REQUIRED, &pmf),
			0);
	ies[20] = 64; /* MFPR without MFPC */
	KUNIT_EXPECT_LT(test,
			mt7932_connect_admit(ies, sizeof(pmf_rsn),
					     WLAN_AKM_SUITE_PSK_SHA256,
					     NL80211_MFP_OPTIONAL, &pmf),
			0);
	ies[20] = 128;
	ies[27] = 11; /* unqualified BIP-GMAC-128 */
	KUNIT_EXPECT_LT(test,
			mt7932_connect_admit(ies, sizeof(pmf_rsn),
					     WLAN_AKM_SUITE_PSK_SHA256,
					     NL80211_MFP_REQUIRED, &pmf),
			0);
	memcpy(ies, pmf_rsn, sizeof(pmf_rsn));
	memcpy(ies + sizeof(pmf_rsn), pmf_rsn, sizeof(pmf_rsn));
	KUNIT_EXPECT_LT(test,
			mt7932_connect_admit(ies, sizeof(ies),
					     WLAN_AKM_SUITE_PSK_SHA256,
					     NL80211_MFP_REQUIRED, &pmf),
			0);
	ies[1] =
		20; /* no PMKID list or explicit management cipher: CMAC default */
	KUNIT_EXPECT_EQ(test,
			mt7932_connect_admit(ies, 22, WLAN_AKM_SUITE_PSK_SHA256,
					     NL80211_MFP_REQUIRED, &pmf),
			0);
	ies[19] = 2; /* legacy PSK cannot satisfy required PMF */
	KUNIT_EXPECT_LT(test,
			mt7932_connect_admit(ies, 22, WLAN_AKM_SUITE_PSK,
					     NL80211_MFP_REQUIRED, &pmf),
			0);
	KUNIT_EXPECT_EQ(test,
			mt7932_connect_admit(ies, 22, WLAN_AKM_SUITE_PSK,
					     NL80211_MFP_OPTIONAL, &pmf),
			0);
	KUNIT_EXPECT_FALSE(test, pmf);
	ies[20] |= 64;
	KUNIT_EXPECT_LT(test,
			mt7932_connect_admit(ies, 22, WLAN_AKM_SUITE_PSK,
					     NL80211_MFP_OPTIONAL, &pmf),
			0);
}

static void mt_pmf_body_test(struct kunit *test)
{
	static const struct mt7932_peer_ids ids = { { 1, 2 }, { 3, 4, 5, 6 } };
	static const u8 peer[] = { 2, 0, 0, 0, 0, 1 };
	u8 *body, *legacy, pmk[32] = {};

	body = kunit_kzalloc(test, 856, GFP_KERNEL);
	legacy = kunit_kzalloc(test, 856, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, body);
	KUNIT_ASSERT_NOT_NULL(test, legacy);
	KUNIT_ASSERT_EQ(test,
			mt7932_connect_body(body, 856, "test", 4, pmk, peer,
					    NL80211_BAND_5GHZ, 36, 20, 2, &ids,
					    false, NULL, 0, false),
			856);
	memcpy(legacy, body, 856);
	KUNIT_ASSERT_EQ(test,
			mt7932_connect_psk_akm(body, 856,
					       WLAN_AKM_SUITE_PSK_SHA256),
			0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 0x14), 0x0400);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 0x40), 32U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 0x44), 6U);
	KUNIT_EXPECT_MEMEQ(test, body + 0x4c, pmk, 32);
	KUNIT_ASSERT_EQ(test,
			mt7932_connect_psk_akm(body, 856, WLAN_AKM_SUITE_PSK), 0);
	KUNIT_EXPECT_MEMEQ(test, body, legacy,
			   856); /* only selected AKM changed */
	KUNIT_EXPECT_LT(test,
			mt7932_connect_psk_akm(body, 855,
					       WLAN_AKM_SUITE_PSK_SHA256),
			0);
}

/* HE operation + uniform default-client PSD TPE at -1 dBm/MHz. */
static const u8 lpi_ies[] = {
	255, 12, 36, 0, 0, 2, 0, 0, 0, 5, 2, 7, 0, 6, 195, 2, 24, 254,
};

static void mt_6g_ap_test(struct kunit *test)
{
	struct mt7932_6g_ap ap;
	u8 ies[sizeof(lpi_ies) + 14];
	unsigned int i;

	KUNIT_ASSERT_EQ(test,
			mt7932_6g_ap_parse(&ap, lpi_ies, sizeof(lpi_ies), 5, 0),
			0);
	KUNIT_EXPECT_EQ(test, ap.mode, MT7932_6G_LPI);
	KUNIT_EXPECT_EQ(test, ap.count, 4);
	KUNIT_EXPECT_EQ(test, ap.limit[0], 24);
	KUNIT_EXPECT_EQ(test, ap.limit[3], 42);
	for (i = 0; i < sizeof(lpi_ies); i++)
		KUNIT_EXPECT_LT(test, mt7932_6g_ap_parse(&ap, lpi_ies, i, 5, 0),
				0);
	memcpy(ies, lpi_ies, sizeof(lpi_ies));
	ies[10] |= 8; /* SP */
	KUNIT_EXPECT_LT(test,
			mt7932_6g_ap_parse(&ap, ies, sizeof(lpi_ies), 5, 0), 0);
	ies[10] = 2 | 16; /* VLP */
	KUNIT_EXPECT_EQ(test,
			mt7932_6g_ap_parse(&ap, ies, sizeof(lpi_ies), 5, 0), 0);
	KUNIT_EXPECT_EQ(test, ap.mode, MT7932_6G_VLP);
	KUNIT_EXPECT_LT(test,
			mt7932_6g_ap_parse(&ap, ies, sizeof(lpi_ies), 5,
					   IEEE80211_CHAN_NO_6GHZ_VLP_CLIENT),
			0);
	ies[10] = 2 | 120; /* reserved cannot authorize LPI */
	KUNIT_EXPECT_LT(test,
			mt7932_6g_ap_parse(&ap, ies, sizeof(lpi_ies), 5, 0), 0);
	memcpy(ies, lpi_ies, sizeof(lpi_ies));
	KUNIT_EXPECT_LT(test,
			mt7932_6g_ap_parse(&ap, ies, sizeof(lpi_ies), 5,
					   IEEE80211_CHAN_NO_IR),
			0);
	KUNIT_EXPECT_LT(test,
			mt7932_6g_ap_parse(&ap, ies, sizeof(lpi_ies), 9, 0), 0);
	memcpy(ies + sizeof(lpi_ies), lpi_ies, 14);
	KUNIT_EXPECT_LT(test, mt7932_6g_ap_parse(&ap, ies, sizeof(ies), 5, 0),
			0);
	memcpy(ies, lpi_ies, sizeof(lpi_ies));
	ies[17] =
		0x80; /* PSD forbids transmission, not a very low finite cap. */
	KUNIT_EXPECT_EQ(test,
			mt7932_6g_ap_parse(&ap, ies, sizeof(lpi_ies), 5, 0),
			-EACCES);
	/* TPE alone cannot select LPI. */
	KUNIT_EXPECT_LT(test, mt7932_6g_ap_parse(&ap, lpi_ies + 14, 4, 5, 0),
			0);
}

static void mt_6g_tpe_body_test(struct kunit *test)
{
	static const s8 gain[3][4] = { { 2, 4, 6, 8 },
				       { 4, 6, 8, 10 },
				       { 6, 8, 10, 12 } };
	struct mt7932_6g_ap ap = {
		.mode = MT7932_6G_LPI,
		.primary = 5,
		.count = 3,
		.limit = { 30, -10, 127, 0 },
	};
	u8 body[61];

	memset(body, 0xa5, sizeof(body));
	KUNIT_ASSERT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), 60);
	KUNIT_EXPECT_EQ(test, body[60], 0xa5);
	KUNIT_EXPECT_EQ(test, body[0], 0);
	KUNIT_EXPECT_EQ(test, body[1], 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(body + 2), 60);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(body + 4), 0x03010301U);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(body + 8, 0, 36), NULL);
	KUNIT_EXPECT_EQ(test, body[44], 1);
	KUNIT_EXPECT_EQ(test, body[45], 3);
	KUNIT_EXPECT_EQ(test, body[46], 5);
	KUNIT_EXPECT_EQ(test, body[47], 3);
	KUNIT_EXPECT_EQ(test, body[48], 28);
	KUNIT_EXPECT_EQ(test, (s8)body[49], -14);
	KUNIT_EXPECT_EQ(test, body[52], 26);
	KUNIT_EXPECT_EQ(test, body[56], 24);
	KUNIT_EXPECT_EQ(test, body[50], 127);
	KUNIT_EXPECT_EQ(test, body[51], 127);
	KUNIT_EXPECT_EQ(test, body[58], 127);
	KUNIT_EXPECT_EQ(test, body[59], 127);
	KUNIT_EXPECT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, NULL),
			-EOPNOTSUPP);
	ap.limit[0] = -128;
	KUNIT_EXPECT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), -ERANGE);
	memset(ap.limit, 127, 4);
	KUNIT_EXPECT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, NULL), 60);
	KUNIT_EXPECT_EQ(test, mt7932_6g_tpe_body(body, 59, &ap, gain), -EINVAL);
	ap.count = 5;
	KUNIT_EXPECT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), -EINVAL);
}

/* t-3573 section 3: bandwidth-specific boundary rows and all six groups. */
static void mt_6g_j700_gain_test(struct kunit *test)
{
	static const struct {
		u8 channel;
		u8 group[4];
	} vectors[] = {
		{ 1, { 1, 1, 1, 1 } },	 { 29, { 1, 1, 1, 1 } },
		{ 33, { 2, 2, 2, 2 } },	 { 61, { 2, 2, 2, 2 } },
		{ 65, { 3, 3, 3, 3 } },	 { 93, { 3, 3, 3, 3 } },
		{ 97, { 4, 4, 4, 4 } },	 { 109, { 4, 4, 4, 4 } },
		{ 113, { 4, 4, 5, 4 } }, { 117, { 5, 4, 5, 4 } },
		{ 121, { 5, 5, 5, 4 } }, { 125, { 5, 5, 5, 4 } },
		{ 129, { 5, 5, 5, 5 } }, { 181, { 5, 5, 5, 5 } },
		{ 185, { 5, 6, 5, 5 } }, { 189, { 6, 6, 5, 5 } },
		{ 193, { 6, 6, 6, 6 } }, { 221, { 6, 6, 6, 6 } },
		{ 225, { 6, 6, 0, 0 } }, { 229, { 6, 6, 0, 0 } },
		{ 233, { 6, 0, 0, 0 } }, { 2, { 1, 0, 0, 0 } },
	};
	static const s8 expected[][3] = {
		{ 16, 16, 10 }, { 14, 14, 8 },	{ 15, 16, 10 },
		{ 15, 15, 9 },	{ 16, 16, 10 }, { 15, 16, 10 },
	};
	s8 gain[3];
	unsigned int i, bw;

	for (i = 0; i < ARRAY_SIZE(vectors); i++) {
		for (bw = 0; bw < 4; bw++) {
			int ret = mt7932_j700_6g_gain(vectors[i].channel, bw,
						      gain);
			u8 group = vectors[i].group[bw];

			if (!group) {
				KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
				continue;
			}
			KUNIT_ASSERT_EQ(test, ret, 0);
			KUNIT_EXPECT_MEMEQ(test, gain, expected[group - 1], 3);
		}
	}
	KUNIT_EXPECT_EQ(test, mt7932_j700_6g_gain(0, 0, gain), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt7932_j700_6g_gain(3, 0, gain), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt7932_j700_6g_gain(237, 0, gain), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt7932_j700_6g_gain(5, 4, gain), -EINVAL);
}

static void mt_6g_finite_projection_test(struct kunit *test)
{
	struct mt7932_6g_ap ap = {
		.mode = MT7932_6G_LPI,
		.primary = 117,
		.count = 4,
		.limit = { 40, 40, 40, 40 },
	};
	/* Channel 117 uses G5/G4/G5/G4, not a frequency-only group. */
	static const u8 expected[] = { 24, 25, 24, 25, 24, 25,
				       24, 25, 30, 31, 30, 31 };
	s8 gain[3][4];
	u8 body[60], lpi[60];

	KUNIT_ASSERT_EQ(test, mt7932_j700_6g_gains(&ap, gain), 0);
	KUNIT_ASSERT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), 60);
	KUNIT_EXPECT_MEMEQ(test, body + 48, expected, sizeof(expected));
	memcpy(lpi, body, 60);
	ap.mode = MT7932_6G_VLP;
	KUNIT_ASSERT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), 60);
	KUNIT_EXPECT_MEMEQ(test, lpi, body, 60);
	/* PSD -1 dBm/MHz => 24/30/36/42 half-dBm EIRP; G1 gains. */
	KUNIT_ASSERT_EQ(test,
			mt7932_6g_ap_parse(&ap, lpi_ies, sizeof(lpi_ies), 5, 0),
			0);
	KUNIT_ASSERT_EQ(test, mt7932_j700_6g_gains(&ap, gain), 0);
	KUNIT_ASSERT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), 60);
	KUNIT_EXPECT_EQ(test, body[48], 8);
	KUNIT_EXPECT_EQ(test, body[51], 26);
	KUNIT_EXPECT_EQ(test, body[56], 14);
	KUNIT_EXPECT_EQ(test, body[59], 32);
	ap.primary = 233;
	KUNIT_EXPECT_EQ(test, mt7932_j700_6g_gains(&ap, gain), -EOPNOTSUPP);
	memset(ap.limit, 127, 4);
	ap.limit[0] = 40;
	KUNIT_ASSERT_EQ(test, mt7932_j700_6g_gains(&ap, gain), 0);
	KUNIT_ASSERT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), 60);
	KUNIT_EXPECT_EQ(test, body[49], 127);
	/* Signed subtraction, overflow in both directions, and sentinel. */
	ap.count = 1;
	ap.limit[0] = 0;
	gain[0][0] = -7;
	KUNIT_ASSERT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), 60);
	KUNIT_EXPECT_EQ(test, body[48], 7);
	ap.limit[0] = 126;
	KUNIT_EXPECT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), -ERANGE);
	ap.limit[0] = -128;
	gain[0][0] = 1;
	KUNIT_EXPECT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), -ERANGE);
}

static void mt_6g_legacy_vlp_finite_test(struct kunit *test)
{
	struct mt7932_6g_ap ap;
	u8 ies[sizeof(lpi_ies)];

	memcpy(ies, lpi_ies, sizeof(ies));
	ies[10] |= 16; /* VLP, ordinary finite default-client PSD TPE */
	/* Legacy admission selects no new class/TPE SETs, regardless of TPE.
	 * The caller retains the two original post-CID6b cached pages.
	 */
	KUNIT_EXPECT_EQ(test,
			mt7932_6g_connect_ap(&ap, ies, sizeof(ies), 5, 0,
					     BIT_ULL(1)),
			1);
	KUNIT_EXPECT_EQ(test,
			mt7932_6g_connect_ap(&ap, ies, 14, 5, 0, BIT_ULL(1)), 1);
	ies[10] = 3 | 16; /* AP advertises 160 MHz; STA still negotiates <=80 */
	KUNIT_EXPECT_EQ(test,
			mt7932_6g_connect_ap(&ap, ies, sizeof(ies), 5, 0,
					     BIT_ULL(1)),
			1);
	ies[10] = 2 | 16;
	KUNIT_EXPECT_EQ(test,
			mt7932_6g_connect_ap(&ap, ies, sizeof(ies), 5, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, ap.mode, MT7932_6G_VLP);
	KUNIT_EXPECT_EQ(test, ap.limit[0], 24);
	ies[10] = 2; /* LPI must never take the legacy VLP bypass. */
	KUNIT_EXPECT_EQ(test,
			mt7932_6g_connect_ap(&ap, ies, sizeof(ies), 5, 0,
					     BIT_ULL(1)),
			0);
	KUNIT_EXPECT_EQ(test, ap.mode, MT7932_6G_LPI);
}

static void mt_6g_tpe_golden_test(struct kunit *test)
{
	/* Default-client EIRP: 20/40/80 MHz caps, including a signed cap and
	 * the no-limit sentinel. The unrelated subordinate envelope is ignored.
	 */
	static const u8 eirp[] = { 18, 40, 0xf6, 127 };
	static const u8 subordinate[] = { 82, 0x80, 0x80, 0x80 };
	static const u8 expected[60] = {
		[1] = 2, [2] = 60, [4] = 1, 3, 1, 3,
		[44] = 1, 3, 5, 3,
		[48] = 24, 0xe6, 127, 127,
		24, 0xe6, 127, 127, 30, 0xec, 127, 127,
	};
	struct mt7932_6g_ap ap = {
		.mode = MT7932_6G_LPI, .primary = 5,
		.limit = { 127, 127, 127, 127 },
	};
	s8 gain[3][4];
	u8 body[60];

	KUNIT_ASSERT_EQ(test, mt7932_6g_tpe(&ap, eirp, sizeof(eirp)), 0);
	KUNIT_ASSERT_EQ(test,
			mt7932_6g_tpe(&ap, subordinate, sizeof(subordinate)), 0);
	KUNIT_ASSERT_EQ(test, mt7932_j700_6g_gains(&ap, gain), 0);
	KUNIT_ASSERT_EQ(test, mt7932_6g_tpe_body(body, 60, &ap, gain), 60);
	KUNIT_EXPECT_MEMEQ(test, body, expected, sizeof(expected));
}

static struct kunit_case mt_pmf_lpi_cases[] = {
	KUNIT_CASE(mt_pmf_admission_test),
	KUNIT_CASE(mt_pmf_body_test),
	KUNIT_CASE(mt_6g_ap_test),
	KUNIT_CASE(mt_6g_tpe_body_test),
	KUNIT_CASE(mt_6g_j700_gain_test),
	KUNIT_CASE(mt_6g_finite_projection_test),
	KUNIT_CASE(mt_6g_legacy_vlp_finite_test),
	KUNIT_CASE(mt_6g_tpe_golden_test),
	{}
};

static struct kunit_suite mt_pmf_lpi_suite = {
	.name = "mt7932-pmf-lpi",
	.test_cases = mt_pmf_lpi_cases,
};

kunit_test_suite(mt_pmf_lpi_suite);

MODULE_LICENSE("GPL");

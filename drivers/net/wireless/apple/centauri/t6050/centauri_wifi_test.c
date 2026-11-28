// SPDX-License-Identifier: GPL-2.0-only
/* Included by centauri_wifi.c to exercise the connect request checks. */
#include <kunit/test.h>

struct connect_case {
	const char *name;
	enum nl80211_auth_type auth_type;
	bool privacy;
	bool key;
	bool wpa;
	bool akm;
	bool cipher;
	bool group;
	bool sae_pwd;
	bool rsn;
	int expected;
};

static const struct connect_case connect_cases[] = {
	{ "open system", NL80211_AUTHTYPE_OPEN_SYSTEM, .expected = 1 },
	{ "automatic", NL80211_AUTHTYPE_AUTOMATIC, .expected = 1 },
	{ "shared key", NL80211_AUTHTYPE_SHARED_KEY, .expected = -EINVAL },
	{ "shared key with ciphers", NL80211_AUTHTYPE_SHARED_KEY,
	  .cipher = true, .group = true, .expected = -EINVAL },
	{ "sae authentication without security", NL80211_AUTHTYPE_SAE,
	  .expected = -EINVAL },
	{ "fast transition without security", NL80211_AUTHTYPE_FT,
	  .expected = -EINVAL },
	{ "privacy without ciphers", NL80211_AUTHTYPE_OPEN_SYSTEM,
	  .privacy = true, .expected = -EINVAL },
	{ "privacy, automatic", NL80211_AUTHTYPE_AUTOMATIC,
	  .privacy = true, .expected = -EINVAL },
	{ "key", NL80211_AUTHTYPE_OPEN_SYSTEM, .key = true,
	  .expected = -EINVAL },
	{ "key with ciphers", NL80211_AUTHTYPE_OPEN_SYSTEM, .privacy = true,
	  .key = true, .cipher = true, .expected = -EINVAL },
	{ "wpa versions", NL80211_AUTHTYPE_OPEN_SYSTEM, .wpa = true,
	  .expected = 0 },
	{ "akm", NL80211_AUTHTYPE_OPEN_SYSTEM, .akm = true, .expected = 0 },
	{ "pairwise cipher", NL80211_AUTHTYPE_OPEN_SYSTEM, .cipher = true,
	  .expected = 0 },
	{ "group cipher", NL80211_AUTHTYPE_AUTOMATIC, .group = true,
	  .expected = 0 },
	{ "sae password", NL80211_AUTHTYPE_SAE, .sae_pwd = true,
	  .expected = 0 },
	{ "rsn element", NL80211_AUTHTYPE_OPEN_SYSTEM, .rsn = true,
	  .expected = 0 },
	{ "wpa2 request", NL80211_AUTHTYPE_OPEN_SYSTEM, .privacy = true,
	  .wpa = true, .akm = true, .cipher = true, .group = true,
	  .rsn = true, .expected = 0 },
};

static void connect_case_desc(const struct connect_case *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(connect_case, connect_cases, connect_case_desc);

static void connect_classify_test(struct kunit *test)
{
	const struct connect_case *c = test->param_value;
	static const u8 rsn_ie[] = { WLAN_EID_RSN, 2, 1, 0 };
	static const u8 key_byte;
	struct cfg80211_connect_params params = {
		.auth_type = c->auth_type,
		.privacy = c->privacy,
	};

	if (c->key) {
		params.key = &key_byte;
		params.key_len = 5;
	}
	if (c->wpa)
		params.crypto.wpa_versions = NL80211_WPA_VERSION_2;
	if (c->akm) {
		params.crypto.n_akm_suites = 1;
		params.crypto.akm_suites[0] = WLAN_AKM_SUITE_PSK;
	}
	if (c->cipher) {
		params.crypto.n_ciphers_pairwise = 1;
		params.crypto.ciphers_pairwise[0] = WLAN_CIPHER_SUITE_CCMP;
	}
	if (c->group)
		params.crypto.cipher_group = WLAN_CIPHER_SUITE_CCMP;
	if (c->sae_pwd) {
		params.crypto.sae_pwd = rsn_ie;
		params.crypto.sae_pwd_len = 8;
	}
	if (c->rsn) {
		params.ie = rsn_ie;
		params.ie_len = sizeof(rsn_ie);
	}
	KUNIT_EXPECT_EQ(test, connect_classify(&params), c->expected);
}

static void connect_band_test(struct kunit *test)
{
	/* A network without security has no SAE and no MFPC. */
	KUNIT_EXPECT_FALSE(test, connect_band_allowed(NL80211_BAND_6GHZ,
						      false, false));
	KUNIT_EXPECT_FALSE(test, connect_band_allowed(NL80211_BAND_6GHZ,
						      false, true));
	KUNIT_EXPECT_FALSE(test, connect_band_allowed(NL80211_BAND_6GHZ,
						      true, false));
	KUNIT_EXPECT_TRUE(test, connect_band_allowed(NL80211_BAND_6GHZ,
						     true, true));
	KUNIT_EXPECT_TRUE(test, connect_band_allowed(NL80211_BAND_5GHZ,
						     false, false));
	KUNIT_EXPECT_TRUE(test, connect_band_allowed(NL80211_BAND_2GHZ,
						     false, false));
}

static struct kunit_case centauri_t6050_wifi_test_cases[] = {
	KUNIT_CASE_PARAM(connect_classify_test, connect_case_gen_params),
	KUNIT_CASE(connect_band_test),
	{}
};

static struct kunit_suite centauri_t6050_wifi_test_suite = {
	.name = "centauri_t6050_wifi",
	.test_cases = centauri_t6050_wifi_test_cases,
};

kunit_test_suite(centauri_t6050_wifi_test_suite);

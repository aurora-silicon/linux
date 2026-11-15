// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "scan_timeout.h"

static void brcmf_scan_timeout_channels(struct kunit *test)
{
	static const struct {
		u32 channels;
		u32 expected;
	} cases[] = {
		{ 1, 10000 },
		{ 3, 10000 },
		{ 37, 10000 },
		{ 38, 10120 },
		{ 97, 24280 },
		{ 233, 56920 },
		{ 246, 60000 },
		{ 17895698, 60000 },
		{ U32_MAX, 60000 },
	};
	struct cfg80211_scan_request *request;
	struct wiphy *wiphy;
	int i;

	request = kunit_kzalloc(test, sizeof(*request), GFP_KERNEL);
	wiphy = kunit_kzalloc(test, sizeof(*wiphy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, request);
	KUNIT_ASSERT_NOT_NULL(test, wiphy);

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		request->n_channels = cases[i].channels;
		request->n_ssids = 0;
		KUNIT_EXPECT_EQ(test, brcmf_scan_timeout_ms(wiphy, request),
				cases[i].expected);
		/* Active requests still need a passive-channel budget. */
		request->n_ssids = 1;
		KUNIT_EXPECT_EQ(test, brcmf_scan_timeout_ms(wiphy, request),
				cases[i].expected);
	}
}

static void brcmf_scan_timeout_all_bands(struct kunit *test)
{
	struct ieee80211_supported_band bands[] = {
		{ .n_channels = 14 },
		{ .n_channels = 24 },
		{ .n_channels = 59 },
	};
	struct cfg80211_scan_request *request;
	struct wiphy *wiphy;

	request = kunit_kzalloc(test, sizeof(*request), GFP_KERNEL);
	wiphy = kunit_kzalloc(test, sizeof(*wiphy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, request);
	KUNIT_ASSERT_NOT_NULL(test, wiphy);
	KUNIT_EXPECT_EQ(test, brcmf_scan_timeout_ms(wiphy, request), 10000U);
	wiphy->bands[NL80211_BAND_2GHZ] = &bands[0];
	wiphy->bands[NL80211_BAND_5GHZ] = &bands[1];
	wiphy->bands[NL80211_BAND_6GHZ] = &bands[2];
	KUNIT_EXPECT_EQ(test, brcmf_scan_timeout_ms(wiphy, request), 24280U);
	/* An explicit channel list must not also count all advertised bands. */
	request->n_channels = 1;
	KUNIT_EXPECT_EQ(test, brcmf_scan_timeout_ms(wiphy, request), 10000U);
}

static struct kunit_case brcmf_scan_timeout_cases[] = {
	KUNIT_CASE(brcmf_scan_timeout_channels),
	KUNIT_CASE(brcmf_scan_timeout_all_bands),
	{}
};

static struct kunit_suite brcmf_scan_timeout_suite = {
	.name = "brcmfmac-scan-timeout",
	.test_cases = brcmf_scan_timeout_cases,
};

kunit_test_suite(brcmf_scan_timeout_suite);

MODULE_DESCRIPTION("KUnit tests for the brcmfmac scan timeout budget");
MODULE_LICENSE("GPL");

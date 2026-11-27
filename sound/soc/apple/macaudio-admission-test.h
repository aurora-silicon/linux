/* SPDX-License-Identifier: GPL-2.0-only */
/* Exercise the admission predicate used for Speaker/Speakers DT links. */
#include <kunit/test.h>

static void macaudio_admission_needs_a_safe_level(struct kunit *test)
{
	struct macaudio_platform_cfg cfg;

	/* Generic apple,macaudio plus a DT Speaker backend. */
	KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&macaudio_fallback_cfg));
	KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&macaudio_j45x_cfg));
	KUNIT_EXPECT_TRUE(test, macaudio_speakers_allowed(&macaudio_j413_cfg));
	KUNIT_EXPECT_TRUE(test, macaudio_speakers_allowed(&macaudio_j700_cfg));
	/*
	 * The AOP-fed machines: J714 (also K114 and K116) and J813 with their safe
	 * level, J873 and J704 without one
	 */
	KUNIT_EXPECT_TRUE(test, macaudio_speakers_allowed(&macaudio_j714_cfg));
	KUNIT_EXPECT_TRUE(test, macaudio_speakers_allowed(&macaudio_j813_cfg));
	KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&macaudio_j873_cfg));
	/* The Mac Studio M5: no sense and no protection owner yet, speaker off */
	KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&macaudio_j775_cfg));
	/* J704: speakers off until their slots and protection are verified */
	KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&macaudio_j704_cfg));
	cfg = macaudio_j413_cfg;
	cfg.safe_vol = 0;
	KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&cfg));
	cfg = macaudio_j413_cfg;
	cfg.speakers = SPKR_NONE;
	KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&cfg));
	cfg = macaudio_j413_cfg;
	cfg.amp = AMP_NONE;
	KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&cfg));
	/* An amp the driver cannot hold at a safe level */
	cfg = macaudio_j413_cfg;
	cfg.amp = AMP_SSM3515;
	KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&cfg));
}

/* Without sense, only a lease lifts the amps, and only to safe_vol */
static void macaudio_admission_fixed_ceiling_is_lease_only(struct kunit *test)
{
	struct macaudio_platform_cfg cfg = macaudio_j813_cfg;

	/* A protection daemon unlocks the J813's amps to 0 dB */
	KUNIT_EXPECT_FALSE(test, macaudio_spk_lease_only(&cfg));
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_vlimit(&cfg, false, false), 1);
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_vlimit(&cfg, true, false), SN012776_0DB);

	cfg.safe_vol = -16;
	cfg.spk_fixed_ceiling = true;
	KUNIT_EXPECT_TRUE(test, macaudio_spk_lease_only(&cfg));
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_vlimit(&cfg, false, false), 1);
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_vlimit(&cfg, true, false), 1);
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_vlimit(&cfg, false, true), 1);
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_vlimit(&cfg, true, true), SN012776_DB(-16));
}

static struct kunit_case macaudio_admission_cases[] = {
	KUNIT_CASE(macaudio_admission_needs_a_safe_level),
	KUNIT_CASE(macaudio_admission_fixed_ceiling_is_lease_only),
	{}
};

static struct kunit_suite macaudio_admission_suite = {
	.name = "apple-speaker-admission",
	.test_cases = macaudio_admission_cases,
};
kunit_test_suite(macaudio_admission_suite);

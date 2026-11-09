/* SPDX-License-Identifier: GPL-2.0-only */
/* Exercise the same admission predicate used for Speaker/Speakers DT links. */
#include <kunit/test.h>

static void macaudio_admission_unsafe_parameter_cannot_admit_speakers(struct kunit *test)
{
	struct macaudio_platform_cfg cfg = macaudio_j413_cfg;
	int saved = please_blow_up_my_speakers;
	int values[] = { 0, 1, 2, 3, -1 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(values); i++) {
		please_blow_up_my_speakers = values[i];
		/* Generic apple,macaudio plus a DT Speaker backend. */
		KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&macaudio_fallback_cfg));
		KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&macaudio_j45x_cfg));
		KUNIT_EXPECT_TRUE(test, macaudio_speakers_allowed(&macaudio_j413_cfg));
		KUNIT_EXPECT_TRUE(test, macaudio_speakers_allowed(&macaudio_j700_cfg));
		cfg = macaudio_j413_cfg;
		cfg.safe_vol = 0;
		KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&cfg));
		cfg = macaudio_j413_cfg;
		cfg.speakers = SPKR_NONE;
		KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&cfg));
		cfg = macaudio_j413_cfg;
		cfg.amp = AMP_NONE;
		KUNIT_EXPECT_FALSE(test, macaudio_speakers_allowed(&cfg));
	}
	please_blow_up_my_speakers = saved;
}

static struct kunit_case macaudio_admission_cases[] = {
	KUNIT_CASE(macaudio_admission_unsafe_parameter_cannot_admit_speakers),
	{}
};

static struct kunit_suite macaudio_admission_suite = {
	.name = "apple-speaker-admission",
	.test_cases = macaudio_admission_cases,
};
kunit_test_suite(macaudio_admission_suite);

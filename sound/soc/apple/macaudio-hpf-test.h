/* SPDX-License-Identifier: GPL-2.0-only */
/* The playback high-pass corner each speaker amp is fixed at. */
#include <kunit/test.h>

/* The tas2764 "HPF Corner Frequency" items, in register order */
static const char * const tas2764_hpf_items[] = {
	"Disabled", "2 Hz", "50 Hz", "100 Hz", "200 Hz", "400 Hz", "800 Hz",
};

static int macaudio_hpf_item(struct kunit *test, const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tas2764_hpf_items); i++)
		if (!strcmp(tas2764_hpf_items[i], name))
			return i;
	KUNIT_FAIL(test, "\"%s\" is not an HPF Corner Frequency item", name);
	return -1;
}

static void macaudio_hpf_woofers_keep_2hz(struct kunit *test)
{
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j316_cfg, false), "2 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j413_cfg, false), "2 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j616_cfg, false), "2 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j493_cfg, false), "2 Hz");
}

static void macaudio_hpf_tweeters_default_800hz(struct kunit *test)
{
	/* Machines that have not opted in keep the 800 Hz tweeter filter */
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j180_cfg, true), "800 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j314_cfg, true), "800 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j316_cfg, true), "800 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j415_cfg, true), "800 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j614_cfg, true), "800 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j713_cfg, true), "800 Hz");
}

static void macaudio_hpf_pilot_tweeters_pass_the_pilot(struct kunit *test)
{
	/*
	 * The in-graph pilot sits well under 100 Hz: the tweeter corner on
	 * these machines must be the woofers' one, below it.
	 */
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j413_cfg, true),
			   macaudio_speaker_hpf(&macaudio_j413_cfg, false));
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j616_cfg, true),
			   macaudio_speaker_hpf(&macaudio_j616_cfg, false));
	KUNIT_EXPECT_LT(test, macaudio_hpf_item(test, macaudio_speaker_hpf(&macaudio_j616_cfg, true)),
			macaudio_hpf_item(test, "50 Hz"));
}

static void macaudio_hpf_every_choice_is_an_item(struct kunit *test)
{
	const struct macaudio_platform_cfg *cfgs[] = {
		&macaudio_j180_cfg, &macaudio_j314_cfg, &macaudio_j316_cfg,
		&macaudio_j37x_j47x_cfg, &macaudio_j413_cfg, &macaudio_j415_cfg,
		&macaudio_j493_cfg, &macaudio_j614_cfg, &macaudio_j616_cfg,
		&macaudio_j713_cfg,
	};
	int i;

	/* Never "Disabled": every amp keeps a DC-blocking playback filter */
	for (i = 0; i < ARRAY_SIZE(cfgs); i++) {
		KUNIT_EXPECT_GE(test, macaudio_hpf_item(test, macaudio_speaker_hpf(cfgs[i], false)), 1);
		KUNIT_EXPECT_GE(test, macaudio_hpf_item(test, macaudio_speaker_hpf(cfgs[i], true)), 1);
	}
}

static struct kunit_case macaudio_hpf_cases[] = {
	KUNIT_CASE(macaudio_hpf_woofers_keep_2hz),
	KUNIT_CASE(macaudio_hpf_tweeters_default_800hz),
	KUNIT_CASE(macaudio_hpf_pilot_tweeters_pass_the_pilot),
	KUNIT_CASE(macaudio_hpf_every_choice_is_an_item),
	{}
};

static struct kunit_suite macaudio_hpf_suite = {
	.name = "apple-speaker-hpf",
	.test_cases = macaudio_hpf_cases,
};
kunit_test_suite(macaudio_hpf_suite);

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The speaker amps' playback high-pass corners: fixed for the woofers, and
 * for the tweeters the safe corner unless protection is granted on a
 * machine that opens it. On the fake card of macaudio-protection-test.h.
 */
#include <kunit/test.h>

/* The tas2764 "HPF Corner Frequency" items, in register order */
static const char * const tas2764_hpf_items[] = {
	"Disabled", "2 Hz", "50 Hz", "100 Hz", "200 Hz", "400 Hz", "800 Hz",
};

#define MACAUDIO_TEST_HPF_2HZ	1
#define MACAUDIO_TEST_HPF_800HZ	6

static int macaudio_hpf_item(struct kunit *test, const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tas2764_hpf_items); i++)
		if (!strcmp(tas2764_hpf_items[i], name))
			return i;
	KUNIT_FAIL(test, "\"%s\" is not an HPF Corner Frequency item", name);
	return -1;
}

struct macaudio_test_hpf {
	unsigned int item;
	int fail;
};

static int macaudio_test_hpf_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(tas2764_hpf_items), tas2764_hpf_items);
}

static int macaudio_test_hpf_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uval)
{
	uval->value.enumerated.item[0] = ((struct macaudio_test_hpf *)kctl->private_data)->item;
	return 0;
}

static int macaudio_test_hpf_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uval)
{
	struct macaudio_test_hpf *hpf = kctl->private_data;
	bool changed;

	if (hpf->fail)
		return hpf->fail;
	changed = hpf->item != uval->value.enumerated.item[0];
	hpf->item = uval->value.enumerated.item[0];
	return changed;
}

static const char *const macaudio_test_hpf_names[] = {
	"Left Tweeter HPF Corner Frequency",
	"Right Tweeter HPF Corner Frequency",
	/* A woofer's corner is fixed: never taken */
	"Left Woofer HPF Corner Frequency",
};

#define MACAUDIO_TEST_NR_HPF ARRAY_SIZE(macaudio_test_hpf_names)

struct macaudio_test_hpfs {
	struct macaudio_test_spk t;
	struct macaudio_test_hpf hpf[MACAUDIO_TEST_NR_HPF];
};

/* The fake card with the amps' corners as macaudio_fixup_controls() leaves them */
static void macaudio_test_hpf_card(struct kunit *test, struct macaudio_test_hpfs *h,
				   const struct macaudio_platform_cfg *cfg, int take)
{
	unsigned int i;

	macaudio_test_spk_card(test, &h->t, cfg, SN012776_0DB);
	for (i = 0; i < MACAUDIO_TEST_NR_HPF; i++) {
		struct snd_kcontrol_new tmpl = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = macaudio_test_hpf_names[i],
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
			.info = macaudio_test_hpf_info,
			.get = macaudio_test_hpf_get,
			.put = macaudio_test_hpf_put,
		};

		h->hpf[i].item = i < 2 ? MACAUDIO_TEST_HPF_800HZ : MACAUDIO_TEST_HPF_2HZ;
		macaudio_test_add_kctl(test, h->t.ma->card.snd_card, &tmpl, &h->hpf[i]);
	}
	/* As macaudio_late_probe() does */
	KUNIT_ASSERT_EQ(test, macaudio_spk_take_hpfs(h->t.ma), take);
	mutex_lock(&h->t.ma->spk_mutex);
	macaudio_spk_floor(h->t.ma, "card bound");
	mutex_unlock(&h->t.ma->spk_mutex);
}

/* Both tweeters at @item, the woofer at 2 Hz throughout */
static void macaudio_test_expect_hpf(struct kunit *test, struct macaudio_test_hpfs *h,
				     unsigned int item)
{
	KUNIT_EXPECT_EQ(test, h->hpf[0].item, item);
	KUNIT_EXPECT_EQ(test, h->hpf[1].item, item);
	KUNIT_EXPECT_EQ(test, h->hpf[2].item, (unsigned int)MACAUDIO_TEST_HPF_2HZ);
}

static void macaudio_hpf_grant_opens_the_tweeters(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};

	macaudio_test_hpf_card(test, &h, &macaudio_j616_cfg, 0);
	KUNIT_EXPECT_EQ(test, h.t.ma->spk_nr_hpfs, 2U);
	KUNIT_EXPECT_FALSE(test, h.t.ma->spk_hpfs[0]->vd[0].access & SNDRV_CTL_ELEM_ACCESS_WRITE);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);

	/* No owner, no grant: the safe corner */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), -EPERM);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);

	macaudio_test_take(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);
	macaudio_test_expect_vols(test, &h.t, SN012776_0DB);
	/* Renewals keep it open */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);
}

static void macaudio_hpf_release_restores_800hz(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};

	macaudio_test_hpf_card(test, &h, &macaudio_j413_cfg, 0);
	macaudio_test_take(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);

	/* An explicit release */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 0), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	macaudio_test_expect_vols(test, &h.t, 0);

	/* The owner goes */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);
	macaudio_spk_unlock(h.t.spk);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	macaudio_test_expect_vols(test, &h.t, 0);
}

static void macaudio_hpf_lease_timeout_restores_800hz(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};

	macaudio_test_hpf_card(test, &h, &macaudio_j316_cfg, 0);
	macaudio_test_take(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, SPEAKER_LEASE_MIN_MS), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);

	msleep(3 * SPEAKER_LEASE_MIN_MS);
	macaudio_test_settle(h.t.ma);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &h.t), 0L);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	macaudio_test_expect_vols(test, &h.t, 0);

	/* The refused renewal opens nothing */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), -ETIMEDOUT);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
}

static void macaudio_hpf_failed_restore_is_a_failed_floor(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};

	macaudio_test_hpf_card(test, &h, &macaudio_j616_cfg, 0);
	macaudio_test_take(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);

	/* One tweeter stops answering: its safe corner cannot be set */
	h.hpf[1].fail = -EIO;
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 0), 0);
	KUNIT_EXPECT_TRUE(test, h.t.ma->spk_floor_failed);
	macaudio_test_expect_vols(test, &h.t, 0);
	h.hpf[1].fail = 0;
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), -EIO);
	macaudio_test_expect_vols(test, &h.t, 0);
}

static void macaudio_hpf_failed_open_floors(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};

	macaudio_test_hpf_card(test, &h, &macaudio_j616_cfg, 0);
	macaudio_test_take(test, &h.t);
	/* Fails on the way up only: the floor still sets the safe corner */
	h.hpf[0].fail = -EIO;
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), -EIO);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &h.t), 0L);
	macaudio_test_expect_vols(test, &h.t, 0);
	KUNIT_EXPECT_EQ(test, h.hpf[1].item, (unsigned int)MACAUDIO_TEST_HPF_800HZ);
}

static void macaudio_hpf_other_machines_stay_at_800hz(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};

	macaudio_test_hpf_card(test, &h, &macaudio_j314_cfg, 0);
	KUNIT_EXPECT_EQ(test, h.t.ma->spk_nr_hpfs, 0U);
	macaudio_test_take(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
}

static void macaudio_hpf_choices(struct kunit *test)
{
	const struct macaudio_platform_cfg *cfgs[] = {
		&macaudio_j180_cfg, &macaudio_j314_cfg, &macaudio_j316_cfg,
		&macaudio_j37x_j47x_cfg, &macaudio_j413_cfg, &macaudio_j415_cfg,
		&macaudio_j493_cfg, &macaudio_j614_cfg, &macaudio_j616_cfg,
		&macaudio_j713_cfg,
	};
	int i, g;

	/* The machine set: these open the tweeters while granted */
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j316_cfg, true, true), "2 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j413_cfg, true, true), "2 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j616_cfg, true, true), "2 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j614_cfg, true, true), "800 Hz");
	KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(&macaudio_j713_cfg, true, true), "800 Hz");

	for (i = 0; i < ARRAY_SIZE(cfgs); i++) {
		for (g = 0; g < 2; g++) {
			/* Woofers fixed at 2 Hz; tweeters safe at 800 Hz */
			KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(cfgs[i], false, g), "2 Hz");
			if (!g)
				KUNIT_EXPECT_STREQ(test, macaudio_speaker_hpf(cfgs[i], true, g),
						   "800 Hz");
			/* Never "Disabled": every amp keeps a DC-blocking filter */
			KUNIT_EXPECT_GE(test, macaudio_hpf_item(test, macaudio_speaker_hpf(cfgs[i], true, g)), 1);
			KUNIT_EXPECT_GE(test, macaudio_hpf_item(test, macaudio_speaker_hpf(cfgs[i], false, g)), 1);
			/* Open means below the pilot band */
			if (g && cfgs[i]->tweeter_hpf_granted)
				KUNIT_EXPECT_LT(test, macaudio_hpf_item(test, cfgs[i]->tweeter_hpf_granted),
						macaudio_hpf_item(test, "50 Hz"));
		}
	}
}

static struct kunit_case macaudio_hpf_cases[] = {
	KUNIT_CASE(macaudio_hpf_grant_opens_the_tweeters),
	KUNIT_CASE(macaudio_hpf_release_restores_800hz),
	KUNIT_CASE(macaudio_hpf_lease_timeout_restores_800hz),
	KUNIT_CASE(macaudio_hpf_failed_restore_is_a_failed_floor),
	KUNIT_CASE(macaudio_hpf_failed_open_floors),
	KUNIT_CASE(macaudio_hpf_other_machines_stay_at_800hz),
	KUNIT_CASE(macaudio_hpf_choices),
	{}
};

static struct kunit_suite macaudio_hpf_suite = {
	.name = "apple-speaker-hpf",
	.test_cases = macaudio_hpf_cases,
};
kunit_test_suite(macaudio_hpf_suite);

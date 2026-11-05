/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The speaker amps' playback high-pass corners: DT-derived for the woofers, and
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

/* The fake card with the amps' DT corners, before control fixup. */
static void macaudio_test_hpf_card_corners(struct kunit *test, struct macaudio_test_hpfs *h,
					   const struct macaudio_platform_cfg *cfg, int take,
					   const unsigned int *corners)
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

		h->hpf[i].item = corners ? corners[i] : MACAUDIO_TEST_HPF_2HZ;
		macaudio_test_add_kctl(test, h->t.ma->card.snd_card, &tmpl, &h->hpf[i]);
	}
	/* Capture the codec's DT values before fixup applies the floor. */
	KUNIT_ASSERT_EQ(test, macaudio_spk_take_hpfs(h->t.ma), take);
	if (!cfg->tweeter_hpf_granted) {
		h->hpf[0].item = MACAUDIO_TEST_HPF_800HZ;
		h->hpf[1].item = MACAUDIO_TEST_HPF_800HZ;
	}
	mutex_lock(&h->t.ma->spk_mutex);
	macaudio_spk_floor(h->t.ma, "card bound");
	mutex_unlock(&h->t.ma->spk_mutex);
}

static void macaudio_test_hpf_card(struct kunit *test, struct macaudio_test_hpfs *h,
				   const struct macaudio_platform_cfg *cfg, int take)
{
	macaudio_test_hpf_card_corners(test, h, cfg, take, NULL);
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

static void macaudio_hpf_restores_each_dt_corner(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};
	const unsigned int corners[] = { 0, 3, MACAUDIO_TEST_HPF_2HZ };

	/* Capture different DT values, including a disabled playback HPF. */
	macaudio_test_hpf_card_corners(test, &h, &macaudio_j413_cfg, 0, corners);
	macaudio_test_take(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	KUNIT_EXPECT_EQ(test, h.hpf[0].item, 0U);
	KUNIT_EXPECT_EQ(test, h.hpf[1].item, 3U);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 0), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
}

static struct kunit_case macaudio_hpf_cases[] = {
	KUNIT_CASE(macaudio_hpf_grant_opens_the_tweeters),
	KUNIT_CASE(macaudio_hpf_release_restores_800hz),
	KUNIT_CASE(macaudio_hpf_lease_timeout_restores_800hz),
	KUNIT_CASE(macaudio_hpf_failed_restore_is_a_failed_floor),
	KUNIT_CASE(macaudio_hpf_failed_open_floors),
	KUNIT_CASE(macaudio_hpf_other_machines_stay_at_800hz),
	KUNIT_CASE(macaudio_hpf_restores_each_dt_corner),
	{}
};

static struct kunit_suite macaudio_hpf_suite = {
	.name = "apple-speaker-hpf",
	.test_cases = macaudio_hpf_cases,
};
kunit_test_suite(macaudio_hpf_suite);

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Under "Speaker Volume Unlock" the speaker volumes sit at its ceiling from
 * the bind, and a system sleep, a retired daemon's lock and ping, or both,
 * never lift it. The card here is shared with macaudio-vlimit-test.h.
 */
#include <kunit/test.h>

struct macaudio_pmt_vol {
	struct soc_mixer_control mc;
	long value;
};

static struct macaudio_pmt_vol *macaudio_pmt_vol(struct snd_kcontrol *kctl)
{
	return container_of((struct soc_mixer_control *)kctl->private_value,
			    struct macaudio_pmt_vol, mc);
}

static int macaudio_pmt_vol_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.max = SN012776_0DB;
	return 0;
}

static int macaudio_pmt_vol_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uval)
{
	uval->value.integer.value[0] = macaudio_pmt_vol(kctl)->value;
	return 0;
}

static int macaudio_pmt_vol_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uval)
{
	struct macaudio_pmt_vol *vol = macaudio_pmt_vol(kctl);
	bool changed = vol->value != uval->value.integer.value[0];

	vol->value = uval->value.integer.value[0];
	return changed;
}

static void macaudio_pmt_free_kctl(void *kctl)
{
	snd_ctl_free_one(kctl);
}

/* Last in, so first out: before the controls go */
static void macaudio_pmt_pm_cleanup(void *data)
{
	struct macaudio_snd_data *ma = data;

	macaudio_spk_fini(ma);
}

static const char *const macaudio_pmt_vol_names[] = {
	"Left Woofer Speaker Volume",
	"Right Woofer Speaker Volume",
	"Left Tweeter Speaker Volume",
	"Right Tweeter Speaker Volume",
	/* Not a speaker volume: never clipped */
	"Headphone Playback Volume",
};

/* What the amps' volumes held before the bind */
static const long macaudio_pmt_vols[] = { 201, 190, 201, 170, 120 };

#define MACAUDIO_PMT_NR_VOLS ARRAY_SIZE(macaudio_pmt_vol_names)

struct macaudio_pmt_pm {
	struct macaudio_snd_data *ma;
	struct snd_kcontrol *slk;
	struct snd_kcontrol *vols[MACAUDIO_PMT_NR_VOLS];
};

static struct snd_kcontrol *macaudio_pmt_add_kctl(struct kunit *test, struct snd_card *card,
						   const struct snd_kcontrol_new *tmpl,
						   void *private_data)
{
	struct snd_kcontrol *kctl = snd_ctl_new1(tmpl, private_data);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, kctl);
	ret = kunit_add_action_or_reset(test, macaudio_pmt_free_kctl, kctl);
	KUNIT_ASSERT_EQ(test, ret, 0);
	list_add_tail(&kctl->list, &card->controls);
	return kctl;
}

/* A bound card with speaker safety, its lock control and the volumes at its ceiling */
static void macaudio_pmt_pm_card(struct kunit *test, struct macaudio_pmt_pm *t)
{
	struct snd_kcontrol_new slk = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Speaker Volume Unlock",
		.info = snd_ctl_boolean_mono_info,
		.get = macaudio_slk_get,
		.put = macaudio_slk_put,
		.lock = macaudio_slk_lock,
	};
	struct macaudio_snd_data *ma;
	struct snd_card *card;
	unsigned int i;
	int ret;

	ma = kunit_kzalloc(test, sizeof(*ma), GFP_KERNEL);
	card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ma);
	KUNIT_ASSERT_NOT_NULL(test, card);

	INIT_LIST_HEAD(&card->controls);
	INIT_LIST_HEAD(&card->ctl_files);
	init_rwsem(&card->controls_rwsem);
	rwlock_init(&card->controls_rwlock);
	ma->card.snd_card = card;
	snd_soc_card_set_drvdata(&ma->card, ma);
	ma->cfg = &macaudio_j493_cfg;
	ma->has_speakers = true;
	ma->has_safety = true;
	INIT_LIST_HEAD(&ma->card.rtd_list);
	mutex_init(&ma->volume_lock_mutex);
	/* The volume lock is the interface, as at every bind */
	mutex_init(&ma->spk_mutex);
	KUNIT_ASSERT_EQ(test, macaudio_spk_init(ma), 0);

	t->ma = ma;
	t->slk = macaudio_pmt_add_kctl(test, card, &slk, &ma->card);
	ma->speaker_lock_kctl = t->slk;

	for (i = 0; i < MACAUDIO_PMT_NR_VOLS; i++) {
		struct macaudio_pmt_vol *vol = kunit_kzalloc(test, sizeof(*vol), GFP_KERNEL);
		struct snd_kcontrol_new tmpl = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = macaudio_pmt_vol_names[i],
			.info = macaudio_pmt_vol_info,
			.get = macaudio_pmt_vol_get,
			.put = macaudio_pmt_vol_put,
		};

		KUNIT_ASSERT_NOT_NULL(test, vol);
		vol->mc.max = SN012776_0DB;
		vol->value = macaudio_pmt_vols[i];
		tmpl.private_value = (unsigned long)&vol->mc;
		t->vols[i] = macaudio_pmt_add_kctl(test, card, &tmpl, NULL);
	}

	ret = kunit_add_action_or_reset(test, macaudio_pmt_pm_cleanup, ma);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* As macaudio_late_probe() does at every bind */
	mutex_lock(&ma->volume_lock_mutex);
	macaudio_vlimit_unlock(ma, false);
	mutex_unlock(&ma->volume_lock_mutex);
}

/*
 * A retired daemon (speakersafetyd, speakerguardd 0.1) holds every speaker
 * volume and asks for the lock control: refused.
 */
static void macaudio_pmt_take(struct kunit *test, struct macaudio_pmt_pm *t)
{
	struct snd_ctl_file *owner = kunit_kzalloc(test, sizeof(*owner), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, owner);
	for (i = 0; i < MACAUDIO_PMT_NR_VOLS - 1; i++)
		t->vols[i]->vd[0].owner = owner;
	KUNIT_EXPECT_EQ(test, macaudio_slk_lock(t->slk, owner), -EPERM);
}

static int macaudio_pmt_ping(struct kunit *test, struct macaudio_pmt_pm *t)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	uval->value.integer.value[0] = SPEAKER_MAGIC_VALUE;
	return macaudio_slk_put(t->slk, uval);
}

static void macaudio_pmt_notify(struct macaudio_pmt_pm *t, unsigned long action)
{
	macaudio_pm_notify(&t->ma->pm_nb, action, NULL);
}

/* Every speaker volume clipped to the lock's ceiling, and limited there */
static void macaudio_pmt_expect_vols(struct kunit *test, struct macaudio_pmt_pm *t)
{
	unsigned int i;

	for (i = 0; i < MACAUDIO_PMT_NR_VOLS; i++) {
		bool speaker = i < MACAUDIO_PMT_NR_VOLS - 1;

		KUNIT_EXPECT_EQ(test, macaudio_pmt_vol(t->vols[i])->value,
				speaker ? 1L : macaudio_pmt_vols[i]);
		if (speaker)
			KUNIT_EXPECT_EQ(test, macaudio_pmt_vol(t->vols[i])->mc.platform_max, 1);
	}
}

/* A sleep under the volume lock leaves the ceiling where it was */
static void macaudio_pm_volumes_sleep_keeps_the_ceiling(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_expect_vols(test, &t);

	macaudio_pmt_notify(&t, PM_SUSPEND_PREPARE);
	macaudio_pmt_expect_vols(test, &t);
	macaudio_pmt_notify(&t, PM_POST_SUSPEND);
	macaudio_pmt_expect_vols(test, &t);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_leased(t.ma));
}

/* A retired daemon's lock and pings around a sleep lift nothing */
static void macaudio_pm_volumes_daemon_across_sleep(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	macaudio_pmt_expect_vols(test, &t);

	macaudio_pmt_notify(&t, PM_SUSPEND_PREPARE);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	macaudio_pmt_notify(&t, PM_POST_SUSPEND);
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	macaudio_pmt_expect_vols(test, &t);
}

static struct kunit_case macaudio_pm_volumes_cases[] = {
	KUNIT_CASE(macaudio_pm_volumes_sleep_keeps_the_ceiling),
	KUNIT_CASE(macaudio_pm_volumes_daemon_across_sleep),
	{}
};

static struct kunit_suite macaudio_pm_volumes_suite = {
	.name = "apple-speaker-pm-volumes",
	.test_cases = macaudio_pm_volumes_cases,
};
kunit_test_suite(macaudio_pm_volumes_suite);

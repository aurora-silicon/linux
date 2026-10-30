/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The speaker volumes the system-sleep lock clips are set again at the
 * daemon's first ping after it, through the lock control and the PM notifier.
 */
#include <kunit/test.h>

struct macaudio_test_vol {
	struct soc_mixer_control mc;
	long value;
};

static struct macaudio_test_vol *macaudio_test_vol(struct snd_kcontrol *kctl)
{
	return container_of((struct soc_mixer_control *)kctl->private_value,
			    struct macaudio_test_vol, mc);
}

static int macaudio_test_vol_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.max = SN012776_0DB;
	return 0;
}

static int macaudio_test_vol_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uval)
{
	uval->value.integer.value[0] = macaudio_test_vol(kctl)->value;
	return 0;
}

static int macaudio_test_vol_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *uval)
{
	struct macaudio_test_vol *vol = macaudio_test_vol(kctl);
	bool changed = vol->value != uval->value.integer.value[0];

	vol->value = uval->value.integer.value[0];
	return changed;
}

static void macaudio_test_free_kctl(void *kctl)
{
	snd_ctl_free_one(kctl);
}

/* Last in, so first out: before the controls go */
static void macaudio_test_pm_cleanup(void *data)
{
	struct macaudio_snd_data *ma = data;

	cancel_delayed_work_sync(&ma->lock_timeout_work);
	macaudio_pm_volumes_drop(ma);
}

static const char *const macaudio_test_vol_names[] = {
	"Left Woofer Speaker Volume",
	"Right Woofer Speaker Volume",
	"Left Tweeter Speaker Volume",
	"Right Tweeter Speaker Volume",
	/* Not a speaker volume: never locked, kept or set */
	"Headphone Playback Volume",
};

/* As the daemon set them before the sleep */
static const long macaudio_test_vols[] = { 201, 190, 201, 170, 120 };

#define MACAUDIO_TEST_NR_VOLS ARRAY_SIZE(macaudio_test_vol_names)

struct macaudio_test_pm {
	struct macaudio_snd_data *ma;
	struct snd_kcontrol *slk;
	struct snd_kcontrol *vols[MACAUDIO_TEST_NR_VOLS];
};

static struct snd_kcontrol *macaudio_test_add_kctl(struct kunit *test, struct snd_card *card,
						   const struct snd_kcontrol_new *tmpl,
						   void *private_data)
{
	struct snd_kcontrol *kctl = snd_ctl_new1(tmpl, private_data);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, kctl);
	ret = kunit_add_action_or_reset(test, macaudio_test_free_kctl, kctl);
	KUNIT_ASSERT_EQ(test, ret, 0);
	list_add_tail(&kctl->list, &card->controls);
	return kctl;
}

/* A card with speaker safety, its lock control and the volumes, none owned yet */
static void macaudio_test_pm_card(struct kunit *test, struct macaudio_test_pm *t)
{
	struct snd_kcontrol_new slk = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Speaker Volume Unlock",
		.info = snd_ctl_boolean_mono_info,
		.get = macaudio_slk_get,
		.put = macaudio_slk_put,
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
	ma->has_safety = true;
	mutex_init(&ma->volume_lock_mutex);
	INIT_DELAYED_WORK(&ma->lock_timeout_work, macaudio_vlimit_timeout_work);

	t->ma = ma;
	t->slk = macaudio_test_add_kctl(test, card, &slk, &ma->card);
	ma->speaker_lock_kctl = t->slk;

	for (i = 0; i < MACAUDIO_TEST_NR_VOLS; i++) {
		struct macaudio_test_vol *vol = kunit_kzalloc(test, sizeof(*vol), GFP_KERNEL);
		struct snd_kcontrol_new tmpl = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = macaudio_test_vol_names[i],
			.info = macaudio_test_vol_info,
			.get = macaudio_test_vol_get,
			.put = macaudio_test_vol_put,
		};

		KUNIT_ASSERT_NOT_NULL(test, vol);
		vol->mc.max = SN012776_0DB;
		vol->value = macaudio_test_vols[i];
		tmpl.private_value = (unsigned long)&vol->mc;
		t->vols[i] = macaudio_test_add_kctl(test, card, &tmpl, NULL);
	}

	ret = kunit_add_action_or_reset(test, macaudio_test_pm_cleanup, ma);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

/* A daemon takes the lock and every speaker volume, as speakersafetyd does */
static void macaudio_test_take(struct kunit *test, struct macaudio_test_pm *t)
{
	struct snd_ctl_file *owner = kunit_kzalloc(test, sizeof(*owner), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, owner);
	for (i = 0; i < MACAUDIO_TEST_NR_VOLS - 1; i++)
		t->vols[i]->vd[0].owner = owner;
	KUNIT_ASSERT_EQ(test, macaudio_slk_lock(t->slk, owner), 0);
}

static int macaudio_test_ping(struct kunit *test, struct macaudio_test_pm *t)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	uval->value.integer.value[0] = SPEAKER_MAGIC_VALUE;
	return macaudio_slk_put(t->slk, uval);
}

static void macaudio_test_notify(struct macaudio_test_pm *t, unsigned long action)
{
	macaudio_pm_notify(&t->ma->pm_nb, action, NULL);
}

/* The speaker volumes as the daemon set them, or clipped to the lock's ceiling */
static void macaudio_test_expect_vols(struct kunit *test, struct macaudio_test_pm *t,
				      bool clipped)
{
	unsigned int i;

	for (i = 0; i < MACAUDIO_TEST_NR_VOLS; i++) {
		bool speaker = i < MACAUDIO_TEST_NR_VOLS - 1;

		KUNIT_EXPECT_EQ(test, macaudio_test_vol(t->vols[i])->value,
				speaker && clipped ? 1L : macaudio_test_vols[i]);
	}
}

static void macaudio_pm_volumes_restored_after_sleep(struct kunit *test)
{
	struct macaudio_test_pm t;

	macaudio_test_pm_card(test, &t);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_ping(test, &t), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);
	macaudio_test_expect_vols(test, &t, false);

	macaudio_test_notify(&t, PM_SUSPEND_PREPARE);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	macaudio_test_expect_vols(test, &t, true);
	/* A ping before the freeze leaves them locked */
	KUNIT_EXPECT_EQ(test, macaudio_test_ping(test, &t), 0);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);

	/* Locked after the resume until the daemon pings */
	macaudio_test_notify(&t, PM_POST_SUSPEND);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	macaudio_test_expect_vols(test, &t, true);

	KUNIT_EXPECT_EQ(test, macaudio_test_ping(test, &t), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);
	macaudio_test_expect_vols(test, &t, false);
	KUNIT_EXPECT_NULL(test, t.ma->pm_volumes);
}

/* The daemon pings only once a stream runs: a second sleep can come first. */
static void macaudio_pm_volumes_second_sleep_before_ping(struct kunit *test)
{
	struct macaudio_test_pm t;

	macaudio_test_pm_card(test, &t);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_ping(test, &t), 0);

	macaudio_test_notify(&t, PM_SUSPEND_PREPARE);
	macaudio_test_notify(&t, PM_POST_SUSPEND);
	macaudio_test_notify(&t, PM_SUSPEND_PREPARE);
	macaudio_test_notify(&t, PM_POST_SUSPEND);
	macaudio_test_expect_vols(test, &t, true);

	KUNIT_EXPECT_EQ(test, macaudio_test_ping(test, &t), 0);
	macaudio_test_expect_vols(test, &t, false);
}

/* A new owner, such as a restarted daemon, sets the volumes itself. */
static void macaudio_pm_volumes_new_owner(struct kunit *test)
{
	struct macaudio_test_pm t;

	macaudio_test_pm_card(test, &t);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_ping(test, &t), 0);
	macaudio_test_notify(&t, PM_SUSPEND_PREPARE);
	macaudio_test_notify(&t, PM_POST_SUSPEND);

	macaudio_test_take(test, &t);
	KUNIT_EXPECT_NULL(test, t.ma->pm_volumes);
	KUNIT_EXPECT_EQ(test, macaudio_test_ping(test, &t), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);
	macaudio_test_expect_vols(test, &t, true);
}

static struct kunit_case macaudio_pm_volumes_cases[] = {
	KUNIT_CASE(macaudio_pm_volumes_restored_after_sleep),
	KUNIT_CASE(macaudio_pm_volumes_second_sleep_before_ping),
	KUNIT_CASE(macaudio_pm_volumes_new_owner),
	{}
};

static struct kunit_suite macaudio_pm_volumes_suite = {
	.name = "apple-speaker-pm-volumes",
	.test_cases = macaudio_pm_volumes_cases,
};
kunit_test_suite(macaudio_pm_volumes_suite);

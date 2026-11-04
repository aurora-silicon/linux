/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Speaker protection ownership: the safe level from the card's binding on,
 * the grant to the owner of "Speaker Protection", its lease and every way
 * protection is withdrawn. Through the control callbacks and the PM
 * notifier, on a card of fake controls.
 */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/delay.h>

struct macaudio_test_vol {
	struct soc_mixer_control mc;
	long value;
	int fail;
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
	uinfo->value.integer.max = macaudio_test_vol(kctl)->mc.max;
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
	bool changed;

	if (vol->fail)
		return vol->fail;
	changed = vol->value != uval->value.integer.value[0];
	vol->value = uval->value.integer.value[0];
	return changed;
}

static void macaudio_test_free_kctl(void *kctl)
{
	snd_ctl_free_one(kctl);
}

/* Last in, so first out: before the controls go */
static void macaudio_test_spk_cleanup(void *data)
{
	struct macaudio_snd_data *ma = data;

	cancel_delayed_work_sync(&ma->spk_lease_work);
	cancel_work_sync(&ma->spk_stop_work);
}

static const char *const macaudio_test_vol_names[] = {
	"Left Woofer Speaker Volume",
	"Right Woofer Speaker Volume",
	"Left Tweeter Speaker Volume",
	"Right Tweeter Speaker Volume",
	/* Not a speaker volume: never taken or set */
	"Headphone Playback Volume",
};

/* The same speakers behind a software gain (J700) */
static const char *const macaudio_test_sw_vol_names[] = {
	"Left Speaker Playback Volume",
	"Right Speaker Playback Volume",
	"Left Rear Speaker Playback Volume",
	"Right Rear Speaker Playback Volume",
};

#define MACAUDIO_TEST_NR_VOLS ARRAY_SIZE(macaudio_test_vol_names)
#define MACAUDIO_TEST_NR_SPK (MACAUDIO_TEST_NR_VOLS - 1)
/* Whatever userspace or the codec left there before the card bound */
#define MACAUDIO_TEST_START_VOL 150

struct macaudio_test_spk {
	struct macaudio_snd_data *ma;
	struct snd_kcontrol *spk;
	struct snd_kcontrol *vols[MACAUDIO_TEST_NR_VOLS];
	struct snd_ctl_file *owner;
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

/* A bound card with speakers: its protection control and volumes, no owner */
static void macaudio_test_spk_card(struct kunit *test, struct macaudio_test_spk *t,
				   const struct macaudio_platform_cfg *cfg, long max)
{
	struct snd_kcontrol_new spk = macaudio_controls[ARRAY_SIZE(macaudio_controls) - 1];
	struct macaudio_snd_data *ma;
	struct snd_card *card;
	struct device *dev;
	unsigned int i;
	int ret;

	ma = kunit_kzalloc(test, sizeof(*ma), GFP_KERNEL);
	card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);
	t->owner = kunit_kzalloc(test, sizeof(*t->owner), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ma);
	KUNIT_ASSERT_NOT_NULL(test, card);
	KUNIT_ASSERT_NOT_NULL(test, t->owner);
	dev = kunit_device_register(test, "macaudio-spk-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));

	INIT_LIST_HEAD(&card->controls);
	INIT_LIST_HEAD(&card->ctl_files);
	init_rwsem(&card->controls_rwsem);
	rwlock_init(&card->controls_rwlock);
	ma->card.snd_card = card;
	ma->card.dev = dev;
	INIT_LIST_HEAD(&ma->card.rtd_list);
	snd_soc_card_set_drvdata(&ma->card, ma);
	ma->cfg = cfg;
	ma->has_speakers = true;
	mutex_init(&ma->spk_mutex);
	INIT_DELAYED_WORK(&ma->spk_lease_work, macaudio_spk_lease_work);
	INIT_WORK(&ma->spk_stop_work, macaudio_spk_floor_work);

	t->ma = ma;
	t->spk = macaudio_test_add_kctl(test, card, &spk, &ma->card);

	for (i = 0; i < MACAUDIO_TEST_NR_VOLS; i++) {
		struct macaudio_test_vol *vol = kunit_kzalloc(test, sizeof(*vol), GFP_KERNEL);
		struct snd_kcontrol_new tmpl = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = macaudio_test_vol_names[i],
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
			.info = macaudio_test_vol_info,
			.get = macaudio_test_vol_get,
			.put = macaudio_test_vol_put,
		};

		KUNIT_ASSERT_NOT_NULL(test, vol);
		vol->mc.max = max;
		vol->value = MACAUDIO_TEST_START_VOL;
		if (cfg->amp == AMP_MAX98360A && i < MACAUDIO_TEST_NR_SPK)
			tmpl.name = macaudio_test_sw_vol_names[i];
		tmpl.private_value = (unsigned long)&vol->mc;
		t->vols[i] = macaudio_test_add_kctl(test, card, &tmpl, NULL);
	}

	ret = kunit_add_action_or_reset(test, macaudio_test_spk_cleanup, ma);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* As macaudio_late_probe() does */
	ma->spk_kctl = t->spk;
	KUNIT_ASSERT_EQ(test, macaudio_spk_take_volumes(ma), 0);
	mutex_lock(&ma->spk_mutex);
	macaudio_spk_floor(ma, "card bound");
	mutex_unlock(&ma->spk_mutex);
}

static int macaudio_test_write(struct kunit *test, struct macaudio_test_spk *t, long ms)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	uval->value.integer.value[0] = ms;
	return macaudio_spk_put(t->spk, uval);
}

static long macaudio_test_read(struct kunit *test, struct macaudio_test_spk *t)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	KUNIT_EXPECT_EQ(test, macaudio_spk_get(t->spk, uval), 0);
	return uval->value.integer.value[0];
}

static void macaudio_test_take(struct kunit *test, struct macaudio_test_spk *t)
{
	KUNIT_ASSERT_EQ(test, macaudio_spk_lock(t->spk, t->owner), 0);
}

/* Every speaker volume at @want, the headphone volume never touched */
static void macaudio_test_expect_vols(struct kunit *test, struct macaudio_test_spk *t, long want)
{
	unsigned int i;

	for (i = 0; i < MACAUDIO_TEST_NR_SPK; i++)
		KUNIT_EXPECT_EQ(test, macaudio_test_vol(t->vols[i])->value, want);
	KUNIT_EXPECT_EQ(test, macaudio_test_vol(t->vols[MACAUDIO_TEST_NR_SPK])->value,
			(long)MACAUDIO_TEST_START_VOL);
}

static void macaudio_spk_floor_from_binding(struct kunit *test)
{
	struct macaudio_test_spk t;
	unsigned int i;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	KUNIT_EXPECT_EQ(test, t.ma->spk_nr_vols, (unsigned int)MACAUDIO_TEST_NR_SPK);

	/* Userspace can read the speaker volumes but never write them */
	for (i = 0; i < MACAUDIO_TEST_NR_SPK; i++)
		KUNIT_EXPECT_FALSE(test, t.vols[i]->vd[0].access & SNDRV_CTL_ELEM_ACCESS_WRITE);
	KUNIT_EXPECT_TRUE(test, t.vols[MACAUDIO_TEST_NR_SPK]->vd[0].access &
			  SNDRV_CTL_ELEM_ACCESS_WRITE);
}

static void macaudio_spk_grant_needs_owner_and_valid_lease(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), -EPERM);
	macaudio_test_expect_vols(test, &t, 0);

	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MIN_MS - 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MAX_MS + 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, -1), -EINVAL);
	/* A write of the read-back state is not a lease */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	macaudio_test_expect_vols(test, &t, 0);

	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
	macaudio_test_expect_vols(test, &t, SN012776_0DB);

	/* Renewals keep the operating gain */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MAX_MS), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
	macaudio_test_expect_vols(test, &t, SN012776_0DB);
}

static void macaudio_spk_release_and_owner_gone(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);

	/* An explicit release is at once, and the owner can grant again */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	macaudio_test_expect_vols(test, &t, SN012776_0DB);

	/* The owner's file closes or unlocks: at once, and nobody else may grant */
	macaudio_spk_unlock(t.spk);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -EPERM);

	/* A new owner starts without protection */
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
}

static void macaudio_spk_lease_expires(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MIN_MS), 0);

	/* Not before the lease ends */
	flush_delayed_work(&t.ma->spk_lease_work);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);

	msleep(3 * SPEAKER_LEASE_MIN_MS);
	flush_delayed_work(&t.ma->spk_lease_work);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	macaudio_test_expect_vols(test, &t, 0);

	/* The stalled owner learns of it once, and that write grants nothing */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
}

static void macaudio_spk_renewal_extends_lease(struct kunit *test)
{
	struct macaudio_test_spk t;
	unsigned int i;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	/* Renewed well within the lease, for several of its lengths */
	for (i = 0; i < 6; i++) {
		KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 4 * SPEAKER_LEASE_MIN_MS), 0);
		msleep(SPEAKER_LEASE_MIN_MS);
		flush_delayed_work(&t.ma->spk_lease_work);
		KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
	}
}

static void macaudio_spk_system_sleep(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);

	macaudio_pm_notify(&t.ma->pm_nb, PM_SUSPEND_PREPARE, NULL);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -EBUSY);

	macaudio_pm_notify(&t.ma->pm_nb, PM_POST_SUSPEND, NULL);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
	macaudio_test_expect_vols(test, &t, SN012776_0DB);
}

static void macaudio_spk_failed_floor_is_permanent(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);

	/* One amp stops answering: the floor fails on it */
	macaudio_test_vol(t.vols[2])->fail = -EIO;
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->spk_floor_failed);
	macaudio_test_vol(t.vols[2])->fail = 0;

	/* No grant again until the driver is reloaded */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -EIO);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
}

static void macaudio_spk_failed_grant_floors(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	macaudio_test_vol(t.vols[1])->fail = -EIO;
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -EIO);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	/* The amps that took the operating gain are back at the safe level */
	KUNIT_EXPECT_EQ(test, macaudio_test_vol(t.vols[0])->value, 0L);
	KUNIT_EXPECT_EQ(test, macaudio_test_vol(t.vols[3])->value, 0L);
}

/* J700: a fixed-gain amp behind a software gain, 20 dB down at the floor */
static void macaudio_spk_j700_levels(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j700_cfg, MAX98360A_0DB);
	macaudio_test_expect_vols(test, &t, MAX98360A_DB(-20));
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	macaudio_test_expect_vols(test, &t, MAX98360A_0DB);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), 0);
	macaudio_test_expect_vols(test, &t, MAX98360A_DB(-20));
}

static struct kunit_case macaudio_protection_cases[] = {
	KUNIT_CASE(macaudio_spk_floor_from_binding),
	KUNIT_CASE(macaudio_spk_grant_needs_owner_and_valid_lease),
	KUNIT_CASE(macaudio_spk_release_and_owner_gone),
	KUNIT_CASE(macaudio_spk_lease_expires),
	KUNIT_CASE(macaudio_spk_renewal_extends_lease),
	KUNIT_CASE(macaudio_spk_system_sleep),
	KUNIT_CASE(macaudio_spk_failed_floor_is_permanent),
	KUNIT_CASE(macaudio_spk_failed_grant_floors),
	KUNIT_CASE(macaudio_spk_j700_levels),
	{}
};

static struct kunit_suite macaudio_protection_suite = {
	.name = "apple-speaker-protection",
	.test_cases = macaudio_protection_cases,
};
kunit_test_suite(macaudio_protection_suite);

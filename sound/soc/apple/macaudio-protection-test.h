/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Speaker protection ownership: the safe level from the card's binding on,
 * the grant to the owner of "Speaker Protection", its lease and every way
 * protection is withdrawn. Through the control callbacks and the PM
 * notifier, on a card of fake controls.
 */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/delay.h>

#define MACAUDIO_TEST_HIST 32

struct macaudio_test_vol {
	struct soc_mixer_control mc;
	long value;
	int fail;
	/* Every value written, in order, while @record */
	bool record;
	long hist[MACAUDIO_TEST_HIST];
	unsigned int nhist;
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
	/* As snd_soc_info_volsw(): a limit is the top of the range */
	uinfo->value.integer.max = macaudio_test_vol(kctl)->mc.platform_max ?:
				   macaudio_test_vol(kctl)->mc.max;
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
	if (vol->record && vol->nhist < MACAUDIO_TEST_HIST)
		vol->hist[vol->nhist++] = uval->value.integer.value[0];
	changed = vol->value != uval->value.integer.value[0];
	vol->value = uval->value.integer.value[0];
	return changed;
}

static void macaudio_test_free_kctl(void *kctl)
{
	snd_ctl_free_one(kctl);
}

/* Every withdrawal the lease timer started has run */
static void macaudio_test_settle(struct macaudio_snd_data *ma)
{
	kthread_flush_worker(ma->spk_worker);
	kthread_flush_worker(ma->spk_stop_worker);
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
	/* "Speaker Protection Crossover" */
	struct snd_kcontrol *xover;
	struct snd_ctl_file *owner;
	/* The codec the speaker volumes belong to */
	struct snd_soc_component component;
};

/* The card's control named @name */
static struct snd_kcontrol_new macaudio_test_control(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(macaudio_controls); i++)
		if (macaudio_controls[i].name && !strcmp(macaudio_controls[i].name, name))
			return macaudio_controls[i];
	return (struct snd_kcontrol_new){};
}

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
	struct snd_kcontrol_new spk = macaudio_test_control("Speaker Protection");
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
	/* Powered (D0), as a bound card is */
	init_waitqueue_head(&card->power_sleep);
	snd_refcount_init(&card->power_ref);
	ma->card.snd_card = card;
	ma->card.dev = dev;
	INIT_LIST_HEAD(&ma->card.rtd_list);
	snd_soc_card_set_drvdata(&ma->card, ma);
	ma->cfg = cfg;
	ma->has_speakers = true;
	mutex_init(&ma->spk_mutex);
	mutex_init(&ma->volume_lock_mutex);
	INIT_DELAYED_WORK(&ma->lock_timeout_work, macaudio_vlimit_timeout_work);
	INIT_WORK(&ma->lock_fail_work, macaudio_lock_fail_work);
	INIT_WORK(&ma->sw_gain_stop_work, macaudio_sw_gain_stop_work);
	/* The lease is the interface, as after "Speaker Protection Interface" */
	ma->spk_iface = MACAUDIO_SPK_IFACE_LEASE;
	KUNIT_ASSERT_EQ(test, macaudio_spk_init(ma), 0);
	t->component.card = &ma->card;

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
		t->vols[i] = macaudio_test_add_kctl(test, card, &tmpl, &t->component);
	}
	t->xover = macaudio_test_add_kctl(test, card, &macaudio_spk_xover_control, &ma->card);

	/* Last in, so first out: before the controls go */
	ret = kunit_add_action_or_reset(test, macaudio_spk_fini, ma);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* As macaudio_late_probe() does */
	ma->spk_kctl = t->spk;
	ma->spk_xover_kctl = t->xover;
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

static int macaudio_test_xover_write(struct kunit *test, struct macaudio_test_spk *t, long val)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	uval->value.integer.value[0] = val;
	return macaudio_spk_xover_put(t->xover, uval);
}

static long macaudio_test_xover_read(struct kunit *test, struct macaudio_test_spk *t)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	KUNIT_EXPECT_EQ(test, macaudio_spk_xover_get(t->xover, uval), 0);
	return uval->value.integer.value[0];
}

/* The owner locks the crossover control, as SNDRV_CTL_IOCTL_ELEM_LOCK does */
static void macaudio_test_lock_xover(struct kunit *test, struct macaudio_test_spk *t)
{
	KUNIT_ASSERT_EQ(test, macaudio_spk_xover_lock(t->xover, t->owner), 0);
	t->xover->vd[0].owner = t->owner;
}

/* Take the speakers and declare the crossover in the engine */
static void macaudio_test_take_with_xover(struct kunit *test, struct macaudio_test_spk *t)
{
	macaudio_test_take(test, t);
	macaudio_test_lock_xover(test, t);
	KUNIT_ASSERT_EQ(test, macaudio_test_xover_write(test, t, 1), 1);
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
		KUNIT_EXPECT_EQ(test, t.vols[i]->write_check(t.vols[i], NULL), -EPERM);
	KUNIT_EXPECT_NULL(test, t.vols[MACAUDIO_TEST_NR_SPK]->write_check);
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
	macaudio_test_settle(t.ma);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);

	msleep(3 * SPEAKER_LEASE_MIN_MS);
	macaudio_test_settle(t.ma);
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
		macaudio_test_settle(t.ma);
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
	/* The owner was frozen: it learns of it before it plays again */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
	macaudio_test_expect_vols(test, &t, SN012776_0DB);

	/* A sleep with no owner reports nothing to the next one */
	macaudio_spk_unlock(t.spk);
	macaudio_pm_notify(&t.ma->pm_nb, PM_SUSPEND_PREPARE, NULL);
	macaudio_pm_notify(&t.ma->pm_nb, PM_POST_SUSPEND, NULL);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
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

/* Every start checks the lease's end itself, whether the timer has run or not */
static void macaudio_spk_gate_follows_lease_end(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(t.ma));
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_admitted(t.ma));

	/* The lease ends before its timer could close the gate */
	hrtimer_cancel(&t.ma->spk_lease_timer);
	WRITE_ONCE(t.ma->spk_lease_end, ktime_sub_ms(ktime_get(), 1));
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t.ma->spk_granted));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(t.ma));
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);

	/* A renewal that comes too late is a stall: floor, -ETIMEDOUT, nothing granted */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(t.ma));
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_admitted(t.ma));
	macaudio_test_expect_vols(test, &t, SN012776_0DB);
}

/* The gate closes at the lease's end even while the floor cannot run */
static void macaudio_spk_timer_beats_a_blocked_worker(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MIN_MS), 0);

	mutex_lock(&t.ma->spk_mutex);
	msleep(3 * SPEAKER_LEASE_MIN_MS);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(t.ma->spk_granted));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(t.ma));
	/* The floor waits for the mutex */
	KUNIT_EXPECT_TRUE(test, t.ma->spk_active);
	mutex_unlock(&t.ma->spk_mutex);

	macaudio_test_settle(t.ma);
	KUNIT_EXPECT_FALSE(test, t.ma->spk_active);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
}

struct macaudio_test_spinner {
	struct macaudio_snd_data *ma;
	ktime_t deadline;
	ktime_t closed;
	struct completion done;
};

/* A stuck real-time thread: it never sleeps until the gate closes or the deadline */
static int macaudio_test_spin(void *data)
{
	struct macaudio_test_spinner *s = data;
	ktime_t now;

	for (;;) {
		now = ktime_get();
		if (!READ_ONCE(s->ma->spk_granted)) {
			s->closed = now;
			break;
		}
		if (!ktime_before(now, s->deadline))
			break;
		cpu_relax();
	}
	complete(&s->done);
	return 0;
}

#define MACAUDIO_TEST_MAX_SPINNERS 8

/*
 * SCHED_FIFO 99 spinners on every CPU, the lease timer's among them: the
 * gate still closes at the lease's end, and the floor follows once a CPU is
 * free again.
 */
static void macaudio_spk_timer_beats_a_fifo_spinner(struct kunit *test)
{
	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_FIFO,
		.sched_priority = MAX_RT_PRIO - 1,
	};
	struct task_struct *tasks[MACAUDIO_TEST_MAX_SPINNERS];
	struct macaudio_test_spinner *s;
	struct macaudio_test_spk t;
	unsigned int cpu, n = 0, i;
	ktime_t end;
	s64 late;

	s = kunit_kcalloc(test, MACAUDIO_TEST_MAX_SPINNERS, sizeof(*s), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, s);
	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);

	/* Ready before the lease starts: once they run, nothing else does */
	for_each_online_cpu(cpu) {
		if (n == MACAUDIO_TEST_MAX_SPINNERS)
			break;
		s[n].ma = t.ma;
		init_completion(&s[n].done);
		tasks[n] = kthread_create(macaudio_test_spin, &s[n], "spk-spin/%u", cpu);
		KUNIT_ASSERT_FALSE(test, IS_ERR(tasks[n]));
		kthread_bind(tasks[n], cpu);
		KUNIT_ASSERT_EQ(test, sched_setattr_nocheck(tasks[n], &attr), 0);
		n++;
	}

	KUNIT_ASSERT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MIN_MS), 0);
	end = READ_ONCE(t.ma->spk_lease_end);
	for (i = 0; i < n; i++)
		s[i].deadline = ktime_add_ms(end, 200);
	/* All at once: the one on this CPU would hold off the others' wakeup */
	preempt_disable();
	for (i = 0; i < n; i++)
		wake_up_process(tasks[i]);
	preempt_enable();

	for (i = 0; i < n; i++) {
		wait_for_completion(&s[i].done);
		late = ktime_to_us(ktime_sub(s[i].closed, end));
		/* Closed at the lease's end plus timer slack, not at the spinner's deadline */
		KUNIT_EXPECT_NE(test, s[i].closed, (ktime_t)0);
		KUNIT_EXPECT_GE(test, late, 0LL);
		KUNIT_EXPECT_LT(test, late, 10000LL);
		kunit_info(test, "spinner %u saw the gate close %lld us after the lease's end\n",
			   i, late);
	}

	macaudio_test_settle(t.ma);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -ETIMEDOUT);
}

static void macaudio_spk_workers_are_fifo(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	KUNIT_EXPECT_EQ(test, t.ma->spk_worker->task->policy, SCHED_FIFO);
	KUNIT_EXPECT_EQ(test, t.ma->spk_worker->task->rt_priority, SPEAKER_WORKER_PRIO);
	KUNIT_EXPECT_EQ(test, t.ma->spk_stop_worker->task->policy, SCHED_FIFO);
	KUNIT_EXPECT_EQ(test, t.ma->spk_stop_worker->task->rt_priority, SPEAKER_WORKER_PRIO);
}

/* Only a CAP_SYS_ADMIN process owns the speakers or writes the lease */
static void macaudio_spk_ownership_needs_cap_sys_admin(struct kunit *test)
{
	struct macaudio_test_spk t;
	const struct cred *old;
	struct cred *seat;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	seat = prepare_creds();
	KUNIT_ASSERT_NOT_NULL(test, seat);
	cap_lower(seat->cap_effective, CAP_SYS_ADMIN);

	old = override_creds(seat);
	KUNIT_EXPECT_EQ(test, macaudio_spk_lock(t.spk, t.owner), -EPERM);
	revert_creds(old);
	KUNIT_EXPECT_NULL(test, t.ma->spk_owner);

	/* The supervisor's file, handed on to a process without the capability */
	macaudio_test_take(test, &t);
	old = override_creds(seat);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), -EPERM);
	revert_creds(old);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	macaudio_test_expect_vols(test, &t, 0);

	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
	put_cred(seat);
}

struct macaudio_test_blocker {
	struct kthread_work work;
	struct completion started;
	struct completion release;
};

static void macaudio_test_block(struct kthread_work *work)
{
	struct macaudio_test_blocker *b = container_of(work, struct macaudio_test_blocker, work);

	complete(&b->started);
	wait_for_completion(&b->release);
}

struct macaudio_test_regrant {
	struct macaudio_test_spk *t;
	struct snd_ctl_elem_value *uval;
	int ret;
	struct completion done;
};

static int macaudio_test_regrant(void *data)
{
	struct macaudio_test_regrant *r = data;

	r->ret = macaudio_spk_put(r->t->spk, r->uval);
	complete(&r->done);
	return 0;
}

/* A grant waits for the stop a withdrawal queued before the amps go up */
static void macaudio_spk_regrant_waits_for_the_stop(struct kunit *test)
{
	struct macaudio_test_blocker b;
	struct macaudio_test_regrant r;
	struct macaudio_test_spk t;
	struct task_struct *task;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);

	/* Hold the stop worker, as a starved one would be */
	kthread_init_work(&b.work, macaudio_test_block);
	init_completion(&b.started);
	init_completion(&b.release);
	kthread_queue_work(t.ma->spk_stop_worker, &b.work);
	wait_for_completion(&b.started);

	/* The release queues its stop behind the held one */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), 0);

	r.t = &t;
	r.uval = kunit_kzalloc(test, sizeof(*r.uval), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r.uval);
	r.uval->value.integer.value[0] = 30;
	init_completion(&r.done);
	task = kthread_run(macaudio_test_regrant, &r, "spk-regrant");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));

	msleep(2 * SPEAKER_LEASE_MIN_MS);
	KUNIT_EXPECT_FALSE(test, completion_done(&r.done));
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(t.ma));

	complete(&b.release);
	wait_for_completion(&r.done);
	KUNIT_EXPECT_EQ(test, r.ret, 0);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_admitted(t.ma));
	macaudio_test_expect_vols(test, &t, SN012776_0DB);
}

/*
 * "Amp Gain Volume": limited at the configured gain, fixed at it while
 * leased and not writable then, the amp's own under the volume lock.
 */
static void macaudio_spk_amp_gain_is_fixed(struct kunit *test)
{
	static const char *const names[] = {
		"Left Woofer Amp Gain Volume",
		"Right Woofer Amp Gain Volume",
	};
	struct macaudio_test_vol *vols[ARRAY_SIZE(names)];
	struct snd_kcontrol *kctls[ARRAY_SIZE(names)];
	struct macaudio_test_spk t;
	const int level = 15;
	unsigned int i;
	int ret;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct snd_kcontrol_new tmpl = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = names[i],
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
			.info = macaudio_test_vol_info,
			.get = macaudio_test_vol_get,
			.put = macaudio_test_vol_put,
		};

		vols[i] = kunit_kzalloc(test, sizeof(*vols[i]), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, vols[i]);
		vols[i]->mc.max = 20;
		tmpl.private_value = (unsigned long)&vols[i]->mc;
		kctls[i] = macaudio_test_add_kctl(test, t.ma->card.snd_card, &tmpl,
						  &t.component);
	}
	/* Above the amp level (any level the ADT gives), and below it */
	vols[0]->value = 20;
	vols[1]->value = 5;

	KUNIT_EXPECT_EQ(test, snd_soc_limit_volume(&t.ma->card, "* Amp Gain Volume", level), 2);
	KUNIT_EXPECT_EQ(test, vols[0]->value, (long)level);
	KUNIT_EXPECT_EQ(test, vols[1]->value, 5L);
	KUNIT_ASSERT_EQ(test, macaudio_spk_take_amp_gains(t.ma), 0);
	KUNIT_EXPECT_EQ(test, t.ma->spk_nr_amp_gains, (unsigned int)ARRAY_SIZE(names));

	mutex_lock(&t.ma->spk_mutex);
	ret = macaudio_spk_fix_amp_gains(t.ma);
	mutex_unlock(&t.ma->spk_mutex);
	KUNIT_EXPECT_EQ(test, ret, 0);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		KUNIT_EXPECT_EQ(test, vols[i]->value, (long)level);
		KUNIT_EXPECT_EQ(test, vols[i]->mc.platform_max, level);
		KUNIT_EXPECT_EQ(test, kctls[i]->write_check(kctls[i], NULL), -EPERM);
	}
	/* The speaker volumes and the headphone volume are not amp gains */
	KUNIT_EXPECT_NULL(test, t.vols[MACAUDIO_TEST_NR_SPK]->write_check);

	/* Under the volume lock they are the amp's again */
	WRITE_ONCE(t.ma->spk_iface, MACAUDIO_SPK_IFACE_UNLOCK);
	for (i = 0; i < ARRAY_SIZE(names); i++)
		KUNIT_EXPECT_EQ(test, kctls[i]->write_check(kctls[i], NULL), 0);
}

struct macaudio_test_sleep {
	struct macaudio_snd_data *ma;
	int ret;
	struct completion done;
};

static int macaudio_test_sleep_prepare(void *data)
{
	struct macaudio_test_sleep *z = data;

	z->ret = macaudio_pm_notify(&z->ma->pm_nb, PM_SUSPEND_PREPARE, NULL);
	complete(&z->done);
	return 0;
}

/*
 * PM_SUSPEND_PREPARE closes the gate before it waits for anything: here a
 * writer holds spk_mutex across the sleep's start, as a supervisor stuck
 * in a write would.
 */
static void macaudio_spk_sleep_closes_the_gate_first(struct kunit *test)
{
	struct macaudio_test_sleep z;
	struct macaudio_test_spk t;
	struct task_struct *task;
	unsigned int i;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MAX_MS), 0);

	z.ma = t.ma;
	init_completion(&z.done);
	mutex_lock(&t.ma->spk_mutex);
	task = kthread_run(macaudio_test_sleep_prepare, &z, "spk-sleep");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	for (i = 0; i < 100 && !READ_ONCE(t.ma->pm_quiesced); i++)
		usleep_range(500, 1000);

	/* Closed at once, with the floor still waiting for the lock */
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t.ma->pm_quiesced));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(t.ma->spk_granted));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(t.ma));
	KUNIT_EXPECT_STREQ(test, t.ma->spk_gate_reason, "system sleep");
	KUNIT_EXPECT_FALSE(test, completion_done(&z.done));
	macaudio_test_expect_vols(test, &t, SN012776_0DB);
	mutex_unlock(&t.ma->spk_mutex);

	wait_for_completion(&z.done);
	KUNIT_EXPECT_EQ(test, z.ret, NOTIFY_OK);
	KUNIT_EXPECT_FALSE(test, t.ma->spk_active);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -EBUSY);

	/* After the resume: the stall first, then a grant */
	KUNIT_EXPECT_EQ(test, macaudio_pm_notify(&t.ma->pm_nb, PM_POST_SUSPEND, NULL), NOTIFY_OK);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
}

/* Hibernation takes the same path, and the lease never outlives a floor */
static void macaudio_spk_hibernation_floors(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_pm_notify(&t.ma->pm_nb, PM_HIBERNATION_PREPARE, NULL),
			NOTIFY_OK);
	KUNIT_EXPECT_FALSE(test, hrtimer_active(&t.ma->spk_lease_timer));
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -EBUSY);
	KUNIT_EXPECT_EQ(test, macaudio_pm_notify(&t.ma->pm_nb, PM_POST_RESTORE, NULL), NOTIFY_OK);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
}

/* The floor runs first in the sleep notifier chain */
static void macaudio_spk_sleep_notifier_is_first(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	KUNIT_EXPECT_TRUE(test, t.ma->pm_nb.notifier_call == macaudio_pm_notify);
	KUNIT_EXPECT_EQ(test, t.ma->pm_nb.priority, INT_MAX);
}

/* A card without speakers (the J616s jack) leaves sleep alone */
static void macaudio_spk_sleep_skips_a_card_without_speakers(struct kunit *test)
{
	struct macaudio_snd_data *ma = kunit_kzalloc(test, sizeof(*ma), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ma);
	mutex_init(&ma->spk_mutex);
	ma->has_speakers = false;
	KUNIT_EXPECT_EQ(test, macaudio_pm_notify(&ma->pm_nb, PM_SUSPEND_PREPARE, NULL),
			NOTIFY_DONE);
	KUNIT_EXPECT_FALSE(test, ma->spk_floor_failed);
	KUNIT_EXPECT_FALSE(test, ma->pm_quiesced);
	KUNIT_EXPECT_EQ(test, macaudio_pm_notify(&ma->pm_nb, PM_POST_SUSPEND, NULL), NOTIFY_DONE);
}

/* alsactl's restore and mixers leave the ownership controls alone */
static void macaudio_spk_ownership_controls_are_inactive(struct kunit *test)
{
	struct macaudio_test_spk t;
	unsigned int want = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			    SNDRV_CTL_ELEM_ACCESS_VOLATILE |
			    SNDRV_CTL_ELEM_ACCESS_INACTIVE;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	KUNIT_EXPECT_EQ(test, t.spk->vd[0].access & want, want);
	KUNIT_EXPECT_EQ(test, t.xover->vd[0].access & want, want);
	/* Still the owner's to write */
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
}

static void macaudio_test_record(struct macaudio_test_spk *t)
{
	unsigned int i;

	for (i = 0; i < MACAUDIO_TEST_NR_VOLS; i++) {
		macaudio_test_vol(t->vols[i])->record = true;
		macaudio_test_vol(t->vols[i])->nhist = 0;
	}
}

/* The steps a withdrawal wrote to one amp: falling, ending at the floor */
static unsigned int macaudio_test_expect_ramp(struct kunit *test, struct macaudio_test_spk *t,
					      unsigned int amp)
{
	struct macaudio_test_vol *vol = macaudio_test_vol(t->vols[amp]);
	unsigned int i;

	KUNIT_ASSERT_GT(test, vol->nhist, 0U);
	for (i = 1; i < vol->nhist; i++)
		KUNIT_EXPECT_LT(test, vol->hist[i], vol->hist[i - 1]);
	KUNIT_EXPECT_LT(test, vol->hist[0], (long)SN012776_0DB);
	KUNIT_EXPECT_EQ(test, vol->hist[vol->nhist - 1], 0L);
	return vol->nhist;
}

/*
 * A release, the owner gone or replaced while its lease runs: the amps
 * ramp down before the stream stops, and the whole withdrawal is short.
 */
static void macaudio_spk_withdrawal_ramps(struct kunit *test)
{
	struct macaudio_test_spk t;
	unsigned int i, n;
	ktime_t t0;
	s64 us;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);

	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MAX_MS), 0);
	macaudio_test_record(&t);
	t0 = ktime_get();
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), 0);
	us = ktime_us_delta(ktime_get(), t0);
	KUNIT_EXPECT_LT(test, us, 20 * USEC_PER_MSEC);
	/* Every step, unless a slow machine ran into the ramp's limit */
	for (i = 0; i < MACAUDIO_TEST_NR_SPK; i++) {
		n = macaudio_test_expect_ramp(test, &t, i);
		KUNIT_EXPECT_GE(test, n, 2U);
		KUNIT_EXPECT_LE(test, n, (unsigned int)ARRAY_SIZE(macaudio_spk_ramp_db) + 1);
	}
	KUNIT_EXPECT_EQ(test, macaudio_test_vol(t.vols[MACAUDIO_TEST_NR_SPK])->nhist, 0U);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);

	/* The owner's file closes */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MAX_MS), 0);
	macaudio_test_record(&t);
	macaudio_spk_unlock(t.spk);
	macaudio_test_expect_ramp(test, &t, 0);
	macaudio_test_expect_vols(test, &t, 0);
}

/* Never past the lease: a withdrawal at its end stops at once */
static void macaudio_spk_ramp_ends_with_the_lease(struct kunit *test)
{
	struct macaudio_test_spk t;
	struct macaudio_test_vol *vol;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MAX_MS), 0);
	macaudio_test_record(&t);
	/* As if the lease ended now; the timer has not run yet */
	mutex_lock(&t.ma->spk_mutex);
	WRITE_ONCE(t.ma->spk_lease_end, ktime_get());
	macaudio_spk_floor_ramped(t.ma, "test");
	mutex_unlock(&t.ma->spk_mutex);
	vol = macaudio_test_vol(t.vols[0]);
	KUNIT_ASSERT_EQ(test, vol->nhist, 1U);
	KUNIT_EXPECT_EQ(test, vol->hist[0], 0L);
}

/* A lapse and a sleep go straight to the floor */
static void macaudio_spk_hard_withdrawals_do_not_ramp(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MIN_MS), 0);
	macaudio_test_record(&t);
	msleep(3 * SPEAKER_LEASE_MIN_MS);
	macaudio_test_settle(t.ma);
	KUNIT_ASSERT_EQ(test, macaudio_test_vol(t.vols[0])->nhist, 1U);
	KUNIT_EXPECT_EQ(test, macaudio_test_vol(t.vols[0])->hist[0], 0L);

	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	macaudio_test_record(&t);
	macaudio_pm_notify(&t.ma->pm_nb, PM_SUSPEND_PREPARE, NULL);
	KUNIT_ASSERT_EQ(test, macaudio_test_vol(t.vols[0])->nhist, 1U);
	KUNIT_EXPECT_EQ(test, macaudio_test_vol(t.vols[0])->hist[0], 0L);
	macaudio_pm_notify(&t.ma->pm_nb, PM_POST_SUSPEND, NULL);
}

/* The J700's software gain goes straight to its floor too */
static void macaudio_spk_j700_does_not_ramp(struct kunit *test)
{
	struct macaudio_test_spk t;

	macaudio_test_spk_card(test, &t, &macaudio_j700_cfg, MAX98360A_0DB);
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	macaudio_test_record(&t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), 0);
	KUNIT_ASSERT_EQ(test, macaudio_test_vol(t.vols[0])->nhist, 1U);
	KUNIT_EXPECT_EQ(test, macaudio_test_vol(t.vols[0])->hist[0], (long)MAX98360A_DB(-20));
}

struct macaudio_test_unlock {
	struct snd_kcontrol *spk;
	struct completion done;
};

static int macaudio_test_unlock_fn(void *data)
{
	struct macaudio_test_unlock *u = data;

	macaudio_spk_unlock(u->spk);
	complete(&u->done);
	return 0;
}

/*
 * The owner dies while the card resumes: the gate closes at once, the amps
 * are written only once the card is back at D0, and never ramped.
 */
static void macaudio_spk_floor_waits_for_the_resume(struct kunit *test)
{
	struct snd_card *card;
	struct macaudio_test_unlock u;
	struct macaudio_test_spk t;
	struct task_struct *task;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	card = t.ma->card.snd_card;
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, SPEAKER_LEASE_MAX_MS), 0);
	macaudio_test_record(&t);

	/* ASoC's resume work: D2 until the codecs and streams are back */
	snd_power_change_state(card, SNDRV_CTL_POWER_D2);
	u.spk = t.spk;
	init_completion(&u.done);
	task = kthread_run(macaudio_test_unlock_fn, &u, "spk-unlock");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	msleep(20);
	KUNIT_EXPECT_FALSE(test, completion_done(&u.done));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(t.ma->spk_granted));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(t.ma));
	KUNIT_EXPECT_EQ(test, macaudio_test_vol(t.vols[0])->nhist, 0U);
	macaudio_test_expect_vols(test, &t, SN012776_0DB);

	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	KUNIT_ASSERT_GT(test, wait_for_completion_timeout(&u.done, HZ), 0UL);
	KUNIT_ASSERT_EQ(test, macaudio_test_vol(t.vols[0])->nhist, 1U);
	macaudio_test_expect_vols(test, &t, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&card->power_ref.count), 0);
}

/* A grant needs a powered card */
static void macaudio_spk_grant_needs_d0(struct kunit *test)
{
	struct macaudio_test_spk t;
	struct snd_card *card;

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	card = t.ma->card.snd_card;
	macaudio_test_take(test, &t);
	snd_power_change_state(card, SNDRV_CTL_POWER_D2);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), -EBUSY);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 0L);
	macaudio_test_expect_vols(test, &t, 0);
	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &t), 1L);
}

/* A front-end driving the speaker back-end, as DPCM connects them */
struct macaudio_test_fe {
	struct snd_soc_dai_link fe_link, be_link;
	struct snd_soc_pcm_runtime *fe, *be;
	struct snd_soc_dpcm dpcm;
	struct snd_pcm_substream play, cap, be_play;
	struct macaudio_link_props props[2];
};

static void macaudio_test_fe_setup(struct kunit *test, struct macaudio_test_spk *t,
				   struct macaudio_test_fe *f)
{
	f->fe = kunit_kzalloc(test, sizeof(*f->fe), GFP_KERNEL);
	f->be = kunit_kzalloc(test, sizeof(*f->be), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->fe);
	KUNIT_ASSERT_NOT_NULL(test, f->be);
	f->fe_link.id = 0;
	f->be_link.id = 1;
	f->fe->dai_link = &f->fe_link;
	f->be->dai_link = &f->be_link;
	f->fe->card = &t->ma->card;
	f->be->card = &t->ma->card;
	INIT_LIST_HEAD(&f->fe->dpcm[SNDRV_PCM_STREAM_PLAYBACK].be_clients);
	INIT_LIST_HEAD(&f->fe->dpcm[SNDRV_PCM_STREAM_CAPTURE].be_clients);
	f->props[1].is_speakers = true;
	t->ma->link_props = f->props;
	f->dpcm.fe = f->fe;
	f->dpcm.be = f->be;
	list_add(&f->dpcm.list_be, &f->fe->dpcm[SNDRV_PCM_STREAM_PLAYBACK].be_clients);
	f->play.stream = SNDRV_PCM_STREAM_PLAYBACK;
	f->play.private_data = f->fe;
	f->cap.stream = SNDRV_PCM_STREAM_CAPTURE;
	f->cap.private_data = f->fe;
	f->be_play.stream = SNDRV_PCM_STREAM_PLAYBACK;
	f->be_play.private_data = f->be;
}

/* Refused starts: -EPERM as before, each one counted, reported at the next grant */
static void macaudio_spk_refused_starts_are_counted(struct kunit *test)
{
	struct macaudio_test_fe *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	struct macaudio_test_spk t;

	KUNIT_ASSERT_NOT_NULL(test, f);
	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	macaudio_test_fe_setup(test, &t, f);

	KUNIT_EXPECT_EQ(test, macaudio_fe_trigger(&f->play, SNDRV_PCM_TRIGGER_START), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_fe_trigger(&f->play, SNDRV_PCM_TRIGGER_RESUME), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_fe_trigger(&f->play, SNDRV_PCM_TRIGGER_PAUSE_RELEASE),
			-EPERM);
	/* The back-end behind it refuses the same way */
	KUNIT_EXPECT_EQ(test, macaudio_be_trigger(&f->be_play, SNDRV_PCM_TRIGGER_START), -EPERM);
	KUNIT_EXPECT_EQ(test, atomic_read(&t.ma->spk_refused), 4);
	/* Stops, and the capture side, always pass and are not counted */
	KUNIT_EXPECT_EQ(test, macaudio_fe_trigger(&f->play, SNDRV_PCM_TRIGGER_STOP), 0);
	KUNIT_EXPECT_EQ(test, macaudio_be_trigger(&f->be_play, SNDRV_PCM_TRIGGER_STOP), 0);
	KUNIT_EXPECT_EQ(test, macaudio_fe_trigger(&f->cap, SNDRV_PCM_TRIGGER_START), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&t.ma->spk_refused), 4);

	/* Granted: starts pass; the grant reports and resets the run */
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, atomic_read(&t.ma->spk_refused_since_grant), 4);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&t.ma->spk_refused_since_grant), 0);
	KUNIT_EXPECT_EQ(test, macaudio_fe_trigger(&f->play, SNDRV_PCM_TRIGGER_START), 0);
	KUNIT_EXPECT_EQ(test, macaudio_be_trigger(&f->be_play, SNDRV_PCM_TRIGGER_START), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&t.ma->spk_refused), 4);

	/* A lease past its end refuses again, timer or not */
	hrtimer_cancel(&t.ma->spk_lease_timer);
	WRITE_ONCE(t.ma->spk_lease_end, ktime_sub_ms(ktime_get(), 1));
	KUNIT_EXPECT_EQ(test, macaudio_fe_trigger(&f->play, SNDRV_PCM_TRIGGER_START), -EPERM);
	KUNIT_EXPECT_EQ(test, atomic_read(&t.ma->spk_refused), 5);

	/* A front-end not connected to the speakers is never refused */
	list_del_init(&f->dpcm.list_be);
	KUNIT_EXPECT_EQ(test, macaudio_fe_trigger(&f->play, SNDRV_PCM_TRIGGER_START), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&t.ma->spk_refused), 5);
	t.ma->link_props = NULL;
}

/* Every link of a card with speakers marks the gate's -EPERM as a quiet refusal */
static void macaudio_spk_refusals_are_quiet(struct kunit *test)
{
	struct snd_soc_dai_link links[3] = {};
	struct snd_soc_card card = { .dai_link = links, .num_links = ARRAY_SIZE(links) };
	unsigned int i;

	macaudio_quiet_refusals(&card);
	for (i = 0; i < ARRAY_SIZE(links); i++) {
		KUNIT_EXPECT_TRUE(test, links[i].quiet_trigger_eperm);
		KUNIT_EXPECT_TRUE(test, snd_soc_link_trigger_refused(&links[i], -EPERM));
		KUNIT_EXPECT_FALSE(test, snd_soc_link_trigger_refused(&links[i], -EIO));
	}
}

static void macaudio_sense_controls_are_protected(struct kunit *test)
{
	static const char *const names[] = {
		"Left Woofer ISENSE Switch", "Left Woofer VSENSE Switch",
		"Right Tweeter ISENSE Switch", "Right Tweeter VSENSE Switch",
		"ISENSE Switch", "VSENSE Switch", "Jack Switch",
	};
	struct macaudio_test_spk t = {};
	struct snd_kcontrol *sense[ARRAY_SIZE(names)];
	unsigned int i;

	macaudio_test_spk_card(test, &t, &macaudio_j413_cfg, SN012776_0DB);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct snd_kcontrol_new tmpl = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = names[i],
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
			.info = snd_ctl_boolean_mono_info,
		};

		sense[i] = macaudio_test_add_kctl(test, t.ma->card.snd_card, &tmpl, NULL);
	}
	macaudio_protect_sense(&t.ma->card);
	for (i = 0; i < ARRAY_SIZE(names) - 1; i++)
		KUNIT_EXPECT_TRUE(test, sense[i]->write_check == macaudio_sense_write_check);
	KUNIT_EXPECT_NULL(test, sense[ARRAY_SIZE(names) - 1]->write_check);
}

static void macaudio_sense_owned_writes(struct kunit *test)
{
	struct macaudio_test_spk t = {};
	struct snd_ctl_file other = {};

	macaudio_test_spk_card(test, &t, &macaudio_j413_cfg, SN012776_0DB);
	KUNIT_EXPECT_EQ(test, macaudio_sense_owner_check(t.ma, &other), 0);
	macaudio_test_take(test, &t);
	/* Ownership alone excludes every other file, even before a grant. */
	KUNIT_EXPECT_EQ(test, macaudio_sense_owner_check(t.ma, t.owner), 0);
	KUNIT_EXPECT_EQ(test, macaudio_sense_owner_check(t.ma, &other), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_sense_owner_check(t.ma, NULL), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_sense_owner_check(t.ma, t.owner), 0);
	KUNIT_EXPECT_EQ(test, macaudio_sense_owner_check(t.ma, &other), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), 0);
	KUNIT_EXPECT_EQ(test, macaudio_sense_owner_check(t.ma, &other), -EPERM);
	macaudio_spk_unlock(t.spk);
	KUNIT_EXPECT_EQ(test, macaudio_sense_owner_check(t.ma, &other), 0);
}

static void macaudio_route_is_protected(struct kunit *test)
{
	static const char *const names[] = { "Speaker Playback Mux", "Headphone Playback Mux" };
	struct macaudio_test_spk t = {};
	struct snd_kcontrol *mux[ARRAY_SIZE(names)];
	unsigned int i;

	macaudio_test_spk_card(test, &t, &macaudio_j413_cfg, SN012776_0DB);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct snd_kcontrol_new tmpl = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = names[i],
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
			.info = snd_ctl_boolean_mono_info,
		};

		mux[i] = macaudio_test_add_kctl(test, t.ma->card.snd_card, &tmpl, NULL);
	}
	macaudio_protect_sense(&t.ma->card);
	KUNIT_EXPECT_TRUE(test, mux[0]->write_check == macaudio_route_write_check);
	KUNIT_EXPECT_NULL(test, mux[1]->write_check);
}

static void macaudio_route_fixed_while_granted(struct kunit *test)
{
	struct macaudio_test_spk t = {};

	macaudio_test_spk_card(test, &t, &macaudio_j316_cfg, SN012776_0DB);
	KUNIT_EXPECT_EQ(test, macaudio_route_check(t.ma), 0);
	macaudio_test_take(test, &t);
	/* Owned but not granted: the owner checks the route before it grants. */
	KUNIT_EXPECT_EQ(test, macaudio_route_check(t.ma), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_route_check(t.ma), -EBUSY);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 0), 0);
	KUNIT_EXPECT_EQ(test, macaudio_route_check(t.ma), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 30), 0);
	macaudio_spk_unlock(t.spk);
	KUNIT_EXPECT_EQ(test, macaudio_route_check(t.ma), 0);
}

static struct kunit_case macaudio_protection_cases[] = {
	KUNIT_CASE(macaudio_route_is_protected),
	KUNIT_CASE(macaudio_route_fixed_while_granted),
	KUNIT_CASE(macaudio_sense_owned_writes),
	KUNIT_CASE(macaudio_sense_controls_are_protected),
	KUNIT_CASE(macaudio_spk_floor_from_binding),
	KUNIT_CASE(macaudio_spk_grant_needs_owner_and_valid_lease),
	KUNIT_CASE(macaudio_spk_release_and_owner_gone),
	KUNIT_CASE(macaudio_spk_lease_expires),
	KUNIT_CASE(macaudio_spk_renewal_extends_lease),
	KUNIT_CASE(macaudio_spk_system_sleep),
	KUNIT_CASE(macaudio_spk_failed_floor_is_permanent),
	KUNIT_CASE(macaudio_spk_failed_grant_floors),
	KUNIT_CASE(macaudio_spk_j700_levels),
	KUNIT_CASE(macaudio_spk_gate_follows_lease_end),
	KUNIT_CASE(macaudio_spk_timer_beats_a_blocked_worker),
	KUNIT_CASE(macaudio_spk_timer_beats_a_fifo_spinner),
	KUNIT_CASE(macaudio_spk_workers_are_fifo),
	KUNIT_CASE(macaudio_spk_ownership_needs_cap_sys_admin),
	KUNIT_CASE(macaudio_spk_regrant_waits_for_the_stop),
	KUNIT_CASE(macaudio_spk_amp_gain_is_fixed),
	KUNIT_CASE(macaudio_spk_sleep_closes_the_gate_first),
	KUNIT_CASE(macaudio_spk_hibernation_floors),
	KUNIT_CASE(macaudio_spk_sleep_notifier_is_first),
	KUNIT_CASE(macaudio_spk_sleep_skips_a_card_without_speakers),
	KUNIT_CASE(macaudio_spk_refused_starts_are_counted),
	KUNIT_CASE(macaudio_spk_refusals_are_quiet),
	KUNIT_CASE(macaudio_spk_ownership_controls_are_inactive),
	KUNIT_CASE(macaudio_spk_withdrawal_ramps),
	KUNIT_CASE(macaudio_spk_ramp_ends_with_the_lease),
	KUNIT_CASE(macaudio_spk_hard_withdrawals_do_not_ramp),
	KUNIT_CASE(macaudio_spk_j700_does_not_ramp),
	KUNIT_CASE(macaudio_spk_floor_waits_for_the_resume),
	KUNIT_CASE(macaudio_spk_grant_needs_d0),
	{}
};

static struct kunit_suite macaudio_protection_suite = {
	.name = "apple-speaker-protection",
	.test_cases = macaudio_protection_cases,
};
kunit_test_suite(macaudio_protection_suite);

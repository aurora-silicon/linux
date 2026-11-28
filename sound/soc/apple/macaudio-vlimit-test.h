/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The "Speaker Volume Unlock" lease: the speaker volumes leave the safe
 * ceiling only while one process holds the lock control and every speaker
 * volume, and pings within SPEAKER_LOCK_TIMEOUT ms while speaker playback
 * runs, and only once an administrator has selected the volume lock. Built
 * on the card of macaudio-pm-volumes-test.h, selected unless a case says.
 */
#include <kunit/test.h>
#include <linux/delay.h>

static int macaudio_vlt_put(struct kunit *test, struct macaudio_pmt_pm *t, long val)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	uval->value.integer.value[0] = val;
	return macaudio_slk_put(t->slk, uval);
}

static long macaudio_vlt_get(struct kunit *test, struct macaudio_pmt_pm *t)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	KUNIT_EXPECT_EQ(test, macaudio_slk_get(t->slk, uval), 0);
	return uval->value.integer.value[0];
}

/* Speaker playback runs (or stops): the ping deadline is armed (or held). */
static void macaudio_vlt_playing(struct macaudio_pmt_pm *t, bool playing)
{
	if (playing)
		macaudio_vlimit_enable_timeout(t->ma);
	else
		macaudio_vlimit_disable_timeout(t->ma);
}

/* Saved mixer states hold 0 or 1: restoring one is a no-op, nothing else is. */
static void macaudio_vlimit_restore_values_are_noops(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, 0), 0);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, 1), 0);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, SPEAKER_MAGIC_VALUE + 1), -EINVAL);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_get(test, &t), 0L);

	/* Even for the owner */
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, 1), 0);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
}

/* A ping without the lock is refused and unlocks nothing. */
static void macaudio_vlimit_ping_needs_the_lock(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_get(test, &t), 0L);
}

/* The lock alone is not enough: every speaker volume must be the owner's. */
static void macaudio_vlimit_needs_every_volume(struct kunit *test)
{
	struct snd_ctl_file *other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	struct macaudio_pmt_pm t;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, other);
	for (i = 0; i < MACAUDIO_PMT_NR_VOLS - 1; i++) {
		macaudio_pmt_pm_card(test, &t);
		macaudio_pmt_take(test, &t);
		t.vols[i]->vd[0].owner = other;
		KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
		KUNIT_EXPECT_FALSE_MSG(test, t.ma->speaker_volume_unlocked,
				       "%s held by another file", macaudio_pmt_vol_names[i]);

		macaudio_pmt_pm_card(test, &t);
		macaudio_pmt_take(test, &t);
		t.vols[i]->vd[0].owner = NULL;
		KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
		KUNIT_EXPECT_FALSE_MSG(test, t.ma->speaker_volume_unlocked,
				       "%s not locked", macaudio_pmt_vol_names[i]);
	}

	/* A volume that is not a speaker volume does not count */
	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_take(test, &t);
	t.vols[MACAUDIO_PMT_NR_VOLS - 1]->vd[0].owner = other;
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);
}

/* Closing the lock relocks at once and clips the speaker volumes only. */
static void macaudio_vlimit_unlock_clips(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_get(test, &t), 1L);
	macaudio_pmt_expect_vols(test, &t, false);

	macaudio_slk_unlock(t.slk);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_get(test, &t), 0L);
	macaudio_pmt_expect_vols(test, &t, true);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
}

/*
 * While playback runs the lease ends SPEAKER_LOCK_TIMEOUT ms after the last
 * ping. The owner's next ping reports the loss with -ETIMEDOUT and unlocks
 * nothing; only the ping after it starts a new lease.
 */
static void macaudio_vlimit_lease_expires_while_playing(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	macaudio_vlt_playing(&t, true);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);

	msleep(SPEAKER_LOCK_TIMEOUT * 2);
	flush_delayed_work(&t.ma->lock_timeout_work);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	macaudio_pmt_expect_vols(test, &t, true);

	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);
	macaudio_vlt_playing(&t, false);
}

/*
 * Each ping moves the deadline to SPEAKER_LOCK_TIMEOUT ms after itself and
 * re-arms the expiry. Checked on the deadline, not by racing the clock.
 */
static void macaudio_vlimit_pings_keep_the_lease(struct kunit *test)
{
	struct macaudio_pmt_pm t;
	ktime_t before, deadline;
	unsigned int i;

	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	macaudio_vlt_playing(&t, true);
	deadline = t.ma->speaker_lock_timeout;
	for (i = 0; i < 3; i++) {
		usleep_range(1000, 2000);
		before = ktime_get();
		KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
		KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);
		KUNIT_EXPECT_GT(test, t.ma->speaker_lock_timeout, deadline);
		KUNIT_EXPECT_GE(test, t.ma->speaker_lock_timeout,
				ktime_add_ms(before, SPEAKER_LOCK_TIMEOUT));
		KUNIT_EXPECT_TRUE(test, delayed_work_pending(&t.ma->lock_timeout_work));
		deadline = t.ma->speaker_lock_timeout;
	}
	macaudio_vlt_playing(&t, false);
}

/*
 * Without playback the deadline is held: an idle owner keeps its lease, and
 * the time it had left runs on when playback starts again.
 */
static void macaudio_vlimit_idle_holds_the_lease(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	msleep(SPEAKER_LOCK_TIMEOUT * 2);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);
	KUNIT_EXPECT_EQ(test, t.ma->speaker_lock_remain, ms_to_ktime(SPEAKER_LOCK_TIMEOUT));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t.ma->lock_timeout_work));

	/* The time left runs from the start of playback and then ends */
	macaudio_vlt_playing(&t, true);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&t.ma->lock_timeout_work));
	msleep(SPEAKER_LOCK_TIMEOUT * 2);
	flush_delayed_work(&t.ma->lock_timeout_work);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	macaudio_vlt_playing(&t, false);
}

/* A new owner never inherits the previous owner's lease. */
static void macaudio_vlimit_new_owner_pings_first(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);

	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	KUNIT_EXPECT_LE(test, t.ma->speaker_lock_remain, 0);
	/* The relock was the switch of owners, not a lost lease */
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);
}

/* A card without speaker safety never touches its volumes. */
static void macaudio_vlimit_without_safety(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	t.ma->has_safety = false;
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	macaudio_slk_unlock(t.slk);
	macaudio_pmt_expect_vols(test, &t, false);
}

/*
 * Without an administrator's selection the lock and the ping are refused,
 * for root too, and nothing is unlocked.
 */
static void macaudio_vlimit_needs_the_selection(struct kunit *test)
{
	struct snd_ctl_file *owner = kunit_kzalloc(test, sizeof(*owner), GFP_KERNEL);
	struct macaudio_pmt_pm t;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, owner);
	macaudio_pmt_pm_card(test, &t);
	t.ma->spk_legacy_selected = false;
	KUNIT_EXPECT_TRUE(test, capable(CAP_SYS_ADMIN));
	for (i = 0; i < MACAUDIO_PMT_NR_VOLS - 1; i++)
		t.vols[i]->vd[0].owner = owner;
	KUNIT_EXPECT_EQ(test, macaudio_slk_lock(t.slk, owner), -EPERM);
	KUNIT_EXPECT_NULL(test, t.ma->speaker_lock_owner);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	/* Saved mixer states still restore */
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, 0), 0);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, 1), 0);
}

/* A daemon that took the lock while selected loses it with the selection */
static void macaudio_vlimit_deselected_owner_is_refused(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->speaker_volume_unlocked);

	mutex_lock(&t.ma->volume_lock_mutex);
	t.ma->spk_legacy_selected = false;
	macaudio_vlimit_update(t.ma);
	mutex_unlock(&t.ma->volume_lock_mutex);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
	macaudio_pmt_expect_vols(test, &t, true);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	KUNIT_EXPECT_FALSE(test, t.ma->speaker_volume_unlocked);
}

static struct kunit_case macaudio_vlimit_cases[] = {
	KUNIT_CASE(macaudio_vlimit_needs_the_selection),
	KUNIT_CASE(macaudio_vlimit_deselected_owner_is_refused),
	KUNIT_CASE(macaudio_vlimit_restore_values_are_noops),
	KUNIT_CASE(macaudio_vlimit_ping_needs_the_lock),
	KUNIT_CASE(macaudio_vlimit_needs_every_volume),
	KUNIT_CASE(macaudio_vlimit_unlock_clips),
	KUNIT_CASE_SLOW(macaudio_vlimit_lease_expires_while_playing),
	KUNIT_CASE(macaudio_vlimit_pings_keep_the_lease),
	KUNIT_CASE_SLOW(macaudio_vlimit_idle_holds_the_lease),
	KUNIT_CASE(macaudio_vlimit_new_owner_pings_first),
	KUNIT_CASE(macaudio_vlimit_without_safety),
	{}
};

static struct kunit_suite macaudio_vlimit_suite = {
	.name = "apple-speaker-volume-unlock",
	.test_cases = macaudio_vlimit_cases,
};

kunit_test_suite(macaudio_vlimit_suite);

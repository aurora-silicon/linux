/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * "Speaker Volume Unlock" lifts nothing: the lock and ping of the daemons
 * that used to lift the speaker volumes from it (speakersafetyd,
 * speakerguardd 0.1) are refused, and the volumes stay at the ceiling.
 * Built on the card of macaudio-pm-volumes-test.h.
 */
#include <kunit/test.h>

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

/* Saved mixer states hold 0 or 1: restoring one is a no-op, nothing else is. */
static void macaudio_vlimit_restore_values_are_noops(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, 0), 0);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, 1), 0);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_put(test, &t, SPEAKER_MAGIC_VALUE + 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_get(test, &t), 0L);
	macaudio_pmt_expect_vols(test, &t);
}

/* No file may take the lock control, whoever it is */
static void macaudio_vlimit_lock_refused(struct kunit *test)
{
	struct snd_ctl_file *file = kunit_kzalloc(test, sizeof(*file), GFP_KERNEL);
	struct macaudio_pmt_pm t;

	KUNIT_ASSERT_NOT_NULL(test, file);
	macaudio_pmt_pm_card(test, &t);
	/* KUnit runs with CAP_SYS_ADMIN: root is refused too */
	KUNIT_EXPECT_TRUE(test, capable(CAP_SYS_ADMIN));
	KUNIT_EXPECT_EQ(test, macaudio_slk_lock(t.slk, file), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_slk_lock(t.slk, file), -EPERM);
	macaudio_pmt_expect_vols(test, &t);
}

/*
 * A daemon that holds every speaker volume and pings, as speakersafetyd
 * does, is refused every time and lifts nothing.
 */
static void macaudio_vlimit_daemon_never_unlocks(struct kunit *test)
{
	struct macaudio_pmt_pm t;
	unsigned int i;

	macaudio_pmt_pm_card(test, &t);
	macaudio_pmt_take(test, &t);
	for (i = 0; i < 8; i++)
		KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_vlt_get(test, &t), 0L);
	macaudio_pmt_expect_vols(test, &t);

	/* A ping without even holding the volumes fares no better */
	macaudio_pmt_pm_card(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_pmt_ping(test, &t), -EPERM);
	macaudio_pmt_expect_vols(test, &t);
}

/* Outside the lease the driver itself cannot lift the ceiling either */
static void macaudio_vlimit_lift_needs_the_lease(struct kunit *test)
{
	struct macaudio_pmt_pm t;

	macaudio_pmt_pm_card(test, &t);
	KUNIT_ASSERT_FALSE(test, macaudio_spk_leased(t.ma));
	KUNIT_ASSERT_FALSE(test, macaudio_spk_lease_only(t.ma->cfg));
	mutex_lock(&t.ma->volume_lock_mutex);
	macaudio_vlimit_unlock(t.ma, true);
	mutex_unlock(&t.ma->volume_lock_mutex);
	macaudio_pmt_expect_vols(test, &t);
}

static struct kunit_case macaudio_vlimit_cases[] = {
	KUNIT_CASE(macaudio_vlimit_restore_values_are_noops),
	KUNIT_CASE(macaudio_vlimit_lock_refused),
	KUNIT_CASE(macaudio_vlimit_daemon_never_unlocks),
	KUNIT_CASE(macaudio_vlimit_lift_needs_the_lease),
	{}
};

static struct kunit_suite macaudio_vlimit_suite = {
	.name = "apple-speaker-volume-unlock",
	.test_cases = macaudio_vlimit_cases,
};

kunit_test_suite(macaudio_vlimit_suite);

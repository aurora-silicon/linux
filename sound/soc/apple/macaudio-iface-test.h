/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * "Speaker Protection Interface": the volume lock in charge from the bind,
 * the switch to the lease and back through the floor of both, and the way
 * back when the lease has no owner. On the card of fake controls that
 * macaudio-protection-test.h builds, with the volume lock's controls added.
 */
#include <kunit/test.h>

struct macaudio_test_iface {
	struct macaudio_test_spk t;
	struct snd_kcontrol *slk;
	struct snd_kcontrol *iface;
	/* The legacy daemon's control file */
	struct snd_ctl_file *daemon;
};

/* A bound card of @cfg with both interfaces: the volume lock in charge, at its ceiling */
static void macaudio_test_iface_card_cfg(struct kunit *test, struct macaudio_test_iface *f,
					 const struct macaudio_platform_cfg *cfg)
{
	struct snd_kcontrol_new slk = macaudio_test_control("Speaker Volume Unlock");
	struct snd_kcontrol_new iface = macaudio_test_control("Speaker Protection Interface");
	struct macaudio_snd_data *ma;
	unsigned int i;

	macaudio_test_spk_card(test, &f->t, cfg, SN012776_0DB);
	ma = f->t.ma;
	f->daemon = kunit_kzalloc(test, sizeof(*f->daemon), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->daemon);
	KUNIT_ASSERT_NOT_NULL(test, slk.name);
	KUNIT_ASSERT_NOT_NULL(test, iface.name);
	f->slk = macaudio_test_add_kctl(test, ma->card.snd_card, &slk, &ma->card);
	f->iface = macaudio_test_add_kctl(test, ma->card.snd_card, &iface, &ma->card);
	ma->speaker_lock_kctl = f->slk;
	ma->spk_iface_kctl = f->iface;
	ma->has_safety = true;

	/* As macaudio_late_probe() does at every bind */
	for (i = 0; i < MACAUDIO_TEST_NR_SPK; i++)
		macaudio_test_vol(f->t.vols[i])->value = MACAUDIO_TEST_START_VOL;
	ma->spk_iface = MACAUDIO_SPK_IFACE_UNLOCK;
	mutex_lock(&ma->volume_lock_mutex);
	macaudio_vlimit_unlock(ma, false);
	mutex_unlock(&ma->volume_lock_mutex);
}

/* As macaudio_test_iface_card_cfg(), for J316 */
static void macaudio_test_iface_card(struct kunit *test, struct macaudio_test_iface *f)
{
	macaudio_test_iface_card_cfg(test, f, &macaudio_j316_cfg);
}

static int macaudio_test_iface_write(struct kunit *test, struct macaudio_test_iface *f,
				     unsigned int item)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	uval->value.enumerated.item[0] = item;
	return macaudio_spk_iface_put(f->iface, uval);
}

/* The legacy daemon takes the lock and every speaker volume */
static void macaudio_test_daemon_take(struct kunit *test, struct macaudio_test_iface *f)
{
	unsigned int i;

	for (i = 0; i < MACAUDIO_TEST_NR_SPK; i++)
		f->t.vols[i]->vd[0].owner = f->daemon;
	KUNIT_ASSERT_EQ(test, macaudio_slk_lock(f->slk, f->daemon), 0);
}

static int macaudio_test_daemon_ping(struct kunit *test, struct macaudio_test_iface *f)
{
	struct snd_ctl_elem_value *uval = kunit_kzalloc(test, sizeof(*uval), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, uval);
	uval->value.integer.value[0] = SPEAKER_MAGIC_VALUE;
	return macaudio_slk_put(f->slk, uval);
}

/* Every speaker volume's value and limit */
static void macaudio_test_expect_limit(struct kunit *test, struct macaudio_test_iface *f,
				       long value, int limit)
{
	unsigned int i;

	for (i = 0; i < MACAUDIO_TEST_NR_SPK; i++) {
		KUNIT_EXPECT_EQ(test, macaudio_test_vol(f->t.vols[i])->value, value);
		KUNIT_EXPECT_EQ(test, macaudio_test_vol(f->t.vols[i])->mc.platform_max, limit);
	}
}

/* At the bind: the volume lock owns the speakers and the lease grants nothing */
static void macaudio_iface_volume_lock_from_binding(struct kunit *test)
{
	struct macaudio_test_iface f;

	macaudio_test_iface_card(test, &f);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_leased(f.t.ma));
	/* The lock's ceiling, 1 on the SN012776, clips what was there */
	macaudio_test_expect_limit(test, &f, 1, 1);

	/* A lease owner may hold its control, but takes nothing */
	macaudio_test_take(test, &f.t);
	macaudio_test_expect_limit(test, &f, 1, 1);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 50), -EBUSY);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &f.t), 0L);
	macaudio_spk_unlock(f.t.spk);
	macaudio_test_expect_limit(test, &f, 1, 1);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&f.t.ma->spk_unowned_work));

	/* Speaker volumes are the daemon's to write */
	KUNIT_EXPECT_EQ(test, f.t.vols[0]->write_check(f.t.vols[0], NULL), 0);

	/* The daemon unlocks them as on a lock-only kernel */
	macaudio_test_daemon_take(test, &f);
	KUNIT_EXPECT_EQ(test, macaudio_test_daemon_ping(test, &f), 0);
	KUNIT_EXPECT_TRUE(test, f.t.ma->speaker_volume_unlocked);
	macaudio_test_expect_limit(test, &f, 1, SN012776_0DB);
}

/* Into the lease: the lock's ceiling, then the lease's floor, then the lift */
static void macaudio_iface_switch_goes_through_the_floor(struct kunit *test)
{
	struct macaudio_test_iface f;
	unsigned int i;

	macaudio_test_iface_card(test, &f);
	macaudio_test_daemon_take(test, &f);
	KUNIT_EXPECT_EQ(test, macaudio_test_daemon_ping(test, &f), 0);
	for (i = 0; i < MACAUDIO_TEST_NR_SPK; i++)
		macaudio_test_vol(f.t.vols[i])->value = 180;

	KUNIT_EXPECT_EQ(test, macaudio_test_iface_write(test, &f, MACAUDIO_SPK_IFACE_LEASE), 1);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_leased(f.t.ma));
	KUNIT_EXPECT_FALSE(test, f.t.ma->speaker_volume_unlocked);
	/* At the lease's floor, below a ceiling that no longer applies */
	macaudio_test_expect_limit(test, &f, 0, SN012776_0DB);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(f.t.ma));

	/* The volume lock is refused, and so are the daemon's volume writes */
	KUNIT_EXPECT_EQ(test, macaudio_test_daemon_ping(test, &f), -EBUSY);
	KUNIT_EXPECT_EQ(test, f.t.vols[0]->write_check(f.t.vols[0], NULL), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_test_iface_write(test, &f, MACAUDIO_SPK_IFACE_LEASE), 0);

	/* The engine is granted its operating gain */
	macaudio_test_take(test, &f.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 50), 0);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_admitted(f.t.ma));
	macaudio_test_expect_limit(test, &f, SN012776_0DB, SN012776_0DB);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 0), 0);
	macaudio_test_settle(f.t.ma);
}

/*
 * Back to the volume lock: the speakers come back at the floor under the
 * lock's ceiling, and go up only once the daemon pings, as after a cold
 * bind; its first ping learns that it lost the lock.
 */
static void macaudio_iface_revert_starts_at_the_floor(struct kunit *test)
{
	struct macaudio_test_iface f;

	macaudio_test_iface_card(test, &f);
	macaudio_test_daemon_take(test, &f);
	KUNIT_EXPECT_EQ(test, macaudio_test_iface_write(test, &f, MACAUDIO_SPK_IFACE_LEASE), 1);
	macaudio_test_take(test, &f.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 50), 0);
	macaudio_test_expect_limit(test, &f, SN012776_0DB, SN012776_0DB);

	KUNIT_EXPECT_EQ(test, macaudio_test_iface_write(test, &f, MACAUDIO_SPK_IFACE_UNLOCK), 1);
	macaudio_test_settle(f.t.ma);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_leased(f.t.ma));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(f.t.ma));
	KUNIT_EXPECT_FALSE(test, f.t.ma->speaker_volume_unlocked);
	macaudio_test_expect_limit(test, &f, 0, 1);
	/* The lease grants nothing any more */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 50), -EBUSY);
	macaudio_test_expect_limit(test, &f, 0, 1);

	/* The daemon's lock was lost: told once, then it unlocks */
	KUNIT_EXPECT_EQ(test, macaudio_test_daemon_ping(test, &f), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, f.t.ma->speaker_volume_unlocked);
	macaudio_test_expect_limit(test, &f, 0, 1);
	KUNIT_EXPECT_EQ(test, macaudio_test_daemon_ping(test, &f), 0);
	KUNIT_EXPECT_TRUE(test, f.t.ma->speaker_volume_unlocked);
	/* Unlocking lifts the ceiling only: the daemon sets the volumes itself */
	macaudio_test_expect_limit(test, &f, 0, SN012776_0DB);
}

/* The engine dies: the lease's floor at once, the volume lock after the grace */
static void macaudio_iface_unowned_lease_reverts(struct kunit *test)
{
	struct macaudio_test_iface f;

	macaudio_test_iface_card(test, &f);
	KUNIT_EXPECT_EQ(test, macaudio_test_iface_write(test, &f, MACAUDIO_SPK_IFACE_LEASE), 1);
	/* Entered without an owner: the grace runs already */
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&f.t.ma->spk_unowned_work));
	macaudio_test_take(test, &f.t);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&f.t.ma->spk_unowned_work));
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 50), 0);

	macaudio_spk_unlock(f.t.spk);
	macaudio_test_settle(f.t.ma);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_leased(f.t.ma));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(f.t.ma));
	macaudio_test_expect_limit(test, &f, 0, SN012776_0DB);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&f.t.ma->spk_unowned_work));

	/* A restarted engine takes it back within the grace: no revert */
	macaudio_test_take(test, &f.t);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&f.t.ma->spk_unowned_work));
	macaudio_spk_unlock(f.t.spk);

	/* The grace runs out */
	mod_delayed_work(system_percpu_wq, &f.t.ma->spk_unowned_work, 0);
	flush_delayed_work(&f.t.ma->spk_unowned_work);
	macaudio_test_settle(f.t.ma);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_leased(f.t.ma));
	KUNIT_EXPECT_FALSE(test, f.t.ma->speaker_volume_unlocked);
	macaudio_test_expect_limit(test, &f, 0, 1);
}

/* Only a CAP_SYS_ADMIN process hands the speakers over */
static void macaudio_iface_needs_cap_sys_admin(struct kunit *test)
{
	struct macaudio_test_iface f;
	const struct cred *old;
	struct cred *cred;

	macaudio_test_iface_card(test, &f);
	cred = prepare_creds();
	KUNIT_ASSERT_NOT_NULL(test, cred);
	cap_lower(cred->cap_effective, CAP_SYS_ADMIN);
	old = override_creds(cred);
	KUNIT_EXPECT_EQ(test, macaudio_test_iface_write(test, &f, MACAUDIO_SPK_IFACE_LEASE),
			-EPERM);
	revert_creds(old);
	put_cred(cred);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_leased(f.t.ma));
	macaudio_test_expect_limit(test, &f, 1, 1);
	KUNIT_EXPECT_EQ(test, macaudio_test_iface_write(test, &f, 2), -EINVAL);
}

/*
 * J813: its four SN012776 (the fake card's Left/Right Woofer/Tweeter
 * volumes, the J813 amps' prefixes) are behind both interfaces from the
 * bind, as on the MCA machines. Under the volume lock they stay at its
 * ceiling until a daemon holds the lock and pings; under the lease they are
 * at the floor and no speaker start is admitted until an owner is granted.
 */
static void macaudio_iface_j813_speakers_protected(struct kunit *test)
{
	struct macaudio_test_iface f;

	KUNIT_ASSERT_TRUE(test, macaudio_speakers_allowed(&macaudio_j813_cfg));
	macaudio_test_iface_card_cfg(test, &f, &macaudio_j813_cfg);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_leased(f.t.ma));
	KUNIT_EXPECT_FALSE(test, f.t.ma->speaker_volume_unlocked);
	macaudio_test_expect_limit(test, &f, 1, 1);

	/* A daemon that holds the lock but has not pinged lifts nothing */
	macaudio_test_daemon_take(test, &f);
	KUNIT_EXPECT_FALSE(test, f.t.ma->speaker_volume_unlocked);
	macaudio_test_expect_limit(test, &f, 1, 1);
	KUNIT_EXPECT_EQ(test, macaudio_test_daemon_ping(test, &f), 0);
	KUNIT_EXPECT_TRUE(test, f.t.ma->speaker_volume_unlocked);
	macaudio_test_expect_limit(test, &f, 1, SN012776_0DB);

	/* Into the lease: the floor, and nothing admitted without an owner */
	KUNIT_EXPECT_EQ(test, macaudio_test_iface_write(test, &f, MACAUDIO_SPK_IFACE_LEASE), 1);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_leased(f.t.ma));
	KUNIT_EXPECT_FALSE(test, f.t.ma->speaker_volume_unlocked);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(f.t.ma));
	macaudio_test_expect_limit(test, &f, 0, SN012776_0DB);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 50), -EPERM);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(f.t.ma));

	/* Its owner is granted the operating gain, and the floor on release */
	macaudio_test_take(test, &f.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 50), 0);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_admitted(f.t.ma));
	macaudio_test_expect_limit(test, &f, SN012776_0DB, SN012776_0DB);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 0), 0);
	macaudio_test_settle(f.t.ma);
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(f.t.ma));
	macaudio_test_expect_limit(test, &f, 0, SN012776_0DB);
}

/*
 * J716 (v2-only): binds directly in lease mode at the hardware floor, refuses
 * legacy unlock and interface revert, and never schedules the unowned fallback
 * when the owner leaves.
 */
static void macaudio_iface_v2_only_never_leaves_lease(struct kunit *test)
{
	struct macaudio_test_iface f;

	KUNIT_ASSERT_TRUE(test, macaudio_speakers_allowed(&macaudio_j716_cfg));
	macaudio_test_spk_card(test, &f.t, &macaudio_j716_cfg, SN012776_0DB);
	f.t.ma->has_safety = true;
	{
		struct snd_kcontrol_new slk = macaudio_test_control("Speaker Volume Unlock");
		struct snd_kcontrol_new iface = macaudio_test_control("Speaker Protection Interface");

		f.daemon = kunit_kzalloc(test, sizeof(*f.daemon), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, f.daemon);
		f.slk = macaudio_test_add_kctl(test, f.t.ma->card.snd_card, &slk, &f.t.ma->card);
		f.iface = macaudio_test_add_kctl(test, f.t.ma->card.snd_card, &iface, &f.t.ma->card);
	}

	/* Cold bind: already leased, muted at floor 0, userspace writes refused */
	KUNIT_EXPECT_TRUE(test, macaudio_spk_leased(f.t.ma));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(f.t.ma));
	KUNIT_EXPECT_EQ(test, f.t.vols[0]->write_check(f.t.vols[0], NULL), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 50), -EPERM);

	/* Legacy unlock and switching back to unlock are refused */
	KUNIT_EXPECT_EQ(test, macaudio_test_iface_write(test, &f, MACAUDIO_SPK_IFACE_UNLOCK),
			-EOPNOTSUPP);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_leased(f.t.ma));
	macaudio_test_daemon_take(test, &f);
	KUNIT_EXPECT_EQ(test, macaudio_test_daemon_ping(test, &f), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, f.t.ma->speaker_volume_unlocked);

	/* Owner takes lease, grants operating gain, then leaves */
	macaudio_test_take(test, &f.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &f.t, 50), 0);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_admitted(f.t.ma));
	macaudio_test_expect_vols(test, &f.t, SN012776_0DB);

	macaudio_spk_unlock(f.t.spk);
	macaudio_test_settle(f.t.ma);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_leased(f.t.ma));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(f.t.ma));
	macaudio_test_expect_vols(test, &f.t, 0);
	/* No revert timer is armed on a v2-only card */
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&f.t.ma->spk_unowned_work));

	/* Even an explicit leave-lease call cannot exit lease mode */
	mutex_lock(&f.t.ma->spk_mutex);
	macaudio_spk_leave_lease(f.t.ma, "test");
	mutex_unlock(&f.t.ma->spk_mutex);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_leased(f.t.ma));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(f.t.ma));
}

/*
 * Component unbind and rebind reuse macaudio_snd_data: removal clears per-bind
 * control state and marks spk_dead, and the next bind clears spk_dead, retakes
 * volumes without overflowing spk_vols, and reapplies the cold-bind mute floor.
 */
static void macaudio_iface_v2_only_rebind_resets_floor(struct kunit *test)
{
	struct macaudio_test_spk t;
	unsigned int i;

	macaudio_test_spk_card(test, &t, &macaudio_j716_cfg, SN012776_0DB);
	t.ma->has_safety = true;
	macaudio_test_take(test, &t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &t, 50), 0);
	KUNIT_EXPECT_TRUE(test, macaudio_spk_admitted(t.ma));
	macaudio_test_expect_vols(test, &t, SN012776_0DB);

	/* Simulate ASoC card unbind */
	KUNIT_EXPECT_EQ(test, macaudio_card_remove(&t.ma->card), 0);
	KUNIT_EXPECT_TRUE(test, t.ma->spk_dead);
	KUNIT_EXPECT_EQ(test, t.ma->spk_nr_vols, 0U);
	KUNIT_EXPECT_NULL(test, t.ma->spk_vols);
	KUNIT_EXPECT_NULL(test, t.ma->spk_owner);

	/* Simulate rebind with stale non-zero codec volumes before cold-bind floor */
	for (i = 0; i < MACAUDIO_TEST_NR_SPK; i++)
		macaudio_test_vol(t.vols[i])->value = MACAUDIO_TEST_START_VOL;

	mutex_lock(&t.ma->spk_mutex);
	t.ma->spk_dead = false;
	t.ma->spk_owner = NULL;
	t.ma->spk_active = false;
	t.ma->spk_lapsed = false;
	WRITE_ONCE(t.ma->spk_granted, false);
	WRITE_ONCE(t.ma->spk_xover, false);
	WRITE_ONCE(t.ma->spk_iface, MACAUDIO_SPK_IFACE_LEASE);
	mutex_unlock(&t.ma->spk_mutex);

	KUNIT_ASSERT_EQ(test, macaudio_spk_take_volumes(t.ma), 0);
	KUNIT_EXPECT_EQ(test, t.ma->spk_nr_vols, (unsigned int)MACAUDIO_TEST_NR_SPK);

	mutex_lock(&t.ma->spk_mutex);
	macaudio_spk_floor(t.ma, "card bound");
	mutex_unlock(&t.ma->spk_mutex);

	KUNIT_EXPECT_TRUE(test, macaudio_spk_leased(t.ma));
	KUNIT_EXPECT_FALSE(test, macaudio_spk_admitted(t.ma));
	macaudio_test_expect_vols(test, &t, 0);
}

/* The switch is no mixer setting: alsactl neither saves nor restores it */
static void macaudio_iface_control_is_inactive(struct kunit *test)
{
	struct snd_kcontrol_new iface = macaudio_test_control("Speaker Protection Interface");

	KUNIT_EXPECT_TRUE(test, iface.access & SNDRV_CTL_ELEM_ACCESS_INACTIVE);
	KUNIT_EXPECT_TRUE(test, iface.access & SNDRV_CTL_ELEM_ACCESS_VOLATILE);
}

static struct kunit_case macaudio_iface_cases[] = {
	KUNIT_CASE(macaudio_iface_volume_lock_from_binding),
	KUNIT_CASE(macaudio_iface_switch_goes_through_the_floor),
	KUNIT_CASE(macaudio_iface_revert_starts_at_the_floor),
	KUNIT_CASE(macaudio_iface_unowned_lease_reverts),
	KUNIT_CASE(macaudio_iface_needs_cap_sys_admin),
	KUNIT_CASE(macaudio_iface_control_is_inactive),
	KUNIT_CASE(macaudio_iface_j813_speakers_protected),
	KUNIT_CASE(macaudio_iface_v2_only_never_leaves_lease),
	KUNIT_CASE(macaudio_iface_v2_only_rebind_resets_floor),
	{}
};

static struct kunit_suite macaudio_iface_suite = {
	.name = "apple-speaker-interface",
	.test_cases = macaudio_iface_cases,
};
kunit_test_suite(macaudio_iface_suite);

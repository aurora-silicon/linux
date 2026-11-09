/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The speaker amps' playback high-pass corners: DT-derived for the woofers, and
 * for the tweeters the safe corner unless protection is granted, on a
 * machine that opens it, to an owner that declares the crossover. On the
 * fake card of macaudio-protection-test.h.
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

	macaudio_test_take_with_xover(test, &h.t);
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
	macaudio_test_take_with_xover(test, &h.t);
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
	macaudio_test_take_with_xover(test, &h.t);
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
	macaudio_test_take_with_xover(test, &h.t);
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
	macaudio_test_take_with_xover(test, &h.t);
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
	macaudio_test_expect_vols(test, &h.t, SN012776_0DB);

	/* The declaration changes nothing where the tweeters never open */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 0), 0);
	macaudio_test_lock_xover(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), 1);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	macaudio_test_expect_vols(test, &h.t, SN012776_0DB);
}

/* An owner that does not declare the crossover plays with the safe corner */
static void macaudio_hpf_undeclared_grant_keeps_800hz(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};

	macaudio_test_hpf_card(test, &h, &macaudio_j413_cfg, 0);
	macaudio_test_take(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 0L);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_read(test, &h.t), 1L);
	macaudio_test_expect_vols(test, &h.t, SN012776_0DB);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	/* Renewals keep it closed */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);

	/* Nor can the declaration open it in the middle of a grant */
	macaudio_test_lock_xover(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), -EBUSY);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 0L);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
}

/* A grant latches the declaration; a change waits for the floor */
static void macaudio_hpf_crossover_latched_by_grant(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};

	macaudio_test_hpf_card(test, &h, &macaudio_j413_cfg, 0);
	macaudio_test_take_with_xover(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 1L);
	/* Unchanged before a grant */
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);

	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 0), -EBUSY);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), -EBUSY);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 1L);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);

	/* Released, the owner withdraws it and the next grant keeps 800 Hz */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 0), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 0), 1);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	macaudio_test_expect_vols(test, &h.t, SN012776_0DB);

	/* A value other than 0 or 1 is refused */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 0), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, -1), -EINVAL);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 0L);
}

/* Only the owner, through its own lock on the control, with CAP_SYS_ADMIN */
static void macaudio_hpf_crossover_is_the_owners(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};
	struct snd_ctl_file *other;
	const struct cred *old;
	struct cred *seat;

	other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, other);
	macaudio_test_hpf_card(test, &h, &macaudio_j616_cfg, 0);

	/* No owner: neither the lock nor a write */
	KUNIT_EXPECT_EQ(test, macaudio_spk_xover_lock(h.t.xover, h.t.owner), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), -EPERM);

	macaudio_test_take(test, &h.t);
	/* The control not locked, or locked by another file */
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), -EPERM);
	KUNIT_EXPECT_EQ(test, macaudio_spk_xover_lock(h.t.xover, other), -EPERM);
	h.t.xover->vd[0].owner = other;
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), -EPERM);
	h.t.xover->vd[0].owner = NULL;
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 0L);

	/* The owner's file, handed on to a process without the capability */
	seat = prepare_creds();
	KUNIT_ASSERT_NOT_NULL(test, seat);
	cap_lower(seat->cap_effective, CAP_SYS_ADMIN);
	old = override_creds(seat);
	KUNIT_EXPECT_EQ(test, macaudio_spk_xover_lock(h.t.xover, h.t.owner), -EPERM);
	revert_creds(old);
	macaudio_test_lock_xover(test, &h.t);
	old = override_creds(seat);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), -EPERM);
	revert_creds(old);
	put_cred(seat);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 0L);

	/* Every refusal left the tweeters closed */
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 0), 0);

	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), 1);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);
}

/* The declaration lapses with the ownership, never with the lease */
static void macaudio_hpf_crossover_cleared_with_the_owner(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};
	struct snd_ctl_file *other;

	other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, other);
	macaudio_test_hpf_card(test, &h, &macaudio_j316_cfg, 0);

	/* A lease that runs out keeps it: the same owner */
	macaudio_test_take_with_xover(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, SPEAKER_LEASE_MIN_MS), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);
	msleep(3 * SPEAKER_LEASE_MIN_MS);
	macaudio_test_settle(h.t.ma);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 1L);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_2HZ);

	/* The owner unlocks, or its file closes */
	macaudio_spk_unlock(h.t.spk);
	h.t.xover->vd[0].owner = NULL;
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 0L);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	macaudio_test_take(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 0), 0);

	/* A new owner starts without it, and the old file's lock is not its own */
	macaudio_test_lock_xover(test, &h.t);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), 1);
	KUNIT_EXPECT_EQ(test, macaudio_spk_lock(h.t.spk, other), 0);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_read(test, &h.t), 0L);
	KUNIT_EXPECT_EQ(test, macaudio_test_xover_write(test, &h.t, 1), -EPERM);
	h.t.owner = other;
	KUNIT_EXPECT_EQ(test, macaudio_test_write(test, &h.t, 30), 0);
	macaudio_test_expect_hpf(test, &h, MACAUDIO_TEST_HPF_800HZ);
	macaudio_test_expect_vols(test, &h.t, SN012776_0DB);
}

static void macaudio_hpf_restores_each_dt_corner(struct kunit *test)
{
	struct macaudio_test_hpfs h = {};
	const unsigned int corners[] = { 0, 3, MACAUDIO_TEST_HPF_2HZ };

	/* Capture different DT values, including a disabled playback HPF. */
	macaudio_test_hpf_card_corners(test, &h, &macaudio_j413_cfg, 0, corners);
	macaudio_test_take_with_xover(test, &h.t);
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
	KUNIT_CASE(macaudio_hpf_undeclared_grant_keeps_800hz),
	KUNIT_CASE(macaudio_hpf_crossover_latched_by_grant),
	KUNIT_CASE(macaudio_hpf_crossover_is_the_owners),
	KUNIT_CASE(macaudio_hpf_crossover_cleared_with_the_owner),
	{}
};

static struct kunit_suite macaudio_hpf_suite = {
	.name = "apple-speaker-hpf",
	.test_cases = macaudio_hpf_cases,
};
kunit_test_suite(macaudio_hpf_suite);

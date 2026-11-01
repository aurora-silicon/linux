/* SPDX-License-Identifier: GPL-2.0-only */
/* Included in the driver so the real control/trigger handlers are exercised. */
#include <kunit/test.h>

struct macaudio_energy_test {
	struct macaudio_snd_data ma;
	struct snd_card card;
	struct snd_ctl_file owner[2];
	struct snd_kcontrol *control[4];
	struct snd_pcm_substream substream;
	struct snd_soc_pcm_runtime runtime;
	struct snd_soc_dai_link link;
};

static void macaudio_energy_test_cancel(void *context)
{
	struct macaudio_snd_data *ma = context;

	cancel_delayed_work_sync(&ma->lock_timeout_work);
	cancel_work_sync(&ma->lock_fail_work);
}

static struct macaudio_energy_test *macaudio_energy_test_new(struct kunit *test)
{
	static const char *const names[] = {
		"Speaker Energy Grant",
		"Left Speaker Volume",
		"Right Speaker Volume",
		"Left Amp Gain Volume",
	};
	struct macaudio_energy_test *t =
		kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	u8 slots[] = { 0, 1 };
	unsigned int k;

	KUNIT_ASSERT_NOT_NULL(test, t);
	INIT_LIST_HEAD(&t->card.controls);
	strscpy(t->card.id, "AppleJ413", sizeof(t->card.id));
	t->ma.card.snd_card = &t->card;
	snd_soc_card_set_drvdata(&t->ma.card, &t->ma);
	t->ma.cfg = &macaudio_j413_cfg;
	t->ma.max_channels = 2;
	t->ma.energy_channels = 2;
	t->ma.energy_format = SNDRV_PCM_FORMAT_S32_LE;
	t->ma.has_energy = true;
	t->ma.energy_enforced = true;
	/* Scalar reference certificate; this does not qualify a real TAS HPF. */
	t->ma.energy_transfer = MACAUDIO_ENERGY_TRANSFER_ENERGY_NORM;
	t->ma.speaker_lock_owner = &t->owner[0];
	mutex_init(&t->ma.volume_lock_mutex);
	apple_energy_init(&t->ma.energy);
	KUNIT_ASSERT_EQ(
		test, apple_energy_configure(&t->ma.energy, 48000, 2, 2, slots),
		0);
	apple_energy_fallback(&t->ma.energy, 21474);
	KUNIT_ASSERT_EQ(test, apple_energy_hardware_safe(&t->ma.energy, true),
			0);
	INIT_DELAYED_WORK(&t->ma.lock_timeout_work,
			  macaudio_vlimit_timeout_work);
	INIT_WORK(&t->ma.lock_fail_work, macaudio_lock_fail_work);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(
				test, macaudio_energy_test_cancel, &t->ma),
			0);
	for (k = 0; k < ARRAY_SIZE(t->control); k++) {
		struct snd_kcontrol *control = kunit_kzalloc(
			test, struct_size(control, vd, 1), GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, control);
		control->count = 1;
		control->private_data = &t->ma.card;
		control->vd[0].owner = &t->owner[0];
		strscpy(control->id.name, names[k], sizeof(control->id.name));
		list_add_tail(&control->list, &t->card.controls);
		t->control[k] = control;
	}
	t->ma.energy_kctl = t->control[0];
	t->ma.link_props =
		kunit_kzalloc(test, sizeof(*t->ma.link_props), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->ma.link_props);
	t->runtime.card = &t->ma.card;
	t->runtime.dai_link = &t->link;
	t->substream.private_data = &t->runtime;
	t->substream.stream = SNDRV_PCM_STREAM_PLAYBACK;
	t->ma.energy_stream[0].ma = &t->ma;
	t->ma.energy_stream[0].channels = 2;
	t->ma.energy_stream[0].format = SNDRV_PCM_FORMAT_S32_LE;
	t->ma.energy_stream[0].epoch = t->ma.energy.state.epoch;
	t->ma.energy_stream[0].prepared = true;
	t->ma.energy_stream[0].attached = true;
	t->ma.energy_stream[0].speakers = true;
	return t;
}

static void macaudio_energy_owner_must_match_every_control(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	unsigned int k;

	KUNIT_EXPECT_TRUE(test, macaudio_energy_owner(&t->ma));
	for (k = 0; k < ARRAY_SIZE(t->control); k++) {
		t->control[k]->vd[0].owner = &t->owner[1];
		KUNIT_EXPECT_FALSE(test, macaudio_energy_owner(&t->ma));
		t->control[k]->vd[0].owner = &t->owner[0];
	}
	KUNIT_EXPECT_EQ(test, macaudio_energy_lock(t->control[0], &t->owner[1]),
			-EPERM);
}

static void
macaudio_energy_control_replay_and_corruption_revoke(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	struct snd_ctl_elem_value *value =
		kunit_kzalloc(test, sizeof(*value), GFP_KERNEL);
	struct macaudio_energy_ctl *packet;
	struct apple_energy_snapshot state;
	unsigned int k;

	KUNIT_ASSERT_NOT_NULL(test, value);
	packet = (void *)value->value.bytes.data;
	KUNIT_ASSERT_EQ(test, macaudio_energy_get(t->control[0], value), 0);
	packet->flags = 0;
	packet->deadline_ns = cpu_to_le64(le64_to_cpu(packet->clock_ns) +
					  100 * NSEC_PER_MSEC);
	for (k = 0; k < 2; k++) {
		packet->ceiling[k] = cpu_to_le64(1ULL << 30);
		packet->gain_q31[k] = cpu_to_le32(APPLE_ENERGY_UNITY);
	}
	KUNIT_ASSERT_EQ(test, macaudio_energy_put(t->control[0], value), 0);
	KUNIT_EXPECT_EQ(test, macaudio_energy_put(t->control[0], value),
			-ESTALE);
	apple_energy_snapshot(&t->ma.energy, &state);
	KUNIT_EXPECT_FALSE(test, state.enabled);
	KUNIT_ASSERT_EQ(test, macaudio_energy_get(t->control[0], value), 0);
	packet->flags = 0;
	packet->version = cpu_to_le32(MACAUDIO_ENERGY_VERSION + 1);
	KUNIT_EXPECT_EQ(test, macaudio_energy_put(t->control[0], value),
			-EINVAL);
	macaudio_energy_unlock(t->control[0]);
	apple_energy_snapshot(&t->ma.energy, &state);
	KUNIT_EXPECT_FALSE(test, state.enabled);
}

/* ALSA close can release the two owned controls in either order. */
static void macaudio_energy_close_revokes_a_live_grant(struct kunit *test)
{
	unsigned int order, k;

	for (order = 0; order < 2; order++) {
		struct macaudio_energy_test *t = macaudio_energy_test_new(test);
		struct snd_ctl_elem_value *value =
			kunit_kzalloc(test, sizeof(*value), GFP_KERNEL);
		struct macaudio_energy_ctl *packet;
		struct apple_energy_snapshot before, after;
		s32 samples[] = { S32_MIN, S32_MIN };
		u64 ticket;

		KUNIT_ASSERT_NOT_NULL(test, value);
		packet = (void *)value->value.bytes.data;
		KUNIT_ASSERT_EQ(test, macaudio_energy_get(t->control[0], value), 0);
		packet->flags = 0;
		packet->deadline_ns = cpu_to_le64(le64_to_cpu(packet->clock_ns) +
						100 * NSEC_PER_MSEC);
		for (k = 0; k < 2; k++) {
			packet->ceiling[k] = cpu_to_le64(16 * APPLE_ENERGY_UNIT_SCALE);
			packet->gain_q31[k] = cpu_to_le32(APPLE_ENERGY_UNITY);
		}
		KUNIT_ASSERT_EQ(test, macaudio_energy_put(t->control[0], value), 0);
		KUNIT_ASSERT_EQ(test,
			apple_energy_filter_pcm(&t->ma.energy, samples, 1, 2,
				SNDRV_PCM_FORMAT_S32_LE, ktime_get_boottime_ns(), &ticket),
			0);
		apple_energy_snapshot(&t->ma.energy, &before);
		KUNIT_ASSERT_TRUE(test, before.enabled);
		KUNIT_ASSERT_GT(test, before.submitted[0], 0ULL);
		KUNIT_ASSERT_EQ(test, before.pending, 1U);

		if (order == 0) {
			macaudio_energy_unlock(t->control[0]);
			macaudio_slk_unlock(t->control[0]);
		} else {
			macaudio_slk_unlock(t->control[0]);
			macaudio_energy_unlock(t->control[0]);
		}
		apple_energy_snapshot(&t->ma.energy, &after);
		KUNIT_EXPECT_FALSE(test, after.enabled);
		KUNIT_EXPECT_PTR_EQ(test, t->ma.speaker_lock_owner, NULL);
		KUNIT_EXPECT_EQ(test, t->ma.speaker_lock_remain, 0LL);
		KUNIT_EXPECT_EQ(test, after.pending, before.pending);
		KUNIT_EXPECT_EQ(test, after.submitted[0], before.submitted[0]);
		KUNIT_EXPECT_EQ(test, after.completed[0], before.completed[0]);
		KUNIT_EXPECT_EQ(test, macaudio_energy_put(t->control[0], value),
				-EPERM);
		KUNIT_ASSERT_EQ(test, apple_energy_complete(&t->ma.energy, ticket), 0);
	}
}

static void
macaudio_energy_expired_write_is_a_retryable_timeout(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	struct snd_ctl_elem_value *value =
		kunit_kzalloc(test, sizeof(*value), GFP_KERNEL);
	struct macaudio_energy_ctl *packet;
	struct apple_energy_snapshot state;

	KUNIT_ASSERT_NOT_NULL(test, value);
	packet = (void *)value->value.bytes.data;
	KUNIT_ASSERT_EQ(test, macaudio_energy_get(t->control[0], value), 0);
	packet->flags = 0;
	packet->deadline_ns = packet->clock_ns;
	KUNIT_EXPECT_EQ(test, macaudio_energy_put(t->control[0], value),
			-ETIMEDOUT);
	apple_energy_snapshot(&t->ma.energy, &state);
	KUNIT_EXPECT_FALSE(test, state.enabled);
}

static void macaudio_energy_normal_stop_is_not_a_gate_fault(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	s32 samples[] = { 1000000, -1000000 };
	u64 ticket;

	KUNIT_ASSERT_EQ(
		test,
		macaudio_fe_trigger(&t->substream, SNDRV_PCM_TRIGGER_STOP), 0);
	KUNIT_EXPECT_FALSE(test, t->ma.speaker_lock_failed);
	KUNIT_EXPECT_TRUE(test, t->ma.energy_stream[0].needs_prepare);
	/* A refill racing normal STOP remains a safe copy, not a hard fault. */
	KUNIT_ASSERT_EQ(test,
			macaudio_energy_process(&t->ma.energy_stream[0],
						samples, sizeof(samples),
						&ticket),
			0);
	KUNIT_EXPECT_EQ(test,
			macaudio_fe_trigger(&t->substream,
					    SNDRV_PCM_TRIGGER_START),
			-EPIPE);
	KUNIT_ASSERT_EQ(test,
			macaudio_energy_retire(&t->ma.energy_stream[0], ticket),
			0);
}

static int macaudio_energy_test_stop_without_proof(struct dma_chan *chan)
{
	return 0;
}

static void macaudio_energy_test_synchronize(struct dma_chan *chan)
{
}

static void macaudio_energy_test_fault_work(struct work_struct *work)
{
	/* No registered card/device exists in this handler-level fixture. */
}

static void macaudio_energy_raw_fe_needs_a_proven_stop(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	struct dma_device *device =
		kunit_kzalloc(test, sizeof(*device), GFP_KERNEL);
	struct dma_chan *chan = kunit_kzalloc(test, sizeof(*chan), GFP_KERNEL);
	struct snd_ctl_elem_value *value =
		kunit_kzalloc(test, sizeof(*value), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, device);
	KUNIT_ASSERT_NOT_NULL(test, chan);
	KUNIT_ASSERT_NOT_NULL(test, value);
	device->device_terminate_all = macaudio_energy_test_stop_without_proof;
	device->device_synchronize = macaudio_energy_test_synchronize;
	chan->device = device;
	t->ma.energy_stream[0].chan = chan;
	t->ma.energy_stream[0].opened = true;
	t->ma.energy_stream[0].attached = false;
	t->ma.energy_stream[0].speakers = false;
	atomic_set(&t->ma.energy_open, 1);
	INIT_WORK(&t->ma.lock_fail_work, macaudio_energy_test_fault_work);
	/* terminate_sync returning zero alone cannot free a raw route. */
	macaudio_energy_shutdown(&t->substream);
	KUNIT_EXPECT_TRUE(test, t->ma.speaker_lock_failed);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->ma.energy_open), 1);
	KUNIT_EXPECT_TRUE(test, t->ma.energy_stream[0].opened);
	KUNIT_EXPECT_EQ(test, macaudio_energy_pin_put(t->control[0], value),
			-EIO);
}

static void macaudio_energy_fixed_transforms_are_read_only(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);

	strscpy(t->control[0]->id.name, "Left HPF Corner Frequency",
		sizeof(t->control[0]->id.name));
	strscpy(t->control[1]->id.name, "Left ASI1 Sel",
		sizeof(t->control[1]->id.name));
	strscpy(t->control[2]->id.name, "Jack HPF Corner Frequency",
		sizeof(t->control[2]->id.name));
	t->control[0]->vd[0].access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
				      SNDRV_CTL_ELEM_ACCESS_INACTIVE;
	t->control[1]->vd[0].access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
				      SNDRV_CTL_ELEM_ACCESS_INACTIVE;
	t->control[2]->vd[0].access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
	macaudio_energy_fixed_controls(&t->ma.card, "Left");
	KUNIT_EXPECT_FALSE(test, t->control[0]->vd[0].access &
					 SNDRV_CTL_ELEM_ACCESS_WRITE);
	KUNIT_EXPECT_FALSE(test, t->control[1]->vd[0].access &
					 SNDRV_CTL_ELEM_ACCESS_WRITE);
	KUNIT_EXPECT_TRUE(test, t->control[2]->vd[0].access &
					SNDRV_CTL_ELEM_ACCESS_WRITE);
	KUNIT_EXPECT_TRUE(test, t->control[0]->vd[0].access &
					SNDRV_CTL_ELEM_ACCESS_READ);
}

static void
macaudio_energy_raw_routes_cannot_acquire_a_grant(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	struct snd_ctl_elem_value *value =
		kunit_kzalloc(test, sizeof(*value), GFP_KERNEL);
	struct macaudio_energy_ctl *packet;
	struct apple_energy_snapshot state;
	unsigned int k;

	KUNIT_ASSERT_NOT_NULL(test, value);
	packet = (void *)value->value.bytes.data;
	KUNIT_ASSERT_EQ(test, macaudio_energy_get(t->control[0], value), 0);
	packet->flags = 0;
	packet->deadline_ns = cpu_to_le64(le64_to_cpu(packet->clock_ns) +
					  100 * NSEC_PER_MSEC);
	for (k = 0; k < 2; k++) {
		packet->ceiling[k] = cpu_to_le64(1ULL << 30);
		packet->gain_q31[k] = cpu_to_le32(APPLE_ENERGY_UNITY);
	}
	/* A headphone FE may become a speaker route through automatic DAPM. */
	t->ma.energy_stream[1].opened = true;
	KUNIT_EXPECT_EQ(test, macaudio_energy_put(t->control[0], value),
			-EAGAIN);
	apple_energy_snapshot(&t->ma.energy, &state);
	KUNIT_EXPECT_FALSE(test, state.enabled);
	KUNIT_EXPECT_FALSE(test, macaudio_energy_permitted(&t->ma));
	KUNIT_ASSERT_EQ(test, macaudio_energy_get(t->control[0], value), 0);
	KUNIT_EXPECT_FALSE(test, le32_to_cpu(packet->flags) &
					 MACAUDIO_ENERGY_CONFIGURED);
	/* Neither an unprepared nor an older epoch's protected FE qualifies. */
	t->ma.energy_stream[1] = t->ma.energy_stream[0];
	t->ma.energy_stream[1].opened = true;
	t->ma.energy_stream[1].epoch--;
	KUNIT_EXPECT_FALSE(test, macaudio_energy_frontends_ready(&t->ma,
								 state.epoch));
	t->ma.energy_stream[1].epoch = state.epoch;
	KUNIT_EXPECT_TRUE(test,
			  macaudio_energy_frontends_ready(&t->ma, state.epoch));
}

static void macaudio_energy_tail_bound_is_immutable(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	struct snd_ctl_elem_value *value =
		kunit_kzalloc(test, sizeof(*value), GFP_KERNEL);
	struct macaudio_energy_ctl *packet;
	struct apple_energy_snapshot state;

	KUNIT_ASSERT_NOT_NULL(test, value);
	t->ma.energy_tail_us[0] = 1001;
	t->ma.energy_tail_us[1] = 2002;
	KUNIT_ASSERT_EQ(test, macaudio_energy_get(t->control[0], value), 0);
	packet = (void *)value->value.bytes.data;
	KUNIT_EXPECT_EQ(test, le32_to_cpu(packet->tail_us[0]), 1001);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(packet->tail_us[1]), 2002);
	packet->flags = 0;
	packet->deadline_ns = cpu_to_le64(le64_to_cpu(packet->clock_ns) +
					  100 * NSEC_PER_MSEC);
	packet->tail_us[1] = 0;
	KUNIT_EXPECT_EQ(test, macaudio_energy_put(t->control[0], value),
			-ESTALE);
	apple_energy_snapshot(&t->ma.energy, &state);
	KUNIT_EXPECT_FALSE(test, state.enabled);
}


static void macaudio_neo_requires_a_complete_transfer_certificate(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	struct snd_ctl_elem_value *value = kunit_kzalloc(test, sizeof(*value), GFP_KERNEL);
	struct macaudio_energy_ctl *packet;

	KUNIT_ASSERT_NOT_NULL(test, value);
	t->ma.cfg = &macaudio_j700_cfg;
	/* Never call a TAS readback helper or infer a norm from a sine tap. */
	KUNIT_EXPECT_EQ(test, macaudio_energy_transfer(&t->ma), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, t->ma.energy_transfer, MACAUDIO_ENERGY_TRANSFER_NONE);
	KUNIT_ASSERT_EQ(test, macaudio_energy_get(t->control[0], value), 0);
	packet = (void *)value->value.bytes.data;
	KUNIT_EXPECT_FALSE(test, le32_to_cpu(packet->flags) & MACAUDIO_ENERGY_TRANSFER_VALID);
	packet->flags = 0;
	KUNIT_EXPECT_EQ(test, macaudio_energy_put(t->control[0], value), -EOPNOTSUPP);
}


static void macaudio_neo_payload_is_only_the_stereo_48k_software_gain_fe(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	struct snd_pcm_runtime *runtime = kunit_kzalloc(test, sizeof(*runtime), GFP_KERNEL);
	__le32 words[] = { cpu_to_le32(0xbf000000), cpu_to_le32(0x3e800000) };
	struct apple_energy_snapshot state;
	u64 cap[] = { APPLE_ENERGY_UNIT_SCALE, APPLE_ENERGY_UNIT_SCALE }, ticket;

	KUNIT_ASSERT_NOT_NULL(test, runtime);
	t->ma.cfg = &macaudio_j700_cfg;
	runtime->format = SNDRV_PCM_FORMAT_S32_LE;
	runtime->channels = 2;
	runtime->rate = 48000;
	KUNIT_EXPECT_EQ(test, macaudio_energy_payload_format(&t->ma, runtime, true),
		APPLE_ENERGY_FORMAT_NEO_F32);
	KUNIT_EXPECT_EQ(test, macaudio_energy_payload_format(&t->ma, runtime, false), -EINVAL);
	runtime->rate = 44100;
	KUNIT_EXPECT_EQ(test, macaudio_energy_payload_format(&t->ma, runtime, true), -EINVAL);
	runtime->rate = 48000;
	runtime->format = SNDRV_PCM_FORMAT_S16_LE;
	KUNIT_EXPECT_EQ(test, macaudio_energy_payload_format(&t->ma, runtime, true), -EINVAL);
	runtime->format = SNDRV_PCM_FORMAT_S32_LE;
	runtime->channels = 1;
	KUNIT_EXPECT_EQ(test, macaudio_energy_payload_format(&t->ma, runtime, true), -EINVAL);
	/* Direct scalar-reference grant exercises the real callback only;
	 * it does not make the production transfer helper return a certificate.
	 */
	apple_energy_snapshot(&t->ma.energy, &state);
	KUNIT_ASSERT_EQ(test, apple_energy_grant(&t->ma.energy, &state, cap, 1, U64_MAX), 0);
	t->ma.energy_stream[0].format = APPLE_ENERGY_FORMAT_NEO_F32;
	KUNIT_ASSERT_EQ(test, macaudio_energy_process(&t->ma.energy_stream[0], words,
		sizeof(words), &ticket), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(words[0]), 0xbf000000U);
	apple_energy_snapshot(&t->ma.energy, &state);
	KUNIT_EXPECT_EQ(test, state.submitted[0], APPLE_ENERGY_UNIT_SCALE);
	KUNIT_EXPECT_EQ(test, state.submitted[1], APPLE_ENERGY_UNIT_SCALE / 4);
	KUNIT_ASSERT_EQ(test, macaudio_energy_retire(&t->ma.energy_stream[0], ticket), 0);
	apple_energy_snapshot(&t->ma.energy, &state);
	KUNIT_EXPECT_EQ(test, state.refunded[0], 0ULL);
}


static void macaudio_unqualified_prepare_leaves_the_legacy_lease_untouched(struct kunit *test)
{
	struct macaudio_energy_test *t = macaudio_energy_test_new(test);
	struct apple_energy_snapshot before, after;

	t->ma.energy_enforced = false;
	t->ma.speaker_lock_remain = 123456;
	t->ma.speaker_lock_timeout = 654321;
	apple_energy_snapshot(&t->ma.energy, &before);
	/* No PCM runtime/channel/codec I/O is available or needed. */
	KUNIT_ASSERT_EQ(test, macaudio_energy_prepare(&t->substream), 0);
	KUNIT_EXPECT_EQ(test, t->ma.speaker_lock_remain, 123456LL);
	KUNIT_EXPECT_EQ(test, t->ma.speaker_lock_timeout, 654321LL);
	KUNIT_EXPECT_EQ(test, t->ma.energy_transfer, MACAUDIO_ENERGY_TRANSFER_ENERGY_NORM);
	KUNIT_EXPECT_PTR_EQ(test, t->ma.energy_stream[0].chan, NULL);
	apple_energy_snapshot(&t->ma.energy, &after);
	KUNIT_EXPECT_EQ(test, before.epoch, after.epoch);
	KUNIT_EXPECT_EQ(test, before.sequence, after.sequence);
}

static struct kunit_case macaudio_energy_cases[] = {
	KUNIT_CASE(macaudio_unqualified_prepare_leaves_the_legacy_lease_untouched),
	KUNIT_CASE(macaudio_neo_payload_is_only_the_stereo_48k_software_gain_fe),
	KUNIT_CASE(macaudio_neo_requires_a_complete_transfer_certificate),
	KUNIT_CASE(macaudio_energy_owner_must_match_every_control),
	KUNIT_CASE(macaudio_energy_control_replay_and_corruption_revoke),
	KUNIT_CASE(macaudio_energy_normal_stop_is_not_a_gate_fault),
	KUNIT_CASE(macaudio_energy_raw_fe_needs_a_proven_stop),
	KUNIT_CASE(macaudio_energy_expired_write_is_a_retryable_timeout),
	KUNIT_CASE(macaudio_energy_close_revokes_a_live_grant),
	KUNIT_CASE(macaudio_energy_fixed_transforms_are_read_only),
	KUNIT_CASE(macaudio_energy_raw_routes_cannot_acquire_a_grant),
	KUNIT_CASE(macaudio_energy_tail_bound_is_immutable),
	{}
};

static struct kunit_suite macaudio_energy_suite = {
	.name = "apple-macaudio-energy-control",
	.test_cases = macaudio_energy_cases,
};

kunit_test_suite(macaudio_energy_suite);

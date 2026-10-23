// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The sink's audio around a failed external mode set, included at the end
 * of iomfb_v14_7.c so that the production dcp_v14_external_modeset() runs.
 * Only the audio service is stubbed. The mode lookup fails on an empty
 * catalog after a complete description, before anything is powered down.
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

struct v14_ext_audio {
	struct apple_dcp *dcp;
	struct apple_dcp_v14 *v14;
	unsigned int offs, ons;
};

static struct v14_ext_audio *v14_ext_audio_current;

static void v14_ext_audio_off(struct apple_dcp *dcp)
{
	v14_ext_audio_current->offs++;
}

static void v14_ext_audio_on(struct apple_dcp *dcp)
{
	v14_ext_audio_current->ons++;
}

static void v14_ext_audio_retry(struct work_struct *work)
{
}

static void v14_ext_audio_stop(void *data)
{
	struct apple_dcp *dcp = data;

	cancel_delayed_work_sync(&dcp->external_retry_wq);
}

/* A started, described native external session, its display @powered. */
static struct v14_ext_audio *v14_ext_audio_setup(struct kunit *test, bool powered)
{
	struct v14_ext_audio *s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, s);
	dev = kunit_device_register(test, "dcp-v14-ext-audio");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	s->dcp = devm_kzalloc(dev, sizeof(*s->dcp), GFP_KERNEL);
	s->v14 = devm_kzalloc(dev, sizeof(*s->v14), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, s->dcp);
	KUNIT_ASSERT_NOT_NULL(test, s->v14);
	s->dcp->dev = dev;
	s->dcp->fw_compat = DCP_FIRMWARE_V_14_7;
	s->dcp->external = true;
	s->dcp->external_native = true;
	/* Never dereferenced: the audio service is stubbed. */
	s->dcp->avep = (struct apple_dcp_afkep *)s;
	mutex_init(&s->dcp->modes_lock);
	INIT_DELAYED_WORK(&s->dcp->external_retry_wq, v14_ext_audio_retry);
	s->dcp->v14 = s->v14;
	s->v14->dev = dev;
	s->v14->dcp = s->dcp;
	s->v14->external = true;
	s->v14->started = true;
	s->v14->opened = true;
	s->v14->powered = powered;
	s->v14->described = DCP_V14_DESC_ALL;
	mutex_init(&s->v14->lock);
	init_waitqueue_head(&s->v14->described_wait);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, v14_ext_audio_stop, s->dcp), 0);
	v14_ext_audio_current = s;
	kunit_activate_static_stub(test, dcp_v14_audio_off, v14_ext_audio_off);
	kunit_activate_static_stub(test, dcp_v14_audio_on, v14_ext_audio_on);
	return s;
}

/* The mode is gone, the display stays on in its old mode: so does its audio. */
static void v14_ext_audio_failed_modeset_keeps_audio(struct kunit *test)
{
	struct v14_ext_audio *s = v14_ext_audio_setup(test, true);
	struct drm_crtc_state *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_EXPECT_EQ(test, dcp_v14_external_modeset(s->dcp, state), -EINVAL);
	KUNIT_EXPECT_EQ(test, s->offs, 1U);
	KUNIT_EXPECT_EQ(test, s->ons, 1U);
	KUNIT_EXPECT_TRUE(test, s->v14->powered);
	KUNIT_EXPECT_FALSE(test, s->v14->failed);
	/* The mode set is still retried. */
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&s->dcp->external_retry_wq));
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&s->v14->lock));
}

/* A display that was off has no audio to stop, and none comes back. */
static void v14_ext_audio_failed_modeset_display_off(struct kunit *test)
{
	struct v14_ext_audio *s = v14_ext_audio_setup(test, false);
	struct drm_crtc_state *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_EXPECT_EQ(test, dcp_v14_external_modeset(s->dcp, state), -EINVAL);
	KUNIT_EXPECT_EQ(test, s->offs, 0U);
	KUNIT_EXPECT_EQ(test, s->ons, 0U);
}

/*
 * Out of retries: the display stays on in its old mode, and so does its
 * audio. Nothing else would connect the sink again.
 */
static void v14_ext_audio_no_retry_left(struct kunit *test)
{
	struct v14_ext_audio *s = v14_ext_audio_setup(test, true);
	struct drm_crtc_state *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, state);
	/* Past any retry budget (DCP_EXTERNAL_RETRIES in dcp.c). */
	atomic_set(&s->dcp->external_retries, 100);
	KUNIT_EXPECT_EQ(test, dcp_v14_external_modeset(s->dcp, state), -EINVAL);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&s->dcp->external_retry_wq));
	KUNIT_EXPECT_EQ(test, s->offs, 1U);
	KUNIT_EXPECT_EQ(test, s->ons, 1U);
}

static struct kunit_case v14_ext_audio_cases[] = {
	KUNIT_CASE(v14_ext_audio_failed_modeset_keeps_audio),
	KUNIT_CASE(v14_ext_audio_failed_modeset_display_off),
	KUNIT_CASE(v14_ext_audio_no_retry_left),
	{}
};

static struct kunit_suite v14_ext_audio_suite = {
	.name = "dcp_v14_external_audio",
	.test_cases = v14_ext_audio_cases,
};

kunit_test_suite(v14_ext_audio_suite);

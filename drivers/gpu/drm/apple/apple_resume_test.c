// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_modeset_helper_vtables.h>

#include "apple_resume.h"

/* One pipe: a primary plane scanning a framebuffer out to one connector. */
struct resume_test {
	struct drm_device drm;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_framebuffer fb;
	/* Whether the mode fixup admits the mode, as a current link would. */
	bool admissible;
};

static struct resume_test *to_resume_test(struct drm_device *drm)
{
	return container_of(drm, struct resume_test, drm);
}

static bool resume_test_mode_fixup(struct drm_crtc *crtc,
				   const struct drm_display_mode *mode,
				   struct drm_display_mode *adjusted_mode)
{
	return to_resume_test(crtc->dev)->admissible;
}

static const struct drm_crtc_helper_funcs resume_test_crtc_helpers = {
	.mode_fixup = resume_test_mode_fixup,
};

/* Scanout itself is not under test; the plane helpers require an update. */
static void resume_test_plane_update(struct drm_plane *plane,
				     struct drm_atomic_commit *state)
{
}

static const struct drm_plane_helper_funcs resume_test_plane_helpers = {
	.atomic_update = resume_test_plane_update,
};

static const struct drm_connector_helper_funcs resume_test_connector_helpers = {
};

static const struct drm_connector_funcs resume_test_connector_funcs = {
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.reset			= drm_atomic_helper_connector_reset,
};

static void resume_test_fb_destroy(struct drm_framebuffer *fb)
{
	drm_framebuffer_cleanup(fb);
}

static const struct drm_framebuffer_funcs resume_test_fb_funcs = {
	.destroy = resume_test_fb_destroy,
};

static void resume_test_teardown(void *data)
{
	struct resume_test *priv = data;

	/* Release the scanout reference before the device goes away. */
	drm_atomic_helper_shutdown(&priv->drm);
	drm_framebuffer_put(&priv->fb);
}

/* Finish the acquire context before an assertion can abort the KUnit thread. */
static int resume_test_commit(struct resume_test *priv,
			      struct drm_display_mode *mode, bool active)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	int ret;

	drm_modeset_acquire_init(&ctx, 0);
retry:
	state = drm_atomic_commit_alloc(&priv->drm);
	if (!state) {
		ret = -ENOMEM;
		goto out;
	}
	state->acquire_ctx = &ctx;
	crtc_state = drm_atomic_get_crtc_state(state, priv->crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto put;
	}
	crtc_state->active = active;
	if (mode) {
		conn_state = drm_atomic_get_connector_state(state, &priv->connector);
		if (IS_ERR(conn_state)) {
			ret = PTR_ERR(conn_state);
			goto put;
		}
		plane_state = drm_atomic_get_plane_state(state, priv->plane);
		if (IS_ERR(plane_state)) {
			ret = PTR_ERR(plane_state);
			goto put;
		}
		ret = drm_atomic_set_crtc_for_connector(conn_state, priv->crtc) ?:
		      drm_atomic_set_mode_for_crtc(crtc_state, mode) ?:
		      drm_atomic_set_crtc_for_plane(plane_state, priv->crtc);
		if (ret)
			goto put;
		drm_atomic_set_fb_for_plane(plane_state, &priv->fb);
		plane_state->crtc_w = 1920;
		plane_state->crtc_h = 1080;
		plane_state->src_w = 1920 << 16;
		plane_state->src_h = 1080 << 16;
	}
	ret = drm_atomic_commit(state);
put:
	drm_atomic_commit_put(state);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}
out:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
}

static struct resume_test *resume_test_init(struct kunit *test)
{
	struct drm_display_mode *mode;
	struct resume_test *priv;
	struct drm_device *drm;
	struct device *dev;
	int ret;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	priv = drm_kunit_helper_alloc_drm_device(test, dev, struct resume_test, drm,
						 DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);
	drm = &priv->drm;
	priv->admissible = true;

	priv->plane = drm_kunit_helper_create_primary_plane(test, drm, NULL,
							    &resume_test_plane_helpers,
							    NULL, 0, NULL);
	priv->crtc = drm_kunit_helper_create_crtc(test, drm, priv->plane, NULL, NULL,
						  &resume_test_crtc_helpers);
	ret = drmm_encoder_init(drm, &priv->encoder, NULL, DRM_MODE_ENCODER_TMDS, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	priv->encoder.possible_crtcs = drm_crtc_mask(priv->crtc);
	ret = drmm_connector_init(drm, &priv->connector, &resume_test_connector_funcs,
				  DRM_MODE_CONNECTOR_USB, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	drm_connector_helper_add(&priv->connector, &resume_test_connector_helpers);
	ret = drm_connector_attach_encoder(&priv->connector, &priv->encoder);
	KUNIT_ASSERT_EQ(test, ret, 0);
	drm_mode_config_reset(drm);

	priv->fb.dev = drm;
	priv->fb.format = drm_format_info(DRM_FORMAT_XRGB8888);
	priv->fb.width = 1920;
	priv->fb.height = 1080;
	priv->fb.pitches[0] = 1920 * 4;
	priv->fb.modifier = DRM_FORMAT_MOD_LINEAR;
	ret = drm_framebuffer_init(drm, &priv->fb, &resume_test_fb_funcs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, resume_test_teardown, priv);
	KUNIT_ASSERT_EQ(test, ret, 0);

	mode = drm_kunit_display_mode_from_cea_vic(test, drm, 16);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	ret = resume_test_commit(priv, mode, true);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_TRUE(test, priv->crtc->state->active);

	return priv;
}

/* The failure the stale CRTC filter exists for: one bad mode fails resume. */
static void resume_stale_mode_fails_unfiltered_test(struct kunit *test)
{
	struct resume_test *priv = resume_test_init(test);

	KUNIT_ASSERT_EQ(test, drm_mode_config_helper_suspend(&priv->drm), 0);
	priv->admissible = false;
	KUNIT_EXPECT_EQ(test, drm_mode_config_helper_resume(&priv->drm), -EINVAL);
	KUNIT_EXPECT_FALSE(test, priv->crtc->state->enable);
}

static void resume_stale_crtc_disabled_test(struct kunit *test)
{
	struct resume_test *priv = resume_test_init(test);
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;

	KUNIT_ASSERT_EQ(test, drm_mode_config_helper_suspend(&priv->drm), 0);
	priv->admissible = false;
	state = priv->drm.mode_config.suspend_state;
	/* The caller tells userspace about the CRTC it had and lost. */
	KUNIT_EXPECT_EQ(test, apple_drm_drop_stale_crtcs(state), drm_crtc_mask(priv->crtc));

	/* The resume check warns about an encoder routed without a CRTC. */
	conn_state = drm_atomic_get_new_connector_state(state, &priv->connector);
	crtc_state = drm_atomic_get_new_crtc_state(state, priv->crtc);
	KUNIT_ASSERT_NOT_NULL(test, conn_state);
	KUNIT_ASSERT_NOT_NULL(test, crtc_state);
	KUNIT_EXPECT_NULL(test, conn_state->best_encoder);
	KUNIT_EXPECT_EQ(test, crtc_state->encoder_mask, 0U);

	KUNIT_EXPECT_EQ(test, drm_mode_config_helper_resume(&priv->drm), 0);

	KUNIT_EXPECT_FALSE(test, priv->crtc->state->enable);
	KUNIT_EXPECT_FALSE(test, priv->crtc->state->active);
	KUNIT_EXPECT_EQ(test, priv->crtc->state->plane_mask, 0U);
	KUNIT_EXPECT_EQ(test, priv->crtc->state->connector_mask, 0U);
	KUNIT_EXPECT_NULL(test, priv->plane->state->crtc);
	KUNIT_EXPECT_NULL(test, priv->plane->state->fb);
	KUNIT_EXPECT_NULL(test, priv->connector.state->crtc);
	/* Userspace must modeset again rather than flip onto a dead CRTC. */
	KUNIT_EXPECT_EQ(test, priv->connector.state->link_status,
			DRM_LINK_STATUS_BAD);
	/* Only the creator's reference is left: the saved scanout was dropped. */
	KUNIT_EXPECT_EQ(test, drm_framebuffer_read_refcount(&priv->fb), 1U);
}

static void resume_admissible_crtc_restored_test(struct kunit *test)
{
	struct resume_test *priv = resume_test_init(test);

	KUNIT_ASSERT_EQ(test, drm_mode_config_helper_suspend(&priv->drm), 0);
	KUNIT_EXPECT_EQ(test, apple_drm_drop_stale_crtcs(priv->drm.mode_config.suspend_state),
			0U);
	KUNIT_EXPECT_EQ(test, drm_mode_config_helper_resume(&priv->drm), 0);

	KUNIT_EXPECT_TRUE(test, priv->crtc->state->enable);
	KUNIT_EXPECT_TRUE(test, priv->crtc->state->active);
	KUNIT_EXPECT_PTR_EQ(test, priv->plane->state->crtc, priv->crtc);
	KUNIT_EXPECT_PTR_EQ(test, priv->plane->state->fb, &priv->fb);
	KUNIT_EXPECT_PTR_EQ(test, priv->connector.state->crtc, priv->crtc);
	KUNIT_EXPECT_EQ(test, priv->connector.state->link_status,
			DRM_LINK_STATUS_GOOD);
	KUNIT_EXPECT_EQ(test, drm_framebuffer_read_refcount(&priv->fb), 2U);
}

/* A DPMS-off CRTC keeps its mode, so it is checked like an active one. */
static void resume_stale_dpms_off_crtc_disabled_test(struct kunit *test)
{
	struct resume_test *priv = resume_test_init(test);

	KUNIT_ASSERT_EQ(test, resume_test_commit(priv, NULL, false), 0);

	KUNIT_ASSERT_EQ(test, drm_mode_config_helper_suspend(&priv->drm), 0);
	priv->admissible = false;
	KUNIT_EXPECT_EQ(test, apple_drm_drop_stale_crtcs(priv->drm.mode_config.suspend_state),
			drm_crtc_mask(priv->crtc));
	KUNIT_EXPECT_EQ(test, drm_mode_config_helper_resume(&priv->drm), 0);
	KUNIT_EXPECT_FALSE(test, priv->crtc->state->enable);
	KUNIT_EXPECT_NULL(test, priv->connector.state->crtc);
	KUNIT_EXPECT_EQ(test, priv->connector.state->link_status,
			DRM_LINK_STATUS_BAD);
	KUNIT_EXPECT_EQ(test, drm_framebuffer_read_refcount(&priv->fb), 1U);
}

static struct kunit_case resume_cases[] = {
	KUNIT_CASE(resume_stale_mode_fails_unfiltered_test),
	KUNIT_CASE(resume_stale_crtc_disabled_test),
	KUNIT_CASE(resume_admissible_crtc_restored_test),
	KUNIT_CASE(resume_stale_dpms_off_crtc_disabled_test),
	{}
};

static struct kunit_suite resume_suite = {
	.name = "apple-drm-resume",
	.test_cases = resume_cases,
};

kunit_test_suite(resume_suite);

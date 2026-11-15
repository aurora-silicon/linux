// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/platform_device.h>
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_managed.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "connector.h"
#include "dcp.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "parser.h"

/*
 * An external H17P display whose DisplayPort route goes away and comes back
 * while userspace keeps its CRTC active, through the real hotplug worker,
 * detect(), timeout work and atomic commits.  The CRTC stands in for the DCP
 * pipe: as for the 180 Hz exception mode, its mode is admitted only once a
 * probe has read the EDID of the current cable generation, and setting it
 * brings the link up unless the test says otherwise.
 */
struct relink_test {
	struct drm_device drm;
	struct apple_dcp dcp;
	struct apple_connector con;
	struct platform_device *pdev;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder encoder;
	struct drm_framebuffer fb;
	struct drm_display_mode *mode;

	u64 probed_generation;	/* the cable generation of the last probe */
	bool offered;		/* the display offers the mode */
	bool trains;		/* setting the mode brings the link up */
	bool bounce_on_probe;	/* the route bounces again during a probe */
	bool real_modeset;	/* atomic_enable is the real dcp_crtc_atomic_modeset() */
	bool force_modeset;	/* a userspace commit changes the connectors */
	bool fault_in_call;	/* the link faults before the firmware answers */
	bool no_reply;		/* the firmware answers a call with no reply */
	u32 reply;		/* what the firmware answers a call with */
	int modeset_ret;	/* the result of the real mode set */
	struct dcp_display_mode catalog;
	bool hold_modeset;	/* a mode set waits for @modeset_release */
	struct completion modeset_entered, modeset_release, userspace_done;
	int userspace_ret;
	atomic_t probes, checks, modesets, events;
};

static struct relink_test *to_relink_test(struct drm_device *drm)
{
	return container_of(drm, struct relink_test, drm);
}

static void relink_test_noop(struct work_struct *work)
{
}

static void relink_test_point(struct apple_dcp *dcp, enum dcp_test_point point)
{
	struct relink_test *t = container_of(dcp, struct relink_test, dcp);

	if (point == DCP_TEST_HOTPLUG_EVENT)
		atomic_inc(&t->events);
}

static enum drm_connector_status relink_test_detect(struct drm_connector *connector, bool force)
{
	return dcp_connector_connected(to_apple_connector(connector)) ?
	       connector_status_connected : connector_status_disconnected;
}

/* As dcp_get_modes(): what a probe reads belongs to the current cable. */
static int relink_test_get_modes(struct drm_connector *connector)
{
	struct relink_test *t = to_relink_test(connector->dev);
	struct drm_display_mode *mode;

	atomic_inc(&t->probes);
	scoped_guard(mutex, &t->dcp.hpd_mutex) {
		t->probed_generation = t->dcp.typec_generation;
		if (t->bounce_on_probe)
			t->dcp.typec_generation++;
	}
	if (!t->offered)
		return 0;
	mode = drm_mode_duplicate(connector->dev, t->mode);
	if (!mode)
		return 0;
	drm_mode_probed_add(connector, mode);
	return 1;
}

/* As lookup_mode() for the exception mode, under the same lock. */
static bool relink_test_mode_fixup(struct drm_crtc *crtc,
				   const struct drm_display_mode *mode,
				   struct drm_display_mode *adjusted_mode)
{
	struct relink_test *t = to_relink_test(crtc->dev);

	atomic_inc(&t->checks);
	guard(mutex)(&t->dcp.hpd_mutex);
	return t->offered && t->probed_generation == t->dcp.typec_generation;
}

/*
 * As dcp_crtc_atomic_modeset(): the mode set trains the link, or fails and
 * has the hotplug worker recover the CRTC.
 */
/* The firmware for the real mode set path: it answers every method call. */
static void relink_test_push(struct apple_dcp *dcp, const struct dcp_method_entry *call,
			     dcp_callback_t cb, void *cookie)
{
	struct relink_test *t = container_of(dcp, struct relink_test, dcp);
	bool modeset = !strcmp(call->name, "dcpep_set_digital_out_mode");

	if (modeset && t->fault_in_call)
		dcp_external_link_fault(dcp);
	cb(dcp, modeset && t->no_reply ? NULL : &t->reply, cookie);
}

static void relink_test_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct relink_test *t = to_relink_test(crtc->dev);
	unsigned int action;

	atomic_inc(&t->modesets);
	if (t->hold_modeset) {
		complete(&t->modeset_entered);
		wait_for_completion(&t->modeset_release);
	}
	if (t->real_modeset) {
		t->modeset_ret = dcp_crtc_atomic_modeset(crtc, state);
		return;
	}
	dcp_mode_begin(&t->dcp.mode_state);
	action = dcp_mode_finish(&t->dcp.mode_state, t->trains, &t->con.connected);
	if (!t->trains)
		action |= DCP_HOTPLUG_NOTIFY;
	dcp_handle_hotplug_actions(&t->dcp, action);
}

/* As dcp_poweroff() for the external display. */
static void relink_test_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct relink_test *t = to_relink_test(crtc->dev);

	if (drm_atomic_get_new_crtc_state(state, crtc)->active)
		return;
	guard(mutex)(&t->dcp.external_lock);
	dcp_mode_invalidate(&t->dcp.mode_state);
	dcp_external_crtc_released(&t->dcp);
}

static const struct drm_crtc_funcs relink_test_crtc_funcs = {
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs relink_test_crtc_helpers = {
	.mode_fixup	= relink_test_mode_fixup,
	.atomic_enable	= relink_test_crtc_enable,
	.atomic_disable	= relink_test_crtc_disable,
};

/* Scanout itself is not under test; the plane helpers require an update. */
static void relink_test_plane_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
}

static const struct drm_plane_helper_funcs relink_test_plane_helpers = {
	.atomic_update = relink_test_plane_update,
};

static const struct drm_connector_helper_funcs relink_test_connector_helpers = {
	.get_modes = relink_test_get_modes,
};

static const struct drm_connector_funcs relink_test_connector_funcs = {
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.detect			= relink_test_detect,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static void relink_test_fb_destroy(struct drm_framebuffer *fb)
{
	drm_framebuffer_cleanup(fb);
}

static const struct drm_framebuffer_funcs relink_test_fb_funcs = {
	.destroy = relink_test_fb_destroy,
};

static void relink_test_exit(void *data)
{
	struct relink_test *t = data;

	complete_all(&t->modeset_release);
	wait_for_completion_timeout(&t->userspace_done, 10 * HZ);
	disable_delayed_work_sync(&t->dcp.announce_wq);
	disable_work_sync(&t->con.hotplug_wq);
	apple_connector_backlight_early_unregister(&t->con);
	drm_atomic_helper_shutdown(&t->drm);
	drm_framebuffer_put(&t->fb);
}

/* Commit @active for the CRTC, as userspace would. */
static int relink_test_commit(struct relink_test *t, bool active, bool init)
{
	struct drm_connector_state *conn_state;
	struct drm_plane_state *plane_state;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_commit *state;
	int ret;

	drm_modeset_acquire_init(&ctx, 0);
retry:
	state = drm_atomic_commit_alloc(&t->drm);
	if (!state) {
		ret = -ENOMEM;
		goto out;
	}
	state->acquire_ctx = &ctx;
	crtc_state = drm_atomic_get_crtc_state(state, t->crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto put;
	}
	crtc_state->active = active;
	if (t->force_modeset)
		crtc_state->connectors_changed = true;
	if (init) {
		conn_state = drm_atomic_get_connector_state(state, &t->con.base);
		if (IS_ERR(conn_state)) {
			ret = PTR_ERR(conn_state);
			goto put;
		}
		plane_state = drm_atomic_get_plane_state(state, t->plane);
		if (IS_ERR(plane_state)) {
			ret = PTR_ERR(plane_state);
			goto put;
		}
		ret = drm_atomic_set_crtc_for_connector(conn_state, t->crtc) ?:
		      drm_atomic_set_mode_for_crtc(crtc_state, t->mode) ?:
		      drm_atomic_set_crtc_for_plane(plane_state, t->crtc);
		if (ret)
			goto put;
		drm_atomic_set_fb_for_plane(plane_state, &t->fb);
		plane_state->crtc_w = t->mode->hdisplay;
		plane_state->crtc_h = t->mode->vdisplay;
		plane_state->src_w = t->mode->hdisplay << 16;
		plane_state->src_h = t->mode->vdisplay << 16;
	}
	ret = drm_atomic_commit(state);
put:
	drm_atomic_commit_put(state);
	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto retry;
	}
out:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
}

/*
 * Let the hotplug worker run until it is idle.  A held connection keeps its
 * timeout work queued, which is flushed too but may stay queued.
 */
static void relink_test_settle(struct kunit *test, struct relink_test *t)
{
	int i;

	for (i = 0; i < 8; i++) {
		flush_delayed_work(&t->dcp.announce_wq);
		flush_work(&t->con.hotplug_wq);
		if (!work_pending(&t->con.hotplug_wq) &&
		    (t->dcp.announce.holding || !delayed_work_pending(&t->dcp.announce_wq)))
			return;
	}
	KUNIT_FAIL(test, "the hotplug worker did not settle");
}

static void relink_test_hotplug(struct relink_test *t)
{
	schedule_work(&t->con.hotplug_wq);
	flush_work(&t->con.hotplug_wq);
}

/*
 * End the hold of a new connection by its timeout: the modes of this
 * fixture never get ready.
 */
static void relink_test_expire(struct kunit *test, struct relink_test *t)
{
	scoped_guard(mutex, &t->dcp.hpd_mutex) {
		t->dcp.announce.timed = true;
		t->dcp.announce.deadline = jiffies - 1;
	}
	mod_delayed_work(system_freezable_wq, &t->dcp.announce_wq, 0);
	relink_test_settle(test, t);
}

/* As dcp_external_disconnect() for a route that went away. */
static void relink_test_tear(struct relink_test *t)
{
	struct apple_dcp *dcp = &t->dcp;

	scoped_guard(mutex, &dcp->hpd_mutex) {
		WRITE_ONCE(dcp->typec_cable_connected, false);
		dcp->typec_generation++;
		dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
		dcp_announce_end(&dcp->announce);
	}
	dcp_mode_invalidate(&dcp->mode_state);
	WRITE_ONCE(t->con.connected, false);
	relink_test_hotplug(t);
}

/*
 * As dcp_dptx_connect_oob() for the route coming back, then the firmware's
 * connected callback, then the end of the new connection's hold.
 */
static void relink_test_reconnect(struct kunit *test, struct relink_test *t)
{
	struct apple_dcp *dcp = &t->dcp;

	scoped_guard(mutex, &dcp->hpd_mutex) {
		WRITE_ONCE(dcp->typec_cable_connected, true);
		dcp->typec_generation++;
		dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true);
		dcp_announce_end(&dcp->announce);
	}
	WRITE_ONCE(t->con.connected, true);
	relink_test_hotplug(t);
	KUNIT_ASSERT_TRUE(test, dcp->announce.holding);
	relink_test_expire(test, t);
}

static bool relink_test_connected(struct relink_test *t)
{
	return relink_test_detect(&t->con.base, false) == connector_status_connected;
}

static bool relink_test_link_up(struct relink_test *t)
{
	return READ_ONCE(t->dcp.mode_state.valid);
}

/*
 * A display on cable generation 2, reported connected, with userspace
 * scanning out to it at a mode admitted for that generation.
 */
static struct relink_test *relink_test_init(struct kunit *test)
{
	struct apple_crtc *apple_crtc;
	struct relink_test *t;
	struct apple_dcp *dcp;
	struct drm_device *drm;
	struct device *dev;
	int ret;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	t = drm_kunit_helper_alloc_drm_device(test, dev, struct relink_test, drm,
					      DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	drm = &t->drm;
	dcp = &t->dcp;
	apple_connector_backlight_init(&t->con);

	t->pdev = kunit_platform_device_alloc(test, "dcp-relink", PLATFORM_DEVID_AUTO);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->pdev);
	KUNIT_ASSERT_EQ(test, kunit_platform_device_add(test, t->pdev), 0);
	platform_set_drvdata(t->pdev, dcp);
	dcp->dev = &t->pdev->dev;
	dcp->hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL;
	iomfb_queue_init(dcp);
	mutex_init(&dcp->external_lock);
	mutex_init(&dcp->hpd_mutex);
	mutex_init(&dcp->dcpavserv.lock);
	dcp_modes_init(dcp);
	spin_lock_init(&dcp->metadata.lock);
	spin_lock_init(&dcp->mode_state.lock);
	INIT_DELAYED_WORK(&dcp->announce_wq, dcp_announce_work);
	INIT_WORK(&dcp->vblank_wq, relink_test_noop);
	apple_connector_backlight_init(&t->con);
	INIT_WORK(&t->con.hotplug_wq, dcp_hotplug);
	dcp->test_point = relink_test_point;
	dcp->typec_cable_connected = true;
	dcp->typec_generation = 2;
	dcp_firmware_metadata_connection(dcp, 2, true);
	dcp->connector = &t->con;
	t->con.dcp = t->pdev;
	t->con.connected = true;
	t->offered = true;
	t->trains = true;
	t->probed_generation = 2;
	init_completion(&t->modeset_entered);
	init_completion(&t->modeset_release);
	init_completion(&t->userspace_done);
	complete_all(&t->userspace_done);

	t->plane = drm_kunit_helper_create_primary_plane(test, drm, NULL,
							 &relink_test_plane_helpers,
							 NULL, 0, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->plane);
	/* An apple_crtc, so that the real mode set path finds its DCP. */
	apple_crtc = drmm_kzalloc(drm, sizeof(*apple_crtc), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, apple_crtc);
	apple_crtc->dcp = t->pdev;
	t->crtc = &apple_crtc->base;
	ret = drmm_crtc_init_with_planes(drm, t->crtc, t->plane, NULL,
					 &relink_test_crtc_funcs, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	drm_crtc_helper_add(t->crtc, &relink_test_crtc_helpers);
	ret = drmm_encoder_init(drm, &t->encoder, NULL, DRM_MODE_ENCODER_TMDS, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	t->encoder.possible_crtcs = drm_crtc_mask(t->crtc);
	ret = drmm_connector_init(drm, &t->con.base, &relink_test_connector_funcs,
				  DRM_MODE_CONNECTOR_USB, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	drm_connector_helper_add(&t->con.base, &relink_test_connector_helpers);
	KUNIT_ASSERT_EQ(test, drm_connector_attach_encoder(&t->con.base, &t->encoder), 0);
	drm_mode_config_reset(drm);
	/* The device is never registered, so it has nobody to send uevents to. */
	dev_set_uevent_suppress(drm->primary->kdev, true);

	t->mode = drm_kunit_display_mode_from_cea_vic(test, drm, 16);
	KUNIT_ASSERT_NOT_NULL(test, t->mode);
	t->fb.dev = drm;
	t->fb.format = drm_format_info(DRM_FORMAT_XRGB8888);
	t->fb.width = t->mode->hdisplay;
	t->fb.height = t->mode->vdisplay;
	t->fb.pitches[0] = t->mode->hdisplay * 4;
	t->fb.modifier = DRM_FORMAT_MOD_LINEAR;
	KUNIT_ASSERT_EQ(test, drm_framebuffer_init(drm, &t->fb, &relink_test_fb_funcs), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, relink_test_exit, t), 0);

	KUNIT_ASSERT_EQ(test, relink_test_commit(t, true, true), 0);
	KUNIT_ASSERT_TRUE(test, t->crtc->state->active);
	KUNIT_ASSERT_TRUE(test, relink_test_link_up(t));
	KUNIT_ASSERT_EQ(test, atomic_read(&t->modesets), 1);
	dcp->announce.announced = true;

	return t;
}

/*
 * The Neo sequence: the route of an active CRTC goes away and comes back,
 * with no valid mode, and before userspace has probed the new cable.  The
 * mode is set again, and the link is good.
 */
static void relink_bounce_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);
	int events;

	relink_test_tear(t);
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
	/* Userspace has not reacted: its CRTC is still on. */
	KUNIT_ASSERT_TRUE(test, t->crtc->state->active);

	events = atomic_read(&t->events);
	relink_test_reconnect(test, t);

	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), 2);
	KUNIT_EXPECT_TRUE(test, relink_test_link_up(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_GOOD);
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
	KUNIT_EXPECT_GT(test, atomic_read(&t->events), events);
}

/*
 * The display on the new link no longer offers the mode: the link is bad
 * and the display gone until userspace switches the CRTC off, and then it
 * is a new connection.
 */
static void relink_mode_gone_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);
	int events, checks;

	relink_test_tear(t);
	t->offered = false;
	events = atomic_read(&t->events);
	relink_test_reconnect(test, t);

	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), 1);
	KUNIT_EXPECT_FALSE(test, relink_test_link_up(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_BAD);
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
	KUNIT_EXPECT_GT(test, atomic_read(&t->events), events);

	/* Later firmware facts neither bring it back nor try again. */
	checks = atomic_read(&t->checks);
	relink_test_hotplug(t);
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->checks), checks);

	/* Userspace takes the display away, so it is new when it is back. */
	KUNIT_ASSERT_EQ(test, relink_test_commit(t, false, false), 0);
	relink_test_settle(test, t);
	KUNIT_EXPECT_TRUE(test, t->dcp.announce.holding);
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
	events = atomic_read(&t->events);
	relink_test_expire(test, t);
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
	KUNIT_EXPECT_GT(test, atomic_read(&t->events), events);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), 1);
}

/* The mode is set again but the link does not come up: as above. */
static void relink_link_fails_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);
	int events;

	relink_test_tear(t);
	t->trains = false;
	events = atomic_read(&t->events);
	relink_test_reconnect(test, t);

	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), 2);
	KUNIT_EXPECT_FALSE(test, relink_test_link_up(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_BAD);
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
	KUNIT_EXPECT_GT(test, atomic_read(&t->events), events);
}

/* A loss belongs to its cable: the next one is set up again. */
static void relink_next_cable_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	relink_test_tear(t);
	t->offered = false;
	relink_test_reconnect(test, t);
	KUNIT_ASSERT_FALSE(test, relink_test_connected(t));

	relink_test_tear(t);
	t->offered = true;
	relink_test_reconnect(test, t);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), 2);
	KUNIT_EXPECT_TRUE(test, relink_test_link_up(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_GOOD);
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
}

/* A CRTC userspace switched off is left to userspace. */
static void relink_inactive_crtc_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	KUNIT_ASSERT_EQ(test, relink_test_commit(t, false, false), 0);
	relink_test_settle(test, t);
	relink_test_tear(t);
	relink_test_reconnect(test, t);

	KUNIT_EXPECT_EQ(test, atomic_read(&t->probes), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), 1);
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
}

/*
 * Other DCPs keep their retraining: the CRTC is reset with its link marked
 * bad, without a probe.
 */
static void relink_other_profile_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	t->dcp.hw.iomfb_method_profile = DCP_IOMFB_METHODS_DEFAULT;
	dcp_mode_invalidate(&t->dcp.mode_state);
	relink_test_hotplug(t);

	KUNIT_EXPECT_EQ(test, atomic_read(&t->probes), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), 2);
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_BAD);
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
}

/* A route that bounces again during the probe leaves it to the new cable. */
static void relink_newer_cable_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	relink_test_tear(t);
	t->bounce_on_probe = true;
	relink_test_reconnect(test, t);

	KUNIT_EXPECT_EQ(test, atomic_read(&t->probes), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), 1);
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_GOOD);
}

/* Before drm_mode_config_reset() the connector has no state to recover. */
static void relink_no_state_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);
	struct drm_connector_state *state = t->con.base.state;

	t->con.base.state = NULL;
	dcp_mode_invalidate(&t->dcp.mode_state);
	relink_test_hotplug(t);
	t->con.base.state = state;

	KUNIT_EXPECT_EQ(test, atomic_read(&t->probes), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), 1);
}

static int relink_test_userspace_off(void *data)
{
	struct relink_test *t = data;

	t->userspace_ret = relink_test_commit(t, false, false);
	complete(&t->userspace_done);
	return 0;
}

/*
 * Userspace switches the CRTC off while the mode set that fails to bring
 * the link up is in flight.  Whichever goes first, the display is not left
 * lost with its CRTC off: it comes back as a new connection.
 */
static void relink_userspace_off_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);
	struct task_struct *task;

	relink_test_tear(t);
	t->trains = false;
	t->hold_modeset = true;
	reinit_completion(&t->userspace_done);

	/* The reconnect, up to the mode set of the end of its hold. */
	scoped_guard(mutex, &t->dcp.hpd_mutex) {
		WRITE_ONCE(t->dcp.typec_cable_connected, true);
		t->dcp.typec_generation++;
		dcp_firmware_metadata_connection(&t->dcp, t->dcp.typec_generation, true);
		dcp_announce_end(&t->dcp.announce);
	}
	WRITE_ONCE(t->con.connected, true);
	relink_test_hotplug(t);
	scoped_guard(mutex, &t->dcp.hpd_mutex) {
		t->dcp.announce.timed = true;
		t->dcp.announce.deadline = jiffies - 1;
	}
	mod_delayed_work(system_freezable_wq, &t->dcp.announce_wq, 0);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&t->modeset_entered, 5 * HZ), 0);

	task = kthread_run(relink_test_userspace_off, t, "relink-userspace");
	if (IS_ERR(task))
		complete(&t->userspace_done);
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	msleep(50);
	t->hold_modeset = false;
	complete_all(&t->modeset_release);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&t->userspace_done, 5 * HZ), 0);
	KUNIT_ASSERT_EQ(test, t->userspace_ret, 0);
	relink_test_settle(test, t);

	KUNIT_EXPECT_FALSE(test, t->crtc->state->active);
	relink_test_expire(test, t);
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
}

/*
 * A link that never comes up: lost, back once after userspace switches the
 * CRTC off, then lost for good when the mode set userspace makes on it
 * fails too, until the next cable.
 */
static void relink_retry_once_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);
	int modesets;

	relink_test_tear(t);
	t->trains = false;
	relink_test_reconnect(test, t);
	KUNIT_ASSERT_FALSE(test, relink_test_connected(t));

	/* Userspace switches the CRTC off: the display comes back once. */
	KUNIT_ASSERT_EQ(test, relink_test_commit(t, false, false), 0);
	relink_test_settle(test, t);
	relink_test_expire(test, t);
	KUNIT_ASSERT_TRUE(test, relink_test_connected(t));

	/* Its mode set fails: lost again, without a mode set of the kernel's own. */
	modesets = atomic_read(&t->modesets);
	KUNIT_ASSERT_EQ(test, relink_test_commit(t, true, false), 0);
	relink_test_settle(test, t);
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_BAD);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->modesets), modesets + 2);

	/* Switched off again, it stays gone on this cable. */
	KUNIT_ASSERT_EQ(test, relink_test_commit(t, false, false), 0);
	relink_test_settle(test, t);
	KUNIT_EXPECT_FALSE(test, t->dcp.announce.holding);
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));

	/* The next cable is announced again. */
	relink_test_tear(t);
	t->trains = true;
	relink_test_reconnect(test, t);
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
}

/*
 * Mode sets go through the real dcp_crtc_atomic_modeset() and the H17P mode
 * set path, with the test answering for the firmware.
 */
static void relink_test_real(struct relink_test *t)
{
	t->real_modeset = true;
	t->dcp.external_running = true;
	t->dcp.fw_compat = DCP_FIRMWARE_H17P;
	t->catalog.mode = *t->mode;
	t->dcp.modes = &t->catalog;
	t->dcp.nr_modes = 1;
	/* The catalog the firmware published for this connection. */
	t->dcp.modes_admitted = true;
	t->dcp.test_push = relink_test_push;
}

/* The bounce, through the real mode set: the link comes back. */
static void relink_real_modeset_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	relink_test_real(t);
	relink_test_tear(t);
	relink_test_reconnect(test, t);
	KUNIT_EXPECT_EQ(test, t->modeset_ret, 0);
	KUNIT_EXPECT_TRUE(test, relink_test_link_up(t));
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_GOOD);
}

/*
 * The pipe was stopped while its loss was still buffered by the paused
 * hotplug callback, and the next activation fails before its mode set
 * begins: the old mode must not pass for a link.
 */
static void relink_early_epipe_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	t->dcp.external_running = true;
	t->dcp.external_cable = true;
	t->dcp.external_power_attempted = true;
	t->dcp.fw_compat = DCP_FIRMWARE_H17P;
	scoped_guard(mutex, &t->dcp.iomfb.lock) {
		t->dcp.iomfb.presentation_paused = true;
		KUNIT_EXPECT_TRUE(test, dcp_external_hotplug(&t->dcp, false));
	}
	KUNIT_ASSERT_TRUE(test, relink_test_link_up(t));
	KUNIT_ASSERT_TRUE(test, t->dcp.external_hotplug_pending);
	t->dcp.external_running = false;
	t->dcp.external_stopped = true;
	t->real_modeset = true;
	t->force_modeset = true;
	KUNIT_ASSERT_EQ(test, relink_test_commit(t, true, false), 0);
	KUNIT_ASSERT_EQ(test, t->modeset_ret, -EPIPE);
	relink_test_settle(test, t);
	KUNIT_EXPECT_FALSE(test, relink_test_link_up(t));
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_BAD);
}

/* A presentation paused on purpose is not a failed link. */
static void relink_paused_kept_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	t->dcp.external_running = true;
	t->dcp.fw_compat = DCP_FIRMWARE_H17P;
	scoped_guard(mutex, &t->dcp.iomfb.lock)
		t->dcp.iomfb.presentation_paused = true;
	t->real_modeset = true;
	t->force_modeset = true;
	KUNIT_ASSERT_EQ(test, relink_test_commit(t, true, false), 0);
	KUNIT_EXPECT_EQ(test, t->modeset_ret, -EPIPE);
	relink_test_settle(test, t);
	KUNIT_EXPECT_TRUE(test, relink_test_link_up(t));
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
	scoped_guard(mutex, &t->dcp.iomfb.lock)
		t->dcp.iomfb.presentation_paused = false;
}

/*
 * Once a lost connection was reported, unchanged facts send no more events:
 * each would restart a compositor's wait for the output to stay gone.
 */
static void relink_lost_quiet_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);
	int events, i;

	relink_test_tear(t);
	t->trains = false;
	relink_test_reconnect(test, t);
	KUNIT_ASSERT_FALSE(test, relink_test_connected(t));
	relink_test_settle(test, t);
	events = atomic_read(&t->events);
	for (i = 0; i < 5; i++)
		relink_test_hotplug(t);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->events), events);
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
}

/*
 * The link faults while the recovery mode set waits for the firmware, which
 * then answers: the mode set did not bring the link up.
 */
static void relink_fault_during_modeset_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	relink_test_real(t);
	t->fault_in_call = true;
	relink_test_tear(t);
	relink_test_reconnect(test, t);
	KUNIT_EXPECT_FALSE(test, relink_test_link_up(t));
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_BAD);
}

/*
 * The link faults after a completed mode set, with no hotplug: the mode is
 * gone at once, and the worker sets it again.
 */
static void relink_fault_after_modeset_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);
	int modesets;

	relink_test_real(t);
	relink_test_tear(t);
	relink_test_reconnect(test, t);
	KUNIT_ASSERT_EQ(test, t->modeset_ret, 0);
	KUNIT_ASSERT_TRUE(test, relink_test_link_up(t));
	/* See the fault's effect before the worker it queues can run. */
	disable_work_sync(&t->con.hotplug_wq);
	modesets = atomic_read(&t->modesets);
	dcp_external_link_fault(&t->dcp);
	KUNIT_EXPECT_FALSE(test, relink_test_link_up(t));
	KUNIT_EXPECT_TRUE(test, t->dcp.mode_state.invalidated);
	enable_work(&t->con.hotplug_wq);
	relink_test_hotplug(t);
	relink_test_settle(test, t);
	KUNIT_EXPECT_GT(test, atomic_read(&t->modesets), modesets);
	KUNIT_EXPECT_TRUE(test, relink_test_link_up(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_GOOD);
}

/* A fault with no mode set queues nothing: there is nothing to recover. */
static void relink_fault_inactive_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	KUNIT_ASSERT_EQ(test, relink_test_commit(t, false, false), 0);
	relink_test_settle(test, t);
	disable_work_sync(&t->con.hotplug_wq);
	dcp_external_link_fault(&t->dcp);
	KUNIT_EXPECT_FALSE(test, work_pending(&t->con.hotplug_wq));
	enable_work(&t->con.hotplug_wq);
}

/* A mode set the firmware gives no reply to has failed. */
static void relink_no_reply_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	relink_test_real(t);
	t->no_reply = true;
	relink_test_tear(t);
	relink_test_reconnect(test, t);
	KUNIT_EXPECT_LT(test, t->modeset_ret, 0);
	KUNIT_EXPECT_FALSE(test, relink_test_link_up(t));
	KUNIT_EXPECT_FALSE(test, relink_test_connected(t));
	KUNIT_EXPECT_EQ(test, t->con.base.state->link_status, DRM_LINK_STATUS_BAD);
}

/*
 * A nonzero result alone is not a failure: its meaning is not established,
 * and it is also seen for a mode set that repeats the current mode.
 */
static void relink_nonzero_reply_test(struct kunit *test)
{
	struct relink_test *t = relink_test_init(test);

	relink_test_real(t);
	t->reply = 1;
	relink_test_tear(t);
	relink_test_reconnect(test, t);
	KUNIT_EXPECT_EQ(test, t->modeset_ret, 0);
	KUNIT_EXPECT_TRUE(test, relink_test_link_up(t));
	KUNIT_EXPECT_TRUE(test, relink_test_connected(t));
}

static struct kunit_case relink_cases[] = {
	KUNIT_CASE(relink_bounce_test),
	KUNIT_CASE(relink_mode_gone_test),
	KUNIT_CASE(relink_link_fails_test),
	KUNIT_CASE(relink_next_cable_test),
	KUNIT_CASE(relink_inactive_crtc_test),
	KUNIT_CASE(relink_other_profile_test),
	KUNIT_CASE(relink_newer_cable_test),
	KUNIT_CASE(relink_no_state_test),
	KUNIT_CASE(relink_userspace_off_test),
	KUNIT_CASE(relink_retry_once_test),
	KUNIT_CASE(relink_real_modeset_test),
	KUNIT_CASE(relink_early_epipe_test),
	KUNIT_CASE(relink_paused_kept_test),
	KUNIT_CASE(relink_lost_quiet_test),
	KUNIT_CASE(relink_fault_during_modeset_test),
	KUNIT_CASE(relink_fault_after_modeset_test),
	KUNIT_CASE(relink_fault_inactive_test),
	KUNIT_CASE(relink_no_reply_test),
	KUNIT_CASE(relink_nonzero_reply_test),
	{}
};

static struct kunit_suite relink_suite = {
	.name = "apple-dcp-external-relink",
	.test_cases = relink_cases,
};

kunit_test_suite(relink_suite);

// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Software camera indicator: a kernel-owned swap layer on the internal panel.
 *
 * The camera driver asks for it before the first frame and drops it after the
 * last. Userspace has no plane, property or ioctl for it. Best effort: the
 * camera does not wait on the result. A software indicator, not the secure
 * one. See dcp-indicator.h.
 */
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/display.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_lock.h>

#include "dcp.h"
#include "dcp-internal.h"
#include "dcp-indicator.h"
#include "plane.h"

/* DCP rejects blended surfaces smaller than 32x32 (see plane.c). */
#define IND_SIDE		48
#define IND_MARGIN		12
#define IND_BORDER		3
#define IND_FILL		0xffff9500
#define IND_EDGE		0xff000000
#define IND_PRESENT_TIMEOUT	msecs_to_jiffies(1000)

int dcp_indicator_probe(struct apple_dcp *dcp)
{
	struct dcp_camera_indicator *ci = &dcp->camera_ind;
	u32 surf;

	spin_lock_init(&ci->lock);
	mutex_init(&ci->ctl);
	init_completion(&ci->presented);

	if (of_property_read_u32(dcp->dev->of_node, "apple,camera-indicator-surface", &surf))
		return 0;
	if (surf >= SWAP_SURFACES || test_bit(surf, dcp->iomfb_surfaces))
		return dev_err_probe(dcp->dev, -EINVAL,
				     "camera indicator surface %u is not free\n", surf);
	ci->surf = surf;
	ci->has_surf = true;
	return 0;
}

static void dcp_indicator_release(struct drm_device *drm, void *data)
{
	struct dcp_camera_indicator *ci = data;

	guard(mutex)(&ci->ctl);
	if (ci->fb)
		drm_framebuffer_put(ci->fb);
	ci->fb = NULL;
	ci->plane = NULL;
	ci->has_surf = false;
}

static const struct drm_framebuffer_funcs dcp_indicator_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

static void dcp_indicator_paint(u32 *px)
{
	unsigned int x, y;

	for (y = 0; y < IND_SIDE; y++)
		for (x = 0; x < IND_SIDE; x++)
			px[y * IND_SIDE + x] =
				(x < IND_BORDER || y < IND_BORDER ||
				 x >= IND_SIDE - IND_BORDER || y >= IND_SIDE - IND_BORDER) ?
				IND_EDGE : IND_FILL;
}

/*
 * Reserve the plane for the kernel and build the square it shows. A failure
 * leaves the plane reserved and without a framebuffer, so userspace still
 * cannot use it and the camera cannot start.
 */
int dcp_indicator_attach_plane(struct apple_dcp *dcp, struct drm_device *drm,
			       struct drm_plane *plane)
{
	struct dcp_camera_indicator *ci = &dcp->camera_ind;
	struct drm_mode_fb_cmd2 cmd = {
		.width = IND_SIDE,
		.height = IND_SIDE,
		.pixel_format = DRM_FORMAT_ARGB8888,
		.pitches[0] = IND_SIDE * 4,
	};
	struct drm_gem_dma_object *obj;
	struct drm_framebuffer *fb;
	int ret;

	to_apple_plane(plane)->indicator = true;
	ci->plane = plane;

	obj = drm_gem_dma_create(drm, PAGE_ALIGN(IND_SIDE * IND_SIDE * 4));
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	dcp_indicator_paint(obj->vaddr);

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb) {
		drm_gem_object_put(&obj->base);
		return -ENOMEM;
	}
	drm_helper_mode_fill_fb_struct(drm, fb,
				       drm_get_format_info(drm, cmd.pixel_format, cmd.modifier[0]),
				       &cmd);
	fb->obj[0] = &obj->base;
	ret = drm_framebuffer_init(drm, fb, &dcp_indicator_fb_funcs);
	if (ret) {
		drm_gem_object_put(&obj->base);
		kfree(fb);
		return ret;
	}
	/* Kernel-private: userspace must not find it by id. */
	drm_framebuffer_unregister_private(fb);

	ci->fb = fb;
	to_apple_plane(plane)->indicator_fb = fb;
	return drmm_add_action_or_reset(drm, dcp_indicator_release, ci);
}

/* The atomic state the kernel itself commits: the square, or nothing. */
static int dcp_indicator_commit(struct apple_dcp *dcp, bool on)
{
	struct dcp_camera_indicator *ci = &dcp->camera_ind;
	struct drm_crtc *crtc;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_plane_state *ps;
	struct drm_crtc_state *cs;
	int ret;

	if (!dcp->crtc || !ci->plane || !ci->fb)
		return -ENODEV;
	crtc = &dcp->crtc->base;

	state = drm_atomic_commit_alloc(crtc->dev);
	if (!state)
		return -ENOMEM;
	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
retry:
	cs = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(cs)) {
		ret = PTR_ERR(cs);
		goto out;
	}
	ps = drm_atomic_get_plane_state(state, ci->plane);
	if (IS_ERR(ps)) {
		ret = PTR_ERR(ps);
		goto out;
	}
	if (on) {
		if (!cs->active) {
			ret = -ENODEV;
			goto out;
		}
		ret = drm_atomic_set_crtc_for_plane(ps, crtc);
		if (ret)
			goto out;
		drm_atomic_set_fb_for_plane(ps, ci->fb);
		ps->src_x = 0;
		ps->src_y = 0;
		ps->src_w = IND_SIDE << 16;
		ps->src_h = IND_SIDE << 16;
		ps->crtc_x = cs->mode.hdisplay - IND_SIDE - IND_MARGIN;
		ps->crtc_y = IND_MARGIN;
		ps->crtc_w = IND_SIDE;
		ps->crtc_h = IND_SIDE;
	} else {
		ret = drm_atomic_set_crtc_for_plane(ps, NULL);
		if (ret)
			goto out;
		drm_atomic_set_fb_for_plane(ps, NULL);
	}
	WRITE_ONCE(to_apple_plane(ci->plane)->indicator_commit, state);
	ret = drm_atomic_nonblocking_commit(state);
	WRITE_ONCE(to_apple_plane(ci->plane)->indicator_commit, NULL);
out:
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}
	drm_atomic_commit_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
}

static void dcp_indicator_end(struct apple_dcp *dcp)
{
	struct dcp_camera_indicator *ci = &dcp->camera_ind;
	unsigned long flags;

	spin_lock_irqsave(&ci->lock, flags);
	dcp_ind_end(&ci->state);
	spin_unlock_irqrestore(&ci->lock, flags);
}

/*
 * on: returns 0 once a present carried the square to the panel, within 1 s;
 * otherwise an error and the indicator is left off. The caller streams either
 * way. off: always succeeds.
 */
int apple_dcp_camera_indicator_set(struct device *dev, bool on)
{
	struct apple_dcp *dcp;
	struct dcp_camera_indicator *ci;
	unsigned long flags;
	int ret;

	/* The device lock keeps the DCP bound, and its state alive, for the call. */
	guard(device)(dev);
	dcp = dev_get_drvdata(dev);
	if (!dcp || !dev->driver)
		return on ? -ENODEV : 0;
	ci = &dcp->camera_ind;

	guard(mutex)(&ci->ctl);
	if (!ci->has_surf || !ci->fb)
		return on ? -ENODEV : 0;
	if (!on) {
		dcp_indicator_end(dcp);
		ret = dcp_indicator_commit(dcp, false);
		if (ret)
			dev_warn(dcp->dev, "camera indicator off commit failed: %d\n", ret);
		return 0;
	}

	spin_lock_irqsave(&ci->lock, flags);
	ret = dcp_ind_begin(&ci->state);
	reinit_completion(&ci->presented);
	spin_unlock_irqrestore(&ci->lock, flags);
	if (ret)
		return ret;

	ret = dcp_indicator_commit(dcp, true);
	if (!ret && !wait_for_completion_timeout(&ci->presented, IND_PRESENT_TIMEOUT))
		ret = -ETIMEDOUT;
	if (ret) {
		dcp_indicator_end(dcp);
		dcp_indicator_commit(dcp, false);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dcp_camera_indicator_set);

/* A completed present: the first one that carried the square confirms it. */
void dcp_indicator_swap_done(struct apple_dcp *dcp, struct drm_framebuffer *slot_fb)
{
	struct dcp_camera_indicator *ci = &dcp->camera_ind;
	unsigned long flags;

	if (!ci->has_surf)
		return;
	spin_lock_irqsave(&ci->lock, flags);
	dcp_ind_present_done(&ci->state, slot_fb && slot_fb == ci->fb);
	if (ci->state.state == DCP_IND_LIT)
		complete(&ci->presented);
	spin_unlock_irqrestore(&ci->lock, flags);
}

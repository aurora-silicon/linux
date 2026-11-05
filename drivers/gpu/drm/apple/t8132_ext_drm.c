// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J713: each T8132 external DCP the boot loader handed over as a DisplayPort
 * output of the DRM device: a primary plane, a CRTC, an encoder and a
 * connector, whose state and modes are the DCP's sink reports. The external
 * DCPs are adopted outside the DRM device: these objects read as
 * disconnected until a DCP has a session and a sink.
 *
 * A modeset powers the display and sets the mode. Each frame is copied into
 * a SID0 surface of the DCP's display DART and swapped in, its flip
 * completed with the swap (D590).
 */
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <drm/drm_atomic.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "iomfb_m4.h"
#include "t8132_ext_drm.h"

/* Copied frames of imported framebuffers are read through their dma-buf. */
MODULE_IMPORT_NS("DMA_BUF");

/* The firmware's IDs of the CRTC's mode, from its atomic check. */
struct t8132_ext_crtc_state {
	struct drm_crtc_state base;
	struct iomfb_m4_ext_ids ids;
};

static struct t8132_ext_crtc_state *to_t8132_ext_crtc_state(struct drm_crtc_state *state)
{
	return container_of(state, struct t8132_ext_crtc_state, base);
}

#define T8132_EXT_MAX 2

struct t8132_ext_out {
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct platform_device *pdev;
	struct work_struct hotplug_work;
	/* The held flip event, sent with its swap's D590 or later (drm->event_lock) */
	struct drm_pending_vblank_event *event;
	struct delayed_work flip_work;
	bool first_frame;
	/* A frame was not shown, so the next one goes whole */
	bool frame_lost;
};

static DEFINE_SPINLOCK(t8132_ext_lock);
/* Registered outputs, for t8132_ext_drm_hotplug() (t8132_ext_lock) */
static struct t8132_ext_out *t8132_ext_live[T8132_EXT_MAX];
static const u32 t8132_ext_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const u64 t8132_ext_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

/* The DCP's driver data once bound; an adopted external DCP is never unbound. */
static struct apple_dcp *t8132_ext_owner(struct t8132_ext_out *out)
{
	return device_is_bound(&out->pdev->dev) ? platform_get_drvdata(out->pdev) : NULL;
}

static int t8132_ext_plane_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *cs = NULL;

	int ret;

	if (ps->crtc)
		cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	ret = drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, false);
	/* Frames go out from the framebuffer's origin. */
	if (!ret && ps->visible && (ps->src.x1 || ps->src.y1))
		ret = -EINVAL;
	return ret;
}

/*
 * Nothing to program here: the CRTC's flush presents the frame.
 * drm_atomic_helper_commit_planes() calls this for every plane it updates
 * or disables (there is no atomic_disable).
 */
static void t8132_ext_plane_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
}

static const struct drm_plane_helper_funcs t8132_ext_plane_helpers = {
	.atomic_check = t8132_ext_plane_check,
	.atomic_update = t8132_ext_plane_update,
};

static const struct drm_plane_funcs t8132_ext_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

/* Send the held flip event now, if any. */
static void t8132_ext_send_event(struct t8132_ext_out *out)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&out->crtc.dev->event_lock, flags);
	event = out->event;
	out->event = NULL;
	if (event)
		drm_crtc_send_vblank_event(&out->crtc, event);
	spin_unlock_irqrestore(&out->crtc.dev->event_lock, flags);
}

static void t8132_ext_flip_work(struct work_struct *work)
{
	t8132_ext_send_event(container_of(to_delayed_work(work), struct t8132_ext_out,
					  flip_work));
}

static void t8132_ext_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);
	const struct drm_display_mode *mode = &crtc->state->mode;

	struct apple_dcp *owner = t8132_ext_owner(out);
	int ret;

	out->first_frame = false;
	out->frame_lost = true;
	ret = owner ? iomfb_m4_ext_enable(owner, &to_t8132_ext_crtc_state(crtc->state)->ids,
					  mode->hdisplay, mode->vdisplay) : -ENODEV;
	if (ret)
		dev_warn(&out->pdev->dev, "%s on: %ux%u@%d Hz failed: %d\n",
			 crtc->name, mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode), ret);
	else
		dev_info(&out->pdev->dev, "%s on: %ux%u@%d Hz, %d kHz\n",
			 crtc->name, mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
			 mode->clock);
}

static void t8132_ext_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);

	struct apple_dcp *owner = t8132_ext_owner(out);
	int ret = 0;

	cancel_delayed_work_sync(&out->flip_work);
	t8132_ext_send_event(out);
	if (owner)
		ret = iomfb_m4_ext_blank(owner);
	dev_info(&out->pdev->dev, "%s off: %d\n", crtc->name, ret);
}

/*
 * Copy the primary plane's new frame into the DCP and swap it in; 1 if the
 * commit has no new frame (no plane state), so nothing was tried. The
 * framebuffer is read through its kernel mapping (dma-buf vmap for an
 * imported one) inside CPU access.
 */
static int t8132_ext_present(struct t8132_ext_out *out, struct drm_atomic_commit *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state, &out->plane);
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, &out->plane);
	struct apple_dcp *owner = t8132_ext_owner(out);
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	struct iosys_map map, data;
	struct drm_rect damage;
	struct dma_buf *buf;
	int ret;

	if (!new || !old)
		return 1;
	fb = new->fb;
	if (!owner || !fb || !new->visible)
		return -ENOLINK;
	if (out->frame_lost)
		damage = DRM_RECT_INIT(0, 0, fb->width, fb->height);
	else if (!drm_atomic_helper_damage_merged(old, new, &damage))
		damage = DRM_RECT_INIT(0, 0, 0, 0);
	obj = drm_gem_fb_get_obj(fb, 0);
	if (!obj)
		return -EINVAL;
	if (!drm_gem_is_imported(obj)) {
		struct drm_gem_dma_object *dma = to_drm_gem_dma_obj(obj);

		if (!dma->vaddr)
			return -EINVAL;
		iosys_map_set_vaddr(&data, dma->vaddr + fb->offsets[0]);
		return iomfb_m4_ext_present(owner, &data, fb->pitches[0], &damage);
	}
	buf = obj->import_attach->dmabuf;
	ret = dma_buf_vmap_unlocked(buf, &map);
	if (ret)
		return ret;
	ret = dma_buf_begin_cpu_access(buf, DMA_FROM_DEVICE);
	if (!ret) {
		data = map;
		iosys_map_incr(&data, fb->offsets[0]);
		ret = iomfb_m4_ext_present(owner, &data, fb->pitches[0], &damage);
		dma_buf_end_cpu_access(buf, DMA_FROM_DEVICE);
	}
	dma_buf_vunmap_unlocked(buf, &map);
	return ret;
}

/* The commit's flip event as the one held (an older one is sent first). */
static struct drm_pending_vblank_event *t8132_ext_hold_event(struct t8132_ext_out *out,
							     struct drm_crtc_state *cs)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&out->crtc.dev->event_lock, flags);
	event = cs->event;
	cs->event = NULL;
	if (event) {
		if (out->event)
			drm_crtc_send_vblank_event(&out->crtc, out->event);
		out->event = event;
	}
	spin_unlock_irqrestore(&out->crtc.dev->event_lock, flags);
	return event;
}

static void t8132_ext_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);
	struct drm_framebuffer *fb = crtc->primary->state->fb;
	struct drm_pending_vblank_event *event;
	int ret;

	ret = t8132_ext_present(out, state);
	/* Its damage is lost with it: whole frames until one is shown. */
	if (ret <= 0)
		out->frame_lost = !!ret;
	if (fb && !out->first_frame && ret <= 0) {
		out->first_frame = true;
		dev_dbg(&out->pdev->dev, "%s: first frame %ux%u %p4cc, modifier %#llx: %d\n",
			crtc->name, fb->width, fb->height, &fb->format->format, fb->modifier, ret);
	} else if (ret < 0 && ret != -EAGAIN) {
		dev_info_ratelimited(&out->pdev->dev, "%s: frame not shown: %d\n",
				     crtc->name, ret);
	}
	/* A shown frame's swap has completed (D590): its flip now, else a frame later. */
	event = t8132_ext_hold_event(out, cs);
	if (event)
		mod_delayed_work(system_wq, &out->flip_work, ret >= 0 ? 0 : msecs_to_jiffies(16));
}

/* A modeset only to a mode the sink offered, within the link. */
static int t8132_ext_crtc_check(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);
	struct apple_dcp *owner;
	int ret = drm_crtc_helper_atomic_check(crtc, state);

	if (ret || !cs->enable || !drm_atomic_crtc_needs_modeset(cs))
		return ret;
	owner = t8132_ext_owner(out);
	ret = owner ? iomfb_m4_ext_mode_ids(owner, &cs->mode,
					    &to_t8132_ext_crtc_state(cs)->ids) : -ENODEV;
	if (ret)
		drm_dbg_kms(crtc->dev, "%s: mode " DRM_MODE_FMT " refused: %d\n", crtc->name,
			    DRM_MODE_ARG(&cs->mode), ret);
	return ret ? -EINVAL : 0;
}

static const struct drm_crtc_helper_funcs t8132_ext_crtc_helpers = {
	.atomic_check = t8132_ext_crtc_check,
	.atomic_enable = t8132_ext_crtc_enable,
	.atomic_disable = t8132_ext_crtc_disable,
	.atomic_flush = t8132_ext_crtc_flush,
};

/* The last object freed: drm_mode_config_cleanup() destroys CRTCs after the rest. */
static void t8132_ext_crtc_destroy(struct drm_crtc *crtc)
{
	struct t8132_ext_out *out = container_of(crtc, struct t8132_ext_out, crtc);

	cancel_delayed_work_sync(&out->flip_work);
	cancel_work_sync(&out->hotplug_work);
	drm_crtc_cleanup(crtc);
	put_device(&out->pdev->dev);
	kfree(out);
}

static void t8132_ext_crtc_reset(struct drm_crtc *crtc)
{
	struct t8132_ext_crtc_state *state = kzalloc_obj(*state);

	if (crtc->state) {
		__drm_atomic_helper_crtc_destroy_state(crtc->state);
		kfree(to_t8132_ext_crtc_state(crtc->state));
		crtc->state = NULL;
	}
	if (state)
		__drm_atomic_helper_crtc_reset(crtc, &state->base);
}

static struct drm_crtc_state *t8132_ext_crtc_duplicate(struct drm_crtc *crtc)
{
	struct t8132_ext_crtc_state *state;

	if (WARN_ON(!crtc->state))
		return NULL;
	state = kzalloc_obj(*state);
	if (!state)
		return NULL;
	__drm_atomic_helper_crtc_duplicate_state(crtc, &state->base);
	state->ids = to_t8132_ext_crtc_state(crtc->state)->ids;
	return &state->base;
}

static void t8132_ext_crtc_destroy_state(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(to_t8132_ext_crtc_state(state));
}

static const struct drm_crtc_funcs t8132_ext_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = t8132_ext_crtc_destroy,
	.reset = t8132_ext_crtc_reset,
	.atomic_duplicate_state = t8132_ext_crtc_duplicate,
	.atomic_destroy_state = t8132_ext_crtc_destroy_state,
};

static enum drm_connector_status t8132_ext_detect(struct drm_connector *connector,
						  bool force)
{
	struct t8132_ext_out *out = container_of(connector, struct t8132_ext_out, connector);
	struct apple_dcp *owner = t8132_ext_owner(out);

	return owner && iomfb_m4_ext_detect(owner) > 0 ? connector_status_connected :
							  connector_status_disconnected;
}

static int t8132_ext_get_modes(struct drm_connector *connector)
{
	struct t8132_ext_out *out = container_of(connector, struct t8132_ext_out, connector);
	struct apple_dcp *owner = t8132_ext_owner(out);

	return owner ? iomfb_m4_ext_get_modes(owner, connector) : 0;
}

static void t8132_ext_hotplug_work(struct work_struct *work)
{
	struct t8132_ext_out *out = container_of(work, struct t8132_ext_out, hotplug_work);

	drm_kms_helper_connector_hotplug_event(&out->connector);
}

/* Registered: hotplug reports from the DCP are passed on from now. */
static int t8132_ext_late_register(struct drm_connector *connector)
{
	struct t8132_ext_out *out = container_of(connector, struct t8132_ext_out, connector);
	unsigned long flags;
	int ret = -EBUSY;

	spin_lock_irqsave(&t8132_ext_lock, flags);
	for (unsigned int i = 0; i < T8132_EXT_MAX; i++) {
		if (!t8132_ext_live[i]) {
			t8132_ext_live[i] = out;
			ret = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&t8132_ext_lock, flags);
	return ret;
}

static void t8132_ext_early_unregister(struct drm_connector *connector)
{
	struct t8132_ext_out *out = container_of(connector, struct t8132_ext_out, connector);
	unsigned long flags;

	spin_lock_irqsave(&t8132_ext_lock, flags);
	for (unsigned int i = 0; i < T8132_EXT_MAX; i++)
		if (t8132_ext_live[i] == out)
			t8132_ext_live[i] = NULL;
	spin_unlock_irqrestore(&t8132_ext_lock, flags);
	cancel_work_sync(&out->hotplug_work);
}

static const struct drm_connector_funcs t8132_ext_connector_funcs = {
	.detect = t8132_ext_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.late_register = t8132_ext_late_register,
	.early_unregister = t8132_ext_early_unregister,
};

static const struct drm_connector_helper_funcs t8132_ext_connector_helpers = {
	.get_modes = t8132_ext_get_modes,
};

bool t8132_ext_crtc(const struct drm_crtc *crtc)
{
	return crtc && crtc->funcs == &t8132_ext_crtc_funcs;
}

bool t8132_ext_connector(const struct drm_connector *connector)
{
	return connector && connector->funcs == &t8132_ext_connector_funcs;
}

bool t8132_ext_plane(const struct drm_plane *plane)
{
	return plane && plane->funcs == &t8132_ext_plane_funcs;
}

void t8132_ext_drm_hotplug(struct device *dcpext)
{
	unsigned long flags;

	spin_lock_irqsave(&t8132_ext_lock, flags);
	for (unsigned int i = 0; i < T8132_EXT_MAX; i++)
		if (t8132_ext_live[i] && &t8132_ext_live[i]->pdev->dev == dcpext)
			schedule_work(&t8132_ext_live[i]->hotplug_work);
	spin_unlock_irqrestore(&t8132_ext_lock, flags);
}

/*
 * One output, all of it or nothing: a failure unwinds what was initialised,
 * so that the DRM device goes on without that output.
 */
static int t8132_ext_out_init(struct drm_device *drm, struct platform_device *pdev)
{
	struct t8132_ext_out *out = kzalloc(sizeof(*out), GFP_KERNEL);
	int ret;

	if (!out)
		return -ENOMEM;
	/* Dropped with @out, by the CRTC's destroy. */
	out->pdev = to_platform_device(get_device(&pdev->dev));
	INIT_WORK(&out->hotplug_work, t8132_ext_hotplug_work);
	INIT_DELAYED_WORK(&out->flip_work, t8132_ext_flip_work);
	/* The CRTC this plane may use is the next one initialised. */
	ret = drm_universal_plane_init(drm, &out->plane, BIT(drm->mode_config.num_crtc),
				       &t8132_ext_plane_funcs, t8132_ext_formats,
				       ARRAY_SIZE(t8132_ext_formats), t8132_ext_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		goto err_free;
	drm_plane_helper_add(&out->plane, &t8132_ext_plane_helpers);
	drm_plane_enable_fb_damage_clips(&out->plane);
	/* Every plane of the device has a zpos, or none may: the panel's do. */
	ret = drm_plane_create_zpos_immutable_property(&out->plane, 0);
	/* Frames go out opaque, ARGB8888 ones too. */
	if (!ret)
		ret = drm_plane_create_blend_mode_property(&out->plane,
							   BIT(DRM_MODE_BLEND_PIXEL_NONE));
	if (ret)
		goto err_plane;
	ret = drm_crtc_init_with_planes(drm, &out->crtc, &out->plane, NULL,
					&t8132_ext_crtc_funcs, "dcpext-%s", dev_name(&pdev->dev));
	if (ret)
		goto err_plane;
	drm_crtc_helper_add(&out->crtc, &t8132_ext_crtc_helpers);
	ret = drm_simple_encoder_init(drm, &out->encoder, DRM_MODE_ENCODER_TMDS);
	if (ret)
		goto err_crtc;
	out->encoder.possible_crtcs = drm_crtc_mask(&out->crtc);
	ret = drm_connector_init(drm, &out->connector, &t8132_ext_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret)
		goto err_encoder;
	drm_connector_helper_add(&out->connector, &t8132_ext_connector_helpers);
	out->connector.polled = DRM_CONNECTOR_POLL_HPD;
	ret = drm_connector_attach_encoder(&out->connector, &out->encoder);
	if (ret)
		goto err_connector;
	dev_dbg(&pdev->dev, "DRM output: CRTC %u, connector %s\n",
		drm_crtc_index(&out->crtc), out->connector.name);
	return 0;

	/* Nothing of a partly initialised output is left for the DRM device. */
err_connector:
	drm_connector_cleanup(&out->connector);
err_encoder:
	drm_encoder_cleanup(&out->encoder);
err_crtc:
	drm_crtc_cleanup(&out->crtc);
err_plane:
	drm_plane_cleanup(&out->plane);
err_free:
	put_device(&pdev->dev);
	kfree(out);
	return ret;
}

/*
 * Called once per DRM device, before drm_mode_config_reset(). Not an error
 * for the DRM device if anything fails here: the panel goes on without.
 */
int t8132_ext_drm_init(struct drm_device *drm)
{
	struct device_node *np;
	unsigned int count = 0;
	int err;
	int ret = 0;

	if (!of_machine_is_compatible("apple,j713"))
		return 0;
	for_each_compatible_node(np, NULL, "apple,t8132-dcpext") {
		struct platform_device *pdev;
		u32 ready;

		/* Only a DCP the boot loader handed over can ever be adopted. */
		if (!of_device_is_available(np) || count >= T8132_EXT_MAX ||
		    of_property_read_u32(np, "apple,t8132-dcpext-firmware-ready", &ready) ||
		    ready != 1)
			continue;
		pdev = of_find_device_by_node(np);
		if (!pdev)
			continue;
		err = t8132_ext_out_init(drm, pdev);
		/* The panel and the other output go on without this one. */
		if (err)
			dev_err(&pdev->dev, "no DRM output: %d\n", err);
		else
			count++;
		put_device(&pdev->dev);
		ret = ret ?: err;
	}
	if (count || ret)
		drm_info(drm, "%u T8132 external output(s): %d\n", count, ret);
	return ret;
}

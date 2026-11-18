// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KMS on the iBoot interface: one primary plane, CRTC, encoder and
 * connector. The firmware only takes width, height and refresh rate for a
 * mode and picks the timing itself, so the modes are built with CVT for
 * userspace.
 *
 * The pipe always scans out one buffer, the boot surface, which holds the
 * largest mode; framebuffers are shadow buffers copied into it, and a
 * modeset only changes its geometry, as aura does. Changing the surface
 * address between Linux buffers made later modesets crash the firmware
 * (IOMFB axi_rd_err). There is no vblank interrupt.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iosys-map.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "dcpib.h"

/*
 * Swap between the scanout buffers per frame. Off: frames are copied into
 * the buffer on screen, which tears during motion.
 */
static bool buffered = true;
module_param(buffered, bool, 0644);
MODULE_PARM_DESC(buffered, "Copy each frame into a scanout buffer that is not on screen (default: on)");

static const u32 dcpib_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static int dcpib_refresh(const struct dcpib_timing_mode *t)
{
	return (le32_to_cpu(t->fps) + 0x8000) >> 16;
}

/*
 * Whether @t works: the link at the PHY's highest rate must carry it (24 bpp
 * plus about 12% blanking against rate x lanes x 0.8 for 8b/10b), and the
 * pixel rate must stay within what the PHY's path scans out; a mode beyond
 * either only blanks the display.
 */
static bool dcpib_mode_fits(struct dcpib *dcp, const struct dcpib_timing_mode *t)
{
	/*
	 * The lanes the firmware may train, as GET_MAX_LANE_COUNT reports them:
	 * the USB-C route's. Not the last training's: a sink that replugs while
	 * the display is off is trained with a single lane.
	 */
	u32 lanes = (dcp->phy->max_lane_count ? dcp->phy->max_lane_count(dcp) : 4) ?: 2;
	u64 link = (u64)dcp->phy->max_link_rate * 270000000ULL * lanes * 8 / 10;
	u64 pixels = (u64)le32_to_cpu(t->width) * le32_to_cpu(t->height) *
		     dcpib_refresh(t);

	/* Hotplug may return modes larger than the buffers allocated at probe. */
	if ((u64)ALIGN(le32_to_cpu(t->width) * 4, 64) * le32_to_cpu(t->height) >
	    dcp->scan[0].size)
		return false;
	if (pixels < dcp->phy->min_pixel_rate ||
	    (dcp->phy->max_pixel_rate && pixels > dcp->phy->max_pixel_rate))
		return false;
	return pixels * 24 * 112 / 100 <= link;
}

/* @mode's refresh rate in 16.16, as drm_mode_vrefresh() computes it. */
static u32 dcpib_mode_fps(const struct drm_display_mode *mode)
{
	u64 num = mul_u32_u32(mode->clock, 1000) << 16;
	u64 den = mul_u32_u32(mode->htotal, mode->vtotal);

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (mode->vscan > 1)
		den *= mode->vscan;
	return den ? DIV64_U64_ROUND_CLOSEST(num, den) : 0;
}

/*
 * The firmware mode for @mode: same size, refresh rate within 1 Hz (EDID's
 * 59.94 against the firmware's 60.00), the nearest one by the exact rate.
 * The firmware picks the timing from the rate, and next to the sink's
 * 2560x1440@59.951 (241.5 MHz) it lists a 60.000 standard mode that it
 * drives with CVT at 312.25 MHz, which the J873g's USB-C EV2780 rejects.
 */
static const struct dcpib_timing_mode *
dcpib_find_mode(struct dcpib *dcp, const struct drm_display_mode *mode)
{
	const struct dcpib_timing_mode *best = NULL;
	u32 fps = dcpib_mode_fps(mode);

	for (unsigned int i = 0; i < dcp->nr_tmodes; i++) {
		const struct dcpib_timing_mode *t = &dcp->tmodes[i];

		if (!t->valid || le32_to_cpu(t->width) != mode->hdisplay ||
		    le32_to_cpu(t->height) != mode->vdisplay ||
		    abs_diff(le32_to_cpu(t->fps), fps) > 0x10000 ||
		    !dcpib_mode_fits(dcp, t))
			continue;
		if (!best || abs_diff(le32_to_cpu(t->fps), fps) <
			     abs_diff(le32_to_cpu(best->fps), fps))
			best = t;
	}
	return best;
}

/*
 * aura's display_choose_color_mode: valid, at most 32 bpp, the highest
 * depth, then the lowest colorimetry, encoding and EOTF. Deeper modes the
 * HDMI converter offers are refused by SET_MODE.
 */
static bool dcpib_color_better(const struct dcpib_color_mode *a,
			       const struct dcpib_color_mode *b)
{
	u32 abpp = le32_to_cpu(a->bpp), bbpp = le32_to_cpu(b->bpp);

	if (!!a->valid != !!b->valid)
		return a->valid;
	if ((abpp <= 32) != (bbpp <= 32))
		return abpp <= 32;
	if (abpp != bbpp)
		return abpp > bbpp;
	if (a->colorimetry != b->colorimetry)
		return le32_to_cpu(a->colorimetry) < le32_to_cpu(b->colorimetry);
	if (a->encoding != b->encoding)
		return le32_to_cpu(a->encoding) < le32_to_cpu(b->encoding);
	return le32_to_cpu(a->eotf) < le32_to_cpu(b->eotf);
}

static const struct dcpib_color_mode *dcpib_find_color(struct dcpib *dcp)
{
	const struct dcpib_color_mode *best = NULL;

	for (unsigned int i = 0; i < dcp->nr_cmodes; i++)
		if (!best || dcpib_color_better(&dcp->cmodes[i], best))
			best = &dcp->cmodes[i];
	return best && best->valid ? best : NULL;
}

/*
 * A conservative console mode from 50 to 60 Hz, up to 1920x1080 on
 * USB-C. Userspace can select the larger modes, including 2560x1440@60,
 * after the sink's EDID is available.
 */
static const struct dcpib_timing_mode *dcpib_preferred(struct dcpib *dcp)
{
	const struct dcpib_timing_mode *best = NULL;

	for (unsigned int i = 0; i < dcp->nr_tmodes; i++) {
		const struct dcpib_timing_mode *t = &dcp->tmodes[i];

		/*
		 * Byte 0 bit 6 marks standard modes a converter adds on its own
		 * (the J873g PS190 lists 3840x2160 for a 2560x1440 monitor), as
		 * aura's "max" mode skips them.
		 */
		if (!t->valid || dcpib_refresh(t) > 60 || dcpib_refresh(t) < 50 ||
		    (t->pad[0] & 0x40) ||
		    (!dcp->hdmi && (le32_to_cpu(t->width) > 1920 ||
				    le32_to_cpu(t->height) > 1080)) ||
		    !dcpib_mode_fits(dcp, t))
			continue;
		if (!best || (u64)le32_to_cpu(t->width) * le32_to_cpu(t->height) >
			     (u64)le32_to_cpu(best->width) * le32_to_cpu(best->height) ||
		    (t->width == best->width && t->height == best->height &&
		     le32_to_cpu(t->fps) > le32_to_cpu(best->fps)))
			best = t;
	}
	return best;
}

/*
 * The sink's EDID modes the firmware also has (mode_valid drops the rest),
 * the largest of them preferred: firmware modes the sink does not list,
 * such as 1400x1050 on the J873g EV2780, stay black.
 */
static int dcpib_edid_get_modes(struct drm_connector *connector)
{
	struct dcpib *dcp = to_dcpib(connector->dev);
	struct drm_display_mode *mode, *best = NULL;
	int count = drm_edid_connector_add_modes(connector);

	mutex_lock(&dcp->lock);
	list_for_each_entry(mode, &connector->probed_modes, head) {
		mode->type &= ~DRM_MODE_TYPE_PREFERRED;
		if (!dcpib_find_mode(dcp, mode))
			continue;
		if (!best || mode->hdisplay * mode->vdisplay >
			     best->hdisplay * best->vdisplay ||
		    (mode->hdisplay * mode->vdisplay ==
			     best->hdisplay * best->vdisplay &&
		     drm_mode_vrefresh(mode) > drm_mode_vrefresh(best)))
			best = mode;
	}
	if (best)
		best->type |= DRM_MODE_TYPE_PREFERRED;
	mutex_unlock(&dcp->lock);
	return count;
}

static int dcpib_connector_get_modes(struct drm_connector *connector)
{
	struct dcpib *dcp = to_dcpib(connector->dev);
	const struct dcpib_timing_mode *preferred;
	int count = 0;

	drm_edid_connector_update(connector, dcp->edid);
	if (dcp->edid)
		return dcpib_edid_get_modes(connector);

	/* Without an EDID, the firmware's list. */
	mutex_lock(&dcp->lock);
	preferred = dcpib_preferred(dcp);
	for (unsigned int i = 0; i < dcp->nr_tmodes; i++) {
		const struct dcpib_timing_mode *t = &dcp->tmodes[i];
		struct drm_display_mode *mode;

		if (!t->valid || !dcpib_mode_fits(dcp, t))
			continue;
		mode = drm_cvt_mode(connector->dev, le32_to_cpu(t->width),
				    le32_to_cpu(t->height), dcpib_refresh(t),
				    false, false, false);
		if (!mode)
			continue;
		if (t == preferred)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	}
	mutex_unlock(&dcp->lock);
	return count;
}

static enum drm_mode_status
dcpib_connector_mode_valid(struct drm_connector *connector,
			   const struct drm_display_mode *mode)
{
	struct dcpib *dcp = to_dcpib(connector->dev);

	return dcpib_find_mode(dcp, mode) ? MODE_OK : MODE_NOMODE;
}

static const struct drm_connector_helper_funcs dcpib_connector_helper = {
	.get_modes = dcpib_connector_get_modes,
	.mode_valid = dcpib_connector_mode_valid,
};

static int dcpib_set_mode_locked(struct dcpib *dcp,
				 const struct dcpib_timing_mode *t,
				 const struct dcpib_color_mode *c);

/* With dcp->lock held; queued work will observe the cleared pending slot. */
static void dcpib_cancel_swap_locked(struct dcpib *dcp)
{
	hrtimer_cancel(&dcp->swap_timer);
	dcp->pending = -1;
}

/* Read the sink's EDID through the phy, if it can, and keep it. */
void dcpib_update_edid(struct dcpib *dcp)
{
	const struct drm_edid *edid = NULL;
	u8 *buf;
	int len;

	if (!dcp->phy->read_edid)
		return;
	buf = kzalloc(4 * EDID_LENGTH, GFP_KERNEL);
	if (!buf)
		return;
	len = dcp->phy->read_edid(dcp, buf, 4 * EDID_LENGTH);
	if (len >= EDID_LENGTH) {
		edid = drm_edid_alloc(buf, len);
		if (edid && !drm_edid_valid(edid)) {
			drm_edid_free(edid);
			edid = NULL;
		}
	}
	if (!edid)
		dev_warn(dcp->dev, "no EDID: %d\n", len);
	else
		dev_info(dcp->dev, "EDID: %d bytes\n", len);
	drm_edid_free(dcp->edid);
	dcp->edid = edid;
	kfree(buf);
}

static enum drm_connector_status
dcpib_connector_detect(struct drm_connector *connector, bool force)
{
	struct dcpib *dcp = to_dcpib(connector->dev);
	bool flipped = false;
	enum drm_connector_status status;
	int hpd;

	mutex_lock(&dcp->lock);
	if (dcp->hpd_gpio)
		hpd = gpiod_get_value_cansleep(dcp->hpd_gpio);
	else if (dcp->phy->detect)
		hpd = dcp->phy->detect(dcp, &flipped);
	else {
		status = dcp->nr_tmodes ? connector_status_connected :
					connector_status_disconnected;
		goto out;
	}
	if (hpd < 0) {
		status = connector_status_unknown;
		goto out;
	}
	if (hpd != dcp->hpd) {
		dev_info(dcp->dev, "sink %s\n", hpd ? "plugged" : "unplugged");
		dcp->hpd = hpd;
		/*
		 * USB-C: the DPTX port's HPD follows the PD controller, and a
		 * new plug may come the other way round.
		 */
		if (dcp->pd) {
			if (!hpd || dcp->powered) {
				dcpib_cancel_swap_locked(dcp);
				if (dcpib_dptx_disconnect(dcp, 0))
					dev_err(dcp->dev, "DPTX release failed\n");
				if (!dcpib_ib_set_power(dcp, false)) {
					dcp->powered = false;
					dcp->mode_set = false;
					dcp->phy->deactivate(dcp);
				}
			}
			/*
			 * A plug while the display stayed on (unplug and plug
			 * between two polls): start the PHY cold again, the
			 * new plug may come the other way round.
			 */
			if (hpd)
				dcp->phy->deactivate(dcp);
			/* The sink may be on another port now. */
			if (hpd && dcp->phy->select)
				dcp->phy->select(dcp);
			if (hpd && dcp->phy->set_orientation)
				dcp->phy->set_orientation(dcp, flipped);
			/* The firmware talks AUX as soon as it sees HPD. */
			if (hpd && dcp->phy->activate(dcp))
				dev_err(dcp->dev, "phy activation failed\n");
			if (hpd && dcpib_dptx_connect(dcp, 0))
				dev_err(dcp->dev, "DPTX connect failed\n");
			if (hpd && dcpib_dptx_set_hpd(dcp, 0, true))
				dev_err(dcp->dev, "DPTX HPD update failed\n");
		}
		/* A new sink: the firmware reads its EDID, take the new lists. */
		if (hpd && (dcpib_wait_hpd(dcp) || dcpib_ib_get_modes(dcp))) {
			dev_err(dcp->dev, "no modes from the new sink\n");
			dcp->nr_tmodes = 0;
			dcp->nr_cmodes = 0;
			/* A sleeping EIZO may need a fresh DP alt mode entry. */
			if (dcp->pd && time_after(jiffies, dcp->pd_reset_time + 60 * HZ)) {
				dcp->pd_reset_time = jiffies;
				schedule_work(&dcp->pd_reset_work);
			}
		}
		/*
		 * The EDID once the firmware has read it, so the sink's DDC
		 * answers, and before SET_MODE: reading it borrows the AUX path
		 * while the firmware does no AUX.
		 */
		if (hpd && dcp->nr_tmodes)
			dcpib_update_edid(dcp);
		/*
		 * A fast reconnect can finish before userspace disables the CRTC.
		 * Restore its requested state even though unplug powered it off.
		 */
		if (hpd && dcp->enabled && dcp->cur_mode.valid) {
			const struct dcpib_timing_mode *t = NULL;
			const struct dcpib_color_mode *c = dcpib_find_color(dcp);
			int ret = 0;

			for (unsigned int i = 0; i < dcp->nr_tmodes; i++) {
				const struct dcpib_timing_mode *candidate = &dcp->tmodes[i];

				if (candidate->valid && candidate->width == dcp->cur_mode.width &&
				    candidate->height == dcp->cur_mode.height &&
				    candidate->fps == dcp->cur_mode.fps) {
					t = candidate;
					break;
				}
			}
			if (t && c && !dcp->powered) {
				ret = dcpib_ib_set_surface(dcp, dcp->scan_dva[dcp->front],
					le32_to_cpu(t->width), le32_to_cpu(t->height),
					ALIGN(le32_to_cpu(t->width) * 4, 64));
				if (!ret)
					ret = dcpib_ib_set_power(dcp, true);
				dcp->powered = !ret;
			}
			if (t && c && !ret)
				dcpib_set_mode_locked(dcp, t, c);
		}
	}
	status = hpd && dcp->nr_tmodes ? connector_status_connected :
					 connector_status_disconnected;
out:
	mutex_unlock(&dcp->lock);
	return status;
}

static const struct drm_connector_funcs dcpib_connector_funcs = {
	.detect = dcpib_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int dcpib_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_commit *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state = NULL;
	struct dcpib *dcp = to_dcpib(plane->dev);
	u32 width, height;
	int ret;

	if (new->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new->crtc);
	/* The surface fills the whole mode; no scaling or positioning. */
	ret = drm_atomic_helper_check_plane_state(new, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
	if (ret || !new->visible)
		return ret;
	/* The helper clips src/dst; raw src_w/src_h may exceed the CRTC. */
	if ((new->src.x1 | new->src.y1 | new->src.x2 | new->src.y2) & 0xffff)
		return -EINVAL;
	width = drm_rect_width(&new->src) >> 16;
	height = drm_rect_height(&new->src) >> 16;
	return (u64)ALIGN(width * 4, 64) * height <= dcp->scan[0].size ? 0 : -EINVAL;
}

static void dcpib_rect_grow(struct drm_rect *r, const struct drm_rect *add)
{
	if (!drm_rect_visible(r)) {
		*r = *add;
		return;
	}
	r->x1 = min(r->x1, add->x1);
	r->y1 = min(r->y1, add->y1);
	r->x2 = max(r->x2, add->x2);
	r->y2 = max(r->y2, add->y2);
}

/* The current mode's refresh period, from its exact 16.16 rate. */
static s64 dcpib_period_ns(struct dcpib *dcp)
{
	u32 fps = le32_to_cpu(dcp->cur_mode.fps);

	if (fps < (24 << 16))
		fps = 60 << 16;
	return div64_u64((u64)NSEC_PER_SEC << 16, fps);
}

/* Show scan[buf]. Called with dcp->lock held. */
static void dcpib_swap_locked(struct dcpib *dcp, unsigned int buf, u32 w,
			      u32 h, u32 pitch)
{
	int ret;

	if (!dcp->powered || !dcp->mode_set || !dcp->hpd ||
	    apple_rtkit_is_crashed(dcp->rtk))
		return;

	/* The pipe must see the whole frame before it is swapped in. */
	wmb();
	ret = dcpib_ib_set_surface(dcp, dcp->scan_dva[buf], w, h, pitch);
	if (ret) {
		/* Still on the old buffer, which misses this frame. */
		dev_err_ratelimited(dcp->dev, "swap failed: %d\n", ret);
		dcp->stale[dcp->front] = DRM_RECT_INIT(0, 0, INT_MAX / 2, INT_MAX / 2);
		return;
	}
	dcp->last_swap = ktime_get();
	dcp->retired[dcp->front] = dcp->last_swap;
	dcp->front = buf;
}

static void dcpib_swap_work(struct work_struct *work)
{
	struct dcpib *dcp = container_of(work, struct dcpib, swap_work);

	mutex_lock(&dcp->lock);
	if (dcp->pending >= 0) {
		dcpib_swap_locked(dcp, dcp->pending, dcp->pending_w,
				  dcp->pending_h, dcp->pending_pitch);
		dcp->pending = -1;
	}
	mutex_unlock(&dcp->lock);
}

static enum hrtimer_restart dcpib_swap_timer(struct hrtimer *t)
{
	struct dcpib *dcp = container_of(t, struct dcpib, swap_timer);

	queue_work(system_highpri_wq, &dcp->swap_work);
	return HRTIMER_NORESTART;
}

/*
 * Copy the frame into a scanout buffer that is not on screen and show it.
 * That buffer already holds an older frame: copy this frame's damage and
 * what changed since that buffer was last written.
 *
 * SET_SURFACE returns before the pipe takes the buffer at its next frame
 * start, and there is no vblank interrupt. Several swaps within one refresh
 * let an older frame stay on screen (a cursor that jumps back), so swaps are
 * spaced one refresh apart: a frame that comes sooner waits in a pending
 * buffer, which a newer frame overwrites, and a timer swaps it in. The
 * commit itself never waits, so a client's next frame is not held up.
 */
static void dcpib_plane_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_commit *state)
{
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new);
	struct dcpib *dcp = to_dcpib(plane->dev);
	struct drm_framebuffer *fb = new->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip, source_clip, src, full, damage = { 0, 0, 0, 0 };
	unsigned int pitch, back;
	struct iosys_map dst;
	s64 period, since;

	if (!fb || !new->visible)
		return;
	src = DRM_RECT_INIT(new->src.x1 >> 16, new->src.y1 >> 16,
			    drm_rect_width(&new->src) >> 16,
			    drm_rect_height(&new->src) >> 16);
	full = DRM_RECT_INIT(0, 0, drm_rect_width(&src), drm_rect_height(&src));
	pitch = ALIGN(drm_rect_width(&full) * 4, 64);
	drm_atomic_helper_damage_iter_init(&iter, old, new);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		if (!drm_rect_intersect(&clip, &src))
			continue;
		drm_rect_translate(&clip, -src.x1, -src.y1);
		dcpib_rect_grow(&damage, &clip);
	}
	if (!drm_rect_visible(&damage))
		return;

	mutex_lock(&dcp->lock);
	if (!dcp->powered || !dcp->mode_set || !dcp->hpd ||
	    apple_rtkit_is_crashed(dcp->rtk))
		goto out;
	period = dcpib_period_ns(dcp);
	if (!READ_ONCE(buffered)) {
		/* Into the buffer on screen (tears during motion). */
		back = dcp->front;
	} else if (dcp->pending >= 0) {
		back = dcp->pending;
	} else {
		/* The other buffer that left the screen first. */
		unsigned int a = (dcp->front + 1) % DCPIB_SCANOUT_BUFS;
		unsigned int b = (dcp->front + 2) % DCPIB_SCANOUT_BUFS;
		s64 wait;

		back = ktime_before(dcp->retired[a], dcp->retired[b]) ? a : b;
		/* Off screen for a full refresh: only after a mode change. */
		wait = period - ktime_to_ns(ktime_sub(ktime_get(), dcp->retired[back]));
		if (dcp->mode_set && wait > 0)
			usleep_range(div_s64(wait, NSEC_PER_USEC),
				     div_s64(wait, NSEC_PER_USEC) + 100);
	}
	clip = dcp->stale[back];
	dcpib_rect_grow(&clip, &damage);
	if (drm_rect_intersect(&clip, &full)) {
		iosys_map_set_vaddr(&dst, dcp->scan_wc[back]);
		iosys_map_incr(&dst, drm_fb_clip_offset(pitch, fb->format, &clip));
		source_clip = clip;
		drm_rect_translate(&source_clip, src.x1, src.y1);
		drm_fb_memcpy(&dst, &pitch, shadow->data, fb, &source_clip);
	}
	dcp->stale[back] = (struct drm_rect){ 0, 0, 0, 0 };
	for (unsigned int i = 0; i < DCPIB_SCANOUT_BUFS; i++)
		if (i != back)
			dcpib_rect_grow(&dcp->stale[i], &damage);
	if (back == dcp->front)
		goto out;

	since = ktime_to_ns(ktime_sub(ktime_get(), dcp->last_swap));
	if (dcp->pending < 0 && (!dcp->mode_set || since >= period)) {
		dcpib_swap_locked(dcp, back, drm_rect_width(&full), drm_rect_height(&full),
				  pitch);
		goto out;
	}
	dcp->pending_w = drm_rect_width(&full);
	dcp->pending_h = drm_rect_height(&full);
	dcp->pending_pitch = pitch;
	if (dcp->pending < 0) {
		dcp->pending = back;
		hrtimer_start(&dcp->swap_timer,
			      ktime_add_ns(dcp->last_swap, period),
			      HRTIMER_MODE_ABS);
	}
out:
	mutex_unlock(&dcp->lock);
}

static const struct drm_plane_helper_funcs dcpib_plane_helper = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = dcpib_plane_atomic_check,
	.atomic_update = dcpib_plane_atomic_update,
};

static const struct drm_plane_funcs dcpib_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static int dcpib_crtc_atomic_check(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct dcpib *dcp = to_dcpib(crtc->dev);

	/* No vblank interrupt: the helpers complete events after the flush. */
	new->no_vblank = true;
	if (new->enable && !dcpib_find_mode(dcp, &new->mode))
		return -EINVAL;
	return 0;
}

/* SET_MODE, then the scanout buffer with the new mode's geometry. */
static int dcpib_set_mode_locked(struct dcpib *dcp,
				 const struct dcpib_timing_mode *t,
				 const struct dcpib_color_mode *c)
{
	int ret;

	/*
	 * Right after power on the firmware still trains the link and fails
	 * SET_MODE with kIOReturnError; it takes the mode once it is up.
	 */
	for (int i = 0; i < 5; i++) {
		ret = dcpib_ib_set_mode(dcp, t, c);
		if (ret != -EIO)
			break;
		msleep(200);
	}
	if (ret) {
		dev_err(dcp->dev, "set_mode failed: %d\n", ret);
		/*
		 * USB-C: a sink that no longer answers AUX (seen after the
		 * monitor's deep sleep) comes back with a fresh DP alt mode
		 * entry; the plug that follows sets the mode again. At most
		 * once a minute, so a dead sink does not loop.
		 */
		if (dcp->pd && time_after(jiffies, dcp->pd_reset_time + 60 * HZ)) {
			dcp->cur_mode = *t;
			dcp->mode_failed = true;
			dcp->pd_reset_time = jiffies;
			schedule_work(&dcp->pd_reset_work);
		}
		return ret;
	}
	dcp->cur_mode = *t;
	dcp->mode_set = true;
	dcp->mode_failed = false;
	/* Both buffers hold the old geometry: the next frame is copied whole. */
	for (unsigned int i = 0; i < DCPIB_SCANOUT_BUFS; i++)
		dcp->stale[i] = DRM_RECT_INIT(0, 0, INT_MAX / 2, INT_MAX / 2);
	/* A pending frame has the old geometry. */
	dcpib_cancel_swap_locked(dcp);
	/* As aura swaps after a modeset. */
	ret = dcpib_ib_set_surface(dcp, dcp->scan_dva[dcp->front], le32_to_cpu(t->width),
				   le32_to_cpu(t->height),
				   ALIGN(le32_to_cpu(t->width) * 4, 64));
	if (ret)
		dev_err(dcp->dev, "set_surface failed: %d\n", ret);
	return ret;
}

/*
 * The boot mode again: a display powered on without SET_MODE counts as idle
 * to the firmware's AV stack, which powers the link off after 30 s.
 */
int dcpib_set_boot_mode(struct dcpib *dcp)
{
	const struct dcpib_timing_mode *best = NULL;
	const struct dcpib_color_mode *c;
	int ret = -ENOENT;

	mutex_lock(&dcp->lock);
	for (unsigned int i = 0; i < dcp->nr_tmodes; i++) {
		const struct dcpib_timing_mode *t = &dcp->tmodes[i];

		if (!t->valid || le32_to_cpu(t->width) != dcp->boot_w ||
		    le32_to_cpu(t->height) != dcp->boot_h ||
		    dcpib_refresh(t) > 60)
			continue;
		if (!best || le32_to_cpu(t->fps) > le32_to_cpu(best->fps))
			best = t;
	}
	c = dcpib_find_color(dcp);
	if (best && c)
		ret = dcpib_set_mode_locked(dcp, best, c);
	mutex_unlock(&dcp->lock);
	return ret;
}

/*
 * The DCPAV service starts the firmware's idle power off timer when its video
 * interface comes up: until a SET_MODE follows, it counts the powered-on
 * display as idle and takes the link down 30 s later. The probe's SET_MODE
 * comes before the interface starts, so one follows it here.
 */
void dcpib_reset_mode(struct dcpib *dcp)
{
	const struct dcpib_color_mode *c;

	mutex_lock(&dcp->lock);
	c = dcpib_find_color(dcp);
	if (c && dcp->powered && dcp->mode_set)
		dcpib_set_mode_locked(dcp, &dcp->cur_mode, c);
	mutex_unlock(&dcp->lock);
}

void dcpib_av_started(struct dcpib *dcp)
{
	/* Let the firmware finish opening the interface before the SET_MODE. */
	queue_delayed_work(system_percpu_wq, &dcp->av_work, msecs_to_jiffies(1000));
}

static void dcpib_crtc_atomic_enable(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct dcpib *dcp = to_dcpib(crtc->dev);
	const struct dcpib_timing_mode *t;
	const struct dcpib_color_mode *c;
	int ret;

	mutex_lock(&dcp->lock);
	t = dcpib_find_mode(dcp, &new->mode);
	c = dcpib_find_color(dcp);
	if (!t || !c)
		goto out;
	dcp->enabled = true;
	if (!dcp->powered) {
		/* The phy starts cold first; the firmware then trains. */
		ret = dcp->phy->activate(dcp);
		if (ret) {
			dev_err(dcp->dev, "phy activation failed: %d\n", ret);
			goto out;
		}
		/* A pipe powered on without a surface fetches from a null base. */
		ret = dcpib_ib_set_surface(dcp, dcp->scan_dva[dcp->front],
					   le32_to_cpu(t->width), le32_to_cpu(t->height),
					   ALIGN(le32_to_cpu(t->width) * 4, 64));
		if (ret) {
			dev_err(dcp->dev, "set_surface failed: %d\n", ret);
			goto out;
		}
		ret = dcpib_ib_set_power(dcp, true);
		if (ret) {
			dev_err(dcp->dev, "set_power failed: %d\n", ret);
			goto out;
		}
		dcp->powered = !ret;
	}
	if (dcp->mode_set && !memcmp(t, &dcp->cur_mode, sizeof(*t)))
		goto out;
	dev_info(dcp->dev, "modeset %ux%u@%d\n", le32_to_cpu(t->width),
		 le32_to_cpu(t->height), dcpib_refresh(t));
	/*
	 * As aura's display_configure: query the sink and both mode lists
	 * again before SET_MODE, and look the mode up in the fresh list. The
	 * DPTX port stays connected; reconnecting it here drops the link and
	 * the HDMI firmware then fails SET_MODE.
	 */
	ret = dcpib_ib_set_power(dcp, true);
	if (!ret)
		ret = dcpib_ib_get_hpd(dcp, NULL, NULL) < 0;
	if (!ret)
		ret = dcpib_ib_get_modes(dcp);
	if (ret)
		dev_err(dcp->dev, "refreshing the modes failed: %d\n", ret);
	t = dcpib_find_mode(dcp, &new->mode);
	c = dcpib_find_color(dcp);
	if (!t || !c)
		goto out;
	dcpib_set_mode_locked(dcp, t, c);
out:
	mutex_unlock(&dcp->lock);
}

static void dcpib_crtc_atomic_disable(struct drm_crtc *crtc,
				      struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct dcpib *dcp = to_dcpib(crtc->dev);
	int ret;

	/* A mode change: SET_MODE alone switches, as before. */
	if (new->active)
		return;
	/*
	 * Display off (DPMS, blanking): the firmware stops the pipe and the
	 * monitor goes to sleep. The next enable sets the mode again.
	 */
	mutex_lock(&dcp->lock);
	dcp->enabled = false;
	dcpib_cancel_swap_locked(dcp);
	if (dcp->powered) {
		ret = dcpib_ib_set_power(dcp, false);
		if (ret) {
			dev_err(dcp->dev, "set_power off failed: %d\n", ret);
		} else {
			dev_info(dcp->dev, "display off\n");
			/* Nothing scans out: power the phy down as well. */
			if (!dcp->phy->deactivate(dcp))
				dcp->port[0].link_rate = 0;
		}
		dcp->powered = !!ret;
		dcp->mode_set = false;
	}
	mutex_unlock(&dcp->lock);
}

static const struct drm_crtc_helper_funcs dcpib_crtc_helper = {
	.atomic_check = dcpib_crtc_atomic_check,
	.atomic_enable = dcpib_crtc_atomic_enable,
	.atomic_disable = dcpib_crtc_atomic_disable,
};

static const struct drm_crtc_funcs dcpib_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs dcpib_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/* Modeset before the plane update, which copies at the new geometry. */
static const struct drm_mode_config_helper_funcs dcpib_mode_config_helpers = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static const struct drm_mode_config_funcs dcpib_mode_config_funcs = {
	/* fbdev writes reach the scanout buffer through the dirty damage */
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void dcpib_pd_reset_work(struct work_struct *work)
{
	struct dcpib *dcp = container_of(work, struct dcpib, pd_reset_work);
	int ret;

	dev_info(dcp->dev, "USB-C sink does not answer, entering DP alt mode again\n");
	/* The reset may finish between polls. Release the old link explicitly. */
	mutex_lock(&dcp->lock);
	dcpib_cancel_swap_locked(dcp);
	ret = dcpib_dptx_disconnect(dcp, 0);
	if (!ret)
		ret = dcpib_ib_set_power(dcp, false);
	if (!ret) {
		dcp->powered = false;
		dcp->mode_set = false;
		ret = dcp->phy->deactivate(dcp);
	}
	if (ret) {
		mutex_unlock(&dcp->lock);
		dev_err(dcp->dev, "cannot release old DP link: %d\n", ret);
		return;
	}
	dcp->hpd = 0;
	ret = dcpib_pd_reconnect(dcp);
	mutex_unlock(&dcp->lock);
	if (ret) {
		dev_err(dcp->dev, "DP alt mode entry failed: %d\n", ret);
		return;
	}
	drm_kms_helper_hotplug_event(&dcp->drm);
}

static void dcpib_hpd_work(struct work_struct *work)
{
	struct dcpib *dcp = container_of(to_delayed_work(work), struct dcpib,
					 hpd_work);

	if (!READ_ONCE(dcp->ready))
		return;
	drm_helper_hpd_irq_event(&dcp->drm);
	if (READ_ONCE(dcp->ready))
		queue_delayed_work(system_freezable_wq, &dcp->hpd_work,
				   msecs_to_jiffies(250));
}

void dcpib_kms_stop(struct dcpib *dcp)
{
	if (!dcp->kms_work_ready)
		return;

	cancel_delayed_work_sync(&dcp->hpd_work);
	hrtimer_cancel(&dcp->swap_timer);
	cancel_work_sync(&dcp->swap_work);
	cancel_work_sync(&dcp->pd_reset_work);
}

static void dcpib_swap_stop(void *data)
{
	dcpib_kms_stop(data);
}

int dcpib_kms_init(struct dcpib *dcp)
{
	struct drm_device *drm = &dcp->drm;
	u32 max_w = 3840, max_h = 2160;
	int ret;

	dcp->pending = -1;
	INIT_WORK(&dcp->swap_work, dcpib_swap_work);
	INIT_WORK(&dcp->pd_reset_work, dcpib_pd_reset_work);
	INIT_DELAYED_WORK(&dcp->hpd_work, dcpib_hpd_work);
	hrtimer_setup(&dcp->swap_timer, dcpib_swap_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_ABS);
	dcp->kms_work_ready = true;
	ret = devm_add_action_or_reset(dcp->dev, dcpib_swap_stop, dcp);
	if (ret)
		return ret;

	for (unsigned int i = 0; i < dcp->nr_tmodes; i++) {
		max_w = max(max_w, le32_to_cpu(dcp->tmodes[i].width));
		max_h = max(max_h, le32_to_cpu(dcp->tmodes[i].height));
	}

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	dcp->pd_reset_time = jiffies - 61 * HZ;

	drm->mode_config.min_width = 320;
	drm->mode_config.min_height = 240;
	drm->mode_config.max_width = max_w;
	drm->mode_config.max_height = max_h;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &dcpib_mode_config_funcs;
	drm->mode_config.helper_private = &dcpib_mode_config_helpers;

	ret = drm_universal_plane_init(drm, &dcp->plane, 0, &dcpib_plane_funcs,
				       dcpib_formats, ARRAY_SIZE(dcpib_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&dcp->plane, &dcpib_plane_helper);
	drm_plane_enable_fb_damage_clips(&dcp->plane);

	ret = drm_crtc_init_with_planes(drm, &dcp->crtc, &dcp->plane, NULL,
					&dcpib_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&dcp->crtc, &dcpib_crtc_helper);

	ret = drm_encoder_init(drm, &dcp->encoder, &dcpib_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	dcp->encoder.possible_crtcs = drm_crtc_mask(&dcp->crtc);

	ret = drm_connector_init(drm, &dcp->connector, &dcpib_connector_funcs,
				 dcp->hdmi ? DRM_MODE_CONNECTOR_HDMIA :
					     DRM_MODE_CONNECTOR_DisplayPort);
	if (ret)
		return ret;
	drm_connector_helper_add(&dcp->connector, &dcpib_connector_helper);
	if (dcp->hpd_gpio || dcp->pd)
		dcp->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
					DRM_CONNECTOR_POLL_DISCONNECT;
	if (dcp->pd)
		dcp->connector.polled = DRM_CONNECTOR_POLL_HPD;
	ret = drm_connector_attach_encoder(&dcp->connector, &dcp->encoder);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	return 0;
}

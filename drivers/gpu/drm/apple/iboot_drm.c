// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KMS of a DCP driven through the iBoot interface (DCP_FIRMWARE_IBOOT): a DRM
 * device of its own per DCP with one primary plane, CRTC, encoder and
 * connector. The firmware only takes width, height and refresh rate for a
 * mode and picks the timing itself; the modes come from the sink's EDID, or
 * from the firmware's list built with CVT.
 *
 * Framebuffers are shadow buffers copied by the CPU into the scanout
 * buffers, and a modeset only changes their geometry, as the bootloader
 * does. Changing the surface address between Linux buffers made later
 * modesets crash the firmware (IOMFB axi_rd_err), so the scanout buffers
 * are the only surfaces the firmware gets. There is no vblank interrupt.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iosys-map.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

#include "dcp-internal.h"
#include "ibootep.h"
#include "iomfb_iboot.h"
#include "t8152_display.h"

/*
 * Swap between the scanout buffers per frame. Off: frames are copied into
 * the buffer on screen, which tears during motion.
 */
static bool buffered = true;
module_param_named(iboot_buffered, buffered, bool, 0444);
MODULE_PARM_DESC(iboot_buffered, "iBoot interface displays: copy each frame into a scanout buffer that is not on screen (default: on)");

DEFINE_DRM_GEM_DMA_FOPS(dcp_iboot_fops);

/*
 * Not the display subsystem's "apple": these DCPs take no direct scanout,
 * every frame is copied, so userspace must not treat them as that one.
 */
static const struct drm_driver dcp_iboot_drm_driver = {
	/*
	 * Frames are copied by the CPU, imported ones too: a compositor
	 * driving both displays imports one device's buffers into the other.
	 */
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name = "apple-dcpib",
	.desc = "Apple DCP (iBoot interface)",
	.major = 1,
	.minor = 0,
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &dcp_iboot_fops,
};

struct dcp_iboot *dcp_iboot_alloc(struct device *dev)
{
	return devm_drm_dev_alloc(dev, &dcp_iboot_drm_driver, struct dcp_iboot, drm);
}

const char *dcp_iboot_driver_name(void)
{
	return dcp_iboot_drm_driver.name;
}

static const u32 dcp_iboot_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static int iboot_refresh(const struct iboot_timing_mode *t)
{
	return (le32_to_cpu(t->fps) + 0x8000) >> 16;
}

/*
 * Whether @t works: the link at the PHY's highest rate must carry it (24 bpp
 * plus about 12% blanking against rate x lanes x 0.8 for 8b/10b); a mode
 * beyond that only blanks the display.
 */
static bool iboot_mode_fits(struct dcp_iboot *ib, const struct iboot_timing_mode *t)
{
	const struct t8152_phy_ops *phy = ib->disp.phy;
	/*
	 * The lanes the firmware may train, as GET_MAX_LANE_COUNT reports them:
	 * the USB-C route's. Not the last training's: a sink that replugs while
	 * the display is off is trained with a single lane.
	 */
	u32 lanes = (phy->max_lane_count ? phy->max_lane_count(&ib->disp) : 4) ?: 2;
	u64 link = (u64)phy->max_link_rate * 270000000ULL * lanes * 8 / 10;
	u64 pixels = (u64)le32_to_cpu(t->width) * le32_to_cpu(t->height) *
		     iboot_refresh(t);

	/* Hotplug may return modes larger than the buffers allocated at probe. */
	if ((u64)ALIGN(le32_to_cpu(t->width) * 4, 64) * le32_to_cpu(t->height) >
	    ib->scan[0].size)
		return false;
	return pixels * 24 * 112 / 100 <= link;
}

/* @mode's refresh rate in 16.16, as drm_mode_vrefresh() computes it. */
static u32 iboot_mode_fps(const struct drm_display_mode *mode)
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
static const struct iboot_timing_mode *
iboot_find_mode(struct dcp_iboot *ib, const struct drm_display_mode *mode)
{
	const struct iboot_timing_mode *best = NULL;
	u32 fps = iboot_mode_fps(mode);

	for (unsigned int i = 0; i < ib->nr_tmodes; i++) {
		const struct iboot_timing_mode *t = &ib->tmodes[i];

		if (!t->valid || le32_to_cpu(t->width) != mode->hdisplay ||
		    le32_to_cpu(t->height) != mode->vdisplay ||
		    abs_diff(le32_to_cpu(t->fps), fps) > 0x10000 ||
		    !iboot_mode_fits(ib, t))
			continue;
		if (!best || abs_diff(le32_to_cpu(t->fps), fps) <
			     abs_diff(le32_to_cpu(best->fps), fps))
			best = t;
	}
	return best;
}

/*
 * Valid, at most 32 bpp, the highest depth, then the lowest colorimetry,
 * encoding and EOTF, as the bootloader chooses. Deeper modes the HDMI
 * converter offers are refused by SET_MODE.
 */
static bool iboot_color_better(const struct iboot_color_mode *a,
			       const struct iboot_color_mode *b)
{
	u32 abpp = le32_to_cpu(a->bpp), bbpp = le32_to_cpu(b->bpp);
	bool afits = abpp <= 32, bfits = bbpp <= 32;

	if (!!a->valid != !!b->valid)
		return a->valid;
	if (afits != bfits)
		return afits;
	if (abpp != bbpp)
		return abpp > bbpp;
	if (a->colorimetry != b->colorimetry)
		return le32_to_cpu(a->colorimetry) < le32_to_cpu(b->colorimetry);
	if (a->encoding != b->encoding)
		return le32_to_cpu(a->encoding) < le32_to_cpu(b->encoding);
	return le32_to_cpu(a->eotf) < le32_to_cpu(b->eotf);
}

static const struct iboot_color_mode *iboot_find_color(struct dcp_iboot *ib)
{
	const struct iboot_color_mode *best = NULL;

	for (unsigned int i = 0; i < ib->nr_cmodes; i++)
		if (!best || iboot_color_better(&ib->cmodes[i], best))
			best = &ib->cmodes[i];
	return best && best->valid ? best : NULL;
}

/*
 * A conservative console mode from 50 to 60 Hz, up to 1920x1080 on
 * USB-C. Userspace can select the larger modes, including 2560x1440@60,
 * after the sink's EDID is available.
 */
static const struct iboot_timing_mode *iboot_preferred(struct dcp_iboot *ib)
{
	const struct iboot_timing_mode *best = NULL;

	for (unsigned int i = 0; i < ib->nr_tmodes; i++) {
		const struct iboot_timing_mode *t = &ib->tmodes[i];

		/*
		 * Byte 0 bit 6 marks standard modes a converter adds on its own
		 * (the J873g PS190 lists 3840x2160 for a 2560x1440 monitor);
		 * the bootloader's largest mode skips them too.
		 */
		if (!t->valid || iboot_refresh(t) > 60 || iboot_refresh(t) < 50 ||
		    (t->flags[0] & 0x40) ||
		    (!ib->disp.hdmi && (le32_to_cpu(t->width) > 1920 ||
					le32_to_cpu(t->height) > 1080)) ||
		    !iboot_mode_fits(ib, t))
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
static int iboot_edid_get_modes(struct drm_connector *connector)
{
	struct dcp_iboot *ib = to_dcp_iboot(connector->dev);
	struct drm_display_mode *mode, *best = NULL;
	int count = drm_edid_connector_add_modes(connector);

	mutex_lock(&ib->lock);
	list_for_each_entry(mode, &connector->probed_modes, head) {
		mode->type &= ~DRM_MODE_TYPE_PREFERRED;
		if (!iboot_find_mode(ib, mode))
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
	mutex_unlock(&ib->lock);
	return count;
}

static int iboot_connector_get_modes(struct drm_connector *connector)
{
	struct dcp_iboot *ib = to_dcp_iboot(connector->dev);
	const struct iboot_timing_mode *preferred;
	int count = 0;

	drm_edid_connector_update(connector, ib->edid);
	if (ib->edid)
		return iboot_edid_get_modes(connector);

	/* Without an EDID, the firmware's list. */
	mutex_lock(&ib->lock);
	preferred = iboot_preferred(ib);
	for (unsigned int i = 0; i < ib->nr_tmodes; i++) {
		const struct iboot_timing_mode *t = &ib->tmodes[i];
		struct drm_display_mode *mode;

		if (!t->valid || !iboot_mode_fits(ib, t))
			continue;
		mode = drm_cvt_mode(connector->dev, le32_to_cpu(t->width),
				    le32_to_cpu(t->height), iboot_refresh(t),
				    false, false, false);
		if (!mode)
			continue;
		if (t == preferred)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	}
	mutex_unlock(&ib->lock);
	return count;
}

static enum drm_mode_status
iboot_connector_mode_valid(struct drm_connector *connector,
			   const struct drm_display_mode *mode)
{
	struct dcp_iboot *ib = to_dcp_iboot(connector->dev);

	return iboot_find_mode(ib, mode) ? MODE_OK : MODE_NOMODE;
}

static const struct drm_connector_helper_funcs iboot_connector_helper = {
	.get_modes = iboot_connector_get_modes,
	.mode_valid = iboot_connector_mode_valid,
};

static int iboot_set_mode_locked(struct dcp_iboot *ib,
				 const struct iboot_timing_mode *t,
				 const struct iboot_color_mode *c);

/* With ib->lock held; queued work will observe the cleared pending slot. */
static void iboot_cancel_swap_locked(struct dcp_iboot *ib)
{
	hrtimer_cancel(&ib->swap_timer);
	ib->pending = -1;
}

/* Read the sink's EDID through the PHY, if it can, and keep it. */
void dcp_iboot_update_edid(struct dcp_iboot *ib)
{
	const struct drm_edid *edid = NULL;
	u8 *buf;
	int len;

	if (!ib->disp.phy->read_edid)
		return;
	buf = kzalloc(4 * EDID_LENGTH, GFP_KERNEL);
	if (!buf)
		return;
	len = ib->disp.phy->read_edid(&ib->disp, buf, 4 * EDID_LENGTH);
	if (len >= EDID_LENGTH) {
		edid = drm_edid_alloc(buf, len);
		if (edid && !drm_edid_valid(edid)) {
			drm_edid_free(edid);
			edid = NULL;
		}
	}
	if (!edid)
		dev_warn(ib->dcp->dev, "no EDID: %d\n", len);
	else
		dev_info(ib->dcp->dev, "EDID: %d bytes\n", len);
	drm_edid_free(ib->edid);
	ib->edid = edid;
	kfree(buf);
}

static enum drm_connector_status
iboot_connector_detect(struct drm_connector *connector, bool force)
{
	struct dcp_iboot *ib = to_dcp_iboot(connector->dev);
	struct t8152_display *disp = &ib->disp;
	struct apple_dcp *dcp = ib->dcp;
	enum drm_connector_status status;
	bool flipped = false;
	int hpd;

	mutex_lock(&ib->lock);
	t8152_hdmi_port_poll(disp);
	if (disp->hpd_gpio) {
		hpd = gpiod_get_value_cansleep(disp->hpd_gpio);
	} else if (disp->phy->detect) {
		hpd = disp->phy->detect(disp, &flipped);
	} else {
		status = ib->nr_tmodes ? connector_status_connected :
					 connector_status_disconnected;
		goto out;
	}
	if (hpd < 0) {
		status = connector_status_unknown;
		goto out;
	}
	if (hpd != disp->hpd) {
		dev_info(dcp->dev, "sink %s\n", hpd ? "plugged" : "unplugged");
		disp->hpd = hpd;
		/*
		 * USB-C: the DPTX port's HPD follows the port's Type-C state,
		 * and a new plug may come the other way round.
		 */
		if (disp->usbc) {
			if (!hpd || ib->powered) {
				iboot_cancel_swap_locked(ib);
				if (t8152_dptx_disconnect(disp, 0))
					dev_err(dcp->dev, "DPTX release failed\n");
				if (!ibootep_set_power(dcp, false)) {
					ib->powered = false;
					ib->mode_set = false;
					disp->phy->deactivate(disp);
				}
			}
			/*
			 * A plug while the display stayed on (unplug and plug
			 * between two polls): start the PHY cold again, the
			 * new plug may come the other way round.
			 */
			if (hpd)
				disp->phy->deactivate(disp);
			/* The sink may be on another port now. */
			if (hpd && disp->phy->select)
				disp->phy->select(disp);
			if (hpd && disp->phy->set_orientation)
				disp->phy->set_orientation(disp, flipped);
			/* The firmware talks AUX as soon as it sees HPD. */
			if (hpd && disp->phy->activate(disp))
				dev_err(dcp->dev, "PHY activation failed\n");
			if (hpd && t8152_dptx_connect(disp, 0))
				dev_err(dcp->dev, "DPTX connect failed\n");
			if (hpd && t8152_dptx_set_hpd(disp, 0, true))
				dev_err(dcp->dev, "DPTX HPD update failed\n");
		}
		/* A new sink: the firmware reads its EDID, take the new lists. */
		if (hpd && (dcp_iboot_wait_hpd(ib) || dcp_iboot_get_modes(ib))) {
			dev_err(dcp->dev, "no modes from the new sink\n");
			ib->nr_tmodes = 0;
			ib->nr_cmodes = 0;
			/* A sleeping EIZO may need its link started anew. */
			if (disp->usbc && !disp->on_hdmi &&
			    time_after(jiffies, ib->relink_time + 60 * HZ)) {
				ib->relink_time = jiffies;
				schedule_work(&ib->relink_work);
			}
		}
		/*
		 * The EDID once the firmware has read it, so the sink's DDC
		 * answers, and before SET_MODE: reading it borrows the AUX path
		 * while the firmware does no AUX.
		 */
		if (hpd && ib->nr_tmodes)
			dcp_iboot_update_edid(ib);
		/*
		 * A fast reconnect can finish before userspace disables the CRTC.
		 * Restore its requested state even though unplug powered it off.
		 */
		if (hpd && ib->enabled && ib->cur_mode.valid) {
			const struct iboot_timing_mode *t = NULL;
			const struct iboot_color_mode *c = iboot_find_color(ib);
			int ret = 0;

			for (unsigned int i = 0; i < ib->nr_tmodes; i++) {
				const struct iboot_timing_mode *candidate = &ib->tmodes[i];

				if (candidate->valid && candidate->width == ib->cur_mode.width &&
				    candidate->height == ib->cur_mode.height &&
				    candidate->fps == ib->cur_mode.fps) {
					t = candidate;
					break;
				}
			}
			if (t && c && !ib->powered) {
				ret = ibootep_set_surface(dcp, ib->scan_dva[ib->front],
							  le32_to_cpu(t->width),
							  le32_to_cpu(t->height),
							  ALIGN(le32_to_cpu(t->width) * 4, 64));
				if (!ret)
					ret = ibootep_set_power(dcp, true);
				ib->powered = !ret;
			}
			if (t && c && !ret)
				iboot_set_mode_locked(ib, t, c);
		}
	}
	status = hpd && ib->nr_tmodes ? connector_status_connected :
					connector_status_disconnected;
out:
	mutex_unlock(&ib->lock);
	return status;
}

static const struct drm_connector_funcs iboot_connector_funcs = {
	.detect = iboot_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int iboot_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_commit *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state, plane);
	struct dcp_iboot *ib = to_dcp_iboot(plane->dev);
	struct drm_crtc_state *crtc_state = NULL;
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
	return (u64)ALIGN(width * 4, 64) * height <= ib->scan[0].size ? 0 : -EINVAL;
}

static void iboot_rect_grow(struct drm_rect *r, const struct drm_rect *add)
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
static s64 iboot_period_ns(struct dcp_iboot *ib)
{
	u32 fps = le32_to_cpu(ib->cur_mode.fps);

	if (fps < (24 << 16))
		fps = 60 << 16;
	return div64_u64((u64)NSEC_PER_SEC << 16, fps);
}

/* Whether a frame may be shown: the display is on, set up and alive. */
static bool iboot_scanning_locked(struct dcp_iboot *ib)
{
	struct apple_rtkit *rtk = READ_ONCE(ib->dcp->rtk);

	return ib->powered && ib->mode_set && ib->disp.hpd && rtk &&
	       !apple_rtkit_is_crashed(rtk);
}

/* Show scan[buf]. Called with ib->lock held. */
static void iboot_swap_locked(struct dcp_iboot *ib, unsigned int buf, u32 w,
			      u32 h, u32 pitch)
{
	int ret;

	if (!iboot_scanning_locked(ib))
		return;

	/* The pipe must see the whole frame before it is swapped in. */
	wmb();
	ret = ibootep_set_surface(ib->dcp, ib->scan_dva[buf], w, h, pitch);
	if (ret) {
		/* Still on the old buffer, which misses this frame. */
		dev_err_ratelimited(ib->dcp->dev, "swap failed: %d\n", ret);
		ib->stale[ib->front] = DRM_RECT_INIT(0, 0, INT_MAX / 2, INT_MAX / 2);
		return;
	}
	ib->last_swap = ktime_get();
	ib->retired[ib->front] = ib->last_swap;
	ib->front = buf;
}

static void iboot_swap_work(struct work_struct *work)
{
	struct dcp_iboot *ib = container_of(work, struct dcp_iboot, swap_work);

	mutex_lock(&ib->lock);
	if (ib->pending >= 0) {
		iboot_swap_locked(ib, ib->pending, ib->pending_w,
				  ib->pending_h, ib->pending_pitch);
		ib->pending = -1;
	}
	mutex_unlock(&ib->lock);
}

static enum hrtimer_restart iboot_swap_timer(struct hrtimer *t)
{
	struct dcp_iboot *ib = container_of(t, struct dcp_iboot, swap_timer);

	queue_work(system_highpri_wq, &ib->swap_work);
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
static void iboot_plane_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_commit *state)
{
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(new);
	struct dcp_iboot *ib = to_dcp_iboot(plane->dev);
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
		iboot_rect_grow(&damage, &clip);
	}
	if (!drm_rect_visible(&damage))
		return;

	mutex_lock(&ib->lock);
	if (!iboot_scanning_locked(ib))
		goto out;
	period = iboot_period_ns(ib);
	if (!READ_ONCE(buffered)) {
		/* Into the buffer on screen (tears during motion). */
		back = ib->front;
	} else if (ib->pending >= 0) {
		back = ib->pending;
	} else {
		/* The other buffer that left the screen first. */
		unsigned int a = (ib->front + 1) % DCP_IBOOT_SCANOUT_BUFS;
		unsigned int b = (ib->front + 2) % DCP_IBOOT_SCANOUT_BUFS;
		s64 wait;

		back = ktime_before(ib->retired[a], ib->retired[b]) ? a : b;
		/* Off screen for a full refresh: only after a mode change. */
		wait = period - ktime_to_ns(ktime_sub(ktime_get(), ib->retired[back]));
		if (ib->mode_set && wait > 0)
			usleep_range(div_s64(wait, NSEC_PER_USEC),
				     div_s64(wait, NSEC_PER_USEC) + 100);
	}
	clip = ib->stale[back];
	iboot_rect_grow(&clip, &damage);
	if (drm_rect_intersect(&clip, &full)) {
		iosys_map_set_vaddr(&dst, ib->scan_wc[back]);
		iosys_map_incr(&dst, drm_fb_clip_offset(pitch, fb->format, &clip));
		source_clip = clip;
		drm_rect_translate(&source_clip, src.x1, src.y1);
		drm_fb_memcpy(&dst, &pitch, shadow->data, fb, &source_clip);
	}
	ib->stale[back] = (struct drm_rect){ 0, 0, 0, 0 };
	for (unsigned int i = 0; i < DCP_IBOOT_SCANOUT_BUFS; i++)
		if (i != back)
			iboot_rect_grow(&ib->stale[i], &damage);
	if (back == ib->front)
		goto out;

	since = ktime_to_ns(ktime_sub(ktime_get(), ib->last_swap));
	if (ib->pending < 0 && (!ib->mode_set || since >= period)) {
		iboot_swap_locked(ib, back, drm_rect_width(&full),
				  drm_rect_height(&full), pitch);
		goto out;
	}
	ib->pending_w = drm_rect_width(&full);
	ib->pending_h = drm_rect_height(&full);
	ib->pending_pitch = pitch;
	if (ib->pending < 0) {
		ib->pending = back;
		hrtimer_start(&ib->swap_timer, ktime_add_ns(ib->last_swap, period),
			      HRTIMER_MODE_ABS);
	}
out:
	mutex_unlock(&ib->lock);
}

static const struct drm_plane_helper_funcs iboot_plane_helper = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = iboot_plane_atomic_check,
	.atomic_update = iboot_plane_atomic_update,
};

static const struct drm_plane_funcs iboot_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static int iboot_crtc_atomic_check(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct dcp_iboot *ib = to_dcp_iboot(crtc->dev);

	/*
	 * No vblank interrupt: page-flip events come from a timer at the
	 * mode's refresh rate (drm_vblank_helper), so a compositor and its
	 * clients render once per refresh instead of as fast as commits
	 * complete. A commit that leaves the CRTC off gets no flush
	 * (commit_tail_rpm flushes active CRTCs only), so its event goes out
	 * at once, as before.
	 */
	new->no_vblank = !new->active;
	if (new->enable && !iboot_find_mode(ib, &new->mode))
		return -EINVAL;
	return 0;
}

/* SET_MODE, then the scanout buffer with the new mode's geometry. */
static int iboot_set_mode_locked(struct dcp_iboot *ib,
				 const struct iboot_timing_mode *t,
				 const struct iboot_color_mode *c)
{
	int ret;

	/*
	 * Right after power on the firmware may still train the link, and it
	 * fails SET_MODE with an error status until the link is up. It sends
	 * no message once it is: the DPTX calls it makes while training (link
	 * configuration, rate, lane count) have no last one, and a sink can
	 * be trained twice in a row. So SET_MODE is retried. In the traced
	 * J873g modesets the link was up 76 to 229 ms after power on; one
	 * failed SET_MODE takes about 200 ms itself, so 200 ms between tries
	 * cover a second training, and five tries give up after about two
	 * seconds. On USB-C the DP alt mode reentry below takes over then.
	 */
	for (int i = 0; i < 5; i++) {
		ret = ibootep_set_mode(ib->dcp, t, c);
		if (ret != -EIO)
			break;
		msleep(200);
	}
	if (ret) {
		dev_err(ib->dcp->dev, "set_mode failed: %d\n", ret);
		/*
		 * USB-C: a sink that no longer answers AUX (seen after the
		 * monitor's deep sleep) is connected anew: its link is
		 * released, and the plug that follows starts the PHY cold
		 * and sets the mode again. At most once a minute, so a dead
		 * sink does not loop.
		 */
		if (ib->disp.usbc && !ib->disp.on_hdmi &&
		    time_after(jiffies, ib->relink_time + 60 * HZ)) {
			ib->cur_mode = *t;
			ib->mode_failed = true;
			ib->relink_time = jiffies;
			schedule_work(&ib->relink_work);
		}
		return ret;
	}
	ib->cur_mode = *t;
	ib->mode_set = true;
	ib->mode_failed = false;
	/* The buffers hold the old geometry: the next frame is copied whole. */
	for (unsigned int i = 0; i < DCP_IBOOT_SCANOUT_BUFS; i++)
		ib->stale[i] = DRM_RECT_INIT(0, 0, INT_MAX / 2, INT_MAX / 2);
	/* A pending frame has the old geometry. */
	iboot_cancel_swap_locked(ib);
	/* As the bootloader swaps after a modeset. */
	ret = ibootep_set_surface(ib->dcp, ib->scan_dva[ib->front],
				  le32_to_cpu(t->width), le32_to_cpu(t->height),
				  ALIGN(le32_to_cpu(t->width) * 4, 64));
	if (ret)
		dev_err(ib->dcp->dev, "set_surface failed: %d\n", ret);
	return ret;
}

/*
 * The boot mode again: a display powered on without SET_MODE counts as idle
 * to the firmware's AV stack, which powers the link off after 30 s.
 */
int dcp_iboot_set_boot_mode(struct dcp_iboot *ib)
{
	const struct iboot_timing_mode *best = NULL;
	const struct iboot_color_mode *c;
	int ret = -ENOENT;

	mutex_lock(&ib->lock);
	for (unsigned int i = 0; i < ib->nr_tmodes; i++) {
		const struct iboot_timing_mode *t = &ib->tmodes[i];

		if (!t->valid || le32_to_cpu(t->width) != ib->boot_w ||
		    le32_to_cpu(t->height) != ib->boot_h ||
		    iboot_refresh(t) > 60)
			continue;
		if (!best || le32_to_cpu(t->fps) > le32_to_cpu(best->fps))
			best = t;
	}
	c = iboot_find_color(ib);
	if (best && c)
		ret = iboot_set_mode_locked(ib, best, c);
	mutex_unlock(&ib->lock);
	return ret;
}

/*
 * The DCPAV service starts the firmware's idle power off timer when its video
 * interface comes up: until a SET_MODE follows, it counts the powered-on
 * display as idle and takes the link down 30 s later. The probe's SET_MODE
 * comes before the interface starts, so one follows it here.
 */
void dcp_iboot_reset_mode(struct dcp_iboot *ib)
{
	const struct iboot_color_mode *c;

	mutex_lock(&ib->lock);
	c = iboot_find_color(ib);
	if (c && ib->powered && ib->mode_set)
		iboot_set_mode_locked(ib, &ib->cur_mode, c);
	mutex_unlock(&ib->lock);
}

/*
 * From the AFK receive work of the DPAV endpoint: let the firmware finish
 * opening the interface before the SET_MODE, which waits for a reply that
 * another endpoint's receive work completes.
 */
void dcp_iboot_av_started(struct dcp_iboot *ib)
{
	queue_delayed_work(system_percpu_wq, &ib->av_work, msecs_to_jiffies(1000));
}

static void iboot_crtc_atomic_enable(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct dcp_iboot *ib = to_dcp_iboot(crtc->dev);
	struct apple_dcp *dcp = ib->dcp;
	const struct iboot_timing_mode *t;
	const struct iboot_color_mode *c;
	int ret;

	mutex_lock(&ib->lock);
	t = iboot_find_mode(ib, &new->mode);
	c = iboot_find_color(ib);
	if (!t || !c)
		goto out;
	ib->enabled = true;
	if (!ib->powered) {
		/* The PHY starts cold first; the firmware then trains. */
		ret = ib->disp.phy->activate(&ib->disp);
		if (ret) {
			dev_err(dcp->dev, "PHY activation failed: %d\n", ret);
			goto out;
		}
		/* A pipe powered on without a surface fetches from a null base. */
		ret = ibootep_set_surface(dcp, ib->scan_dva[ib->front],
					  le32_to_cpu(t->width), le32_to_cpu(t->height),
					  ALIGN(le32_to_cpu(t->width) * 4, 64));
		if (ret) {
			dev_err(dcp->dev, "set_surface failed: %d\n", ret);
			goto out;
		}
		ret = ibootep_set_power(dcp, true);
		if (ret) {
			dev_err(dcp->dev, "set_power failed: %d\n", ret);
			goto out;
		}
		ib->powered = true;
	}
	if (ib->mode_set && !memcmp(t, &ib->cur_mode, sizeof(*t)))
		goto out;
	dev_info(dcp->dev, "modeset %ux%u@%d\n", le32_to_cpu(t->width),
		 le32_to_cpu(t->height), iboot_refresh(t));
	/*
	 * As the bootloader configures a display: query the sink and both mode
	 * lists again before SET_MODE, and look the mode up in the fresh list.
	 * The DPTX port stays connected; reconnecting it here drops the link
	 * and the HDMI firmware then fails SET_MODE.
	 */
	ret = ibootep_set_power(dcp, true);
	if (!ret)
		ret = ibootep_get_hpd(dcp, NULL, NULL) < 0;
	if (!ret)
		ret = dcp_iboot_get_modes(ib);
	if (ret)
		dev_err(dcp->dev, "refreshing the modes failed: %d\n", ret);
	t = iboot_find_mode(ib, &new->mode);
	c = iboot_find_color(ib);
	if (!t || !c)
		goto out;
	iboot_set_mode_locked(ib, t, c);
out:
	mutex_unlock(&ib->lock);
	drm_crtc_vblank_on(crtc);
}

static void iboot_crtc_atomic_disable(struct drm_crtc *crtc,
				      struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct dcp_iboot *ib = to_dcp_iboot(crtc->dev);
	int ret, idx;

	drm_crtc_vblank_off(crtc);
	/* A mode change: SET_MODE alone switches, as before. */
	if (new->active)
		return;
	/*
	 * After an unbind the firmware session is retained as it is, and the
	 * shutdown commit only releases the state.
	 */
	if (!drm_dev_enter(crtc->dev, &idx))
		return;
	/*
	 * Display off (DPMS, blanking): the firmware stops the pipe and the
	 * monitor goes to sleep. The next enable sets the mode again.
	 */
	mutex_lock(&ib->lock);
	ib->enabled = false;
	iboot_cancel_swap_locked(ib);
	if (ib->powered) {
		ret = ibootep_set_power(ib->dcp, false);
		if (ret) {
			dev_err(ib->dcp->dev, "set_power off failed: %d\n", ret);
		} else {
			dev_info(ib->dcp->dev, "display off\n");
			/* Nothing scans out: power the PHY down as well. */
			if (!ib->disp.phy->deactivate(&ib->disp))
				ib->disp.port[0].link_rate = 0;
		}
		ib->powered = !!ret;
		ib->mode_set = false;
	}
	mutex_unlock(&ib->lock);
	drm_dev_exit(idx);
}

static const struct drm_crtc_helper_funcs iboot_crtc_helper = {
	.atomic_check = iboot_crtc_atomic_check,
	.atomic_flush = drm_crtc_vblank_atomic_flush,
	.atomic_enable = iboot_crtc_atomic_enable,
	.atomic_disable = iboot_crtc_atomic_disable,
};

static const struct drm_crtc_funcs iboot_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	DRM_CRTC_VBLANK_TIMER_FUNCS,
};

static const struct drm_encoder_funcs iboot_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/* Modeset before the plane update, which copies at the new geometry. */
static const struct drm_mode_config_helper_funcs iboot_mode_config_helpers = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static const struct drm_mode_config_funcs iboot_mode_config_funcs = {
	/* fbdev writes reach the scanout buffer through the dirty damage */
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * Release the link of a USB-C sink that does not answer, and have the port's
 * sink plugged again: the port's Type-C state still shows it, so the next
 * look at the ports connects it as a new one.
 */
static void iboot_relink_work(struct work_struct *work)
{
	struct dcp_iboot *ib = container_of(work, struct dcp_iboot, relink_work);
	struct t8152_display *disp = &ib->disp;
	int ret;

	dev_info(ib->dcp->dev, "USB-C sink does not answer, connecting it again\n");
	mutex_lock(&ib->lock);
	iboot_cancel_swap_locked(ib);
	ret = t8152_dptx_disconnect(disp, 0);
	if (!ret)
		ret = ibootep_set_power(ib->dcp, false);
	if (!ret) {
		ib->powered = false;
		ib->mode_set = false;
		ret = disp->phy->deactivate(disp);
	}
	if (ret) {
		mutex_unlock(&ib->lock);
		dev_err(ib->dcp->dev, "cannot release old DP link: %d\n", ret);
		return;
	}
	disp->hpd = 0;
	mutex_unlock(&ib->lock);
	drm_kms_helper_hotplug_event(&ib->drm);
	dcp_iboot_hpd_event(ib);
}

static void iboot_hpd_work(struct work_struct *work)
{
	struct dcp_iboot *ib = container_of(to_delayed_work(work), struct dcp_iboot,
					    hpd_work);

	if (!READ_ONCE(ib->ready))
		return;
	drm_helper_hpd_irq_event(&ib->drm);
	/*
	 * USB-C changes come from the ports' display switches. The HDMI port's
	 * HPD line has no interrupt: poll it while this DCP can take the port.
	 */
	if (READ_ONCE(ib->ready) && ib->disp.hdmi_port)
		queue_delayed_work(system_freezable_wq, &ib->hpd_work,
				   msecs_to_jiffies(250));
}

/*
 * A USB-C port's Type-C state changed: look at the ports shortly. A HPD pulse
 * the port driver learns of after the fact comes as HPD low and high at once;
 * the delay takes both together.
 */
void dcp_iboot_hpd_event(struct dcp_iboot *ib)
{
	if (READ_ONCE(ib->ready))
		mod_delayed_work(system_freezable_wq, &ib->hpd_work,
				 msecs_to_jiffies(100));
}

void dcp_iboot_kms_stop(struct dcp_iboot *ib)
{
	if (!ib->kms_work_ready)
		return;

	cancel_delayed_work_sync(&ib->hpd_work);
	hrtimer_cancel(&ib->swap_timer);
	cancel_work_sync(&ib->swap_work);
	cancel_work_sync(&ib->relink_work);
}

static void iboot_swap_stop(void *data)
{
	dcp_iboot_kms_stop(data);
}

int dcp_iboot_kms_init(struct dcp_iboot *ib)
{
	struct drm_device *drm = &ib->drm;
	u32 max_w = 3840, max_h = 2160;
	int ret;

	ib->pending = -1;
	INIT_WORK(&ib->swap_work, iboot_swap_work);
	INIT_WORK(&ib->relink_work, iboot_relink_work);
	INIT_DELAYED_WORK(&ib->hpd_work, iboot_hpd_work);
	hrtimer_setup(&ib->swap_timer, iboot_swap_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_ABS);
	ib->kms_work_ready = true;
	ret = devm_add_action_or_reset(ib->dcp->dev, iboot_swap_stop, ib);
	if (ret)
		return ret;

	for (unsigned int i = 0; i < ib->nr_tmodes; i++) {
		max_w = max(max_w, le32_to_cpu(ib->tmodes[i].width));
		max_h = max(max_h, le32_to_cpu(ib->tmodes[i].height));
	}

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	ib->relink_time = jiffies - 61 * HZ;

	drm->mode_config.min_width = 320;
	drm->mode_config.min_height = 240;
	drm->mode_config.max_width = max_w;
	drm->mode_config.max_height = max_h;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &iboot_mode_config_funcs;
	drm->mode_config.helper_private = &iboot_mode_config_helpers;

	ret = drm_universal_plane_init(drm, &ib->plane, 0, &iboot_plane_funcs,
				       dcp_iboot_formats, ARRAY_SIZE(dcp_iboot_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&ib->plane, &iboot_plane_helper);
	drm_plane_enable_fb_damage_clips(&ib->plane);

	ret = drm_crtc_init_with_planes(drm, &ib->crtc, &ib->plane, NULL,
					&iboot_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&ib->crtc, &iboot_crtc_helper);

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;

	ret = drm_encoder_init(drm, &ib->encoder, &iboot_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	ib->encoder.possible_crtcs = drm_crtc_mask(&ib->crtc);

	ret = drm_connector_init(drm, &ib->connector, &iboot_connector_funcs,
				 ib->disp.hdmi ? DRM_MODE_CONNECTOR_HDMIA :
						 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret)
		return ret;
	drm_connector_helper_add(&ib->connector, &iboot_connector_helper);
	if (ib->disp.hpd_gpio || ib->disp.usbc)
		ib->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
				       DRM_CONNECTOR_POLL_DISCONNECT;
	if (ib->disp.usbc)
		ib->connector.polled = DRM_CONNECTOR_POLL_HPD;
	ret = drm_connector_attach_encoder(&ib->connector, &ib->encoder);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	return 0;
}

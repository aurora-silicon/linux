// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Kernel-drawn camera-in-use indicator on the internal panel.
 *
 * On T8140, T8142 and T6050 the camera has no indicator light on its
 * sensor supply: macOS has firmware outside the OS draw the indicator
 * through the display pipeline, and the image processor blanks its frames
 * unless that state is signalled. Linux runs no such firmware, so the
 * kernel draws the indicator itself and the camera driver captures only
 * while this driver reports it on the panel (drivers/soc/apple/
 * camera-indicator.c). It is a software indicator: it cannot be removed
 * from user space, but it is not a hardware privacy guarantee.
 *
 * The indicator is a small image in one firmware swap surface that no DRM
 * plane uses, above every plane. Every present while the camera wants it
 * carries that surface, so the compositor's commits cannot drop it, and a
 * present the kernel makes on its own (a replay of the frame on screen)
 * puts it up when the compositor is idle. It counts as shown once the
 * firmware completes a present carrying it while the panel is lit, and as
 * lost at once on any power-off, sleep, blank, mode set or firmware crash,
 * until a later present carrying it completes. While it is wanted the
 * backlight is kept at a visible floor.
 */

#include <kunit/visibility.h>
#include <linux/bitmap.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/camera-indicator.h>
#include <drm/drm_device.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>

#include "dcp.h"
#include "dcp-fabric.h"
#include "dcp-camera-indicator.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "iomfb_h17p.h"

/* The highest of the three surfaces every firmware clears on power-off. */
#define DCP_CAMIND_SLOT		2

#define DCP_CAMIND_RETRY_MS	100
#define DCP_CAMIND_CHECK_MS	250

/* ARGB8888, straight alpha: green disc, black ring, transparent outside. */
#define DCP_CAMIND_FILL		0xff30d158
#define DCP_CAMIND_RING		0xff000000
#define DCP_CAMIND_CLEAR	0x00000000
#define DCP_CAMIND_R_OUT	20
#define DCP_CAMIND_R_IN		17

void dcp_camind_draw(u32 *pixels)
{
	int c = DCP_CAMIND_SIZE / 2;
	int x, y;

	for (y = 0; y < DCP_CAMIND_SIZE; y++) {
		for (x = 0; x < DCP_CAMIND_SIZE; x++) {
			int dx = 2 * (x - c) + 1, dy = 2 * (y - c) + 1;
			int d2 = dx * dx + dy * dy;	/* in half pixels */
			u32 px = DCP_CAMIND_CLEAR;

			if (d2 <= 4 * DCP_CAMIND_R_IN * DCP_CAMIND_R_IN)
				px = DCP_CAMIND_FILL;
			else if (d2 <= 4 * DCP_CAMIND_R_OUT * DCP_CAMIND_R_OUT)
				px = DCP_CAMIND_RING;
			pixels[y * (DCP_CAMIND_PITCH / 4) + x] = px;
		}
	}
}
EXPORT_SYMBOL_IF_KUNIT(dcp_camind_draw);

/* Top right of the panel area below any notch. */
bool dcp_camind_place(u32 mode_w, u32 mode_h, u32 notch, struct dcp_rect *dst)
{
	u32 need = DCP_CAMIND_SIZE + DCP_CAMIND_MARGIN;

	if (mode_w < need || mode_h < need)
		return false;
	*dst = (struct dcp_rect){
		.x = mode_w - need,
		.y = notch + DCP_CAMIND_MARGIN,
		.w = DCP_CAMIND_SIZE,
		.h = DCP_CAMIND_SIZE,
	};
	return true;
}
EXPORT_SYMBOL_IF_KUNIT(dcp_camind_place);

/*
 * The internal panel of firmware that takes its presents through the
 * serialized queue (H17P and firmware 27: T8140, T8142, T6050), with a
 * swap surface free above the DRM planes.
 */
bool dcp_camind_eligible(struct apple_dcp *dcp, const unsigned long *surfaces)
{
	if (dcp->fw_compat != DCP_FIRMWARE_H17P &&
	    dcp->fw_compat != DCP_FIRMWARE_V_27_0)
		return false;
	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17G ||
	    dcp->external || dcp_is_external_h17p(dcp))
		return false;
	if (dcp_get_connector_type(to_platform_device(dcp->dev)) !=
	    DRM_MODE_CONNECTOR_eDP)
		return false;
	/* It must sit above every DRM plane. */
	return find_last_bit(surfaces, DCP_MAX_PLANES) < DCP_CAMIND_SLOT;
}

static bool dcp_camind_panel_ok(struct apple_dcp *dcp)
{
	return !READ_ONCE(dcp->crashed) && READ_ONCE(dcp->mode_state.valid) &&
	       dcp->connector && READ_ONCE(dcp->connector->connected) &&
	       dcp_backlight_lit(dcp);
}

bool dcp_camind_pending(struct apple_dcp *dcp)
{
	struct dcp_camind *ci = &dcp->camind;

	return ci->registered &&
	       ((ci->want && !atomic_read(&ci->confirmed)) || ci->clear);
}

void dcp_camind_lost(struct apple_dcp *dcp)
{
	struct dcp_camind *ci = &dcp->camind;

	if (!ci->registered)
		return;
	atomic_inc(&ci->epoch);
	if (atomic_xchg(&ci->confirmed, 0))
		apple_cam_indicator_set_shown(false);
	mod_delayed_work(system_dfl_wq, &ci->work,
			 msecs_to_jiffies(DCP_CAMIND_RETRY_MS));
}

void dcp_camind_mode(struct apple_dcp *dcp, u32 width, u32 height)
{
	dcp->camind.mode_w = width;
	dcp->camind.mode_h = height;
}

/* Called for every present, under the IOMFB lock, before it is sent. */
void dcp_camind_prepare(struct apple_dcp *dcp,
			struct dcp_swap_submit_req_h17p *req, u32 swap_id)
{
	struct dcp_camind *ci = &dcp->camind;
	unsigned int s = ci->slot;
	struct dcp_rect dst;

	if (!ci->registered)
		return;
	ci->carrying = false;

	if (ci->want && dcp_camind_place(ci->mode_w, ci->mode_h,
					 dcp->notch_height, &dst)) {
		req->surf[s].base = ci->surf;
		req->surf_iova[s] = ci->obj->dma_addr;
		req->surf_null[s] = false;
		req->swap.src_rect[s] = (struct dcp_rect){
			0, 0, DCP_CAMIND_SIZE, DCP_CAMIND_SIZE };
		req->swap.dst_rect[s] = dst;
		req->swap.swap_enabled |= BIT(s);
		req->swap.swap_completed |= BIT(s);
		ci->carrying = true;
		ci->carry_id = swap_id;
		ci->carry_epoch = atomic_read(&ci->epoch);
		ci->clear = false;
	} else if (ci->clear) {
		req->surf_null[s] = true;
		req->surf_iova[s] = 0;
		req->swap.swap_enabled |= BIT(s);
		req->swap.swap_completed |= BIT(s);
		ci->clear = false;
	}
}

/* A present completed, under the IOMFB lock. */
void dcp_camind_complete(struct apple_dcp *dcp, u32 swap_id)
{
	struct dcp_camind *ci = &dcp->camind;

	if (!ci->registered || !ci->carrying || ci->carry_id != swap_id)
		return;
	ci->carrying = false;
	if (!ci->want || ci->carry_epoch != atomic_read(&ci->epoch) ||
	    !dcp_camind_panel_ok(dcp))
		return;
	if (!atomic_xchg(&ci->confirmed, 1))
		apple_cam_indicator_set_shown(true);
}

/*
 * The backlight level a present carries, under the IOMFB lock: raise it to
 * the floor while the indicator is wanted, and restore the requested level
 * once it is not.
 */
void dcp_camind_backlight(struct apple_dcp *dcp, struct dcp_present_h17p *wire,
			  u32 nits)
{
	struct dcp_camind *ci = &dcp->camind;
	u32 max = dcp->brightness.maximum;
	u32 floor = max_t(u32, max * DCP_CAMIND_FLOOR_TENTHS / 10, 1);

	if (!ci->registered || !max)
		return;
	if (ci->want && nits < floor) {
		iomfb_encode_backlight_h17p(wire, floor, max, true);
		ci->floor = true;
	} else if (ci->floor) {
		iomfb_encode_backlight_h17p(wire, nits, max, true);
		ci->floor = false;
	}
}

static void dcp_camind_work(struct work_struct *work)
{
	struct dcp_camind *ci = container_of(to_delayed_work(work),
					     struct dcp_camind, work);
	struct apple_dcp *dcp = container_of(ci, struct apple_dcp, camind);
	bool want, pending;

	scoped_guard(mutex, &dcp->iomfb.lock) {
		want = ci->want;
		pending = dcp_camind_pending(dcp);
	}
	if (want && atomic_read(&ci->confirmed) && !dcp_camind_panel_ok(dcp))
		dcp_camind_lost(dcp);
	if (pending)
		iomfb_camind_kick(dcp);
	if (want)
		mod_delayed_work(system_dfl_wq, &ci->work,
				 msecs_to_jiffies(pending ? DCP_CAMIND_RETRY_MS :
						  DCP_CAMIND_CHECK_MS));
}

static void dcp_camind_set(void *priv, bool on)
{
	struct apple_dcp *dcp = priv;
	struct dcp_camind *ci = &dcp->camind;

	scoped_guard(mutex, &dcp->iomfb.lock) {
		ci->want = on;
		if (!on) {
			atomic_set(&ci->confirmed, 0);
			apple_cam_indicator_set_shown(false);
			ci->clear = true;
		}
	}
	dev_info(dcp->dev, "camera indicator %s\n", on ? "requested" : "removed");
	mod_delayed_work(system_dfl_wq, &ci->work, 0);
}

static const struct apple_cam_indicator_ops dcp_camind_ops = {
	.set = dcp_camind_set,
};

/* The linear ARGB8888 surface of the image, as plane.c describes a framebuffer. */
void dcp_camind_surface(struct dcp_surface *surf, bool element)
{
	u32 size = DCP_CAMIND_PITCH * DCP_CAMIND_SIZE;

	*surf = (struct dcp_surface){
		.plane_cnt = 1,
		.plane_cnt2 = 1,
		.format = DCP_FORMAT_BGRA,
		.xfer_func = DCP_XFER_FUNC_SRGB,
		.colorspace = DCP_COLORSPACE_P3_D65,
		.stride = DCP_CAMIND_PITCH,
		.width = DCP_CAMIND_SIZE,
		.height = DCP_CAMIND_SIZE,
		.buf_size = size,
		.pix_size = element ? 4 : 1,
		.pel_w = 1,
		.pel_h = 1,
		.has_comp = 1,
		.has_planes = 1,
		.has_compr_info = element ? 0 : 1,
		.planes[0] = {
			.width = DCP_CAMIND_SIZE,
			.height = DCP_CAMIND_SIZE,
			.stride = DCP_CAMIND_PITCH,
			.size = size,
			.tile_w = 1,
			.tile_h = 1,
			.tile_size = element ? 4 : 1,
			.address_fmt = DCP_ADDR_FMT_LINEAR,
		},
	};
}
EXPORT_SYMBOL_IF_KUNIT(dcp_camind_surface);

static void dcp_camind_detach(struct drm_device *drm, void *data)
{
	struct apple_dcp *dcp = data;
	struct dcp_camind *ci = &dcp->camind;

	apple_cam_indicator_unregister(dcp->dev);
	scoped_guard(mutex, &dcp->iomfb.lock)
		ci->want = false;
	cancel_delayed_work_sync(&ci->work);
	ci->registered = false;
	drm_gem_object_put(&ci->obj->base);
	ci->obj = NULL;
}

int dcp_camind_attach(struct apple_dcp *dcp, struct drm_device *drm)
{
	struct dcp_camind *ci = &dcp->camind;
	u32 size = DCP_CAMIND_PITCH * DCP_CAMIND_SIZE;
	int ret;

	ci->obj = drm_gem_dma_create(drm, size);
	if (IS_ERR(ci->obj)) {
		ret = PTR_ERR(ci->obj);
		ci->obj = NULL;
		return ret;
	}
	dcp_camind_draw(ci->obj->vaddr);

	ci->slot = DCP_CAMIND_SLOT;
	dcp_camind_surface(&ci->surf, dcp_surface_element_geometry(dcp));
	INIT_DELAYED_WORK(&ci->work, dcp_camind_work);
	atomic_set(&ci->epoch, 0);
	atomic_set(&ci->confirmed, 0);
	ci->registered = true;

	ret = drmm_add_action_or_reset(drm, dcp_camind_detach, dcp);
	if (ret)
		return ret;
	ret = apple_cam_indicator_register(dcp->dev, &dcp_camind_ops, dcp);
	if (ret)
		dev_warn(dcp->dev, "camera indicator not registered: %d\n", ret);
	return 0;
}

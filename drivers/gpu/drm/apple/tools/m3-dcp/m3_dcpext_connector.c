// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/slab.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include "m3_dcpext_connector.h"
#define M3_DCPEXT_EDID_RAW_MAX 1024

struct m3_connector_state {
	struct drm_connector_state base;
	u64 generation;
};

static struct m3_dcpext_connector *to_m3(struct drm_connector *connector)
{
	return container_of(connector, struct m3_dcpext_connector, base);
}

static void mode_geometry(const struct drm_display_mode *mode,
			  struct m3_dcpext_mode *geometry)
{
	*geometry = (struct m3_dcpext_mode) {
		.width = mode->hdisplay, .height = mode->vdisplay,
		.clock_khz = mode->clock, .htotal = mode->htotal,
		.vtotal = mode->vtotal,
		.interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE,
		.doublescan = mode->flags & DRM_MODE_FLAG_DBLSCAN,
	};
}

static int select_mode(struct m3_dcpext_connector *c,
		       const struct drm_display_mode *mode, u8 timing[24])
{
	struct m3_dcpext_mode geometry;

	/* Firmware does not accept porch timings here. Only modes produced by
	 * DRM's EDID parser are eligible; never accept arbitrary custom timings
	 * just because their size and rounded refresh match a firmware record.
	 */
	if (mode->vscan > 1 || mode->flags & ~(DRM_MODE_FLAG_PHSYNC |
	    DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC))
		return -EINVAL;
	mode_geometry(mode, &geometry);
	return m3_dcpext_mode_select(&geometry, c->timings,
				    c->timings_count, timing);
}

static bool mode_known(struct m3_dcpext_connector *c,
		       const struct drm_display_mode *mode)
{
	for (u32 i = 0; i < c->modes_count; i++)
		if (drm_mode_equal(mode, &c->modes[i]))
			return true;
	return false;
}

static int select_native_mode(struct m3_dcpext_connector *c,
			      const struct drm_display_mode *mode, u32 *selected)
{
	struct m3_dcpext_native_mode requested = {};
 int ret;

	if (mode->vscan > 1 || mode->flags & ~(DRM_MODE_FLAG_PHSYNC |
	    DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC) ||
	    mode->hsync_start < mode->hdisplay || mode->hsync_end < mode->hsync_start ||
	    mode->vsync_start < mode->vdisplay || mode->vsync_end < mode->vsync_start)
		return -EINVAL;
	mode_geometry(mode, &requested.geometry);
	requested.hfront = mode->hsync_start - mode->hdisplay;
	requested.hsync = mode->hsync_end - mode->hsync_start;
	requested.vfront = mode->vsync_start - mode->vdisplay;
	requested.vsync = mode->vsync_end - mode->vsync_start;
	requested.hpositive = mode->flags & DRM_MODE_FLAG_PHSYNC;
	requested.vpositive = mode->flags & DRM_MODE_FLAG_PVSYNC;
	ret = m3_dcpext_native_mode_select(&requested, c->native_modes,
                                           c->native_modes_count, selected);
 /* This first HDMI path qualifies exact 1080p60 only. Do not advertise an
  * EDID 59.94 mode while silently selecting the firmware's 60-Hz ID. */
 if (!ret && requested.geometry.clock_khz != c->native_modes[*selected].geometry.clock_khz)
  ret = -EOPNOTSUPP;
 return ret;
}

static int get_modes(struct drm_connector *connector)
{
	struct m3_dcpext_connector *c = to_m3(connector);
	struct drm_display_mode *mode, *next, *modes;
	u32 count = 0;
	u32 selected;
	u8 timing[24];
	int ret;

	mutex_lock(&c->lock);
	kfree(c->modes);
	c->modes = NULL;
	c->modes_count = 0;
	ret = drm_edid_connector_update(connector, c->connected ? c->edid : NULL);
	if (ret || !c->connected || !c->edid)
		goto out;
	drm_edid_connector_add_modes(connector);
	list_for_each_entry_safe(mode, next, &connector->probed_modes, head) {
		if (c->native_modes ? select_native_mode(c, mode, &selected) :
				      select_mode(c, mode, timing)) {
			list_del(&mode->head);
			drm_mode_destroy(connector->dev, mode);
		} else {
			count++;
		}
	}
	modes = kcalloc(count, sizeof(*modes), GFP_KERNEL);
	if (!modes) {
		/* mode_valid will reject all modes, including core fallbacks. */
		count = 0;
		goto out;
	}
	c->modes = modes;
	list_for_each_entry(mode, &connector->probed_modes, head)
		drm_mode_copy(&modes[c->modes_count++], mode);
	/* EDID reports sink capability; v1 transports only RGB at eight bpc. */
	connector->display_info.bpc = 8;
	connector->display_info.color_formats = BIT(DRM_OUTPUT_COLOR_FORMAT_RGB444);
 out:
	mutex_unlock(&c->lock);
	return count;
}

static enum drm_connector_status detect(struct drm_connector *connector, bool force)
{
	struct m3_dcpext_connector *c = to_m3(connector);
	enum drm_connector_status status;

	mutex_lock(&c->lock);
	status = c->connected ? connector_status_connected : connector_status_disconnected;
	mutex_unlock(&c->lock);
	return status;
}

static enum drm_mode_status mode_valid(struct drm_connector *connector,
				       const struct drm_display_mode *mode)
{
	struct m3_dcpext_connector *c = to_m3(connector);
	bool valid;

	mutex_lock(&c->lock);
	valid = c->connected && c->edid && mode_known(c, mode);
	mutex_unlock(&c->lock);
	return valid ? MODE_OK : MODE_BAD;
}

static void destroy_state(struct drm_connector *connector,
			  struct drm_connector_state *state)
{
	struct m3_connector_state *s = container_of(state, struct m3_connector_state, base);

	__drm_atomic_helper_connector_destroy_state(state);
	kfree(s);
}

static void reset(struct drm_connector *connector)
{
	struct m3_connector_state *s;

	if (connector->state)
		destroy_state(connector, connector->state);
	connector->state = NULL;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (s)
		__drm_atomic_helper_connector_reset(connector, &s->base);
}

static struct drm_connector_state *duplicate_state(struct drm_connector *connector)
{
	struct m3_connector_state *s;

	if (!connector->state)
		return NULL;
	s = kmemdup(container_of(connector->state, struct m3_connector_state, base),
		    sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;
	__drm_atomic_helper_connector_duplicate_state(connector, &s->base);
	return &s->base;
}

static int atomic_check(struct drm_connector *connector, struct drm_atomic_state *state)
{
	struct m3_dcpext_connector *c = to_m3(connector);
	struct drm_connector_state *cs = drm_atomic_get_new_connector_state(state, connector);
	struct drm_crtc_state *crtc;
	struct m3_connector_state *s = container_of(cs, struct m3_connector_state, base);
	int ret = 0;

	if (!cs->crtc) {
		s->generation = 0;
		return 0;
	}
	crtc = drm_atomic_get_new_crtc_state(state, cs->crtc);
	if (!crtc || !crtc->enable)
		return -EINVAL;
	mutex_lock(&c->lock);
	if (!c->connected || !c->edid || !mode_known(c, &crtc->mode))
		ret = -ENODEV;
	else
		s->generation = c->generation;
	mutex_unlock(&c->lock);
	return ret;
}

static void destroy(struct drm_connector *connector)
{
	struct m3_dcpext_connector *c = to_m3(connector);

	/* Owner has stopped connection workers and drained atomic commits. */
	drm_connector_cleanup(connector);
	drm_edid_free(c->edid);
	kfree(c->modes);
	kfree(c->native_modes);
}

static const struct drm_connector_funcs connector_funcs = {
	.detect = detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = destroy,
	.reset = reset,
	.atomic_duplicate_state = duplicate_state,
	.atomic_destroy_state = destroy_state,
};

static const struct drm_connector_helper_funcs connector_helpers = {
	.get_modes = get_modes,
	.mode_valid = mode_valid,
	.atomic_check = atomic_check,
};

int m3_dcpext_connector_init(struct drm_device *drm,
			   struct m3_dcpext_connector *c, struct drm_encoder *encoder, int connector_type)
{
	int ret;

	mutex_init(&c->lock);
	c->generation = 1;
	ret = drm_connector_init(drm, &c->base, &connector_funcs, connector_type);
	if (ret)
		return ret;
	c->base.polled = DRM_CONNECTOR_POLL_HPD;
	c->base.interlace_allowed = false;
	c->base.doublescan_allowed = false;
	drm_connector_helper_add(&c->base, &connector_helpers);
	ret = drm_connector_attach_encoder(&c->base, encoder);
	if (ret)
		drm_connector_cleanup(&c->base);
	return ret;
}

u64 m3_dcpext_connector_invalidate(struct m3_dcpext_connector *c, bool connected)
{
	u64 generation;

	mutex_lock(&c->base.dev->mode_config.mutex);
	mutex_lock(&c->lock);
	generation = ++c->generation;
	c->connected = connected;
	drm_edid_free(c->edid);
	c->edid = NULL;
	kfree(c->modes);
	c->modes = NULL;
	c->modes_count = 0;
	c->timings_count = 0;
	kfree(c->native_modes);
	c->native_modes = NULL;
	c->native_modes_count = 0;
	c->native_generation = 0;
	drm_edid_connector_update(&c->base, NULL);
	mutex_unlock(&c->lock);
	mutex_unlock(&c->base.dev->mode_config.mutex);
	return generation;
}

static int publish_snapshot(struct m3_dcpext_connector *c, u64 generation,
			    const u8 *raw, u32 bytes, const u8 *timings, u32 count,
			    const struct m3_dcpext_native_mode *native, u64 native_generation)
{
	const struct drm_edid *edid;
	struct m3_dcpext_native_mode *native_copy = NULL;
	int ret = 0;

	if (!raw || bytes < 128 || bytes > M3_DCPEXT_EDID_RAW_MAX ||
	    bytes % 128 || bytes != ((u32)raw[126] + 1) * 128 ||
	    !count || (!native && (!timings || count > 64)) ||
	    (native && (timings || !native_generation || count > M3_DCPEXT_NATIVE_MAX_MODES)))
		return -EINVAL;
	if (native) {
		for (u32 i = 0; i < count; i++) {
			u32 rate;
			if (m3_dcpext_mode_validate(&native[i].geometry, &rate) ||
			    !native[i].hsync || !native[i].vsync ||
			    (u64)native[i].geometry.width + native[i].hfront + native[i].hsync > native[i].geometry.htotal ||
			    (u64)native[i].geometry.height + native[i].vfront + native[i].vsync > native[i].geometry.vtotal)
				return -EINVAL;
		}
	}
	edid = drm_edid_alloc(raw, bytes);
	if (!edid)
		return -ENOMEM;
	if (!drm_edid_valid(edid)) {
		drm_edid_free(edid);
		return -EINVAL;
	}
	if (native) {
		native_copy = kmemdup(native, count * sizeof(*native), GFP_KERNEL);
		if (!native_copy) {
			drm_edid_free(edid);
			return -ENOMEM;
		}
	}
	mutex_lock(&c->base.dev->mode_config.mutex);
	mutex_lock(&c->lock);
	if (!c->connected || c->generation != generation) {
		ret = -ESTALE;
	} else if (c->edid) {
		/* A new snapshot requires invalidation, even on the same cable. */
		ret = -EALREADY;
	} else {
		c->edid = edid;
		edid = NULL;
		if (native) {
			c->native_modes = native_copy;
			native_copy = NULL;
			c->native_modes_count = count;
			c->native_generation = native_generation;
		} else {
			memcpy(c->timings, timings, count * 24);
			c->timings_count = count;
		}
	}
	mutex_unlock(&c->lock);
	mutex_unlock(&c->base.dev->mode_config.mutex);
	drm_edid_free(edid);
	kfree(native_copy);
	return ret;
}

int m3_dcpext_connector_publish(struct m3_dcpext_connector *c, u64 generation,
			      const u8 *raw, u32 bytes, const u8 *timings, u32 count)
{
	return publish_snapshot(c, generation, raw, bytes, timings, count, NULL, 0);
}

int m3_dcpext_connector_publish_native(struct m3_dcpext_connector *c,
				     u64 generation, const u8 *raw, u32 bytes,
				     const struct m3_dcpext_native_mode *modes,
				     u32 count, u64 native_generation)
{
	if (!modes)
		return -EINVAL;
	return publish_snapshot(c, generation, raw, bytes, NULL, count, modes, native_generation);
}

void m3_dcpext_connector_hotplug(struct m3_dcpext_connector *c)
{
	drm_kms_helper_connector_hotplug_event(&c->base);
}

int m3_dcpext_connector_commit_mode(struct m3_dcpext_connector *c,
				  const struct drm_connector_state *state,
				  const struct drm_display_mode *mode,
				  struct m3_dcpext_mode *geometry, u8 timing[24])
{
	const struct m3_connector_state *s;
	int ret;

	if (!state || state->connector != &c->base || !state->crtc || !mode || !geometry || !timing)
		return -EINVAL;
	s = container_of(state, struct m3_connector_state, base);
	mutex_lock(&c->lock);
	if (!c->connected || !c->edid || s->generation != c->generation)
		ret = -ESTALE;
	else if (c->native_modes)
		ret = -EOPNOTSUPP;
	else if (!mode_known(c, mode))
		ret = -EINVAL;
	else {
		ret = select_mode(c, mode, timing);
		if (!ret)
			mode_geometry(mode, geometry);
	}
	mutex_unlock(&c->lock);
	return ret;
}

int m3_dcpext_connector_commit_native(struct m3_dcpext_connector *c,
				     const struct drm_connector_state *state,
				     const struct drm_display_mode *mode,
				     struct m3_dcpext_native_mode *selected,
				     u64 *native_generation)
{
	const struct m3_connector_state *s;
	u32 index;
	int ret;

	if (!state || state->connector != &c->base || !state->crtc ||
	    !mode || !selected || !native_generation)
		return -EINVAL;
	s = container_of(state, struct m3_connector_state, base);
	mutex_lock(&c->lock);
	if (!c->connected || !c->edid || s->generation != c->generation)
		ret = -ESTALE;
	else if (!c->native_modes || !mode_known(c, mode))
		ret = -EINVAL;
	else {
		ret = select_native_mode(c, mode, &index);
		if (!ret) {
			*selected = c->native_modes[index];
			*native_generation = c->native_generation;
		}
	}
	mutex_unlock(&c->lock);
	return ret;
}

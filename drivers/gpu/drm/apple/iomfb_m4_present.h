/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* The T8132 panel's single-surface profile on common DMA GEM objects. */
#ifndef __APPLE_IOMFB_M4_PRESENT_H__
#define __APPLE_IOMFB_M4_PRESENT_H__

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_plane.h>

#include "dcp-m4-frame.h"

static inline int iomfb_m4_plane_frame(const struct drm_plane_state *state,
				       u32 width, u32 height,
				       struct dcp_m4_frame *frame)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_dma_object *obj;

	if (!fb || fb->format->num_planes != 1 || fb->offsets[0] ||
	    fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    (fb->format->format != DRM_FORMAT_XRGB8888 &&
	     fb->format->format != DRM_FORMAT_ARGB8888) ||
	    state->src_x || state->src_y || state->crtc_x || state->crtc_y ||
	    state->src_w != (u64)width << 16 || state->src_h != (u64)height << 16 ||
	    state->crtc_w != width || state->crtc_h != height)
		return -EINVAL;
	obj = drm_fb_dma_get_gem_obj(fb, 0);
	if (!obj)
		return -EINVAL;
	return dcp_m4_direct_frame(frame, obj->dma_addr, obj->base.size,
				   fb->width, fb->height, fb->pitches[0],
				   fb->format->format == DRM_FORMAT_ARGB8888);
}

#endif

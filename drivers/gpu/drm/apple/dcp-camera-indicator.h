/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Kernel-drawn camera-in-use indicator on the internal panel.
 *
 * A software indicator: see drivers/soc/apple/camera-indicator.c.
 */

#ifndef __APPLE_DCP_CAMERA_INDICATOR_H__
#define __APPLE_DCP_CAMERA_INDICATOR_H__

#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "iomfb_plane.h"

struct apple_dcp;
struct drm_device;
struct drm_gem_dma_object;
struct dcp_swap_submit_req_h17p;
struct dcp_present_h17p;

/* Square ARGB8888 image; its 192-byte pitch keeps DCP's 64-byte alignment. */
#define DCP_CAMIND_SIZE		48
#define DCP_CAMIND_PITCH	(DCP_CAMIND_SIZE * 4)
/* Distance from the top right corner of the area below any notch. */
#define DCP_CAMIND_MARGIN	16
/* While shown, the panel stays at least this lit, in tenths of its maximum. */
#define DCP_CAMIND_FLOOR_TENTHS	1

struct dcp_camind {
	struct drm_gem_dma_object *obj;
	struct dcp_surface surf;
	struct delayed_work work;
	unsigned int slot;
	bool registered;

	/* Under the IOMFB lock: */
	bool want;		/* the camera asked for it */
	bool clear;		/* the next present takes it off */
	bool floor;		/* the backlight is held at the floor */
	bool carrying;		/* the present in flight draws it */
	u32 carry_id;
	u32 carry_epoch;
	u32 mode_w;
	u32 mode_h;

	/* Lockless: */
	atomic_t epoch;		/* bumped on every loss */
	atomic_t confirmed;	/* a present drawing it completed since */
};

bool dcp_camind_eligible(struct apple_dcp *dcp, const unsigned long *surfaces);
int dcp_camind_attach(struct apple_dcp *dcp, struct drm_device *drm);
void dcp_camind_mode(struct apple_dcp *dcp, u32 width, u32 height);
void dcp_camind_prepare(struct apple_dcp *dcp,
			struct dcp_swap_submit_req_h17p *req, u32 swap_id);
void dcp_camind_complete(struct apple_dcp *dcp, u32 swap_id);
void dcp_camind_backlight(struct apple_dcp *dcp, struct dcp_present_h17p *wire,
			  u32 nits);
void dcp_camind_lost(struct apple_dcp *dcp);
bool dcp_camind_pending(struct apple_dcp *dcp);

/* Exposed for KUnit. */
void dcp_camind_draw(u32 *pixels);
void dcp_camind_surface(struct dcp_surface *surf, bool element);
bool dcp_camind_place(u32 mode_w, u32 mode_h, u32 notch, struct dcp_rect *dst);

#endif /* __APPLE_DCP_CAMERA_INDICATOR_H__ */

/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * DCP_FIRMWARE_IBOOT: a T8152 DCP driven through its firmware's iBoot
 * interface alone, with a DRM device of its own (iomfb_iboot.c, iboot_drm.c).
 */
#ifndef __APPLE_IOMFB_IBOOT_H__
#define __APPLE_IOMFB_IBOOT_H__

#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/kconfig.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>

#include "ibootep.h"
#include "t8152_display.h"

struct apple_dcp;
struct device;
struct drm_edid;
struct iommu_domain;
struct platform_device;

struct dcp_iboot_dma {
	void *cpu;
	dma_addr_t iova;
	size_t size;
};

/*
 * Scanout buffers, each for the largest mode: a frame is copied into one not
 * on screen, which SET_SURFACE then shows. Copying into the buffer being
 * scanned out tore every frame. Three, so the one written next left the
 * screen a frame ago.
 */
#define DCP_IBOOT_SCANOUT_BUFS 3

struct dcp_iboot {
	struct drm_device drm;
	struct apple_dcp *dcp;
	struct t8152_display disp;

	/* The DRM device is registered and the session up. */
	bool ready;
	bool kms_work_ready;

	/* The scanout DART's own device and domain (dcp_iboot_scanout_init()). */
	struct platform_device *scanout;
	struct iommu_domain *scanout_dom;
	struct dcp_iboot_dma scan[DCP_IBOOT_SCANOUT_BUFS];
	/* their pages write-combined: the pipe does not snoop the CPU caches */
	void *scan_wc[DCP_IBOOT_SCANOUT_BUFS];
	/* their addresses in the display pipe's DART */
	dma_addr_t scan_dva[DCP_IBOOT_SCANOUT_BUFS];
	/* scan[front] is on screen (or set as the surface at the next power on) */
	unsigned int front;
	/* the part of the current picture scan[i] does not hold yet */
	struct drm_rect stale[DCP_IBOOT_SCANOUT_BUFS];
	/* when scan[i] stopped being the requested surface */
	ktime_t retired[DCP_IBOOT_SCANOUT_BUFS];
	/* the last SET_SURFACE of a new buffer */
	ktime_t last_swap;
	/*
	 * A frame written within one refresh of the last swap waits in
	 * scan[pending] (-1: none) for swap_timer, at most one swap per
	 * refresh; a newer frame overwrites it.
	 */
	int pending;
	u32 pending_w, pending_h, pending_pitch;
	struct hrtimer swap_timer;
	struct work_struct swap_work;
	u32 boot_w, boot_h, boot_stride;

	/* the sink's EDID, read through the PHY (NULL if unavailable) */
	const struct drm_edid *edid;

	/* iBoot state below, under lock */
	struct mutex lock;
	bool enabled;
	bool powered;
	struct iboot_timing_mode *tmodes;
	unsigned int nr_tmodes;
	struct iboot_color_mode *cmodes;
	unsigned int nr_cmodes;
	struct iboot_timing_mode cur_mode;
	bool mode_set;
	/* SET_MODE of cur_mode failed; the next plug sets it again */
	bool mode_failed;
	/* USB-C: connect the sink anew when it stopped answering */
	struct work_struct relink_work;
	unsigned long relink_time;
	/*
	 * The DPAV video interface started: SET_MODE again, or the
	 * firmware's idle power off timer takes the link down 30 s later.
	 */
	struct delayed_work av_work;
	struct delayed_work hpd_work;

	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

static inline struct dcp_iboot *to_dcp_iboot(struct drm_device *drm)
{
	return container_of(drm, struct dcp_iboot, drm);
}

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T8152)
/* t8152_typec.c */
void t8152_typec_register(struct device *dev);
void t8152_typec_unregister(void);

/* iomfb_iboot.c */
int dcp_iboot_probe(struct apple_dcp *dcp);
void dcp_iboot_remove(struct apple_dcp *dcp);
int dcp_iboot_wait_hpd(struct dcp_iboot *ib);
int dcp_iboot_get_modes(struct dcp_iboot *ib);

/* iboot_drm.c */
struct dcp_iboot *dcp_iboot_alloc(struct device *dev);
const char *dcp_iboot_driver_name(void);
int dcp_iboot_kms_init(struct dcp_iboot *ib);
void dcp_iboot_kms_stop(struct dcp_iboot *ib);
int dcp_iboot_set_boot_mode(struct dcp_iboot *ib);
void dcp_iboot_update_edid(struct dcp_iboot *ib);
void dcp_iboot_reset_mode(struct dcp_iboot *ib);
void dcp_iboot_av_started(struct dcp_iboot *ib);
void dcp_iboot_hpd_event(struct dcp_iboot *ib);
#else
static inline void t8152_typec_register(struct device *dev) { }
static inline void t8152_typec_unregister(void) { }
static inline int dcp_iboot_probe(struct apple_dcp *dcp) { return -ENODEV; }
static inline void dcp_iboot_remove(struct apple_dcp *dcp) { }
#endif

#endif

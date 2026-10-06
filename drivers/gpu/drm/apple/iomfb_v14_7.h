/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef __APPLE_IOMFB_V14_7_H__
#define __APPLE_IOMFB_V14_7_H__

#include <linux/types.h>

struct apple_dcp;
struct apple_dcp_v14;
struct drm_atomic_state;
struct drm_crtc;
struct drm_crtc_state;

/* Platform probe: firmware identity, boot loader handoff, PMP acknowledgment. */
int iomfb_v14_7_probe(struct apple_dcp *dcp);
/* Component bind: the running DCP's RTKit session. */
int iomfb_v14_7_bind(struct apple_dcp *dcp);
int iomfb_v14_7_external_start(struct apple_dcp *dcp);
/*
 * Native external processor: probe-time checks, then a firmware start on
 * first use. _ready() is true once DPTX can be used, and requests the start
 * if nothing has yet; _open() opens the display interface after the first
 * display is attached; _busy() vetoes system sleep.
 */
int iomfb_v14_7_external_prepare(struct apple_dcp *dcp);
bool iomfb_v14_7_external_ready(struct apple_dcp *dcp);
int iomfb_v14_7_external_open(struct apple_dcp *dcp);
bool iomfb_v14_7_external_busy(struct apple_dcp *dcp);
/* Component unbind: KMS goes away; the firmware session is kept until reboot. */
void iomfb_v14_7_unbind(struct apple_dcp *dcp);
/* Platform remove: the apple_dcp is freed next; the firmware session is kept. */
void iomfb_v14_7_remove(struct apple_dcp *dcp);
/* DCPLink, start signal, first client open and the panel mode. */
int iomfb_v14_7_start(struct apple_dcp *dcp);

int iomfb_v14_7_atomic_check(struct apple_dcp *dcp, struct drm_crtc *crtc,
			     struct drm_atomic_state *state);
int iomfb_v14_7_modeset(struct apple_dcp *dcp, struct drm_crtc_state *crtc_state);
void iomfb_v14_7_flush(struct apple_dcp *dcp, struct drm_crtc *crtc,
		       struct drm_atomic_state *state);
void iomfb_v14_7_poweron(struct apple_dcp *dcp);
void iomfb_v14_7_poweroff(struct apple_dcp *dcp);

#endif /* __APPLE_IOMFB_V14_7_H__ */

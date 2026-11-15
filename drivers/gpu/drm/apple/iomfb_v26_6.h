/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * J613 (M3 Air 13 inch) internal display: macOS 26.6.2 (25G83) IOMFB over DCPLink
 *
 * Copyright The Asahi Linux Contributors
 */

#ifndef __APPLE_IOMFB_V26_6_H__
#define __APPLE_IOMFB_V26_6_H__

#include <linux/types.h>

struct apple_dcp;
struct apple_dcp_v26;
struct dcp_v26_board;
struct device;
struct drm_atomic_commit;
struct drm_crtc;
struct drm_crtc_state;

/*
 * The board of a 25G83 IOMFB DCP node (by compatible and machine): NULL if the
 * node is not one, ERR_PTR(-ENODEV) if the machine is not a known board.
 */
const struct dcp_v26_board *iomfb_v26_6_board(struct device *dev);
const char *iomfb_v26_6_board_name(const struct dcp_v26_board *board);

/* Platform probe: firmware identity, boot loader handoff, PMP acknowledgment. */
int iomfb_v26_6_probe(struct apple_dcp *dcp);
/* Component bind: the running DCP's RTKit session. */
int iomfb_v26_6_bind(struct apple_dcp *dcp);
/* Component unbind: KMS goes away; the firmware session is kept until reboot. */
void iomfb_v26_6_unbind(struct apple_dcp *dcp);
/* DCPLink, start signal, first client open and the panel mode. */
int iomfb_v26_6_start(struct apple_dcp *dcp);

/*
 * The PMP device-power API (include/linux/soc/apple/pmp.h, exported by the
 * PMP driver). Declared here until that header is in this tree; the backend
 * resolves both with symbol_get() and fails closed without a provider.
 */
#ifndef _LINUX_SOC_APPLE_PMP_H
int apple_pmp_link_device(struct device *consumer);
int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled);
#endif

int iomfb_v26_6_atomic_check(struct apple_dcp *dcp, struct drm_crtc *crtc,
			     struct drm_atomic_commit *state);
int iomfb_v26_6_modeset(struct apple_dcp *dcp, struct drm_crtc_state *crtc_state);
void iomfb_v26_6_flush(struct apple_dcp *dcp, struct drm_crtc *crtc,
		       struct drm_atomic_commit *state);
void iomfb_v26_6_poweron(struct apple_dcp *dcp);
void iomfb_v26_6_poweroff(struct apple_dcp *dcp);

#endif /* __APPLE_IOMFB_V26_6_H__ */

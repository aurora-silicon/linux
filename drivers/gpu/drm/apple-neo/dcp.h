// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#ifndef __APPLE_DCP_H__
#define __APPLE_DCP_H__

#include <drm/drm_atomic.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>

#include "connector.h"
#include "dcp-internal.h"
#include "parser.h"

struct neo_apple_crtc {
	struct drm_crtc base;
	struct drm_pending_vblank_event *event;
	bool vsync_disabled;

	/* Reference to the DCP device owning this CRTC */
	struct platform_device *neo_dcp;
};

#define to_apple_crtc(x) container_of(x, struct neo_apple_crtc, base)

struct apple_encoder {
	struct drm_encoder base;
};

#define to_apple_encoder(x) container_of(x, struct apple_encoder, base)

void neo_dcp_poweroff(struct platform_device *pdev);
void neo_dcp_poweron(struct platform_device *pdev);
int neo_dcp_set_crc(struct drm_crtc *crtc, bool enabled);
int neo_dcp_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state);
int neo_dcp_get_connector_type(struct platform_device *pdev);
bool neo_dcp_has_typec_routes(struct platform_device *pdev);

/*
 * The Type-C display fabric.  Ports are enumerated in device-tree order, not
 * DCP probe order, so a given physical port keeps the same DRM connector index
 * across boots -- userspace keys its per-monitor configuration on that name.
 */
unsigned int neo_dcp_typec_nr_ports(void);
struct device_node *neo_dcp_typec_port_of_node(unsigned int idx);
bool neo_dcp_typec_port_has_candidate(unsigned int idx, struct platform_device *pdev);
void neo_dcp_typec_port_set_connector(unsigned int idx, bool secondary,
				  struct neo_apple_connector *connector);
bool neo_dcp_typec_dual_stream(void);
void neo_dcp_typec_reorder(void);
bool neo_dcp_is_typec_only(struct platform_device *pdev);
bool neo_dcp_fw_compat_is_12_x(struct platform_device *pdev);
unsigned long* neo_dcp_get_iomfb_surfaces(struct platform_device *pdev);
void neo_dcp_link(struct platform_device *pdev, struct neo_apple_crtc *apple,
	      struct neo_apple_connector *connector);
int neo_dcp_start(struct platform_device *pdev);
int neo_dcp_wait_ready(struct platform_device *pdev, u64 timeout);
void neo_dcp_flush(struct drm_crtc *crtc, struct drm_atomic_state *state);
void neo_dcp_retain_framebuffer(struct platform_device *pdev,
			    struct neo_dcp_fb_reference *entry);
void neo_dcp_arm_retained_framebuffers(struct neo_apple_dcp *neo_dcp, u32 swap_id);
void neo_dcp_unarm_retained_framebuffers(struct neo_apple_dcp *neo_dcp, u32 swap_id);
void neo_dcp_release_retained_framebuffers(struct neo_apple_dcp *neo_dcp, u32 swap_id);
void neo_dcp_release_all_retained_framebuffers(struct neo_apple_dcp *neo_dcp);
bool neo_dcp_is_initialized(struct platform_device *pdev);
void neo_apple_crtc_vblank(struct neo_apple_crtc *apple);
void neo_dcp_drm_crtc_vblank(struct neo_apple_crtc *crtc);
int neo_dcp_get_modes(struct drm_connector *connector);
enum drm_mode_status neo_dcp_mode_valid(struct drm_connector *connector,
				    const struct drm_display_mode *mode);
int neo_dcp_crtc_atomic_modeset(struct drm_crtc *crtc,
			    struct drm_atomic_state *state);
bool neo_dcp_crtc_mode_fixup(struct drm_crtc *crtc,
			 const struct drm_display_mode *mode,
			 struct drm_display_mode *adjusted_mode);
void neo_dcp_set_dimensions(struct neo_apple_dcp *neo_dcp);
void neo_dcp_send_message(struct neo_apple_dcp *neo_dcp, u8 endpoint, u64 message);

int neo_dcp_dptx_connect_oob(struct platform_device *pdev, u32 port);
int neo_dcp_dptx_disconnect_oob(struct platform_device *pdev, u32 port);

struct neo_apple_dcp;
struct phy;
/* Thunderbolt DP tunnels, called from the DPTX endpoint */
int neo_dcp_tunnel_crossbar_up(struct neo_apple_dcp *neo_dcp);
int neo_dcp_tunnel_crossbar_down(struct neo_apple_dcp *neo_dcp);
int neo_dcp_tunnel_set_rate(struct neo_apple_dcp *neo_dcp, struct phy *phy, u32 link_rate);
int neo_dcp_tunnel_dpin_activate(struct neo_apple_dcp *neo_dcp, bool active);

int neo_iomfb_start_rtkit(struct neo_apple_dcp *neo_dcp);
void neo_iomfb_shutdown(struct neo_apple_dcp *neo_dcp);
/* rtkit message handler for IOMFB messages */
void neo_iomfb_recv_msg(struct neo_apple_dcp *neo_dcp, u64 message);

int neo_systemep_init(struct neo_apple_dcp *neo_dcp);
int dptxep_init(struct neo_apple_dcp *neo_dcp);
int neo_ibootep_init(struct neo_apple_dcp *neo_dcp);
int dpavservep_init(struct neo_apple_dcp *neo_dcp);
int neo_avep_init(struct neo_apple_dcp *neo_dcp);


void __init neo_dcp_register(void);
void __exit neo_dcp_unregister(void);

void __init neo_dcp_audio_register(void);
void __exit neo_dcp_audio_unregister(void);

#endif

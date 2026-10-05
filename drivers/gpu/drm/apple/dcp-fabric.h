/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCP_FABRIC_H__
#define __APPLE_DCP_FABRIC_H__

#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/types.h>

#include "dcp-fabric-core.h"

struct apple_connector;
struct apple_crtc;
struct device_node;
struct drm_device;
struct mux_control;
struct platform_device;
struct phy;
struct typec_mux_dev;
struct work_struct;

#define DCP_MAX_TYPEC_ROUTES 4

struct apple_dcp;
struct apple_dcp_typec_port;

struct apple_dcp_typec_route {
	struct dcp_fabric_route core;
	struct apple_dcp *dcp;
	struct apple_dcp_typec_port *port;
	struct list_head port_link;
	struct phy *phy;
	struct mux_control *xbar;
	struct mux_control *dpin[2];
	bool dual_stream;
	struct typec_mux_dev *typec_mux;
	u32 dptx_phy;
	u32 mux_index;
	bool selected;
	/* crossbar output actually selected: xbar (dpphy) or a Thunderbolt dpin */
	unsigned int tunnel_dpin;
	struct mux_control *active_xbar;
	bool tunnel;
	/* tunnel: crossbar brought up (at DidChangeLinkConfiguration) */
	bool xbar_up;
};

bool dcp_is_typec_output(struct apple_dcp *dcp);
bool dcp_is_usb4_output(struct apple_dcp *dcp);
bool dcp_uses_t6020_tunnel_flow(struct apple_dcp *dcp);

bool dcp_has_typec_routes(struct platform_device *pdev);

/*
 * The Type-C display fabric.  Ports are enumerated in device-tree order, not
 * DCP probe order, so a given physical port keeps the same DRM connector index
 * across boots -- userspace keys its per-monitor configuration on that name.
 */
unsigned int dcp_typec_nr_ports(void);
struct device_node *dcp_typec_port_of_node(unsigned int idx);
bool dcp_typec_port_has_candidate(unsigned int idx, struct platform_device *pdev);
void dcp_typec_port_set_connector(unsigned int idx, bool secondary,
				  struct apple_connector *connector);
bool dcp_typec_dual_stream(void);
void dcp_typec_reorder(void);
bool dcp_is_typec_only(struct platform_device *pdev);
void dcp_link(struct platform_device *pdev, struct apple_crtc *apple,
	      struct apple_connector *connector);
void dcp_unlink(struct drm_device *drm);

/* Thunderbolt DP tunnels, called from the DPTX endpoint */
int dcp_tunnel_crossbar_up(struct apple_dcp *dcp);
int dcp_tunnel_crossbar_down(struct apple_dcp *dcp);
int dcp_tunnel_set_rate(struct apple_dcp *dcp, struct phy *phy, u32 link_rate);
int dcp_tunnel_dpin_activate(struct apple_dcp *dcp, bool active);

/* DCP probe, resume and endpoint teardown hooks. */
int dcp_register_typec_routes(struct apple_dcp *dcp);
void dcp_typec_retrain_work(struct work_struct *work);
void dcp_fabric_init(struct apple_dcp *dcp);
void dcp_fabric_hdmi_resume(struct apple_dcp *dcp);
irqreturn_t dcp_dp2hdmi_hpd_edge(int irq, void *data);
irqreturn_t dcp_dp2hdmi_hpd(int irq, void *data);
void dcp_fabric_shutdown_dptx(struct apple_dcp *dcp);

/* DPTX operations supplied by the DCP firmware layer. */
int dcp_dptx_connect(struct apple_dcp *dcp, u32 port);
int dcp_dptx_disconnect(struct apple_dcp *dcp, u32 port);

#endif /* __APPLE_DCP_FABRIC_H__ */

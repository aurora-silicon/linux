// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <kunit/visibility.h>
#include <linux/align.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/device/driver.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/property.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/apple/pmp-display.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/string.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/workqueue.h>

#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_module.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "afk.h"
#include "av.h"
#include "dcp.h"
#include "dcp-internal.h"
#include "dcp-external-bw.h"
#include "dptx-service.h"
#include "dcp-property-store.h"
#include "iomfb.h"
#include "parser.h"
#include "trace.h"

#define APPLE_DCP_COPROC_CPU_CONTROL	 0x44
#define APPLE_DCP_COPROC_CPU_CONTROL_RUN BIT(4)
#define APPLE_DCP_COPROC_CPU_STATUS 0x48
#define APPLE_DCP_COPROC_CPU_STATUS_STOPPED BIT(1)
#define APPLE_DCP_COPROC_CPU_STATUS_IDLE BIT(5)

struct dcp_session_ref {
	struct list_head link;
	struct device *dev;
	struct iommu_group *group;
};

static LIST_HEAD(dcp_retained_sessions);
static DEFINE_MUTEX(dcp_retained_sessions_lock);

static bool dcp_session_retained(struct device *dev)
{
	struct dcp_session_ref *session;
	bool retained = false;

	mutex_lock(&dcp_retained_sessions_lock);
	list_for_each_entry(session, &dcp_retained_sessions, link) {
		if (dev_fwnode(session->dev) == dev_fwnode(dev)) {
			retained = true;
			break;
		}
	}
	mutex_unlock(&dcp_retained_sessions_lock);
	return retained;
}

static int dcp_pin_live_session(struct apple_dcp *dcp)
{
	struct dcp_session_ref *session;

	if ((!dcp->hw.adopt_live_session &&
	     dcp->hw.iomfb_method_profile !=
	     DCP_IOMFB_METHODS_H17P_EXTERNAL) ||
	    !dcp_fail_stop(dcp) ||
	    dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17G || dcp->retain_dma)
		return 0;
	session = kzalloc_obj(*session);
	if (!session)
		return -ENOMEM;

	/* Keep the DMA device, domain usage and driver code valid until reset. */
	session->dev = get_device(dcp->dev);
	session->group = iommu_group_get(dcp->dev);
	__module_get(THIS_MODULE);
	WRITE_ONCE(dcp->retain_dma, true);
	mutex_lock(&dcp_retained_sessions_lock);
	list_add_tail(&session->link, &dcp_retained_sessions);
	mutex_unlock(&dcp_retained_sessions_lock);
	dev_dbg(dcp->dev, "live H17P DMA resources retained until reboot\n");
	return 0;
}

static void dcp_release_dma_domain(void *data)
{
	struct apple_dcp *dcp = data;

	if (!dcp->retain_dma)
		iommu_device_unuse_default_domain(dcp->dev);
}

static void dcp_release_context(void *data)
{
	struct apple_dcp *dcp = data;

	if (!dcp->retain_dma)
		kfree(dcp);
}

static void dcp_release_rtkit(void *data)
{
	struct apple_dcp *dcp = data;

	if (dcp->retain_dma) {
		WRITE_ONCE(dcp->quiescing, true);
		apple_rtkit_free_retaining_buffers(dcp->rtk);
	} else {
		apple_rtkit_free(dcp->rtk);
	}
	dcp->rtk = NULL;
}

#define DCP_BOOT_TIMEOUT msecs_to_jiffies(1000)

bool dcp_h17p_external_supported(struct device_node *np)
{
	u32 firmware[DCP_EXTERNAL_FW_CELLS], compat[DCP_EXTERNAL_FW_CELLS];
	int firmware_cells, compat_cells;

	if (!of_machine_is_compatible("apple,j700") ||
	    !of_device_is_compatible(np, "apple,t8140-dcpext"))
		return false;
	firmware_cells = of_property_read_variable_u32_array(
		np, "apple,firmware-version", firmware,
		DCP_EXTERNAL_FW_CELLS, DCP_EXTERNAL_FW_CELLS);
	compat_cells = of_property_read_variable_u32_array(
		np, "apple,firmware-compat", compat,
		DCP_EXTERNAL_FW_CELLS, DCP_EXTERNAL_FW_CELLS);
	return apple_dcp_external_firmware_supported(firmware, firmware_cells,
						    compat, compat_cells);
}

bool dcp_h17p_external_ready(struct device_node *np)
{
	struct platform_device *pdev;
	struct apple_dcp *dcp;
	bool ready = false;

	if (!dcp_h17p_external_supported(np))
		return false;
	pdev = of_find_device_by_node(np);
	if (!pdev)
		return false;
	/*
	 * Called after component binding. component_add() may synchronously
	 * bind the master before the platform probe itself returns, so the
	 * driver core's bound flag is not a readiness condition here.
	 */
	dcp = platform_get_drvdata(pdev);
	ready = dcp && READ_ONCE(dcp->external_prepared) &&
		!READ_ONCE(dcp->quiescing) && !READ_ONCE(dcp->crashed);
	put_device(&pdev->dev);
	return ready;
}

static bool show_notch;
module_param(show_notch, bool, 0644);
MODULE_PARM_DESC(show_notch, "Use the full display height and shows the notch");

static bool panel_hw_off = true;
module_param(panel_hw_off, bool, 0644);
MODULE_PARM_DESC(panel_hw_off, "J514S panel: stop the display controller (A472 hardware power) on DPMS off and system sleep instead of only blanking it [default: true]");

bool hdmi_audio;
module_param(hdmi_audio, bool, 0644);
MODULE_PARM_DESC(hdmi_audio, "Enable unstable HDMI audio support");

static bool unstable_edid = true;
module_param(unstable_edid, bool, 0644);
MODULE_PARM_DESC(unstable_edid, "Enable unstable EDID retrival support");

struct apple_dcp_typec_port {
	struct list_head link;
	struct list_head routes;
	struct device_node *connector_np;
	struct apple_dcp_typec_route *owner;
	struct apple_dcp_typec_route *secondary_owner;
	/* Keep a port on its last DCP while that pipeline remains free. */
	struct apple_dcp_typec_route *preferred_route;
	/* DRM connector for this physical port, driven by whichever DCP owns it */
	struct apple_connector *connector;
	/* A second logical stream through this port's USB4 dock. */
	struct apple_connector *secondary_connector;
	/* last mux state acted on, to collapse the per-candidate notifications */
	struct typec_altmode *applied_alt;
	unsigned long applied_mode;
	u32 applied_status;
	u32 applied_conf;
	bool applied_valid;
	/* SAFE preparation retains this owner until the set phase completes. */
	bool safe_pending;
	bool safe_finish_ready;
	bool hpd;
	/* Direct DP-alt: the port is in DP mode, routed or not, and its HPD. */
	bool dp_wanted;
	bool dp_hpd;
	/* dcp_typec_rebalance_locked()'s plan for the port's two streams */
	struct apple_dcp_typec_route *target;
	struct apple_dcp_typec_route *secondary_target;
	/* left out of the plan: its planned pipeline is held by a tunnel */
	bool plan_dark;
};

static DEFINE_MUTEX(dcp_typec_fabric_lock);
static LIST_HEAD(dcp_typec_ports);

static int dcp_dptx_connect(struct apple_dcp *dcp, u32 port);
static void disconnected_hpd_event(struct apple_connector *connector);

bool dcp_is_typec_output(struct apple_dcp *dcp)
{
	return dcp->active_typec_route ||
	       dcp->fixed_connector_type == DRM_MODE_CONNECTOR_USB;
}

bool dcp_is_usb4_output(struct apple_dcp *dcp)
{
	return dcp->active_typec_route && dcp->active_typec_route->tunnel;
}

static bool dcp_typec_route_is_dp(const struct typec_mux_state *state)
{
	return state->alt && state->alt->svid == USB_TYPEC_DP_SID &&
	       state->mode >= TYPEC_DP_STATE_A &&
	       state->mode <= TYPEC_DP_STATE_F;
}

/* Keep a live fixed output on its own pipeline. */
static bool dcp_typec_route_fixed_output_busy(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;

	if (dcp->fixed_connector_type == DRM_MODE_CONNECTOR_USB)
		return false;
	if (dcp->fixed_connector && dcp->fixed_connector->connected)
		return true;
	if (dcp->hdmi_hpd && gpiod_get_value_cansleep(dcp->hdmi_hpd))
		return true;

	return false;
}

static bool dcp_typec_route_available(struct apple_dcp_typec_route *route)
{
	struct apple_dcp_typec_port *port;

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (port->safe_pending && port->owner &&
		    port->owner->dcp == route->dcp)
			return false;
	}
	return !route->dcp->active_typec_route &&
	       !dcp_typec_route_fixed_output_busy(route);
}

/*
 * Machines whose USB4 docks carry two independent DP streams through one
 * port: DPIN0 drives the hybrid dcpext0, DPIN1 the Type-C-only dcpext1, and
 * each port has a second connector for the DPIN1 stream.
 */
bool dcp_typec_dual_stream(void)
{
	return apple_dp_tunnel_t602x();
}

bool dcp_is_typec_only(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return !dcp->fixed_phy;
}

/*
 * Can this route feed the given connector without changing its
 * possible_crtcs?  On dual-stream machines those are fixed at probe, since
 * compositors read them once when the connector appears and pair the
 * connector with a CRTC before this fabric has routed it.
 */
static bool dcp_typec_route_fits(struct apple_dcp_typec_route *route,
				 struct apple_connector *connector)
{
	struct apple_dcp *dcp = route->dcp;

	if (!dcp_typec_dual_stream() || !connector || !dcp->crtc)
		return true;
	return connector->candidate_crtcs & drm_crtc_mask(&dcp->crtc->base);
}

static bool dcp_is_external_h17p(struct apple_dcp *dcp)
{
	return dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL;
}

static int dcp_external_stop_locked(struct apple_dcp *dcp, u64 generation);
static int dcp_external_disconnect(struct apple_dcp *dcp, bool physical);

static int dcp_typec_route_activate(struct apple_dcp_typec_route *route,
				    struct mux_control *xbar);
static int dcp_typec_route_deactivate(struct apple_dcp_typec_route *route);
static int dcp_dpxbar_preselect(struct mux_control *mux, int state);
static int dcp_dptx_disconnect(struct apple_dcp *dcp, u32 port);

static int dcp_dpxbar_tunnel_select_source(struct mux_control *mux, int state)
{
	typeof(&apple_dpxbar_tunnel_select_source) select =
		symbol_get(apple_dpxbar_tunnel_select_source);
	int ret;

	if (!select)
		return -ENOENT;
	ret = select(mux, state);
	symbol_put(apple_dpxbar_tunnel_select_source);
	return ret;
}

/*
 * For a port without a prior owner, rank pipelines by CRTC index. A pipeline
 * whose fixed output is live is not a candidate at all, so a hybrid is only
 * ever ranked here when it is genuinely free.
 */
static unsigned int dcp_typec_route_score(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;

	if (!dcp->crtc)
		return UINT_MAX - 1;

	return drm_crtc_index(&dcp->crtc->base);
}

static int dcp_typec_route_activate(struct apple_dcp_typec_route *route,
				    struct mux_control *xbar)
{
	struct apple_dcp *dcp = route->dcp;
	struct apple_connector *connector =
		xbar != route->xbar && route->tunnel_dpin == 1 ?
		route->port->secondary_connector : route->port->connector;
	int ret;

	dcp_firmware_metadata_connection(dcp, READ_ONCE(dcp->typec_generation), false);

	/*
	 * The fixed output's HPD handler leaves disconnects to DCP, so the port
	 * can still be marked connected to a display that is gone. Release it
	 * (a no-op otherwise), or connecting the borrowed route returns early.
	 */
	ret = dcp_dptx_disconnect(dcp, 0);
	if (ret)
		return ret;

	if (dcp->fixed_route_selected) {
		ret = mux_control_deselect(dcp->xbar);
		dcp->fixed_route_selected = false;
		if (ret)
			return ret;
	}

	/*
	 * Thunderbolt DP IN: the crossbar connection may only be brought up
	 * once DCP has configured the link and the tunnel pixel clock runs,
	 * see dcp_tunnel_crossbar_up(). Just remember the output here.
	 */
	if (xbar != route->xbar && route->mux_index) {
		/*
		 * The T602X crossbar must point a DP IN at its pipeline before
		 * DCP probes AUX; other crossbars select it at link-up and
		 * report -EOPNOTSUPP here.
		 */
		ret = dcp_dpxbar_tunnel_select_source(xbar, route->mux_index);
		if (ret == -EOPNOTSUPP)
			ret = 0;
	} else if (iomfb_is_v14_7_external(dcp)) {
		/* PHY DP hold first, then the crossbar at rate 0. */
		ret = xbar == route->xbar ? dcp_ext_v14_7_route_select(route) : -EINVAL;
	} else {
		ret = xbar == route->xbar ?
			mux_control_select(xbar, route->mux_index) : 0;
	}
	if (ret) {
		if (dcp->xbar) {
			int restore_ret;

			restore_ret = mux_control_select(dcp->xbar,
							 dcp->fixed_mux_index);
			if (!restore_ret)
				dcp->fixed_route_selected = true;
			else
				dev_err(dcp->dev,
					"failed to restore fixed display route: %d\n",
					restore_ret);
		}
		return ret;
	}

	dcp->phy = route->phy;
	dcp->dptx_phy = route->dptx_phy;
	dcp->connector_type = DRM_MODE_CONNECTOR_USB;
	if (connector) {
		WRITE_ONCE(connector->dcp, to_platform_device(dcp->dev));
		dcp->typec_connector = connector;
		dcp->connector = connector;

		/*
		 * Narrow the port to the pipeline now driving it.  The encoder
		 * spans every pipeline that could, which is what lets userspace
		 * see the port as usable at all -- but only one of them is
		 * routed to the display, and userspace has no way to tell which.
		 * Offering it the choice makes it pair the port with a pipeline
		 * holding a different monitor's mode list, and the modeset is
		 * rejected with no way for it to recover.  The hotplug that
		 * follows publishes the actual binding on dual-stream machines
		 * too. Clients must re-read encoder topology after a hotplug.
		 */
		if (connector->port_encoder && dcp->crtc)
			connector->port_encoder->possible_crtcs =
				drm_crtc_mask(&dcp->crtc->base);
	}
	dcp->active_typec_route = route;
	scoped_guard(mutex, &dcp->tb_lock) {
		route->deactivate_xbar_done = false;
		route->deactivate_error = 0;
		route->active_xbar = xbar;
		route->tunnel = xbar != route->xbar;
		route->xbar_up = !route->tunnel;
		dcp->dptx_tunnel = route->tunnel;
		/* crossbar controls are dpphy, dpin0, dpin1: same order as the DFP port */
		dcp->dptx_dfp_port = route->tunnel ? xbar - &route->xbar->chip->mux[0] : 0;
		dcp->tb_clock_ok = false;
	}
	route->selected = true;

	dev_info(dcp->dev, "allocated Type-C DPTX PHY %u\n", route->dptx_phy);
	return 0;
}

static int dcp_typec_route_deactivate(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;
	struct apple_connector *connector = dcp->typec_connector;
	struct mux_control *active_xbar = route->active_xbar;
	bool was_tunnel = route->tunnel;
	int ret = 0;

	guard(mutex)(&dcp->external_lock);
	if (dcp_is_external_h17p(dcp) &&
	    (dcp->external_error || (dcp->external_power_attempted && !dcp->external_stopped)))
		return dcp->external_error ?: -EAGAIN;
	/* 14.7 external: sink released and link stopped first; retryable. */
	if (iomfb_is_v14_7_external(dcp)) {
		ret = dcp_ext_v14_7_route_release(route);
		if (ret)
			return ret;
	}

	dcp_firmware_metadata_connection(dcp, READ_ONCE(dcp->typec_generation), false);

	/*
	 * Under tb_lock so a DCP link (re)configuration can neither select the
	 * crossbar nor restart the tunnel pixel clock behind our back.
	 */
	scoped_guard(mutex, &dcp->tb_lock) {
		if (route->deactivate_error)
			return route->deactivate_error;
		if (!route->deactivate_xbar_done) {
			if (iomfb_is_v14_7_external(dcp))
				ret = dcp_ext_v14_7_route_deselect(route);
			else if (!route->tunnel || route->xbar_up)
				ret = mux_control_deselect(route->active_xbar ?: route->xbar);
			else
				ret = dcp_dpxbar_preselect(route->active_xbar, MUX_IDLE_DISCONNECT);
			/* Deselect always unlocks: never consume that selection twice. */
			route->deactivate_xbar_done = true;
			route->xbar_up = false;
			if (ret) {
				/* Idle is unproven; retain the owner and the real error. */
				route->deactivate_error = ret;
				return ret;
			}
		}

		if (route->tunnel && dcp->phy) {
			/* the tunnel pixel clock must not outlive the tunnel */
			typeof(&apple_atc_dp_tunnel_rate) stop =
				symbol_get(apple_atc_dp_tunnel_rate);

			if (stop) {
				stop(dcp->phy, route->tunnel_dpin, 0);
				symbol_put(apple_atc_dp_tunnel_rate);
			}
		}
		route->active_xbar = NULL;
		route->tunnel = false;
		dcp->dptx_tunnel = false;
		dcp->dptx_dfp_port = 0;
		dcp->tb_dpin_set_active = NULL;
		dcp->tb_dpin_ctx = NULL;
		dcp->tb_clock_ok = false;
	}
	if (was_tunnel && route->mux_index) {
		int sel = dcp_dpxbar_tunnel_select_source(active_xbar, -1);

		if (sel && sel != -EOPNOTSUPP)
			dev_warn(dcp->dev, "DP tunnel source reset failed: %d\n", sel);
	}
	if (dcp_is_external_h17p(dcp))
		dpavservep_detach(dcp);
	route->selected = false;
	if (dcp->active_typec_route == route)
		dcp->active_typec_route = NULL;

	if (connector && connector->dcp == to_platform_device(dcp->dev)) {
		/*
		 * Until the port is activated again it has no pipeline behind
		 * it, and nothing can read modes or EDID from it.  Report it
		 * disconnected for that window: leaving a connected connector
		 * whose ->dcp is NULL lets anything probing it in between --
		 * a compositor starting up while the fabric is still settling
		 * -- see an output it cannot get a mode for, and give up on
		 * it.  The new pipeline marks it connected again once the
		 * display has come back up on it.
		 */
		WRITE_ONCE(connector->connected, false);
		/* hotplug work queued before this checks for it */
		WRITE_ONCE(connector->dcp, NULL);

		/* Unrouted: the port could go to any of its pipelines again. */
		if (connector->port_encoder)
			connector->port_encoder->possible_crtcs =
				connector->candidate_crtcs;
	}
	dcp->typec_connector = NULL;
	dcp->connector = dcp->fixed_connector;

	if (dcp->fixed_connector_type != DRM_MODE_CONNECTOR_USB) {
		dcp->connector_type = dcp->fixed_connector_type;

		/*
		 * Only hand the pipeline back to its fixed output if that output
		 * is live.  Re-targeting the DPTX endpoint at the fixed PHY while
		 * nothing is attached there leaves DCP unable to train a link on a
		 * later Type-C target: it answers DEVICE_NOT_STARTED and every
		 * following DPTX call times out.  Park on a Type-C PHY instead,
		 * for the same reason the USB-C-only case does below.
		 */
		if (dcp->hdmi_hpd && gpiod_get_value_cansleep(dcp->hdmi_hpd)) {
			dcp->phy = dcp->fixed_phy;
			dcp->dptx_phy = dcp->fixed_dptx_phy;

			if (dcp->xbar && !dcp->fixed_route_selected) {
				ret = mux_control_select(dcp->xbar,
							 dcp->fixed_mux_index);
				if (ret)
					return ret;
				dcp->fixed_route_selected = true;
			}
		} else {
			dcp->phy = dcp->typec_routes[0].phy;
			dcp->dptx_phy = dcp->typec_routes[0].dptx_phy;
		}
	} else {
		/* Keep DPTX endpoint discovery working before a cable is attached. */
		dcp->phy = dcp->typec_routes[0].phy;
		dcp->dptx_phy = dcp->typec_routes[0].dptx_phy;
		dcp->connector_type = DRM_MODE_CONNECTOR_USB;
	}

	return 0;
}

static struct apple_dcp_typec_route *
dcp_typec_port_route(struct apple_dcp_typec_port *port, struct apple_dcp *dcp)
{
	struct apple_dcp_typec_route *route;

	list_for_each_entry(route, &port->routes, port_link)
		if (route->dcp == dcp)
			return route;

	return NULL;
}

/*
 * The DRM device once it has bound, that is once every pipeline has its
 * CRTC and every port its connectors; NULL before.
 */
static struct drm_device *dcp_typec_drm(void)
{
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route *route;
	struct drm_device *drm = NULL;

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (!port->connector || !port->secondary_connector)
			return NULL;
		list_for_each_entry(route, &port->routes, port_link) {
			if (!route->dcp->crtc)
				return NULL;
			drm = route->dcp->crtc->base.dev;
		}
	}

	return drm;
}

/*
 * Has a compositor (or boot splash) taken the display?  It paired its
 * connectors with CRTCs when it started and keeps that pairing, also
 * across a VT switch, where it drops DRM master but keeps the device
 * open; moving routes under it would hand connectors to pipelines driving
 * other displays.  So the routes are frozen while any open file has ever
 * been master, and thaw when the last such file is closed.
 *
 * Taken under dcp_typec_fabric_lock, next to the routing it decides, so
 * the order is the fabric lock, then filelist_mutex.  Nothing nests them
 * the other way: filelist_mutex only covers list edits and walks, and
 * drm_release() drops it before drm_file_free() calls postclose, which is
 * where a close takes the fabric lock.  drm_open() makes a file master
 * before adding it here, but its owner cannot have paired anything before
 * open() returns, and it re-probes on the hotplugs the moves send.
 */
static bool dcp_typec_frozen(struct drm_device *drm)
{
	struct drm_file *file;

	guard(mutex)(&drm->filelist_mutex);
	list_for_each_entry(file, &drm->filelist, lhead) {
		/* set under master_mutex, and only ever from false to true */
		if (READ_ONCE(file->was_master))
			return true;
	}

	return false;
}

/* Are the routes kept in compositor pairing order right now? */
static bool dcp_typec_keep_order(void)
{
	struct drm_device *drm;

	if (!dcp_typec_dual_stream())
		return false;
	drm = dcp_typec_drm();

	return drm && READ_ONCE(drm->registered) && !dcp_typec_frozen(drm);
}

/* A Thunderbolt tunnel holds @dcp's pipeline: it never moves. */
static bool dcp_typec_tunnel_held(struct apple_dcp *dcp)
{
	return dcp->active_typec_route && dcp->active_typec_route->tunnel;
}

/*
 * Give @dcp's pipeline to the first stream, in connector order, that
 * wants one, has none yet and has it in its possible_crtcs.  A tunnel
 * stream wants one once it is set up, or as @arriving/@dpin.  A direct
 * DP-alt stream wants one once its sink asserts HPD: only then can its
 * connector read connected, and a compositor pairs only those.
 */
static void dcp_typec_plan_pipeline(struct apple_dcp *dcp, struct drm_crtc *crtc,
				    struct apple_dcp_typec_port *arriving,
				    unsigned int dpin)
{
	struct apple_dcp_typec_port *port;
	bool secondary = false;

	/* primary connectors in port order, then the DPIN1 ones */
	do {
		list_for_each_entry(port, &dcp_typec_ports, link) {
			struct apple_dcp_typec_route **slot = secondary ?
				&port->secondary_target : &port->target;
			struct apple_connector *connector = secondary ?
				port->secondary_connector : port->connector;
			struct apple_dcp_typec_route *route =
				dcp_typec_port_route(port, dcp);
			bool wants;

			if (secondary)
				wants = port->secondary_owner ||
					(port == arriving && dpin == 1);
			else
				wants = (port->owner && port->owner->tunnel) ||
					(port->dp_wanted && port->dp_hpd &&
					 !port->plan_dark) ||
					(port == arriving && dpin == 0);

			if (!wants || *slot || !route ||
			    !(connector->candidate_crtcs & drm_crtc_mask(crtc)))
				continue;

			*slot = route;
			return;
		}
		secondary = !secondary;
	} while (secondary);
}

/*
 * The pairing a compositor starting now would make, wherever the streams
 * sit at the moment: pipelines in CRTC index order, each to the first
 * stream that wants it.  A pipeline whose fixed output is live is left to
 * that output, unless a tunnel holds it: the output cannot have it back
 * then, and its connector reads disconnected.
 *
 * Tunnels are planned in connector order like any other stream, not
 * seated on the pipelines they hold: the compositor pairs their connectors
 * by order too, and seating them would plan the direct streams around
 * pairings it never makes.  A direct stream planned onto a pipeline a
 * tunnel holds cannot have it and stays dark, so it is left out and the
 * pass re-run, or every stream after it would be planned one pipeline
 * off.  Streams only ever drop out, so that settles within one pass per
 * port.  It also brings a tunnel's plan onto the pipeline it holds
 * wherever direct streams ahead of it were in the way.  A tunnel still
 * planned elsewhere cannot be brought there by any direct stream dropping
 * out, and the compositor pairs its connector wrongly whatever they do.
 */
static void dcp_typec_plan(struct drm_device *drm,
			   struct apple_dcp_typec_port *arriving, unsigned int dpin)
{
	struct apple_dcp_typec_port *port;
	struct drm_crtc *crtc;
	bool again;

	list_for_each_entry(port, &dcp_typec_ports, link)
		port->plan_dark = false;

	do {
		list_for_each_entry(port, &dcp_typec_ports, link) {
			port->target = NULL;
			port->secondary_target = NULL;
		}

		/* CRTCs are listed in index order */
		drm_for_each_crtc(crtc, drm) {
			struct apple_dcp *dcp =
				platform_get_drvdata(to_apple_crtc(crtc)->dcp);

			if (dcp->nr_typec_routes &&
			    (dcp_typec_tunnel_held(dcp) ||
			     !dcp_typec_route_fixed_output_busy(&dcp->typec_routes[0])))
				dcp_typec_plan_pipeline(dcp, crtc, arriving, dpin);
		}

		again = false;
		list_for_each_entry(port, &dcp_typec_ports, link) {
			if (port == arriving || !port->target ||
			    (port->owner && port->owner->tunnel) ||
			    !dcp_typec_tunnel_held(port->target->dcp))
				continue;
			port->plan_dark = true;
			again = true;
		}
	} while (again);
}

/* Replay HPD to the pipeline a direct DP-alt port has just been given. */
static void dcp_typec_port_attach(struct apple_dcp_typec_port *port)
{
	struct apple_dcp *dcp = port->owner->dcp;

	port->hpd = port->dp_hpd;
	if (!port->hpd)
		return;

	WRITE_ONCE(dcp->typec_cable_connected, true);
	if (dcp->typec_connector)
		dcp_dptx_connect_oob(to_platform_device(dcp->dev), 0);
}

static struct apple_dcp_typec_route *
dcp_typec_lowest_free(struct apple_dcp_typec_port *port)
{
	struct apple_dcp_typec_route *candidate, *best = NULL;
	unsigned int best_score = UINT_MAX;

	list_for_each_entry(candidate, &port->routes, port_link) {
		unsigned int score;

		if (!dcp_typec_route_available(candidate))
			continue;
		/*
		 * Prefer the lowest free CRTC index for a stable allocation order.
		 */
		score = dcp_typec_route_score(candidate);
		if (score < best_score) {
			best = candidate;
			best_score = score;
		}
	}

	return best;
}

/*
 * Route the direct DP-alt ports left waiting for a pipeline.  Their DP
 * state is not reported again while it stays the same, so nothing else
 * would.  Each goes back to the pipeline it last had if that is free, as
 * a compositor keeps a reconnected connector's CRTC, and otherwise takes
 * the lowest free one, the CRTC a compositor gives a new connector.
 */
static void dcp_typec_route_waiting(void)
{
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route *route;

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (port->owner || !port->dp_wanted || !port->dp_hpd)
			continue;

		route = port->preferred_route;
		if (!route || !dcp_typec_route_available(route))
			route = dcp_typec_lowest_free(port);
		if (!route || dcp_typec_route_activate(route, route->xbar))
			continue;
		port->owner = route;

		dcp_typec_port_attach(port);
	}
}

/*
 * Prefer connector order when placing Type-C streams before a DRM master
 * starts. This is an allocation policy, not a promise about the CRTC that
 * userspace must choose: each connected encoder advertises only its actual
 * route, and atomic encoder selection checks that ownership. Route changes
 * publish the new mask through hotplug; the plan uses candidate_crtcs so
 * narrowing the advertised mask does not remove hardware routing choices.
 *
 * Only direct DP-alt routes move: each goes to exactly its planned
 * pipeline.  A Thunderbolt tunnel never moves once set up, so where one
 * sits on a pipeline planned for a direct stream, or the plan has nothing
 * for it, that stream stays unrouted and its connector disconnected.
 * Placing it on some other pipeline would have the compositor cross both
 * displays; dark is the better failure.  @arriving/@dpin is a tunnel
 * stream asking for a pipeline, planned like any other; its route is
 * returned for apple_dcp_tb_dp_tunnel() to set up, or NULL if the plan
 * has none for it or another tunnel holds that one.
 *
 * A move is an unplug and replug.  Every route that moves is taken down
 * first, so that two never share a pipeline midway, then each goes up on
 * its new pipeline with its HPD replayed; the hotplugs this sends make
 * fbdev and userspace re-probe.
 */
static struct apple_dcp_typec_route *
dcp_typec_rebalance_locked(struct apple_dcp_typec_port *arriving,
			   unsigned int dpin)
{
	struct drm_device *drm = dcp_typec_drm();
	struct apple_dcp_typec_route *planned = NULL;
	struct apple_dcp_typec_port *port;

	lockdep_assert_held(&dcp_typec_fabric_lock);

	dcp_typec_plan(drm, arriving, dpin);
	if (arriving) {
		planned = dpin ? arriving->secondary_target : arriving->target;
		/* it will be refused: plan as if it had not asked */
		if (planned && dcp_typec_tunnel_held(planned->dcp)) {
			planned = NULL;
			dcp_typec_plan(drm, NULL, 0);
		}
	}

	list_for_each_entry(port, &dcp_typec_ports, link) {
		struct apple_dcp_typec_route *owner = port->owner;
		struct apple_dcp *dcp;

		if (port->safe_pending)
			continue;
		if (!owner || owner->tunnel || owner == port->target)
			continue;

		dcp = owner->dcp;
		dev_info(dcp->dev, "re-routing %pOF from %s to %s for Type-C connector order\n",
			 port->connector_np, dev_name(dcp->dev),
			 port->target ? dev_name(port->target->dcp->dev) : "none");
		if (port->hpd || dcp->typec_cable_connected ||
		    (dcp->typec_connector && dcp->typec_connector->connected))
			dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
		port->hpd = false;
		if (dcp_typec_route_deactivate(owner) && owner->selected) {
			/* still routed: leave the display where it is */
			dcp_typec_port_attach(port);
			continue;
		}
		port->owner = NULL;

		/* as after a DP exit, hand the hybrid back to a live HDMI */
		if (dcp->hdmi_hpd && dcp->active &&
		    gpiod_get_value_cansleep(dcp->hdmi_hpd))
			dcp_dptx_connect(dcp, 0);
	}

	list_for_each_entry(port, &dcp_typec_ports, link) {
		struct apple_dcp_typec_route *route = port->target;

		if (port->owner || !port->dp_wanted || !port->dp_hpd ||
		    port == arriving)
			continue;

		if (!route || !dcp_typec_route_available(route) ||
		    dcp_typec_route_activate(route, route->xbar)) {
			/*
			 * Dark for now.  Its unchanged DP state is not reported
			 * again, so it is placed when the plan next runs, or
			 * once a compositor owns the display, when a pipeline
			 * is freed (dcp_typec_route_waiting()).
			 */
			port->applied_valid = false;
			continue;
		}
		port->owner = route;
		port->preferred_route = route;

		dcp_typec_port_attach(port);
	}

	return planned;
}

/*
 * A route has let its pipeline go.  Until a compositor owns the display
 * the plan places everything anew; after that the pipeline goes to a port
 * left waiting for one.
 */
static void dcp_typec_pipeline_freed(void)
{
	if (!dcp_typec_dual_stream())
		return;

	if (dcp_typec_keep_order())
		dcp_typec_rebalance_locked(NULL, 0);
	else
		dcp_typec_route_waiting();
}

/*
 * Re-run the pairing pass from outside the fabric: once DRM is registered,
 * as ports routed before that could not follow it, and when the last
 * compositor or boot splash has closed the device, as the next one pairs
 * the connectors from scratch.
 */
void dcp_typec_reorder(void)
{
	if (!dcp_typec_dual_stream())
		return;

	guard(mutex)(&dcp_typec_fabric_lock);

	if (dcp_typec_keep_order())
		dcp_typec_rebalance_locked(NULL, 0);
}

static int dcp_typec_route_prepare(struct typec_mux_dev *mux,
				   struct typec_mux_state *state)
{
	struct apple_dcp_typec_route *route = typec_mux_get_drvdata(mux);
	struct apple_dcp_typec_port *port = route->port;
	struct apple_dcp *dcp;
	int ret;

	guard(mutex)(&dcp_typec_fabric_lock);
	if (state->mode != TYPEC_STATE_SAFE)
		return port->safe_pending ? -EAGAIN : 0;
	if (!port->owner || port->owner->tunnel)
		return 0;
	dcp = port->owner->dcp;
	/* Stop new admission, but retain the owner until the set phase succeeds. */
	if (!port->safe_pending)
		port->safe_finish_ready = false;
	port->safe_pending = true;
	port->applied_valid = false;
	port->dp_wanted = false;
	port->dp_hpd = false;
	/* HPD absence alone is not proof of a completed firmware shutdown. */
	if (dcp_is_external_h17p(dcp)) {
		scoped_guard(mutex, &dcp->external_lock) {
			if (dcp->external_error)
				return dcp->external_error;
			if (!dcp->external_cable && dcp->external_stopped &&
			    !READ_ONCE(dcp->dptxport[0].connected)) {
				port->hpd = false;
				return 0;
			}
		}
	}
	ret = dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
	if (ret)
		return ret;
	port->hpd = false;
	return 0;
}

static int dcp_typec_route_finish(struct typec_mux_dev *mux,
				  struct typec_mux_state *state)
{
	struct apple_dcp_typec_port *port =
		((struct apple_dcp_typec_route *)typec_mux_get_drvdata(mux))->port;
	int ret;

	guard(mutex)(&dcp_typec_fabric_lock);
	if (state->mode != TYPEC_STATE_SAFE || !port->safe_pending ||
	    port->safe_finish_ready)
		return 0;
	ret = port->owner ? dcp_typec_route_deactivate(port->owner) : 0;
	if (ret)
		return ret;
	port->safe_finish_ready = true;
	return 0;
}

static void dcp_typec_route_commit(struct typec_mux_dev *mux,
				   struct typec_mux_state *state)
{
	struct apple_dcp_typec_port *port =
		((struct apple_dcp_typec_route *)typec_mux_get_drvdata(mux))->port;

	guard(mutex)(&dcp_typec_fabric_lock);
	if (state->mode != TYPEC_STATE_SAFE || !port->safe_pending)
		return;
	if (port->owner)
		port->preferred_route = port->owner;
	port->owner = NULL;
	port->safe_pending = false;
	port->safe_finish_ready = false;
	port->applied_alt = state->alt;
	port->applied_mode = state->mode;
	port->applied_status = 0;
	port->applied_conf = 0;
	port->applied_valid = true;
	dcp_typec_pipeline_freed();
}

static int dcp_typec_route_set(struct typec_mux_dev *mux,
			       struct typec_mux_state *state)
{
	struct apple_dcp_typec_route *route = typec_mux_get_drvdata(mux);
	struct apple_dcp_typec_port *port = route->port;
	struct apple_dcp_typec_route *best = NULL;
	bool is_dp = dcp_typec_route_is_dp(state);
	struct typec_displayport_data *dp_data = is_dp ? state->data : NULL;
	u32 dp_status = dp_data ? dp_data->status : 0;
	u32 dp_conf = dp_data ? dp_data->conf : 0;
	bool hpd, was_counted;
	int ret = 0;

	guard(mutex)(&dcp_typec_fabric_lock);

	if (state->mode == TYPEC_STATE_SAFE && port->safe_pending)
		return 0;
	/* The previous SAFE transition has not finished; retry. */
	if (port->safe_pending)
		return -EAGAIN;

	/*
	 * Every candidate route for this port is notified with the same state,
	 * so only the first to arrive does the work; the others return early.
	 *
	 * Deliberately not a nominated coordinator: fwnode_typec_mux_get()
	 * caps the providers one connector may have and drops the remainder
	 * without a word, so a nominated route might never be called at all --
	 * and the port would then never be routed.
	 */
	if (port->applied_valid && port->applied_alt == state->alt &&
	    port->applied_mode == state->mode &&
	    port->applied_status == dp_status && port->applied_conf == dp_conf &&
	    !dcp_typec_route_irq_hpd(route, port->owner, dp_status))
		return 0;

	port->applied_alt = state->alt;
	port->applied_mode = state->mode;
	port->applied_status = dp_status;
	port->applied_conf = dp_conf;
	/* Failed route acquisition must remain retryable on the next update. */
	port->applied_valid = false;

	/* did the pairing pass count this port's direct stream so far? */
	was_counted = port->dp_wanted && port->dp_hpd;

	if (!is_dp) {
		port->dp_wanted = false;
		port->dp_hpd = false;

		/* a Thunderbolt/USB4 DP tunnel is torn down by its own path */
		if (port->owner && port->owner->tunnel) {
			port->applied_valid = true;
			return 0;
		}
		if (port->owner) {
			struct apple_dcp *dcp = port->owner->dcp;

			port->preferred_route = port->owner;
			if (port->hpd || dcp->typec_cable_connected ||
			    (dcp->typec_connector &&
			     dcp->typec_connector->connected))
				dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
			port->hpd = false;
			ret = dcp_typec_route_deactivate(port->owner);
			if (ret)
				return ret;
			port->owner = NULL;

			if (dcp->hdmi_hpd && dcp->active &&
			    gpiod_get_value_cansleep(dcp->hdmi_hpd))
				dcp_dptx_connect(dcp, 0);
			dcp_typec_pipeline_freed();
		} else if (was_counted && dcp_typec_keep_order()) {
			/* a stream the plan left dark is gone: plan the rest */
			dcp_typec_rebalance_locked(NULL, 0);
		}

		port->safe_pending = false;
		/* Data-only USB4 changes do not invalidate other display routes. */
		port->applied_valid = true;
		return 0;
	}

	/*
	 * A Thunderbolt DP tunnel still owns the port (its teardown is on the
	 * way): don't act on or remember this state; the tunnel teardown drops
	 * what was recorded so the next update is applied.
	 */
	if (port->owner && port->owner->tunnel) {
		port->applied_valid = false;
		return 0;
	}

	hpd = dp_data && (dp_data->status & DP_STATUS_HPD_STATE);
	port->dp_wanted = true;
	port->dp_hpd = hpd;

	/*
	 * Until a compositor owns the display, a direct DP-alt stream has a
	 * pipeline only while its sink asserts HPD, and the pairing pass
	 * places it: HPD coming or going is the stream connecting or
	 * disconnecting as far as a compositor can tell.  Otherwise the port
	 * takes the lowest free pipeline on DP entry, as a compositor does.
	 */
	if (dcp_typec_keep_order()) {
		struct apple_dcp_typec_route *owner = port->owner;

		/* it connects or disconnects, or is not routed as it should be */
		if (hpd != was_counted || !owner != !hpd)
			dcp_typec_rebalance_locked(NULL, 0);
		if (!port->owner) {
			if (hpd)
				return -ENOSPC;
			port->applied_valid = true;
			return 0;
		}
		/* just attached, its HPD replayed: nothing left to apply */
		if (port->owner != owner) {
			port->applied_valid = true;
			return 0;
		}
	}

	if (!port->owner) {
		if (port->preferred_route &&
		    dcp_typec_route_available(port->preferred_route))
			best = port->preferred_route;

		if (!best)
			best = dcp_typec_lowest_free(port);

		/* Every pipeline is driving another port's display. */
		if (!best)
			return -ENOSPC;

		ret = dcp_typec_route_activate(best, best->xbar);
		if (ret)
			return ret;
		port->owner = best;

	}


	if (!hpd && port->hpd) {
		dcp_dptx_disconnect_oob(to_platform_device(port->owner->dcp->dev), 0);
	} else if (hpd && !port->hpd) {
		struct apple_dcp *dcp = port->owner->dcp;

		WRITE_ONCE(dcp->typec_cable_connected, true);
		if (dcp->typec_connector)
			dcp_dptx_connect_oob(to_platform_device(dcp->dev), 0);
	} else if (hpd && dp_data && (dp_data->status & DP_STATUS_IRQ_HPD)) {
		struct apple_dcp *dcp = port->owner->dcp;

		/* 14.7 external: InterruptRequestOccurred, not a retrain. */
		if (iomfb_is_v14_7_external(dcp))
			dcp_ext_v14_7_irq_hpd(dcp);
		else if (dcp->typec_connector)
			dcp_retrain_oob(dcp->typec_connector);
	}
	port->hpd = hpd;
	port->applied_valid = true;

	return 0;
}

/*
 * Crossbar connection up/down, looked up at runtime so appledrm does not
 * require the crossbar driver to be built.
 */
static int dcp_dpxbar_link(struct mux_control *mux, bool up)
{
	typeof(&apple_dpxbar_link_up) fn;
	int ret;

	fn = up ? symbol_get(apple_dpxbar_link_up) :
		  symbol_get(apple_dpxbar_link_down);
	if (!fn)
		return -ENOENT;
	ret = fn(mux);
	if (up)
		symbol_put(apple_dpxbar_link_up);
	else
		symbol_put(apple_dpxbar_link_down);
	return ret;
}

static int dcp_dpxbar_preselect(struct mux_control *mux, int state)
{
	typeof(&apple_dpxbar_preselect) fn = symbol_get(apple_dpxbar_preselect);
	int ret;

	if (!fn)
		return -ENOENT;
	ret = fn(mux, state);
	symbol_put(apple_dpxbar_preselect);
	return ret;
}

/*
 * Thunderbolt DP IN, before DCP hears about the display: point the DP IN
 * output at this pipeline and wake the ATC's DP clock path. The crossbar
 * connection itself still only comes up in dcp_tunnel_crossbar_up(), but an
 * output left at its idle source (dispext0) only suits the pipeline that is
 * dispext0: any other one activates the link, gets no answer from the sink
 * and gives up with DEVICE_NOT_RESPONDING before it ever sets a link rate.
 */
static void dcp_tunnel_prepare(struct apple_dcp_typec_route *route,
			       struct mux_control *xbar)
{
	typeof(&apple_atc_dp_tunnel_open) open;
	struct apple_dcp *dcp = route->dcp;
	int ret;

	ret = dcp_dpxbar_preselect(xbar, route->mux_index);
	if (ret && ret != -EOPNOTSUPP)
		dev_warn(dcp->dev, "DP tunnel crossbar preselect failed: %d\n", ret);

	open = symbol_get(apple_atc_dp_tunnel_open);
	if (!open)
		return;
	ret = open(dcp->phy);
	symbol_put(apple_atc_dp_tunnel_open);
	if (ret && ret != -EOPNOTSUPP)
		dev_warn(dcp->dev, "DP tunnel PHY open failed: %d\n", ret);
}

/* DP IN adapter handshake through the thunderbolt glue; tb_lock held */
static int dcp_tunnel_dpin_locked(struct apple_dcp *dcp, bool active)
{
	lockdep_assert_held(&dcp->tb_lock);
	if (!dcp->tb_dpin_set_active)
		return -ENODEV;
	return dcp->tb_dpin_set_active(dcp->tb_dpin_ctx, active);
}

/*
 * Thunderbolt DP IN, from DCP's DidChangeLinkConfiguration once a link rate
 * is set: bring the crossbar connection up (FIFO/PCLK/ATC enables) now that
 * the tunnel pixel clock runs, and re-assert the DP IN adapter's
 * DPTX_INACTIVE=0 afterwards. The first time this is the mux selection; after
 * a re-link only the clocks are brought back (the mux selection and the ATC
 * output enable are kept, see dcp_tunnel_crossbar_down()).
 * Runs inside a DCP apcall: only tb_lock, which nobody holds across a DCP call.
 */
int dcp_tunnel_crossbar_up(struct apple_dcp *dcp)
{
	struct apple_dcp_typec_route *route;
	int ret;

	guard(mutex)(&dcp->tb_lock);
	route = dcp->active_typec_route;
	if (!route || !route->tunnel || !route->active_xbar ||
	    route->deactivate_xbar_done)
		return -ENODEV;
	if (!dcp->tb_clock_ok) {
		dev_warn(dcp->dev, "no DP tunnel pixel clock, crossbar left down\n");
		return -EIO;
	}
	if (!route->xbar_up) {
		/* never block a DCP call on the mux semaphore */
		ret = mux_control_try_select(route->active_xbar, route->mux_index);
		if (!ret)
			route->xbar_up = true;
	} else {
		ret = dcp_dpxbar_link(route->active_xbar, true);
	}
	if (ret) {
		dev_warn(dcp->dev, "DP tunnel crossbar up failed: %d\n", ret);
		return ret;
	}
	return dcp_tunnel_dpin_locked(dcp, true);
}

/*
 * Thunderbolt DP IN, from DCP's WillChangeLinkConfiguration on an established
 * link: DP IN inactive, crossbar clocks down (mux selection and ATC output
 * enable kept). DP IN goes active again in dcp_tunnel_crossbar_up().
 */
int dcp_tunnel_crossbar_down(struct apple_dcp *dcp)
{
	struct apple_dcp_typec_route *route;

	guard(mutex)(&dcp->tb_lock);
	route = dcp->active_typec_route;
	if (!route || !route->tunnel || !route->xbar_up)
		return 0;
	dcp_tunnel_dpin_locked(dcp, false);
	return dcp_dpxbar_link(route->active_xbar, false);
}

/*
 * Thunderbolt DP IN, from DCP's SetLinkRate: start (rate != 0) or stop the
 * tunnel pixel clock. A stopped clock also takes the crossbar connection down.
 */
int dcp_tunnel_set_rate(struct apple_dcp *dcp, struct phy *phy, u32 link_rate)
{
	typeof(&apple_atc_dp_tunnel_rate) fn;
	struct apple_dcp_typec_route *route;
	int ret;

	guard(mutex)(&dcp->tb_lock);
	if (!dcp->dptx_tunnel)
		return -ENODEV;
	fn = symbol_get(apple_atc_dp_tunnel_rate);
	if (!fn) {
		dev_err(dcp->dev, "phy-apple-atc not loaded, no DP tunnel clock\n");
		return -ENOENT;
	}
	route = dcp->active_typec_route;
	if (!route) {
		symbol_put(apple_atc_dp_tunnel_rate);
		return -ENODEV;
	}
	if (!link_rate && route->xbar_up)
		dcp_dpxbar_link(route->active_xbar, false);
	ret = fn(phy, route->tunnel_dpin, link_rate);
	symbol_put(apple_atc_dp_tunnel_rate);
	dcp->tb_clock_ok = !ret && link_rate;
	if (ret)
		dev_warn(dcp->dev, "DP tunnel pixel clock (rate 0x%x) failed: %d\n",
			 link_rate, ret);
	return ret;
}

/* Thunderbolt DP IN: DCP Activate/Deactivate */
int dcp_tunnel_dpin_activate(struct apple_dcp *dcp, bool active)
{
	guard(mutex)(&dcp->tb_lock);
	if (!dcp->dptx_tunnel)
		return 0;
	return dcp_tunnel_dpin_locked(dcp, active);
}

/*
 * Thunderbolt DP tunnels: the host router's DP IN adapters sit behind the
 * crossbar's dpin0/dpin1 outputs of the port's ATC. When the Thunderbolt
 * connection manager has set up a tunnel from one of them, route a free
 * display pipeline there and tell DCP a display is attached, so it trains
 * the link (and completes DPRX) through the tunnel.
 */
int apple_dcp_tb_dp_tunnel(struct device_node *connector_np, unsigned int dpin,
			   bool active, int (*set_active)(void *ctx, bool active),
			   void *ctx)
{
	struct apple_dcp_typec_port *port = NULL, *pos;
	struct apple_dcp_typec_route *candidate, *best = NULL, *planned = NULL;
	struct apple_dcp_typec_route **slot;
	unsigned int best_score = UINT_MAX;
	struct mux_control *ctl;
	struct apple_dcp *dcp;
	bool ordered;
	int ret;

	if (!connector_np || dpin > 1)
		return -EINVAL;

	guard(mutex)(&dcp_typec_fabric_lock);

	list_for_each_entry(pos, &dcp_typec_ports, link) {
		if (pos->connector_np == connector_np) {
			port = pos;
			break;
		}
	}
	if (!port)
		return -ENODEV;
	slot = dpin ? &port->secondary_owner : &port->owner;

	if (!active) {
		if (!*slot || !(*slot)->tunnel ||
		    (*slot)->tunnel_dpin != dpin)
			return 0;
		dcp = (*slot)->dcp;
		if (port->hpd || dcp->typec_cable_connected)
			dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
		ret = dcp_typec_route_deactivate(*slot);
		if (ret) {
			/* the caller's context is going away regardless */
			scoped_guard(mutex, &dcp->tb_lock) {
				dcp->dptx_tunnel = false;
				dcp->tb_dpin_set_active = NULL;
				dcp->tb_dpin_ctx = NULL;
			}
			return ret;
		}
		*slot = NULL;
		port->hpd = !!(port->owner || port->secondary_owner);
		/* re-apply the next Type-C mux state in full */
		port->applied_valid = false;
		if (dcp->hdmi_hpd && dcp->active &&
		    gpiod_get_value_cansleep(dcp->hdmi_hpd))
			dcp_dptx_connect(dcp, 0);
		dcp_typec_pipeline_freed();
		return 0;
	}

	if (*slot) {
		if ((*slot)->tunnel && (*slot)->tunnel_dpin == dpin)
			return 0;
		dev_warn((*slot)->dcp->dev,
			 "port already routed, not taking DP tunnel dpin%u\n", dpin);
		return -EBUSY;
	}
	if (port->owner && !port->owner->tunnel)
		return -EBUSY;

	/*
	 * Until a compositor owns the display, the pairing pass decides where
	 * the stream goes, moving direct DP-alt routes out of its way, and
	 * whether it gets a pipeline at all.
	 */
	ordered = dcp_typec_keep_order();
	if (ordered)
		planned = dcp_typec_rebalance_locked(port, dpin);

	list_for_each_entry(candidate, &port->routes, port_link) {
		unsigned int score;

		if (ordered && candidate != planned)
			continue;
		if (!dcp_typec_route_available(candidate))
			continue;
		if (!dcp_typec_route_fits(candidate, dpin ?
					  port->secondary_connector :
					  port->connector))
			continue;
		score = dcp_typec_route_score(candidate);
		/*
		 * DPIN0 prefers the hybrid dcpext0, which completes tunneled
		 * link training. On dual-stream machines DPIN1 is confined to
		 * dcpext1 by its connector's possible_crtcs, so both pipelines
		 * drive independent streams through one dock.
		 */
		if (dcp_typec_dual_stream() && dpin == 0 &&
		    !candidate->dcp->fixed_phy && score < UINT_MAX - 100)
			score += 100;
		if (score < best_score) {
			best = candidate;
			best_score = score;
		}
	}
	if (!best) {
		ret = -EBUSY;
		goto err_reorder;
	}

	/* The route's crossbar control is dpphy (0); dpin0/dpin1 are 1/2. */
	if (best->xbar != &best->xbar->chip->mux[0] ||
	    best->xbar->chip->controllers < 3) {
		ret = -EOPNOTSUPP;
		goto err_reorder;
	}
	ctl = &best->xbar->chip->mux[1 + dpin];

	dcp = best->dcp;
	scoped_guard(mutex, &dcp->tb_lock) {
		dcp->tb_dpin_set_active = set_active;
		dcp->tb_dpin_ctx = ctx;
	}
	best->tunnel_dpin = dpin;
	ret = dcp_typec_route_activate(best, ctl);
	if (ret) {
		scoped_guard(mutex, &dcp->tb_lock) {
			dcp->tb_dpin_set_active = NULL;
			dcp->tb_dpin_ctx = NULL;
		}
		goto err_reorder;
	}
	*slot = best;
	/* the port is in USB4 mode, not DP-alt */
	port->dp_wanted = false;
	dcp_tunnel_prepare(best, ctl);

	dev_info(dcp->dev, "display routed to Thunderbolt DP tunnel dpin%u\n", dpin);

	/*
	 * The DP IN adapter may only be woken (DPTX_INACTIVE=0) while DCP
	 * drives the DPTX, i.e. from DCP's Activate call; waking it earlier
	 * hangs the machine. dptxep calls set_active back from Activate and
	 * Deactivate (set above, before the route became a tunnel).
	 */
	if (!dcp->typec_connector)
		dev_warn(dcp->dev, "no Type-C connector for the DP tunnel\n");
	WRITE_ONCE(dcp->typec_cable_connected, true);
	port->hpd = true;
	if (dcp->typec_connector)
		dcp_dptx_connect_oob(to_platform_device(dcp->dev), 0);

	return 0;

err_reorder:
	/* the pass kept a pipeline for this stream: give it to the others */
	if (planned)
		dcp_typec_rebalance_locked(NULL, 0);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dcp_tb_dp_tunnel);

static struct apple_dcp_typec_port *
dcp_typec_port_get(struct device_node *connector_np)
{
	struct apple_dcp_typec_port *port, *pos;

	lockdep_assert_held(&dcp_typec_fabric_lock);

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (port->connector_np == connector_np) {
			of_node_put(connector_np);
			return port;
		}
	}

	port = kzalloc_obj(*port);
	if (!port) {
		of_node_put(connector_np);
		return NULL;
	}

	INIT_LIST_HEAD(&port->routes);
	port->connector_np = connector_np;

	/*
	 * Insert in device-tree order rather than DCP probe order.  The list
	 * index becomes the DRM connector index, and userspace keys its
	 * per-monitor configuration (scale, rotation, layout) on the connector
	 * name -- so it has to mean the same physical port on every boot.
	 */
	list_for_each_entry(pos, &dcp_typec_ports, link)
		if (strcmp(of_node_full_name(connector_np),
			   of_node_full_name(pos->connector_np)) < 0)
			break;
	list_add_tail(&port->link, &pos->link);
	return port;
}

static struct apple_dcp_typec_port *dcp_typec_port_by_index(unsigned int idx)
{
	struct apple_dcp_typec_port *port;
	unsigned int i = 0;

	lockdep_assert_held(&dcp_typec_fabric_lock);

	list_for_each_entry(port, &dcp_typec_ports, link)
		if (i++ == idx)
			return port;

	return NULL;
}

unsigned int dcp_typec_nr_ports(void)
{
	struct apple_dcp_typec_port *port;
	unsigned int n = 0;

	guard(mutex)(&dcp_typec_fabric_lock);

	list_for_each_entry(port, &dcp_typec_ports, link)
		n++;

	return n;
}

struct device_node *dcp_typec_port_of_node(unsigned int idx)
{
	struct apple_dcp_typec_port *port;

	guard(mutex)(&dcp_typec_fabric_lock);

	port = dcp_typec_port_by_index(idx);

	return port ? port->connector_np : NULL;
}

bool dcp_typec_port_has_candidate(unsigned int idx, struct platform_device *pdev)
{
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route *route;

	guard(mutex)(&dcp_typec_fabric_lock);

	port = dcp_typec_port_by_index(idx);
	if (!port)
		return false;

	list_for_each_entry(route, &port->routes, port_link)
		if (route->dcp->dev == &pdev->dev)
			return true;

	return false;
}

void dcp_typec_port_set_connector(unsigned int idx, bool secondary,
				  struct apple_connector *connector)
{
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route *owner;

	guard(mutex)(&dcp_typec_fabric_lock);

	port = dcp_typec_port_by_index(idx);
	if (!port)
		return;

	if (secondary) {
		port->secondary_connector = connector;
		owner = port->secondary_owner;
	} else {
		port->connector = connector;
		owner = port->owner;
	}

	/*
	 * The port may already have been routed, either before DRM bound or
	 * while these connectors were being created.  Adopt that owner now,
	 * otherwise its display would be reported on the pipeline's fixed
	 * connector instead of the port it is actually plugged into.
	 */
	if (owner) {
		struct apple_dcp *dcp = owner->dcp;

		if (dcp->crtc && connector->port_encoder)
			connector->port_encoder->possible_crtcs =
				drm_crtc_mask(&dcp->crtc->base);

		connector->dcp = to_platform_device(dcp->dev);
		dcp->typec_connector = connector;
		dcp->connector = connector;
		dcp->connector_type = DRM_MODE_CONNECTOR_USB;
	}
}

static void dcp_typec_route_unregister(void *data)
{
	struct apple_dcp_typec_route *route = data;
	struct apple_dcp_typec_port *port = route->port;

	typec_mux_unregister(route->typec_mux);

	guard(mutex)(&dcp_typec_fabric_lock);
	if (port->preferred_route == route)
		port->preferred_route = NULL;
	if (port->owner == route) {
		struct apple_dcp *dcp = route->dcp;

		if (port->hpd || dcp->typec_cable_connected)
			dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
		port->hpd = false;
		if (dcp_typec_route_deactivate(route) && dcp_is_external_h17p(dcp))
			return;
		port->owner = NULL;
	}
	if (port->secondary_owner == route) {
		struct apple_dcp *dcp = route->dcp;

		if (port->hpd || dcp->typec_cable_connected)
			dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
		if (dcp_typec_route_deactivate(route) && dcp_is_external_h17p(dcp))
			return;
		port->secondary_owner = NULL;
	}
	port->hpd = !!(port->owner || port->secondary_owner);
	list_del(&route->port_link);
	if (list_empty(&port->routes)) {
		list_del(&port->link);
		of_node_put(port->connector_np);
		kfree(port);
	}
}

static int dcp_register_typec_routes(struct apple_dcp *dcp)
{
	struct device_node *routes __free(device_node) =
		of_get_child_by_name(dcp->dev->of_node, "typec-routes");
	struct device *dev = dcp->dev;
	u32 route_index;
	int ret;

	if (!routes)
		return 0;

	for_each_available_child_of_node_scoped(routes, route_np) {
		struct apple_dcp_typec_route *route;
		struct device_node *endpoint __free(device_node) = NULL;
		struct device_node *connector_np;
		struct typec_mux_desc desc = {};
		const char *name, *mux_name;

		if (dcp->nr_typec_routes == DCP_MAX_TYPEC_ROUTES)
			return dev_err_probe(dev, -E2BIG, "Too many Type-C display routes\n");

		ret = of_property_read_u32(route_np, "reg", &route_index);
		if (ret)
			return dev_err_probe(dev, ret, "%pOF: missing route index\n", route_np);
		if (route_index >= DCP_MAX_TYPEC_ROUTES)
			return dev_err_probe(dev, -EINVAL, "%pOF: invalid route index %u\n",
					     route_np, route_index);

		name = devm_kasprintf(dev, GFP_KERNEL, "typec%u", route_index);
		if (!name)
			return -ENOMEM;

		/*
		 * The DT lookups above are per-DCP, but the typec_mux class is
		 * global. Several DCPs can offer a route to the same Type-C port,
		 * so the registered mux needs a name unique across all of them.
		 */
		mux_name = devm_kasprintf(dev, GFP_KERNEL, "%s-typec%u",
					  dev_name(dev), route_index);
		if (!mux_name)
			return -ENOMEM;

		route = &dcp->typec_routes[dcp->nr_typec_routes];
		route->dcp = dcp;
		INIT_LIST_HEAD(&route->port_link);
		route->phy = devm_phy_get(dev, name);
		if (IS_ERR(route->phy))
			return dev_err_probe(dev, PTR_ERR(route->phy),
					     "%pOF: failed to get DP PHY\n", route_np);

		route->xbar = devm_mux_control_get(dev, name);
		if (IS_ERR(route->xbar))
			return dev_err_probe(dev, PTR_ERR(route->xbar),
					     "%pOF: failed to get display crossbar\n", route_np);

		ret = of_property_read_u32_index(dev->of_node, "apple,typec-mux-indices",
						 route_index, &route->mux_index);
		if (ret)
			return dev_err_probe(dev, ret, "%pOF: missing crossbar state\n",
					     route_np);

		ret = of_property_read_u32_index(dev->of_node, "apple,typec-dptx-phys",
						 route_index, &route->dptx_phy);
		if (ret)
			return dev_err_probe(dev, ret, "%pOF: missing DPTX PHY index\n",
					     route_np);

		endpoint = of_graph_get_next_endpoint(route_np, NULL);
		if (!endpoint)
			return dev_err_probe(dev, -EINVAL,
					     "%pOF: missing Type-C graph endpoint\n",
					     route_np);
		connector_np = of_graph_get_remote_port_parent(endpoint);
		if (!connector_np)
			return dev_err_probe(dev, -EINVAL,
					     "%pOF: missing Type-C connector\n",
					     route_np);

		mutex_lock(&dcp_typec_fabric_lock);
		route->port = dcp_typec_port_get(connector_np);
		if (route->port)
			list_add_tail(&route->port_link, &route->port->routes);
		mutex_unlock(&dcp_typec_fabric_lock);
		if (!route->port)
			return -ENOMEM;

		desc.fwnode = of_fwnode_handle(route_np);
		desc.set = dcp_typec_route_set;
		desc.prepare = dcp_typec_route_prepare;
		desc.finish = dcp_typec_route_finish;
		desc.commit = dcp_typec_route_commit;
		desc.name = mux_name;
		desc.drvdata = route;
		route->typec_mux = typec_mux_register(dev, &desc);
		if (IS_ERR(route->typec_mux)) {
			mutex_lock(&dcp_typec_fabric_lock);
			list_del(&route->port_link);
			if (list_empty(&route->port->routes)) {
				list_del(&route->port->link);
				of_node_put(route->port->connector_np);
				kfree(route->port);
			}
			mutex_unlock(&dcp_typec_fabric_lock);
			return dev_err_probe(dev, PTR_ERR(route->typec_mux),
					     "%pOF: failed to register Type-C route\n", route_np);
		}

		ret = devm_add_action_or_reset(dev, dcp_typec_route_unregister, route);
		if (ret)
			return ret;

		if (!dcp->phy)
			dcp->phy = route->phy;
		dcp->nr_typec_routes++;
	}

	if (!dcp->nr_typec_routes)
		return dev_err_probe(dev, -EINVAL, "Type-C route container is empty\n");

	dcp->phy_managed_by_typec = true;
	return 0;
}


/* copied and simplified from drm_vblank.c */
static void send_vblank_event(struct drm_device *dev,
		struct drm_pending_vblank_event *e,
		u64 seq, ktime_t now)
{
	struct timespec64 tv;

	if (e->event.base.type != DRM_EVENT_FLIP_COMPLETE)
		return;

	tv = ktime_to_timespec64(now);
	e->event.vbl.sequence = seq;
	/*
		* e->event is a user space structure, with hardcoded unsigned
		* 32-bit seconds/microseconds. This is safe as we always use
		* monotonic timestamps since linux-4.15
		*/
	e->event.vbl.tv_sec = tv.tv_sec;
	e->event.vbl.tv_usec = tv.tv_nsec / 1000;

	/*
	 * Use the same timestamp for any associated fence signal to avoid
	 * mismatch in timestamps for vsync & fence events triggered by the
	 * same HW event. Frameworks like SurfaceFlinger in Android expects the
	 * retire-fence timestamp to match exactly with HW vsync as it uses it
	 * for its software vsync modeling.
	 */
	drm_send_event_timestamp_locked(dev, &e->base, now);
}

/**
 * dcp_crtc_send_page_flip_event - helper to send vblank event after pageflip
 * @crtc: CRTC whose flip completed
 * @e: pending vblank event
 * @now: swap completion timestamp
 * @start: swap submission timestamp
 *
 * Compensate for unknown slack between page flip and arrival of the
 * swap_complete callback. Minimal observed duration on DCP with HDMI output
 * was around 2.3 ms. If the fb swap was submitted closer to the expected
 * swap_complete it gets a penalty of one frame duration. This is on the border
 * of unreasonable considering that Apple advertises support for 240 Hz (frame
 * duration of 4.167 ms).
 * It is unreasonable considering kwin's kms commit scheduling. Kwin commits
 * 1.5 ms + the mode's vblank time before the expected next page flip
 * completion. This results in presenting at half the display's rate for HDMI
 * outputs.
 * This might be a difference between dcp and dcpext.
 */
static void dcp_crtc_send_page_flip_event(struct apple_crtc *crtc,
					  struct drm_pending_vblank_event *e,
					  ktime_t now, ktime_t start)
{
	struct drm_device *dev = crtc->base.dev;
	u64 seq;
	unsigned int pipe = drm_crtc_index(&crtc->base);
	ktime_t flip;

	seq = 0;
	if (start != KTIME_MIN) {
		s64 delta = ktime_us_delta(now, start);
		if (delta <= 500)
			flip = now;
		else if (delta >= 2500)
			flip = ktime_sub_us(now, 1000);
		else
			flip = ktime_sub_us(now, (delta - 500) / 2);
	} else {
		flip = now;
	}
	e->pipe = pipe;
	send_vblank_event(dev, e, seq, flip);
}

/* HACK: moved here to avoid circular dependency between apple_drv and dcp */
void dcp_drm_crtc_vblank(struct apple_crtc *crtc)
{
	unsigned long flags;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		crtc->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

void dcp_drm_crtc_page_flip(struct apple_dcp *dcp, ktime_t now)
{
	unsigned long flags;
	struct apple_crtc *crtc = dcp->crtc;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		if (crtc->event->event.base.type == DRM_EVENT_FLIP_COMPLETE)
			dcp_crtc_send_page_flip_event(crtc, crtc->event, now, dcp->swap_start);
		else
			drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		crtc->event = NULL;
		dcp->swap_start = KTIME_MIN;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

static bool dcp_firmware_metadata_enabled(struct apple_dcp *dcp)
{
	return dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL;
}

void dcp_firmware_metadata_connection(struct apple_dcp *dcp, u64 generation, bool connected)
{
	unsigned long flags;

	if (!dcp_firmware_metadata_enabled(dcp))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (generation != dcp->metadata.epoch.generation || !connected) {
		dcp->rate_proof.connection_sequence++;
		dcp->rate_proof.sequence++;
		dcp->rate_proof.rate.valid = false;
		dcp->rate_proof.lanes.valid = false;
		dcp->metadata.attributes_valid = false;
		dcp->metadata.timing_valid = false;
		dcp->metadata.hpd_seen = false;
	}
	dcp_metadata_epoch_connection(&dcp->metadata.epoch, generation, connected);
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
}

static void dcp_firmware_metadata_present_locked(struct apple_dcp *dcp)
{
	lockdep_assert_held(&dcp->metadata.lock);
	dcp->metadata.hpd_seen = true;
	/*
	 * Fresh proof may precede HPD assertion after a same-cable loss.
	 * Keep invalidated firmware metadata and old receipt tickets revoked.
	 */
	if (READ_ONCE(dcp->typec_cable_connected) &&
	    dcp->metadata.epoch.generation == READ_ONCE(dcp->typec_generation))
		dcp->metadata.epoch.connected = true;
}

/*
 * A fact a held connection waits for has changed: evaluate it again.  The
 * unlocked checks may miss a hold that has just started; such a hold ends at
 * its timeout instead.  A stopped announcement is evaluated again when its
 * work is enabled; teardown quiesces the callers before draining the worker.
 */
static void dcp_announce_kick(struct apple_dcp *dcp)
{
	struct apple_connector *con = READ_ONCE(dcp->connector);

	if (READ_ONCE(dcp->announce.holding) && !READ_ONCE(dcp->announce.stopped) && con &&
	    READ_ONCE(con->dcp) == to_platform_device(dcp->dev))
		schedule_work(&con->hotplug_wq);
}

/*
 * While its pipe is stopped an external display reports no hotplug to DRM
 * (see dcp_external_hotplug()). When the firmware announces the display
 * there, as when the link returns from system sleep with the CRTC off, the
 * announcement still holds for the modes that need it, so record it. The
 * connector is not notified, just as for an announcement on DPMS wake.
 *
 * Only a link requested for the current cable generation can be announced:
 * a callback that arrives before that belongs to a link already released.
 */
void dcp_firmware_metadata_announced(struct apple_dcp *dcp)
{
	unsigned long flags;

	if (!dcp_firmware_metadata_enabled(dcp) ||
	    !READ_ONCE(dcp->dptxport[0].connected))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	dcp_firmware_metadata_present_locked(dcp);
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	dcp_announce_kick(dcp);
}

void dcp_firmware_metadata_hotplug(struct apple_dcp *dcp, bool connected)
{
	struct apple_connector *con = READ_ONCE(dcp->connector);
	unsigned long flags;

	if (!dcp_firmware_metadata_enabled(dcp))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (connected) {
		dcp_firmware_metadata_present_locked(dcp);
	} else if (dcp->metadata.hpd_seen) {
		dcp->metadata.hpd_seen = false;
		dcp->rate_proof.connection_sequence++;
		dcp->rate_proof.sequence++;
		dcp->rate_proof.rate.valid = false;
		dcp->rate_proof.lanes.valid = false;
		if (con && READ_ONCE(con->dcp) == to_platform_device(dcp->dev))
			WRITE_ONCE(con->edid_invalidation_pending, true);
		dcp_metadata_epoch_connection(&dcp->metadata.epoch,
					      dcp->metadata.epoch.generation, false);
		dcp->metadata.attributes_valid = false;
		dcp->metadata.timing_valid = false;
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (connected)
		dcp_180hz_notify(dcp);
}

void dcp_firmware_metadata_begin(struct apple_dcp *dcp)
{
	unsigned long flags;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	dcp->chunks.metadata_ticket = dcp_metadata_begin(&dcp->metadata.epoch);
	dcp->chunks.received = 0;
	dcp->chunks.contiguous = true;
	dcp->chunks.payload_complete = false;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
}

static void dcp_firmware_metadata_notify(struct apple_dcp *dcp)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	ready = dcp->metadata.attributes_valid && dcp->metadata.timing_valid &&
		dcp_metadata_matches(&dcp->metadata.epoch, dcp->metadata.attributes_ticket) &&
		dcp_metadata_matches(&dcp->metadata.epoch, dcp->metadata.timing_ticket);
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (ready && dcp->connector && READ_ONCE(dcp->connector->connected))
		schedule_work(&dcp->connector->hotplug_wq);
}

static bool dcp_firmware_metadata_complete(struct apple_dcp *dcp)
{
	return dcp->chunks.contiguous && dcp->chunks.payload_complete &&
	       dcp_metadata_matches(&dcp->metadata.epoch, dcp->chunks.metadata_ticket);
}

void dcp_firmware_metadata_attributes(struct apple_dcp *dcp,
				      const struct dcp_display_metadata *attributes)
{
	unsigned long flags;

	if (!dcp_firmware_metadata_enabled(dcp))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (dcp_metadata_matches(&dcp->metadata.epoch, dcp->chunks.metadata_ticket)) {
		dcp->metadata.attributes_valid = attributes && dcp_firmware_metadata_complete(dcp);
		if (dcp->metadata.attributes_valid) {
			dcp->metadata.attributes = *attributes;
			dcp->metadata.attributes_ticket = dcp->chunks.metadata_ticket;
		}
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	dcp_firmware_metadata_notify(dcp);
}

void dcp_firmware_metadata_timing(struct apple_dcp *dcp)
{
	unsigned long flags;
	unsigned int i;

	if (!dcp_firmware_metadata_enabled(dcp))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (!dcp_metadata_matches(&dcp->metadata.epoch, dcp->chunks.metadata_ticket))
		goto unlock;
	dcp->metadata.timing_valid = false;
	if (!dcp_firmware_metadata_complete(dcp))
		goto unlock;
	for (i = 0; i < dcp->nr_modes; i++) {
		if (!(dcp->modes[i].mode.type & DRM_MODE_TYPE_PREFERRED) ||
		    !dcp->modes[i].metadata_timing_valid)
			continue;
		dcp->metadata.preferred = dcp->modes[i].mode;
		dcp->metadata.timing_ticket = dcp->chunks.metadata_ticket;
		dcp->metadata.timing_valid = true;
		break;
	}
unlock:
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	dcp_firmware_metadata_notify(dcp);
}

bool dcp_firmware_metadata_current(struct apple_dcp *dcp, u64 generation)
{
	unsigned long flags;
	bool valid;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	valid = !dcp->metadata.epoch.invalidated && dcp->metadata.epoch.connected &&
		generation == dcp->metadata.epoch.generation;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	return valid;
}

const struct drm_edid *dcp_firmware_edid(struct apple_dcp *dcp, u64 *generation)
{
	struct dcp_display_metadata attributes;
	struct drm_display_mode preferred;
	const struct drm_edid *edid;
	unsigned long flags;
	bool valid;

	if (!dcp_firmware_metadata_enabled(dcp))
		return ERR_PTR(-ENODEV);
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	valid = dcp->metadata.attributes_valid && dcp->metadata.timing_valid &&
		dcp_metadata_matches(&dcp->metadata.epoch, dcp->metadata.attributes_ticket) &&
		dcp_metadata_matches(&dcp->metadata.epoch, dcp->metadata.timing_ticket);
	if (valid) {
		attributes = dcp->metadata.attributes;
		preferred = dcp->metadata.preferred;
		*generation = dcp->metadata.epoch.generation;
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (!valid)
		return ERR_PTR(-ENODATA);
	edid = dcp_metadata_edid(&attributes, &preferred);
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	valid = !dcp->metadata.epoch.invalidated && dcp->metadata.epoch.connected &&
		*generation == dcp->metadata.epoch.generation;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (!valid && !IS_ERR(edid)) {
		drm_edid_free(edid);
		return ERR_PTR(-ESTALE);
	}
	return edid;
}

void dcp_set_dimensions(struct apple_dcp *dcp)
{
	int i;
	int width_mm = dcp->width_mm;
	int height_mm = dcp->height_mm;

	if (width_mm == 0 || height_mm == 0) {
		width_mm = dcp->panel.width_mm;
		height_mm = dcp->panel.height_mm;
	}

	/*
	 * This callback runs on the ordered RTKit RX worker. A mode probe
	 * can hold mode_config.mutex while waiting for an AFK reply which
	 * that same worker still has to dispatch. Defer the DRM update.
	 */
	if (dcp->connector && !READ_ONCE(dcp->quiescing))
		schedule_work(&dcp->connector->hotplug_wq);

	/*
	 * Fix up any probed modes. Modes are created when parsing
	 * TimingElements, dimensions are calculated when parsing
	 * DisplayAttributes, and TimingElements may be sent first
	 */
	for (i = 0; i < dcp->nr_modes; ++i) {
		dcp->modes[i].mode.width_mm = width_mm;
		dcp->modes[i].mode.height_mm = height_mm;
	}
}

void dcp_update_connector_dimensions(struct apple_dcp *dcp,
				     struct drm_connector *connector)
{
	int width_mm = READ_ONCE(dcp->width_mm);
	int height_mm = READ_ONCE(dcp->height_mm);

	lockdep_assert_held(&connector->dev->mode_config.mutex);
	if (!width_mm || !height_mm) {
		width_mm = dcp->panel.width_mm;
		height_mm = dcp->panel.height_mm;
	}
	connector->display_info.width_mm = width_mm;
	connector->display_info.height_mm = height_mm;
}

bool dcp_has_panel(struct apple_dcp *dcp)
{
	return dcp->panel.width_mm > 0;
}

int dcp_set_crc(struct drm_crtc *crtc, bool enabled)
{
	struct apple_crtc *ac = to_apple_crtc(crtc);
	struct apple_dcp *dcp = platform_get_drvdata(ac->dcp);

	WRITE_ONCE(dcp->crc_enabled, enabled);

	return 0;
}

/*
 * Helper to send a DRM vblank event. We do not know how call swap_submit_dcp
 * without surfaces. To avoid timeouts in drm_atomic_helper_wait_for_vblanks
 * send a vblank event via a workqueue.
 */
static void dcp_delayed_vblank(struct work_struct *work)
{
	struct apple_dcp *dcp;

	struct apple_crtc *crtc;

	dcp = container_of(work, struct apple_dcp, vblank_wq);
	/* RTKit can report a crash before the pipeline has a CRTC. */
	crtc = READ_ONCE(dcp->crtc);
	if (!crtc)
		return;
	mdelay(5);
	dcp_drm_crtc_vblank(crtc);
}

static struct apple_dcp_afkep *dcp_afkep(struct apple_dcp *dcp, u8 endpoint)
{
	/* DPAV 0x2a on H17P overlaps the legacy DPTX endpoint number. */
	if (dcp->dcpavservep && endpoint == dcp->dcpavservep->endpoint)
		return dcp->dcpavservep;
	if (dcp->dptxep && endpoint == dcp->dptxep->endpoint)
		return dcp->dptxep;
	switch (endpoint) {
	case AV_ENDPOINT:
		return dcp->avep;
	case SYSTEM_ENDPOINT:
		return dcp->systemep;
	case DISP0_ENDPOINT:
		return dcp->ibootep;
	case DPTX_CONTROLLER_ENDPOINT:
		return dcp->dptx_controller_ep;
	default:
		return NULL;
	}
}

#define DCP_SWAP_WATCHDOG_MS		1000
#define DCP_SWAP_WATCHDOG_RETRAINS	5

/*
 * DCP drops swaps without completing them while an external pipe is not
 * enabled, for instance after a modeset that raced a Type-C sink which had
 * not asserted HPD yet. Userspace would then wait for the flip until the
 * commit times out, stalling every output it drives. Complete the flip,
 * mark the mode invalid so later commits do not wait for DCP, and let the
 * hotplug worker re-apply the active CRTC.  On H17P the dropped present is
 * also retired, so that the commands queued behind it can run.
 */
VISIBLE_IF_KUNIT void dcp_swap_watchdog(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     swap_watchdog_wq);

	if (!iomfb_retire_dropped_present_h17p(dcp))
		return;

	dev_warn(dcp->dev, "swap not completed, retraining the display\n");
	dcp_mode_invalidate(&dcp->mode_state);
	dcp_drm_crtc_vblank(dcp->crtc);
	if (dcp->connector &&
	    dcp->swap_watchdog_retrains++ < DCP_SWAP_WATCHDOG_RETRAINS)
		schedule_work(&dcp->connector->hotplug_wq);
}

void dcp_swap_watchdog_arm(struct apple_dcp *dcp)
{
	/* 14.7: a missing D589 times the IOMFB queue out instead. */
	if (dcp_is_typec_output(dcp) && !iomfb_is_v14_7_external(dcp)) {
		dcp->swap_watchdog_deadline = jiffies +
			msecs_to_jiffies(DCP_SWAP_WATCHDOG_MS);
		mod_delayed_work(system_wq, &dcp->swap_watchdog_wq,
				 msecs_to_jiffies(DCP_SWAP_WATCHDOG_MS));
	}
}

void dcp_swap_watchdog_complete(struct apple_dcp *dcp)
{
	cancel_delayed_work(&dcp->swap_watchdog_wq);
	dcp->swap_watchdog_retrains = 0;
}

/*
 * The firmware reports that the DisplayPort sink stopped answering, or that
 * the link did not start.  On the external H17P display, the mode set
 * before, or being set, has no link any more: drop it, also across a mode
 * set in flight, and have the hotplug worker set the mode of the CRTC again
 * or report the display lost (dcp_external_relink()).  A mode set still in
 * flight reports this itself when it finishes, and before any mode was set
 * there is nothing to recover.
 */
void dcp_external_link_fault(struct apple_dcp *dcp)
{
	dcp_180hz_invalidate(dcp);
	if (dcp_is_external_h17p(dcp) && dcp_mode_invalidate(&dcp->mode_state))
		dcp_handle_hotplug_actions(dcp, DCP_HOTPLUG_NOTIFY);
}

static void dcp_recv_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;
	struct apple_dcp_afkep *ep;

	if (READ_ONCE(dcp->quiescing))
		return;

	trace_dcp_recv_msg(dcp, endpoint, message);

	/*
	 * H17P starts endpoints that have no AFK instance behind them (the
	 * remote allocator endpoint, for one).  Log their messages rather than
	 * dereferencing a NULL afkep or warning about an unknown endpoint.
	 */
	if (dcp_fail_stop(dcp) && endpoint != IOMFB_ENDPOINT &&
	    !dcp_afkep(dcp, endpoint)) {
		dev_dbg_ratelimited(dcp->dev, "ep %#04x: %#llx\n", endpoint,
				    message);
		return;
	}

	if (endpoint == IOMFB_ENDPOINT)
		return iomfb_recv_msg(dcp, message);

	ep = dcp_afkep(dcp, endpoint);

	if (!ep) {
		dev_warn_ratelimited(dcp->dev,
				     "dropping message for unhandled endpoint %#x\n",
				     endpoint);
		return;
	}

	afk_receive_message(ep, message);
}

static void dcp_rtk_crashed(void *cookie, const void *crashlog, size_t crashlog_size)
{
	struct apple_dcp *dcp = cookie;

	if (READ_ONCE(dcp->quiescing))
		return;

	dcp->crashed = true;
	dev_err(dcp->dev, "DCP has crashed\n");
	if (dcp_fail_stop(dcp) &&
	    dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G) {
		/* No in-place restart exists; the last scanout stays retained. */
		dev_err(dcp->dev, "a reboot is required to restore the display\n");
		/*
		 * Nothing will complete a present that was in flight.  Before
		 * the pipeline is linked there is no event to complete.
		 */
		if (READ_ONCE(dcp->crtc))
			schedule_work(&dcp->vblank_wq);
	}
	if (dcp->connector) {
		dcp->connector->connected = 0;
		drm_edid_free(dcp->connector->drm_edid);
		dcp->connector->drm_edid = NULL;
		schedule_work(&dcp->connector->hotplug_wq);
	}
	complete(&dcp->start_done);
}

/*
 * Check that every IOMMU page of [iova, iova + size) translates to the same
 * offset from @phys as it has from @iova.
 */
static int dcp_check_iova_contiguous(struct iommu_domain *domain,
				     dma_addr_t iova, size_t size,
				     phys_addr_t phys)
{
	unsigned long pgsize = domain->pgsize_bitmap ?
			       1UL << __ffs(domain->pgsize_bitmap) : PAGE_SIZE;
	dma_addr_t end = iova + size;
	dma_addr_t addr = iova;

	while (addr < end) {
		if (iommu_iova_to_phys(domain, addr) != phys + (addr - iova))
			return -EINVAL;
		addr = ALIGN_DOWN(addr, pgsize) + pgsize;
	}

	return 0;
}

static bool dcp_v14_7_in(u64 base, u64 length, u64 addr, u64 size)
{
	return length && size && addr >= base && addr - base < length &&
	       size <= length - (addr - base);
}

/*
 * Only the DCP's own no-map "apple,asc-mem" reservations may back a buffer
 * the firmware names; @oslog selects the "apple,dcp-os-log" carveout.
 */
static bool dcp_v14_7_reserved(struct apple_dcp *dcp, u64 pa, size_t size,
			       bool oslog)
{
	struct device_node *node;
	struct resource res;
	bool found = false;
	int i;

	for (i = 0; (node = of_parse_phandle(dcp->dev->of_node, "memory-region", i)); i++) {
		struct reserved_mem *rmem = of_reserved_mem_lookup(node);

		if (of_device_is_compatible(node, "apple,asc-mem") &&
		    of_property_read_bool(node, "no-map") &&
		    oslog == of_property_read_bool(node, "apple,dcp-os-log") &&
		    !of_address_to_resource(node, 0, &res) && rmem &&
		    rmem->base == res.start && rmem->size == resource_size(&res) &&
		    dcp_v14_7_in(res.start, resource_size(&res), pa, size))
			found = true;
		of_node_put(node);
		if (found)
			break;
	}
	return found;
}

/*
 * RTKit system buffers of the adopted 14.7 session.  A buffer the firmware
 * names by DVA must translate contiguously into one of the DCP's reserved
 * regions; the OS log names its carveout by physical address instead.  New
 * buffers come from the DCP's DMA window.  Nothing is ever returned: the
 * session keeps them until reboot.
 */
static int dcp_rtk_shmem_setup_v14_7(struct apple_dcp *dcp,
				     struct apple_rtkit_shmem *bfr)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dcp->dev);
	struct dcp_v14_7_session *s = dcp->v14_7;
	phys_addr_t pa;
	size_t off;

	if (!domain || !bfr->size || bfr->size > SZ_16M ||
	    s->rtkit_bytes > SZ_64M - bfr->size)
		goto fail;

	if (!bfr->iova) {
		bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size, &bfr->iova,
						 GFP_KERNEL);
		if (!bfr->buffer)
			goto fail;
		s->rtkit_bytes += bfr->size;
		if (!dcp_v14_7_in(DCP_V14_7_DVA_BASE, DCP_V14_7_DVA_SIZE,
				  bfr->iova, bfr->size))
			goto fail;
		bfr->is_mapped = false;
		dev_info(dcp->dev, "RTKit allocated %zu bytes at %pad\n",
			 bfr->size, &bfr->iova);
		return 0;
	}

	if (dcp_v14_7_reserved(dcp, bfr->iova, bfr->size, true)) {
		/* A physical address can also fall in the DVA window: refuse that. */
		for (off = 0; off < bfr->size;) {
			if (iommu_iova_to_phys(domain, bfr->iova + off))
				goto fail;
			off += SZ_16K - ((bfr->iova + off) & (SZ_16K - 1));
		}
		pa = bfr->iova;
	} else {
		if (!dcp_v14_7_in(DCP_V14_7_DVA_BASE, DCP_V14_7_DVA_SIZE,
				  bfr->iova, bfr->size))
			goto fail;
		pa = iommu_iova_to_phys(domain, bfr->iova);
		if (!pa || !dcp_v14_7_reserved(dcp, pa, bfr->size, false))
			goto fail;
		/* The initial unaligned span and every crossed 16 KiB page. */
		for (off = SZ_16K - (bfr->iova & (SZ_16K - 1)); off < bfr->size;
		     off += SZ_16K)
			if (iommu_iova_to_phys(domain, bfr->iova + off) != pa + off)
				goto fail;
	}

	/* Firmware writes without CPU cache maintenance. */
	bfr->buffer = memremap(pa, bfr->size, MEMREMAP_WC);
	if (!bfr->buffer)
		goto fail;
	bfr->is_mapped = true;
	s->rtkit_bytes += bfr->size;
	dev_info(dcp->dev, "RTKit borrowed %zu bytes at %pad -> %pa\n",
		 bfr->size, &bfr->iova, &pa);
	return 0;

fail:
	dev_err(dcp->dev, "RTKit buffer request for %zu bytes at %pad refused\n",
		bfr->size, &bfr->iova);
	iomfb_v14_7_fail(dcp, "RTKit buffer request refused");
	return -EINVAL;
}

static int dcp_rtk_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;
	int ret;

	if (iomfb_is_v14_7(dcp))
		return dcp_rtk_shmem_setup_v14_7(dcp, bfr);

	if (bfr->iova) {
		struct iommu_domain *domain =
			iommu_get_domain_for_dev(dcp->dev);
		phys_addr_t phy_addr;

		if (!domain)
			return -ENOMEM;

		// TODO: get map from device-tree
		phy_addr = iommu_iova_to_phys(domain, bfr->iova);
		if (!phy_addr)
			return -ENOMEM;

		/* memremap() below needs the whole buffer physically contiguous */
		ret = dcp_check_iova_contiguous(domain, bfr->iova, bfr->size,
						phy_addr);
		if (ret) {
			dev_err(dcp->dev,
				"shmem_setup: iova %pad (%#zx bytes) is not physically contiguous\n",
				&bfr->iova, bfr->size);
			return ret;
		}

		/* Firmware writes without CPU cache maintenance. */
		bfr->buffer = memremap(phy_addr, bfr->size, MEMREMAP_WC);
		if (!bfr->buffer)
			return -ENOMEM;

		bfr->is_mapped = true;
		dev_info(dcp->dev,
			 "shmem_setup: iova: %lx -> pa: %lx -> iomem: %lx\n",
			 (uintptr_t)bfr->iova, (uintptr_t)phy_addr,
			 (uintptr_t)bfr->buffer);
	} else {
		bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size,
						 &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;

		dev_info(dcp->dev, "shmem_setup: iova: %lx, buffer: %lx\n",
			 (uintptr_t)bfr->iova, (uintptr_t)bfr->buffer);
	}

	return 0;
}

static void dcp_rtk_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(dcp->dev, bfr->size, bfr->buffer, bfr->iova);
}

static struct apple_rtkit_ops rtkit_ops = {
	.crashed = dcp_rtk_crashed,
	.recv_message = dcp_recv_msg,
	.shmem_setup = dcp_rtk_shmem_setup,
	.shmem_destroy = dcp_rtk_shmem_destroy,
};

static const struct apple_rtkit_ops external_h17p_rtkit_ops = {
	.flags = APPLE_RTKIT_COPROC_PLACES_BUFFERS,
	.crashed = dcp_rtk_crashed,
	.recv_message = dcp_recv_msg,
	.shmem_setup = dcp_rtk_shmem_setup,
	.shmem_destroy = dcp_rtk_shmem_destroy,
};

void dcp_send_message(struct apple_dcp *dcp, u8 endpoint, u64 message)
{
	if (READ_ONCE(dcp->quiescing))
		return;
	trace_dcp_send_msg(dcp, endpoint, message);
	apple_rtkit_send_message(dcp->rtk, endpoint, message, NULL,
				 true);
}

int dcp_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct platform_device *pdev = to_apple_crtc(crtc)->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct drm_crtc_state *crtc_state;
	bool needs_modeset;

	if (dcp->crashed)
		return -EINVAL;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	/* External H17P does not program adaptive sync. */
	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	    crtc_state->vrr_enabled)
		return -EINVAL;

	needs_modeset = drm_atomic_crtc_needs_modeset(crtc_state) ||
			!READ_ONCE(dcp->mode_state.valid);
	if (!needs_modeset && (!dcp->connector || !dcp->connector->connected)) {
		/*
		 * Resume restores the mode before the firmware reports the
		 * display back, so a plane-only commit lands here while the
		 * connector is still marked disconnected.  Rejecting it makes
		 * the compositor fail every flip and give up on the output;
		 * dcp_flush() defers the commit until the link returns.
		 */
		dev_dbg(dcp->dev,
			"crtc_atomic_check: deferring commit, link still down\n");
	}

	return 0;
}

int dcp_get_connector_type(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fixed_connector_type;
}

bool dcp_has_typec_routes(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->nr_typec_routes;
}

#define DPTX_CONNECT_TIMEOUT msecs_to_jiffies(2000)
#define DPTX_TUNNEL_CONNECT_TIMEOUT msecs_to_jiffies(8000)
#define DPTX_RECONNECT_DELAY msecs_to_jiffies(1000)
#define DPTX_RECONNECT_RETRIES 5

/*
 * Connect DPTX port @port. On failure, @retryable (if given) tells whether
 * the attempt left nothing outstanding in the firmware, so that a later
 * connection may simply try again: the service was not published yet, or a
 * link that never came up was withdrawn and the firmware acknowledged that.
 */
static int dcp_dptx_connect_raw(struct apple_dcp *dcp, u32 port, bool *retryable)
{
	unsigned long timeout;
	int ret = 0, err;

	if (retryable)
		*retryable = false;
	if (!dcp->phy) {
		dev_warn(dcp->dev, "dcp_dptx_connect: missing phy\n");
		return -ENODEV;
	}
	dev_dbg(dcp->dev,
		"%s(port=%d) target=%u:%u typec=%d route=%s conn_type=%d connected=%d\n",
		__func__, port, dcp->dptx_die, dcp->dptx_phy,
		dcp_is_typec_output(dcp),
		dcp->active_typec_route ? "borrowed" : "fixed",
		dcp->connector_type, dcp->dptxport[port].connected);

	mutex_lock(&dcp->hpd_mutex);
	if (!dptx_service_available(&dcp->dptxport[port])) {
		dev_warn(dcp->dev, "dcp_dptx_connect: dptx service for port %d not enabled\n", port);
		ret = -ENODEV;
		/* No firmware request has been made; the first publication can retry. */
		if (retryable && !READ_ONCE(dcp->dptxport[port].closed)) {
			*retryable = true;
			ret = -EAGAIN;
		}
		goto out_unlock;
	}

	if (dcp->dptxport[port].connected)
		goto out_unlock;

	reinit_completion(&dcp->dptxport[port].linkcfg_completion);
	dcp->dptxport[port].atcphy = dcp->phy;
	ret = dptxport_validate_connection(dcp->dptxport[port].service,
					   dcp->dptx_dfp_port,
					   dcp->dptx_phy, dcp->dptx_die);
	if (ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: failed to validate DPTX target %u:%u: %d\n",
			dcp->dptx_die, dcp->dptx_phy, ret);
		goto out_unlock;
	}

	ret = dptxport_connect(dcp->dptxport[port].service,
			       dcp->dptx_dfp_port,
			       dcp->dptx_phy, dcp->dptx_die,
		       dcp_is_typec_output(dcp));
	if (ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: failed to connect DPTX target %u:%u: %d\n",
			dcp->dptx_die, dcp->dptx_phy, ret);
		goto out_unlock;
	}

	ret = dptxport_request_display(dcp->dptxport[port].service);
	if (ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: failed to request display: %d\n",
			ret);
		goto out_release;
	}
	dcp->dptxport[port].connected = true;
	if (dcp_is_typec_output(dcp)) {
		if (dcp_is_usb4_output(dcp) && apple_dp_tunnel_t602x())
			ret = dptxport_set_hpd_timeout(dcp->dptxport[port].service,
						       true, 8000);
		else
			ret = dptxport_set_hpd(dcp->dptxport[port].service, true);
		if (ret) {
			dev_err(dcp->dev,
				"dcp_dptx_connect: failed to assert Type-C HPD: %d\n",
				ret);
			dcp->dptxport[port].connected = false;
			goto out_release;
		}
	}

	mutex_unlock(&dcp->hpd_mutex);
	timeout = dcp_is_usb4_output(dcp) && apple_dp_tunnel_t602x() ?
		  DPTX_TUNNEL_CONNECT_TIMEOUT : DPTX_CONNECT_TIMEOUT;
	ret = wait_for_completion_timeout(&dcp->dptxport[port].linkcfg_completion,
					  timeout);
	if (!ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: timed out waiting for port %u link configuration\n",
			port);
		ret = -ETIMEDOUT;
		goto out_withdraw;
	}

	dev_dbg(dcp->dev, "dcp_dptx_connect: waited %d ms for link\n",
		jiffies_to_msecs(timeout - ret));

	usleep_range(5, 10);

	if (dcp->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
		dptxport_set_hpd(dcp->dptxport[port].service, true);

	if (dcp->avep)
		av_service_connect(dcp);

	return 0;

out_withdraw:
	mutex_lock(&dcp->hpd_mutex);
	if (!dcp_is_external_h17p(dcp)) {
		dcp->dptxport[port].connected = false;
		goto out_release;
	}
	/*
	 * No link configuration arrived, typically because the sink went away
	 * while the firmware trained it.  Withdraw the request the way a
	 * disconnect does, HPD first.  Only once the firmware has acknowledged
	 * both is nothing of this attempt outstanding; until then the port
	 * stays held.
	 */
	err = -ENODEV;
	if (dptx_service_available(&dcp->dptxport[port])) {
		err = dcp_is_typec_output(dcp) ?
		      dptxport_set_hpd(dcp->dptxport[port].service, false) : 0;
		if (!err)
			err = dptxport_release_display(dcp->dptxport[port].service);
	}
	if (err) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: port %u not withdrawn, still held: %d\n",
			port, err);
		goto out_unlock;
	}
	dev_info(dcp->dev, "dcp_dptx_connect: port %u withdrawn, the next connection retries\n",
		 port);
	dcp->dptxport[port].connected = false;
	if (retryable)
		*retryable = true;
	goto out_unlock;

out_release:
	dptxport_release_display(dcp->dptxport[port].service);

out_unlock:
	mutex_unlock(&dcp->hpd_mutex);
	return ret;
}

static void dcp_external_report_disconnected(struct apple_dcp *dcp)
{
	unsigned int action;

	guard(mutex)(&dcp->iomfb.lock);
	/* A paused callback from the old sink must not be replayed on wake. */
	dcp->external_hotplug_pending = false;
	action = dcp_mode_hotplug(&dcp->mode_state, false,
				  dcp->connector ? &dcp->connector->connected : NULL);
	if (dcp->connector) {
		WRITE_ONCE(dcp->connector->edid_invalidation_pending, true);
		action |= DCP_HOTPLUG_NOTIFY;
	}
	/* The existing worker delivers DRM notifications outside our locks. */
	dcp_handle_hotplug_actions(dcp, action);
}

static void dcp_external_publish_generation(struct apple_dcp *dcp)
{
	lockdep_assert_held(&dcp->external_lock);
	guard(mutex)(&dcp->iomfb.lock);
	dcp->external_rx_generation = dcp->external_generation;
}

/* external_lock is never taken by the serialized IOMFB receive callbacks. */
static int dcp_external_link_locked(struct apple_dcp *dcp)
{
	bool retryable;
	int ret;

	lockdep_assert_held(&dcp->external_lock);
	if (!dcp->external_prepared)
		return -EAGAIN;
	if (dcp->external_error)
		return dcp->external_error;
	if (!dcp->external_cable)
		return -ENODEV;
	ret = dcp_dptx_connect_raw(dcp, 0, &retryable);
	if (ret && !retryable)
		dcp->external_error = ret;
	return ret;
}

static int dcp_external_start_locked(struct apple_dcp *dcp)
{
	int ret;

	lockdep_assert_held(&dcp->external_lock);
	ret = dcp_external_link_locked(dcp);
	if (ret || dcp->external_running)
		return ret;
	/* A deferred stop sent nothing: the pipe is still powered. */
	if (dcp->external_stop_deferred) {
		dcp->external_stop_deferred = false;
		dcp->external_stop_retries = 0;
		dcp->external_running = true;
		iomfb_pause_presentation(dcp, false);
		return 0;
	}
	iomfb_pause_presentation(dcp, true);
	/* Even a timed-out first activation can have changed firmware state. */
	dcp->external_power_attempted = true;
	ret = iomfb_external_power_h17p(dcp, true);
	if (ret) {
		dcp->external_error = ret;
		return ret;
	}
	dcp->external_running = true;
	dcp->external_stopped = false;
	WRITE_ONCE(dcp->pipe_enabled_h17p, true);
	WRITE_ONCE(dcp->typec_crtc_off, false);
	iomfb_pause_presentation(dcp, false);
	return 0;
}

static int dcp_dptx_connect(struct apple_dcp *dcp, u32 port)
{
	if (iomfb_is_v14_7_external(dcp))
		return dcp_ext_v14_7_connect(dcp);
	if (!dcp_is_external_h17p(dcp))
		return dcp_dptx_connect_raw(dcp, port, NULL);
	guard(mutex)(&dcp->external_lock);
	return dcp_external_link_locked(dcp);
}

#define DCP_EXTERNAL_STOP_RETRIES	3

/* The generation token belongs to the CRTC that was actually enabled. */
static int dcp_external_stop_locked(struct apple_dcp *dcp, u64 generation)
{
	const char *step;
	int ret;

	lockdep_assert_held(&dcp->external_lock);
	if (generation != dcp->external_generation)
		return 0;
	if (dcp->external_error)
		return dcp->external_error;
	if (dcp->external_stopped && !READ_ONCE(dcp->dptxport[0].connected))
		return 0;
	iomfb_pause_presentation(dcp, true);
	dcp->external_running = false;
	/* Route setup can call this before IOMFB or its shared memory exists. */
	if (dcp->external_stopped || !dcp->external_power_attempted)
		goto release_link;
	step = "settling the queue";
	ret = iomfb_queue_settle(dcp, msecs_to_jiffies(500));
	if (ret == -ETIMEDOUT) {
		/*
		 * Nothing was sent, so no late reply can race a retry.  The
		 * session stays owned, but not latched: a SAFE transition, the
		 * retry work or the next enable of the CRTC takes it from here.
		 */
		dev_warn(dcp->dev, "external display stop deferred: IOMFB queue busy\n");
		dcp->external_stop_deferred = true;
		if (dcp->external_stop_retries++ < DCP_EXTERNAL_STOP_RETRIES)
			mod_delayed_work(system_wq, &dcp->external_stop_retry_wq,
					 msecs_to_jiffies(DCP_SWAP_WATCHDOG_MS));
		return ret;
	}
	dcp->external_stop_deferred = false;
	dcp->external_stop_retries = 0;
	if (!ret) {
		step = "powering the pipe off";
		ret = iomfb_external_power_h17p(dcp, false);
	}
	if (!ret) {
		step = "settling the queue after power-off";
		ret = iomfb_queue_settle(dcp, msecs_to_jiffies(500));
	}
	if (ret)
		goto failed;
	/* This is a transport acknowledgment, not permission to free scanout. */
	WRITE_ONCE(dcp->pipe_enabled_h17p, false);
release_link:
	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (dcp->dptxport[0].connected) {
			step = "withdrawing the DPTX link";
			if (!dptx_service_available(&dcp->dptxport[0])) {
				ret = -ENODEV;
				goto failed;
			}
			ret = dptxport_set_hpd(dcp->dptxport[0].service, false);
			if (!ret)
				ret = dptxport_release_display(dcp->dptxport[0].service);
			if (ret)
				goto failed;
			dcp->dptxport[0].connected = false;
		}
	}
	dcp->external_stopped = true;
	WRITE_ONCE(dcp->typec_crtc_off, true);
	if (dcp->avep)
		av_service_disconnect(dcp);
	return 0;
failed:
	/* A retry could race a late reply. Keep this session blocked and owned. */
	dev_err(dcp->dev, "external display stop failed %s, session held: %d\n", step, ret);
	dcp->external_error = ret;
	return ret;
}

static int dcp_external_disconnect(struct apple_dcp *dcp, bool physical)
{
	int ret;

	cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
	cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
	guard(mutex)(&dcp->external_lock);
	/* Invalidate physical EDID before waiting for firmware power/link teardown. */
	if (physical) {
		WRITE_ONCE(dcp->external_physically_absent, true);
		scoped_guard(mutex, &dcp->hpd_mutex) {
			WRITE_ONCE(dcp->typec_cable_connected, false);
			dcp->typec_generation++;
			dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
			dcp_announce_end(&dcp->announce);
		}
		/* HPD is fenced; retire the AV owner after releasing hpd_mutex. */
		dpavservep_detach(dcp);
	}
	ret = dcp_external_stop_locked(dcp, dcp->external_generation);
	if (physical) {
		dcp->external_cable = false;
		dcp->external_generation++;
		dcp_external_publish_generation(dcp);
		/* A missing cable is reported even when its hardware remains owned. */
		dcp_external_report_disconnected(dcp);
	}
	return ret;
}

/* Stop a deferred session again once the swap watchdog had its turn. */
static void dcp_external_stop_retry(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp,
					     external_stop_retry_wq);

	guard(mutex)(&dcp->external_lock);
	if (dcp->external_stop_deferred)
		dcp_external_stop_locked(dcp, dcp->external_generation);
}

static void dcp_external_stop_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(work, struct apple_dcp, external_stop_work);

	u64 generation;

	scoped_guard(mutex, &dcp->iomfb.lock)
		generation = dcp->external_stop_generation;
	cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
	cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
	guard(mutex)(&dcp->external_lock);
	if (generation == dcp->external_generation) {
		/* The deferred disconnect also covers missing out-of-band HPD. */
		dpavservep_detach(dcp);
		dcp_external_stop_locked(dcp, generation);
		dcp_external_report_disconnected(dcp);
	}
}

bool dcp_external_hotplug(struct apple_dcp *dcp, bool connected)
{
	if (!dcp_is_external_h17p(dcp))
		return false;
	/* No lifecycle or HPD mutex may be acquired from the RX callback. */
	lockdep_assert_held(&dcp->iomfb.lock);
	if (READ_ONCE(dcp->quiescing) || READ_ONCE(dcp->external_physically_absent))
		return true;
	if (dcp->iomfb.presentation_paused) {
		dcp->external_hotplug_pending = true;
		dcp->external_hotplug_connected = connected;
		if (connected)
			dcp_firmware_metadata_announced(dcp);
		/* Publish initial/new-cable discovery, never same-cable DPMS wake. */
		if (connected && READ_ONCE(dcp->external_cable) &&
		    !READ_ONCE(dcp->external_error) &&
		    (!READ_ONCE(dcp->external_power_attempted) ||
		     (READ_ONCE(dcp->external_stopped) &&
		      READ_ONCE(dcp->external_rx_generation) !=
		      READ_ONCE(dcp->external_crtc_generation) &&
		      READ_ONCE(dcp->dptxport[0].connected)))) {
			unsigned int action = dcp_mode_hotplug(&dcp->mode_state, true,
							     dcp->connector ?
							     &dcp->connector->connected : NULL);

			dcp_handle_hotplug_actions(dcp, action);
		}
		return true;
	}
	if (!connected && !READ_ONCE(dcp->iomfb.presentation_paused)) {
		/* Fence admission in RX before a busy actor can delay teardown. */
		dcp_firmware_metadata_hotplug(dcp, false);
		dcp->external_stop_generation = dcp->external_rx_generation;
		schedule_work(&dcp->external_stop_work);
		return true;
	}
	return READ_ONCE(dcp->iomfb.presentation_paused);
}

static bool dcp_edid_is_placeholder(const struct drm_edid *drm_edid)
{
	const u8 *raw = (const u8 *)drm_edid_raw(drm_edid);
	static const u8 name[] = "Non-PnP";
	unsigned int i;

	if (!raw)
		return false;

	/* EDID contains interior NUL bytes, so this cannot use strnstr(). */
	for (i = 0; i + sizeof(name) - 1 <= sizeof(struct edid); i++) {
		if (!memcmp(raw + i, name, sizeof(name) - 1))
			return true;
	}

	return false;
}

void dcp_retry_placeholder_edid(struct apple_dcp *dcp,
				const struct drm_edid *drm_edid)
{
	guard(mutex)(&dcp->hpd_mutex);
	if (!dcp_is_typec_output(dcp) || !dcp->typec_cable_connected ||
	    dcp->placeholder_retried)
		return;
	if (!dcp_edid_is_placeholder(drm_edid))
		return;

	dcp->placeholder_retried = true;
	dcp->placeholder_generation = dcp->typec_generation;
	schedule_delayed_work(&dcp->placeholder_edid_wq, msecs_to_jiffies(300));
}

static void dcp_placeholder_edid_work(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     placeholder_edid_wq);
	struct apple_epic_service *service;
	u64 generation;
	int ret;

	mutex_lock(&dcp->hpd_mutex);
	generation = dcp->placeholder_generation;
	if (!dcp->typec_cable_connected || !dcp->dptxport[0].connected ||
	    !dptx_service_available(&dcp->dptxport[0]) || generation != dcp->typec_generation)
		goto out_unlock;
	service = dcp->dptxport[0].service;

	/*
	 * Some adapters answer the first connection with a 1024x768
	 * placeholder and publish the panel EDID only after HPD drops
	 * and returns. One pulse; a second placeholder is left alone.
	 */
	ret = dptxport_set_hpd(service, false);
	if (ret) {
		dev_info(dcp->dev, "placeholder EDID: HPD drop failed: %d\n",
			 ret);
		goto out_unlock;
	}
	mutex_unlock(&dcp->hpd_mutex);

	msleep(1000);

	mutex_lock(&dcp->hpd_mutex);
	if (!dcp->typec_cable_connected || !dcp->dptxport[0].connected ||
	    generation != dcp->typec_generation)
		goto out_unlock;

	ret = dptxport_set_hpd(service, true);
	if (ret)
		dev_info(dcp->dev, "placeholder EDID: HPD assert failed: %d\n",
			 ret);
out_unlock:
	mutex_unlock(&dcp->hpd_mutex);
}

static void dcp_typec_reconnect_work(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     typec_reconnect_wq);
	int ret;

	if (!READ_ONCE(dcp->typec_cable_connected))
		return;

	ret = dcp_dptx_connect(dcp, 0);
	if (!ret) {
		dcp->typec_reconnect_tries = 0;
		return;
	}
	/* A display held for this link will not get ready. */
	if (READ_ONCE(dcp->external_error))
		dcp_announce_kick(dcp);

	if (++dcp->typec_reconnect_tries <
	    (dcp_is_usb4_output(dcp) && apple_dp_tunnel_t602x() ?
	     1 : DPTX_RECONNECT_RETRIES)) {
		mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq,
				 DPTX_RECONNECT_DELAY);
		return;
	}

	dev_err(dcp->dev, "Type-C DPTX reconnect failed after %u retries: %d\n",
		dcp->typec_reconnect_tries, ret);
}

/**
 * dcp_hold_resume_event - hold the resume hotplug event of an external display
 * @pdev: the DCP of a CRTC resumed disabled
 *
 * System sleep releases the link of an external H17P display, and with it
 * the proof of modes such as 1080p at 180 Hz, so apple_drm_drop_stale_crtcs()
 * resumes the CRTC of such a mode disabled with its link marked bad.  A
 * hotplug event then has userspace set the mode again.  Sent at once, it has
 * userspace probe before the link is back, pick a fallback mode, and set its
 * own mode again once that is offered: two mode sets the monitor has to
 * resynchronize to.
 *
 * Hold that event, not the connection, until the display's modes are in for
 * the new cable generation, or for DCP_ANNOUNCE_RESUME_TIMEOUT_MS after the
 * link work can run again, then send it once from the hotplug worker.  No
 * lock is held while waiting; a probe in the meantime reports the display
 * connected with the modes it has.
 *
 * Returns false if the caller must send the event itself.
 */
bool dcp_hold_resume_event(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct apple_connector *con = READ_ONCE(dcp->connector);

	if (!dcp_is_external_h17p(dcp) || !con || READ_ONCE(con->dcp) != pdev)
		return false;

	guard(mutex)(&dcp->hpd_mutex);
	if (!READ_ONCE(con->connected) || !dcp->typec_cable_connected ||
	    !dcp_announce_hold(&dcp->announce, dcp->typec_generation))
		return false;
	/* Freezable, like the link work: the timeout starts after thaw. */
	dcp_announce_arm(dcp);
	return true;
}

static void disconnected_hpd_event(struct apple_connector *con)
{
	if (!con)
		return;

	/* May run with DRM modeset locks held; defer property/cache cleanup. */
	WRITE_ONCE(con->connected, false);
	WRITE_ONCE(con->edid_invalidation_pending, true);
	schedule_work(&con->hotplug_wq);
}

static int dcp_dptx_disconnect(struct apple_dcp *dcp, u32 port)
{
	if (iomfb_is_v14_7_external(dcp))
		return dcp_ext_v14_7_disconnect(dcp, false);
	if (dcp_is_external_h17p(dcp))
		return dcp_external_disconnect(dcp, false);

	dev_info(dcp->dev, "%s(port=%d)\n", __func__, port);

	mutex_lock(&dcp->hpd_mutex);
	if (dptx_service_available(&dcp->dptxport[port]) && dcp->dptxport[port].connected) {
		dptxport_release_display(dcp->dptxport[port].service);
		dcp->dptxport[port].connected = false;
	}
	mutex_unlock(&dcp->hpd_mutex);

	return 0;
}

int dcp_dptx_connect_oob(struct platform_device *pdev, u32 port)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (iomfb_is_v14_7_external(dcp))
		return dcp_ext_v14_7_connect(dcp);
	if (dcp_is_external_h17p(dcp)) {
		bool new_cable;

		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
		cancel_work_sync(&dcp->external_stop_work);
		cancel_delayed_work_sync(&dcp->external_stop_retry_wq);
		guard(mutex)(&dcp->external_lock);
		new_cable = !dcp->external_cable;
		if (new_cable)
			dcp->external_generation++;
		dcp->external_cable = true;
		WRITE_ONCE(dcp->external_physically_absent, false);
		dcp_external_publish_generation(dcp);
		scoped_guard(mutex, &dcp->hpd_mutex) {
			WRITE_ONCE(dcp->typec_cable_connected, true);
			if (new_cable) {
				dcp->typec_generation++;
				dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true);
				dcp_announce_end(&dcp->announce);
			}
			dcp->placeholder_retried = false;
		}
		return dcp_external_link_locked(dcp);
	}

	if (dcp_is_typec_output(dcp)) {
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
		guard(mutex)(&dcp->hpd_mutex);
		dcp->typec_generation++;
		WRITE_ONCE(dcp->typec_cable_connected, true);
		dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true);
		dcp->typec_reconnect_tries = 0;
		dcp->placeholder_retried = false;
		cancel_delayed_work(&dcp->typec_reconnect_wq);
	}

	ret = dcp_dptx_connect(dcp, port);
	if (ret && dcp_is_typec_output(dcp))
		mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq,
				 DPTX_RECONNECT_DELAY);

	return ret;
}

int dcp_dptx_disconnect_oob(struct platform_device *pdev, u32 port)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (iomfb_is_v14_7_external(dcp))
		return dcp_ext_v14_7_disconnect(dcp, true);
	if (dcp_is_external_h17p(dcp))
		return dcp_external_disconnect(dcp, true);

	if (dcp_is_typec_output(dcp)) {
		scoped_guard(mutex, &dcp->hpd_mutex) {
			WRITE_ONCE(dcp->typec_cable_connected, false);
			dcp->typec_generation++;
			dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
		}
		WRITE_ONCE(dcp->typec_crtc_off, false);
		reinit_completion(&dcp->typec_iomfb_hpd_ready);
		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
	}

	disconnected_hpd_event(dcp->connector);

	if (dcp->avep)
		av_service_disconnect(dcp);

	if (dptx_service_available(&dcp->dptxport[port]))
		dptxport_set_hpd(dcp->dptxport[port].service, false);

	return dcp_dptx_disconnect(dcp, port);
}

static irqreturn_t dcp_dp2hdmi_hpd(int irq, void *data)
{
	struct apple_dcp *dcp = data;
	bool connected;

	guard(mutex)(&dcp_typec_fabric_lock);

	if (READ_ONCE(dcp->active_typec_route)) {
		/*
		 * Until a compositor owns the display, a live HDMI output
		 * takes its pipeline back from a direct DP-alt route: the
		 * compositor pairs the HDMI connector with it first.
		 */
		if (dcp_typec_keep_order() &&
		    gpiod_get_value_cansleep(dcp->hdmi_hpd)) {
			msleep(500);
			if (gpiod_get_value_cansleep(dcp->hdmi_hpd))
				dcp_typec_rebalance_locked(NULL, 0);
		}
		return IRQ_HANDLED;
	}
	connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);

	/* do nothing on disconnect and trust that dcp detects it itself.
	 * Parallel disconnect HPDs result drm disabling the CRTC even when it
	 * should not.
	 * The interrupt should be changed to rising but for now the disconnect
	 * IRQs might be helpful for debugging.
	 */
	dev_info(dcp->dev, "DP2HDMI HPD irq, connected:%d\n", connected);

	if (connected) {
		msleep(500);
		connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "DP2HDMI HPD irq, 500ms debounce: connected:%d\n", connected);
	}

	if (connected)
		dcp_dptx_connect(dcp, 0);

	return IRQ_HANDLED;
}

void dcp_link(struct platform_device *pdev, struct apple_crtc *crtc,
	      struct apple_connector *connector)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	WRITE_ONCE(dcp->crtc, crtc);

	/*
	 * Type-C connectors belong to physical ports and are bound by the
	 * display fabric when a pipeline takes a route, so a pipeline with no
	 * fixed output simply has no connector until then.
	 */
	if (!connector)
		return;

	dcp->fixed_connector = connector;
	if (!dcp->active_typec_route) {
		dcp->connector = connector;
		dcp->connector_type = dcp->fixed_connector_type;
	}
}


bool dcp_fw_compat_is_12_x(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fw_compat == DCP_FIRMWARE_V_12_3;
}

unsigned long* dcp_get_iomfb_surfaces(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->iomfb_surfaces;
}

int dcp_start(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	/* The external session starts lazily, at its first sink. */
	if (iomfb_is_v14_7_external(dcp))
		return dcp->rtk ? dcp_ext_v14_7_start(dcp) : -ENODEV;

	/* Even if nothing starts, dcp_wait_ready() must find it initialised. */
	init_completion(&dcp->start_done);

	if (IS_ERR_OR_NULL(dcp->rtk))
		return -ENODEV;

	/*
	 * The adopted 14.7 panel session runs IOMFB only: it starts no AFK
	 * service endpoint (system, DPAV, iBoot, DPTX, AV).
	 */
	if (iomfb_is_v14_7(dcp)) {
		ret = iomfb_start_rtkit(dcp);
		if (ret)
			dev_err(dcp->dev, "Failed to start IOMFB endpoint: %d\n", ret);
		return ret;
	}

	/* Make the H17P allocator available before external service discovery. */
	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		ret = apple_rtkit_start_ep(dcp->rtk, REMOTE_ALLOC_ENDPOINT);
		if (ret)
			return dev_err_probe(dcp->dev, ret, "Failed to start remote allocator\n");
	}

	/* start RTKit endpoints */
	ret = systemep_init(dcp);
	if (ret)
		dev_warn(dcp->dev, "Failed to start system endpoint: %d\n", ret);

	if (unstable_edid && !dcp_has_panel(dcp)) {
		ret = dpavservep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start DPAVSERV endpoint: %d",
				 ret);
	}

	if (dcp->phy && dcp->fw_compat >= DCP_FIRMWARE_V_13_5) {
		ret = ibootep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start IBOOT endpoint: %d\n",
				 ret);

		if (dcp->hw.iomfb_method_profile ==
		    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
			ret = dptx_controller_init(dcp);
			if (ret)
				return dev_err_probe(dcp->dev, ret,
						     "Failed to start DPTX controller\n");
		}

		ret = dptxep_init(dcp);
		if (ret) {
			dev_warn(dcp->dev, "Failed to start DPTX endpoint: %d\n",
				 ret);
#ifdef DCP_DPTX_DISCONNECT_ON_INIT
		/*
		 * This disconnect / connect cycle on init is only necessary
		 * when using dcp0 on j473, j474s and presumedly j475c.
		 * Since dcp0 is not used at the moment let's avoid this
		 * since it is possibly the cause for startup issues.
		 */
		} else if (dptx_service_available(&dcp->dptxport[0])) {
			bool connected;
			/* force disconnect on start - necessary if the display
			 * is already up from m1n1
			 */
			dptxport_set_hpd(dcp->dptxport[0].service, false);
			dptxport_release_display(dcp->dptxport[0].service);
			usleep_range(10 * USEC_PER_MSEC, 25 * USEC_PER_MSEC);

			connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
			dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

			// necessary on j473/j474 but not on j314c
			if (connected)
				dcp_dptx_connect(dcp, 0);
#endif
		}
	} else if (dcp->phy) {
		dev_warn(dcp->dev, "OS firmware incompatible with dptxport EP\n");
	}
	ret = iomfb_start_rtkit(dcp);
	if (ret)
		dev_err(dcp->dev, "Failed to start IOMFB endpoint: %d\n", ret);

#if IS_ENABLED(CONFIG_DRM_APPLE_AUDIO)
	if (hdmi_audio) {
		ret = avep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start AV endpoint: %d", ret);
		ret = 0;
	}
#endif

	return ret;
}

static void _dcp_poweroff(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_poweroff_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_poweroff_v13_3(dcp);
		break;
	case DCP_FIRMWARE_H17P:
		iomfb_poweroff_h17p(dcp);
		break;
	case DCP_FIRMWARE_V_14_7:
		iomfb_poweroff_v14_7(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

static int dcp_enable_dp2hdmi_hpd(struct apple_dcp *dcp)
{
	if (dcp_is_typec_output(dcp)) {
		if (READ_ONCE(dcp->typec_cable_connected))
			dcp_dptx_connect(dcp, 0);
	} else if (dcp->hdmi_hpd) {
		/* Check HPD before enabling the edge-triggered IRQ. */
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

		if (connected)
			dcp_dptx_connect(dcp, 0);
		else
			_dcp_poweroff(dcp);
	}

	if (dcp->hdmi_hpd_irq)
		enable_irq(dcp->hdmi_hpd_irq);

	return 0;
}

/* Startup budget of the 14.7 session: READY, A401 and A455 with callbacks. */
#define DCP_V14_7_START_TIMEOUT	msecs_to_jiffies(10000)

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030)
static int dcp_t6030_display_prepare(struct device *dev)
{
	return apple_pmp_j514s_display_prepare(dev);
}

static int dcp_t6030_display_get(struct device *dev)
{
	return apple_pmp_j514s_display_get(dev);
}

static int dcp_t6030_display_check(struct device *dev)
{
	return apple_pmp_j514s_display_check(dev);
}
#else
static int dcp_t6030_display_prepare(struct device *dev) { return -ENODEV; }
static int dcp_t6030_display_get(struct device *dev) { return -ENODEV; }
static int dcp_t6030_display_check(struct device *dev) { return -ENODEV; }
#endif

/* KMS scans out through the display subsystem's own DART stream (SID0). */
static bool dcp_t6030_scanout_domain(void)
{
	struct device_node *np = of_find_node_by_path("disp0");
	struct platform_device *pdev = np ? of_find_device_by_node(np) : NULL;
	bool ok = pdev && iommu_get_domain_for_dev(&pdev->dev);

	of_node_put(np);
	if (pdev)
		put_device(&pdev->dev);
	return ok;
}

/*
 * The 14.7 session is usable once A455 has replied and the preferred mode is
 * known; the PMP must still hold the display (DISP requested and
 * acknowledged) and the scanout device must have its DMA domain before KMS
 * takes the panel over.  Anything else is a failed session.
 */
static int dcp_wait_ready_v14_7(struct apple_dcp *dcp)
{
	int ret;

	if (!READ_ONCE(dcp->crashed) && !dcp->active)
		wait_for_completion_timeout(&dcp->start_done,
					    DCP_V14_7_START_TIMEOUT);
	if (READ_ONCE(dcp->crashed))
		return -ENODEV;
	if (!dcp->active) {
		iomfb_v14_7_fail(dcp, "IOMFB startup did not complete");
		return -ETIMEDOUT;
	}
	if (!dcp_t6030_scanout_domain()) {
		iomfb_v14_7_fail(dcp, "scanout DMA domain missing");
		return -ENODEV;
	}
	ret = dcp_t6030_display_check(dcp->dev);
	if (ret) {
		dev_err(dcp->dev, "PMP display power check failed: %d\n", ret);
		iomfb_v14_7_fail(dcp, "PMP display power lost before KMS");
		return ret;
	}
	return 0;
}

int dcp_wait_ready(struct platform_device *pdev, u64 timeout)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	/* Nothing to wait for: the external session starts at its first sink. */
	if (iomfb_is_v14_7_external(dcp))
		return 0;
	if (iomfb_is_v14_7(dcp))
		return dcp_wait_ready_v14_7(dcp);

	if (dcp->crashed)
		return -ENODEV;
	if (dcp->active)
		return dcp_enable_dp2hdmi_hpd(dcp);
	if (timeout <= 0)
		return -ETIMEDOUT;

	ret = wait_for_completion_timeout(&dcp->start_done, timeout);
	if (ret < 0)
		return ret;

	if (dcp->crashed)
		return -ENODEV;

	if (dcp->active)
		dcp_enable_dp2hdmi_hpd(dcp);

	return dcp->active ? 0 : -ETIMEDOUT;
}

static void __maybe_unused dcp_sleep(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_sleep_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_sleep_v13_3(dcp);
		break;
	case DCP_FIRMWARE_H17P:
		iomfb_sleep_h17p(dcp);
		break;
	case DCP_FIRMWARE_V_14_7:
		iomfb_sleep_v14_7(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

#define DCP_BL_FALLBACK_DELAY msecs_to_jiffies(2000)

static bool dcp_uses_soft_dpms(struct apple_dcp *dcp)
{
	return (dcp_backlight_active(dcp) && dcp_has_panel(dcp)) ||
	       (dcp->fw_compat == DCP_FIRMWARE_H17P &&
		dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G &&
		dcp->connector_type == DRM_MODE_CONNECTOR_eDP);
}

/*
 * The 14.7 panel's display controller off: once the zero-nits blank has been
 * presented, A472 with the hardware selector stops scanout and the panel. The
 * session, its buffers and the DART mappings stay; dcp_panel_hw_on() restarts
 * the controller before the backlight is restored.
 */
static void dcp_panel_hw_off(struct apple_dcp *dcp)
{
	int ret;

	if (!panel_hw_off || !iomfb_is_v14_7(dcp) || iomfb_is_v14_7_external(dcp) ||
	    !dcp_has_panel(dcp) || dcp->panel_hw_off || READ_ONCE(dcp->crashed))
		return;

	flush_work(&dcp->bl_update_wq);
	if (!iomfb_queue_drain(dcp, msecs_to_jiffies(500))) {
		dev_warn(dcp->dev, "blank still pending; display controller left on\n");
		return;
	}
	/*
	 * Nothing may present to the stopped controller, e.g. the backlight
	 * core's level at system sleep, or a brightness retry.
	 */
	iomfb_hold_panel_presents(dcp, true);
	ret = iomfb_v14_7_panel_power(dcp, false);
	if (ret) {
		dev_err(dcp->dev, "display controller off failed: %d\n", ret);
		iomfb_hold_panel_presents(dcp, false);
		return;
	}
	dcp->panel_hw_off = true;
	dev_dbg(dcp->dev, "display controller off\n");
}

static void dcp_panel_hw_on(struct apple_dcp *dcp)
{
	int ret;

	if (!dcp->panel_hw_off)
		return;
	ret = iomfb_v14_7_panel_power(dcp, true);
	if (ret) {
		/* Presents stay held; the next power-on tries again. */
		dev_err(dcp->dev, "display controller on failed: %d\n", ret);
		return;
	}
	dcp->panel_hw_off = false;
	iomfb_hold_panel_presents(dcp, false);
	dev_dbg(dcp->dev, "display controller on\n");
}

void dcp_poweron(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	bool wait_for_typec_hpd = false;
	unsigned long remaining;
	int ret;

	/* 14.7 external: the modeset that follows sets the mode and A472 on. */
	if (iomfb_is_v14_7_external(dcp))
		return;
	if (dcp_is_external_h17p(dcp)) {
		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
		cancel_work_sync(&dcp->external_stop_work);
		cancel_delayed_work_sync(&dcp->external_stop_retry_wq);
		guard(mutex)(&dcp->external_lock);
		ret = dcp_external_start_locked(dcp);
		if (!ret)
			dcp->external_crtc_generation = dcp->external_generation;
		else
			dev_err(dcp->dev, "external display activation failed: %d\n", ret);
		return;
	}

	if (dcp_uses_soft_dpms(dcp)) {
		if (dcp->fw_compat == DCP_FIRMWARE_H17P &&
		    dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G &&
		    !READ_ONCE(dcp->pipe_enabled_h17p)) {
			iomfb_poweron_h17p(dcp);
			if (!READ_ONCE(dcp->pipe_enabled_h17p)) {
				dev_err(dcp->dev, "initial display pipe enable failed\n");
				WRITE_ONCE(dcp->crashed, true);
				return;
			}
			/* The powerlog takeover report normally precedes this. */
			if (dcp_has_panel(dcp))
				queue_delayed_work(system_freezable_wq,
						   &dcp->bl_fallback_wq,
						   DCP_BL_FALLBACK_DELAY);
		}
		dcp_panel_hw_on(dcp);
		ret = dcp_backlight_dpms(dcp, true);
		if (ret)
			dev_warn(dcp->dev, "backlight restore unavailable: %d\n", ret);
		return;
	}

	if (dcp_is_typec_output(dcp)) {
		wait_for_typec_hpd = READ_ONCE(dcp->typec_crtc_off) &&
				    READ_ONCE(dcp->typec_cable_connected);
		WRITE_ONCE(dcp->typec_crtc_off, false);

		/*
		 * A Type-C CRTC disable releases its DPTX session. Re-establish it
		 * synchronously before IOMFB is powered back on.
		 */
		if (READ_ONCE(dcp->typec_cable_connected)) {
			cancel_delayed_work(&dcp->typec_reconnect_wq);
			dcp->typec_reconnect_tries = 0;
			ret = dcp_dptx_connect(dcp, 0);
			if (ret)
				mod_delayed_work(system_freezable_wq,
						 &dcp->typec_reconnect_wq,
						 DPTX_RECONNECT_DELAY);
			else if (wait_for_typec_hpd) {
				remaining = wait_for_completion_timeout(
					&dcp->typec_iomfb_hpd_ready,
					msecs_to_jiffies(3000));
				if (!remaining)
					dev_warn(dcp->dev,
						 "Type-C IOMFB hotplug not ready on wake\n");
			}
		}
	} else if (dcp->hdmi_hpd) {
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

		if (connected)
			dcp_dptx_connect(dcp, 0);
	}

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_poweron_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_poweron_v13_3(dcp);
		break;
	case DCP_FIRMWARE_H17P:
		iomfb_poweron_h17p(dcp);
		break;
	case DCP_FIRMWARE_V_14_7:
		iomfb_poweron_v14_7(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
	if (dcp->avep)
		av_service_connect(dcp);
}

/* Same-CRTC helper dependencies order this token refresh before later disables. */
void dcp_external_recover(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp_is_external_h17p(dcp))
		dcp_poweron(pdev);
}

void dcp_poweroff(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (iomfb_is_v14_7_external(dcp)) {
		dcp_ext_v14_7_poweroff(dcp);
		return;
	}
	if (dcp_is_external_h17p(dcp)) {
		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
		guard(mutex)(&dcp->external_lock);
		ret = dcp_external_stop_locked(dcp, dcp->external_crtc_generation);
		if (ret && !dcp->external_stop_deferred)
			dev_err(dcp->dev, "external display stop failed: %d\n", ret);
		dcp_external_crtc_released(dcp);
		return;
	}

	cancel_delayed_work(&dcp->swap_watchdog_wq);

	/*
	 * Internal H17P DPMS is a brightness present, never a pipe stop. The
	 * 14.7 panel then also stops its display controller.
	 */
	if (dcp_uses_soft_dpms(dcp)) {
		ret = dcp_backlight_dpms(dcp, false);
		if (ret)
			dev_warn(dcp->dev, "backlight blank unavailable: %d\n", ret);
		else
			dcp_panel_hw_off(dcp);
		return;
	}

	if (dcp->avep)
		av_service_disconnect(dcp);

	/*
	 * Powering a Type-C CRTC off drops DCP's synthetic HPD, and the firmware
	 * reports that as an unplug. The display is still attached: keep the
	 * connector connected (see dcpep_cb_hotplug()) and let dcp_poweron()
	 * re-establish the DPTX session. Recreating it here instead makes the
	 * display vanish and come back, and compositors light a returning
	 * display, so DPMS off never sticks. Cable removal is reported through
	 * the Type-C mux.
	 */
	if (dcp_is_typec_output(dcp)) {
		reinit_completion(&dcp->typec_iomfb_hpd_ready);
		if (READ_ONCE(dcp->typec_cable_connected))
			WRITE_ONCE(dcp->typec_crtc_off, true);
		/* dcp_poweron() reconnects the link on DPMS wake. */
		cancel_delayed_work(&dcp->typec_reconnect_wq);
	}

	_dcp_poweroff(dcp);

	if (dcp_is_typec_output(dcp)) {
		/* DCP owns a synthetic HPD for Type-C. Release it with the CRTC. */
		if (dptx_service_available(&dcp->dptxport[0]) && dcp->dptxport[0].connected) {
			ret = dptxport_set_hpd(dcp->dptxport[0].service, false);
			if (ret)
				dev_warn(dcp->dev,
					 "failed to deassert Type-C DPTX HPD: %d\n", ret);
			dcp_dptx_disconnect(dcp, 0);
		}
	} else if (dcp->hdmi_hpd) {
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		if (!connected) {
			disconnected_hpd_event(dcp->connector);
			dcp_dptx_disconnect(dcp, 0);
		}
	}
}

static void dcp_work_register_backlight(struct work_struct *work)
{
	int ret;
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, bl_register_wq);

	mutex_lock(&dcp->bl_register_mutex);
	if (dcp->brightness.bl_dev)
		goto out_unlock;

	/* try to register backlight device, */
	ret = dcp_backlight_register(dcp);
	if (ret == -ENODATA)
		goto out_unlock;
	if (ret) {
		dev_err(dcp->dev, "Unable to register backlight device: %d\n", ret);
		/*
		 * The H17P policy encodes the panel ceiling into every present
		 * and keeps working without a class device; only older
		 * firmware stops sending brightness here.
		 */
		if (!dcp_backlight_active(dcp))
			dcp->brightness.maximum = 0;
	}

out_unlock:
	mutex_unlock(&dcp->bl_register_mutex);
}

static void dcp_work_update_backlight(struct work_struct *work)
{
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, bl_update_wq);

	dcp_backlight_update(dcp);
}

/*
 * H17P never publishes the panel level, and the loader's level arrives only
 * as an optional powerlog report.  Without one, register the backlight at
 * the middle of the panel range.  That value is only reported: no level is
 * presented until userspace writes one, so the panel keeps the loader's
 * level.
 */
static void dcp_work_backlight_fallback(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work),
					     struct apple_dcp, bl_fallback_wq);
	u32 nits = dcp->brightness.maximum / 2;
	int ret;

	if (READ_ONCE(dcp->quiescing) || READ_ONCE(dcp->crashed) ||
	    dcp_backlight_active(dcp))
		return;

	ret = iomfb_configure_backlight_h17p(dcp, dcp->brightness.maximum,
					     false, 0, true, nits);
	if (!ret)
		dev_info(dcp->dev,
			 "no loader brightness reported; backlight starts at %u nits without a panel change\n",
			 nits);
	else if (ret != -EBUSY)
		dev_warn(dcp->dev, "backlight registration failed: %d\n", ret);
}

static void dcp_release_piodma_iommu_dev(struct apple_dcp *dcp)
{
	if (dcp->retain_dma) {
		WRITE_ONCE(dcp->quiescing, true);
		return;
	}
	if (dcp->piodma) {
		if (dcp->piodma_created)
			of_platform_device_destroy(&dcp->piodma->dev, NULL);
		else
			put_device(&dcp->piodma->dev);
	}
	dcp->piodma = NULL;
	dcp->iommu_dom = NULL;
	dcp->piodma_created = false;
}

void dcp_retain_framebuffer(struct platform_device *pdev,
			    struct dcp_fb_reference *entry)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_add_tail(&entry->head, &dcp->swapped_out_fbs);
	mutex_unlock(&dcp->swapped_out_fbs_lock);
}

void dcp_arm_retained_framebuffers(struct apple_dcp *dcp, u32 swap_id)
{
	struct dcp_fb_reference *entry;

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_for_each_entry(entry, &dcp->swapped_out_fbs, head) {
		if (entry->armed)
			continue;
		entry->swap_id = swap_id;
		entry->armed = true;
	}
	mutex_unlock(&dcp->swapped_out_fbs_lock);
}

void dcp_unarm_retained_framebuffers(struct apple_dcp *dcp, u32 swap_id)
{
	struct dcp_fb_reference *entry;

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_for_each_entry(entry, &dcp->swapped_out_fbs, head)
		if (entry->armed && entry->swap_id == swap_id)
			entry->armed = false;
	mutex_unlock(&dcp->swapped_out_fbs_lock);
}

void dcp_release_retained_framebuffers(struct apple_dcp *dcp, u32 swap_id)
{
	struct dcp_fb_reference *entry, *tmp;
	LIST_HEAD(completed);

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_for_each_entry_safe(entry, tmp, &dcp->swapped_out_fbs, head) {
		if (!entry->armed || entry->swap_id != swap_id)
			continue;
		list_move_tail(&entry->head, &completed);
	}
	mutex_unlock(&dcp->swapped_out_fbs_lock);

	list_for_each_entry_safe(entry, tmp, &completed, head) {
		list_del(&entry->head);
		drm_framebuffer_put(entry->fb);
		kfree(entry);
	}
}

void dcp_release_all_retained_framebuffers(struct apple_dcp *dcp)
{
	struct dcp_fb_reference *entry, *tmp;
	LIST_HEAD(completed);

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_splice_init(&dcp->swapped_out_fbs, &completed);
	mutex_unlock(&dcp->swapped_out_fbs_lock);

	list_for_each_entry_safe(entry, tmp, &completed, head) {
		list_del(&entry->head);
		drm_framebuffer_put(entry->fb);
		kfree(entry);
	}
}

static int dcp_create_piodma_iommu_dev(struct apple_dcp *dcp)
{
	int ret;
	struct device_node *node __free(device_node) = of_get_child_by_name(dcp->dev->of_node, "piodma");

	if (!node)
		return dev_err_probe(dcp->dev, -ENODEV,
				     "Failed to get piodma child DT node\n");

	dcp->piodma = of_find_device_by_node(node);
	dcp->piodma_created = !dcp->piodma;
	if (dcp->piodma_created)
		dcp->piodma = of_platform_device_create(node, NULL, dcp->dev);
	if (!dcp->piodma)
		return dev_err_probe(dcp->dev, -ENODEV,
				     "Failed to create piodma pdev for %pOF\n", node);

	ret = dma_set_mask_and_coherent(&dcp->piodma->dev, DMA_BIT_MASK(42));
	if (ret)
		goto err_destroy_pdev;

	ret = of_dma_configure(&dcp->piodma->dev, node, true);
	if (ret) {
		ret = dev_err_probe(dcp->dev, ret,
			"Failed to configure IOMMU child DMA\n");
		goto err_destroy_pdev;
	}

	dcp->iommu_dom = iommu_get_domain_for_dev(&dcp->piodma->dev);
	if (IS_ERR(dcp->iommu_dom)) {
		ret = dev_err_probe(dcp->dev, PTR_ERR(dcp->iommu_dom),
				    "Failed to get default iommu domain for "
				    "piodma device\n");
		dcp->iommu_dom = NULL;
		goto err_destroy_pdev;
	}

	return 0;
err_destroy_pdev:
	dcp_release_piodma_iommu_dev(dcp);
	return ret;
}

static int dcp_get_bw_scratch_reg(struct apple_dcp *dcp, u32 expected)
{
	struct of_phandle_args ph_args;
	u32 addr_idx, disp_idx, offset;
	int ret;

	ret = of_parse_phandle_with_args(dcp->dev->of_node, "apple,bw-scratch",
				   "#apple,bw-scratch-cells", 0, &ph_args);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to read 'apple,bw-scratch': %d\n", ret);
		return ret;
	}

	if (ph_args.args_count != 3) {
		dev_err(dcp->dev, "Unexpected 'apple,bw-scratch' arg count %d\n",
			ph_args.args_count);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	addr_idx = ph_args.args[0];
	disp_idx = ph_args.args[1];
	offset = ph_args.args[2];

	if (disp_idx != expected || disp_idx >= MAX_DISP_REGISTERS) {
		dev_err(dcp->dev, "Unexpected disp_reg value in 'apple,bw-scratch': %d\n",
			disp_idx);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	ret = of_address_to_resource(ph_args.np, addr_idx, &dcp->disp_bw_scratch_res);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to get 'apple,bw-scratch' resource %d from %pOF\n",
			addr_idx, ph_args.np);
		goto err_of_node_put;
	}
	if (offset > resource_size(&dcp->disp_bw_scratch_res) - 4) {
		ret = -EINVAL;
		goto err_of_node_put;
	}

	dcp->disp_registers[disp_idx] = &dcp->disp_bw_scratch_res;
	dcp->disp_bw_scratch_index = disp_idx;
	dcp->disp_bw_scratch_offset = offset;
	ret = 0;

err_of_node_put:
	of_node_put(ph_args.np);
	return ret;
}

static int dcp_get_bw_doorbell_reg(struct apple_dcp *dcp, u32 expected)
{
	struct of_phandle_args ph_args;
	u32 addr_idx, disp_idx;
	int ret;

	ret = of_parse_phandle_with_args(dcp->dev->of_node, "apple,bw-doorbell",
				   "#apple,bw-doorbell-cells", 0, &ph_args);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to read 'apple,bw-doorbell': %d\n", ret);
		return ret;
	}

	if (ph_args.args_count != 2) {
		dev_err(dcp->dev, "Unexpected 'apple,bw-doorbell' arg count %d\n",
			ph_args.args_count);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	addr_idx = ph_args.args[0];
	disp_idx = ph_args.args[1];

	if (disp_idx != expected || disp_idx >= MAX_DISP_REGISTERS) {
		dev_err(dcp->dev, "Unexpected disp_reg value in 'apple,bw-doorbell': %d\n",
			disp_idx);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	ret = of_address_to_resource(ph_args.np, addr_idx, &dcp->disp_bw_doorbell_res);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to get 'apple,bw-doorbell' resource %d from %pOF\n",
			addr_idx, ph_args.np);
		goto err_of_node_put;
	}
	dcp->disp_bw_doorbell_index = disp_idx;
	dcp->disp_registers[disp_idx] = &dcp->disp_bw_doorbell_res;
	ret = 0;

err_of_node_put:
	of_node_put(ph_args.np);
	return ret;
}

static int dcp_get_disp_regs(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	int count = 0;
	int i, ret;
	char name[16];
	const char *reg_name;
	struct resource *res;

	if (of_property_present(dcp->dev->of_node, "reg-names")) {
		ret = of_property_count_strings(dcp->dev->of_node, "reg-names");
		if (ret < 0)
			return ret;

		for (i = 0; i < ret; i++) {
			if (of_property_read_string_index(dcp->dev->of_node,
							  "reg-names", i, &reg_name))
				return -EINVAL;
			if (!strncmp(reg_name, "disp-", 5))
				count++;
		}
	} else {
		count = pdev->num_resources - 1;
	}

	if (count <= 0 || count > MAX_DISP_REGISTERS)
		return -EINVAL;

	for (i = 0; i < count; ++i) {
		if (of_property_present(dcp->dev->of_node, "reg-names")) {
			snprintf(name, sizeof(name), "disp-%d", i);
			res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							   name);
		} else {
			res = platform_get_resource(pdev, IORESOURCE_MEM, 1 + i);
		}
		if (!res)
			return -EINVAL;
		dcp->disp_registers[i] = res;
	}

	if (dcp->hw.firmware_clock) {
		unsigned int windows = dcp->hw.firmware_display_windows;
		bool external = dcp->hw.iomfb_method_profile ==
			DCP_IOMFB_METHODS_H17P_EXTERNAL;

		if (!windows || windows >= MAX_DISP_REGISTERS)
			return -EINVAL;
		if (external) {
			/* The external clock aperture is already described by disp-4. */
			if (count != windows + 1)
				return -EINVAL;
			res = dcp->disp_registers[windows];
			if (!apple_dcp_external_clock_window(res->start,
							     resource_size(res),
							     dcp->hw.firmware_clock->start,
							     resource_size(dcp->hw.firmware_clock)))
				return -EINVAL;
			count = windows;
		}
		if (count != windows || resource_size(dcp->hw.firmware_clock) < 4 ||
		    dcp->hw.firmware_scratch > resource_size(dcp->hw.firmware_clock) - 4 ||
		    dcp->hw.firmware_request > resource_size(dcp->hw.firmware_clock) - 4)
			return -EINVAL;

		dcp->disp_bw_scratch_res = *dcp->hw.firmware_clock;
		dcp->disp_bw_scratch_index = count;
		dcp->disp_bw_scratch_offset = dcp->hw.firmware_scratch;
		dcp->disp_bw_doorbell_res = dcp->disp_bw_scratch_res;
		dcp->disp_bw_doorbell_res.start += dcp->hw.firmware_request;
		dcp->disp_bw_doorbell_res.end = dcp->disp_bw_doorbell_res.start + 3;
		dcp->disp_registers[count] = &dcp->disp_bw_scratch_res;
		dcp->nr_disp_registers = count + 1;
		return 0;
	}

	/* Without bandwidth registers D003 is declined, as on the J514S. */
	if (dcp->hw.no_rt_bandwidth) {
		dcp->nr_disp_registers = count;
		return 0;
	}

	/* load pmgr bandwidth scratch resource and offset */
	ret = dcp_get_bw_scratch_reg(dcp, count);
	if (ret < 0)
		return ret;
	count += 1;

	/* load pmgr bandwidth doorbell resource if present (only on t8103) */
	if (of_property_present(dcp->dev->of_node, "apple,bw-doorbell")) {
		ret = dcp_get_bw_doorbell_reg(dcp, count);
		if (ret < 0)
			return ret;
		count += 1;
	}

	dcp->nr_disp_registers = count;
	return 0;
}

#define DCP_FW_VERSION_MIN_LEN	3
#define DCP_FW_VERSION_MAX_LEN	5
#define DCP_FW_VERSION_STR_LEN	(DCP_FW_VERSION_MAX_LEN * 4)

static int dcp_read_fw_version(struct device *dev, const char *name,
			       char *version_str)
{
	u32 ver[DCP_FW_VERSION_MAX_LEN];
	int len_str;
	int len;

	len = of_property_read_variable_u32_array(dev->of_node, name, ver,
						  DCP_FW_VERSION_MIN_LEN,
						  DCP_FW_VERSION_MAX_LEN);

	switch (len) {
	case 3:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d", ver[0], ver[1], ver[2]);
		break;
	case 4:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d.%d", ver[0], ver[1], ver[2],
				    ver[3]);
		break;
	case 5:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d.%d.%d", ver[0], ver[1], ver[2],
				    ver[3], ver[4]);
		break;
	default:
		len_str = strscpy(version_str, "UNKNOWN",
				  DCP_FW_VERSION_STR_LEN);
		if (len >= 0)
			len = -EOVERFLOW;
		break;
	}

	if (len_str >= DCP_FW_VERSION_STR_LEN)
		dev_warn(dev, "'%s' truncated: '%s'\n", name, version_str);

	return len;
}

static enum dcp_firmware_version dcp_check_firmware_version(struct device *dev)
{
	const struct apple_dcp_hw_data *hw = of_device_get_match_data(dev);
	char compat_str[DCP_FW_VERSION_STR_LEN];
	char fw_str[DCP_FW_VERSION_STR_LEN];
	int ret;

	/*
	 * SoCs introduced with H17-generation firmware pin its interface; the
	 * loader may not recognise their boot firmware version.
	 */
	if (hw->firmware_compat != DCP_FIRMWARE_UNKNOWN)
		return hw->firmware_compat;

	/* firmware version is just informative */
	dcp_read_fw_version(dev, "apple,firmware-version", fw_str);

	ret = dcp_read_fw_version(dev, "apple,firmware-compat", compat_str);
	if (ret < 0) {
		dev_err(dev, "Could not read 'apple,firmware-compat': %d\n", ret);
		return DCP_FIRMWARE_UNKNOWN;
	}

	if (strncmp(compat_str, "12.3.0", sizeof(compat_str)) == 0)
		return DCP_FIRMWARE_V_12_3;
	/*
	 * m1n1 reports firmware version 13.5 as compatible with 13.3. This is
	 * only true for the iomfb endpoint. The interface for the dptx-port
	 * endpoint changed between 13.3 and 13.5. The driver will only support
	 * firmware 13.5. Check the actual firmware version for compat version
	 * 13.3 until m1n1 reports 13.5 as "firmware-compat".
	 */
	else if ((strncmp(compat_str, "13.3.0", sizeof(compat_str)) == 0) &&
		 (strncmp(fw_str, "13.5.0", sizeof(compat_str)) == 0))
		return DCP_FIRMWARE_V_13_5;
	else if (strncmp(compat_str, "13.5.0", sizeof(compat_str)) == 0)
		return DCP_FIRMWARE_V_13_5;

	dev_err(dev, "DCP firmware-compat %s (FW: %s) is not supported\n",
		compat_str, fw_str);

	return DCP_FIRMWARE_UNKNOWN;
}

static int dcp_connector_type_from_dt(struct device_node *np)
{
	if (of_property_match_string(np, "apple,connector-type", "HDMI-A") >= 0)
		return DRM_MODE_CONNECTOR_HDMIA;
	if (of_property_match_string(np, "apple,connector-type", "DP") >= 0)
		return DRM_MODE_CONNECTOR_DisplayPort;
	if (of_property_match_string(np, "apple,connector-type", "USB-C") >= 0)
		return DRM_MODE_CONNECTOR_USB;

	return DRM_MODE_CONNECTOR_Unknown;
}

/*
 * The fixed route's crossbar selection and Type-C mux are taken at probe and
 * must be released with the device: a crossbar left selected blocks the next
 * probe's mux_control_select() for good.
 */
static void dcp_release_fixed_route(void *data)
{
	struct apple_dcp *dcp = data;

	if (dcp->fixed_route_selected) {
		mux_control_deselect(dcp->xbar);
		dcp->fixed_route_selected = false;
	}
	typec_mux_put(dcp->typec_mux);
	dcp->typec_mux = NULL;
}

VISIBLE_IF_KUNIT void dcp_disable_typec_work(struct apple_dcp *dcp, bool release_cable)
{
	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (release_cable)
			WRITE_ONCE(dcp->typec_cable_connected, false);
		dcp->typec_generation++;
		dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
		/* Nothing arms the timeout work past its drain below. */
		dcp->announce.stopped = true;
	}
	if (release_cable && dcp_is_external_h17p(dcp))
		dpavservep_detach(dcp);
	/* Block new enqueues as well as draining users of the AFK endpoints. */
	disable_delayed_work_sync(&dcp->typec_reconnect_wq);
	disable_delayed_work_sync(&dcp->placeholder_edid_wq);
	disable_delayed_work_sync(&dcp->announce_wq);
	cancel_work_sync(&dcp->external_stop_work);
	cancel_delayed_work_sync(&dcp->external_stop_retry_wq);
}

VISIBLE_IF_KUNIT void dcp_enable_typec_work(struct apple_dcp *dcp)
{
	enable_delayed_work(&dcp->typec_reconnect_wq);
	enable_delayed_work(&dcp->placeholder_edid_wq);
	enable_delayed_work(&dcp->announce_wq);
	/* A cable can be routed before the DRM component binds. */
	if (READ_ONCE(dcp->typec_cable_connected))
		mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq, 0);
	if (dcp_is_external_h17p(dcp)) {
		struct apple_connector *con = READ_ONCE(dcp->connector);

		guard(mutex)(&dcp->hpd_mutex);
		dcp->announce.stopped = false;
		/* A hold that crossed system sleep waits for the new link from now. */
		dcp_announce_arm(dcp);
		/* Send an event that came due while stopped. */
		if (dcp_announce_owed(&dcp->announce) && con &&
		    READ_ONCE(con->dcp) == to_platform_device(dcp->dev))
			schedule_work(&con->hotplug_wq);
	}
}

static bool dcp_external_is_idle(struct apple_dcp *dcp)
{
	u32 control = readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	u32 status = readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS);

	/* Same externally inherited idle state accepted by the public loader. */
	return (control & APPLE_DCP_COPROC_CPU_CONTROL_RUN) &&
	       (status & APPLE_DCP_COPROC_CPU_STATUS_STOPPED) &&
	       (status & APPLE_DCP_COPROC_CPU_STATUS_IDLE);
}

static int dcp_comp_bind(struct device *dev, struct device *main, void *data)
{
	struct device_node *panel_np;
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	u32 cpu_ctrl;
	int ret;

	if (READ_ONCE(dcp->quiescing))
		return dev_err_probe(dev, -EBUSY,
				     "Live DCP session cannot be rebound; reboot required\n");

	if (dcp->external_prepared)
		return 0;

	/* A timed-out prior session may still scan these mappings. */
	mutex_lock(&dcp->swapped_out_fbs_lock);
	ret = list_empty(&dcp->swapped_out_fbs) ? 0 : -EBUSY;
	mutex_unlock(&dcp->swapped_out_fbs_lock);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Previous scanout has not been stopped\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;

	dcp->coproc_reg = devm_platform_ioremap_resource_byname(to_platform_device(dev), "coproc");
	if (IS_ERR(dcp->coproc_reg))
		return PTR_ERR(dcp->coproc_reg);

	/*
	 * Display coprocessors running H17-generation firmware are left running
	 * by the bootloader with their firmware mapped through a locked DART.
	 * Linux attaches to that session with a standard RTKit INIT/HELLO
	 * exchange and must not stop or restart the ASC.
	 */
	if (dcp->hw.adopt_live_session) {
		cpu_ctrl = readl_relaxed(dcp->coproc_reg +
					 APPLE_DCP_COPROC_CPU_CONTROL);
		if (!(cpu_ctrl & APPLE_DCP_COPROC_CPU_CONTROL_RUN))
			return dev_err_probe(dev, -EOPNOTSUPP,
					     "DCP was not left running by the bootloader\n");
	}

	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	    !dcp_external_is_idle(dcp))
		return dev_err_probe(dev, -EBUSY,
				     "External DCP is not in the inherited idle state\n");

	if (dcp->index || dcp->dptx_phy || dcp->dptx_die)
		dev_info(dev, "DCP index:%u dptx target phy: %u dptx die: %u\n",
			 dcp->index, dcp->dptx_phy, dcp->dptx_die);

	/* The 14.7 panel profile scans out the full panel, notch included. */
	if (!show_notch && !iomfb_is_v14_7(dcp))
		ret = of_property_read_u32(dev->of_node, "apple,notch-height",
					   &dcp->notch_height);

	if (dcp->notch_height > MAX_NOTCH_HEIGHT)
		dcp->notch_height = MAX_NOTCH_HEIGHT;
	if (dcp->notch_height > 0)
		dev_info(dev, "Detected display with notch of %u pixel\n", dcp->notch_height);

	/* initialize brightness scale to a sensible default to avoid divide by 0*/
	dcp->brightness.scale = 65536;
	panel_np = of_get_compatible_child(dev->of_node, "apple,panel-mini-led");
	if (panel_np)
		dcp->panel.has_mini_led = true;
	else
		panel_np = of_get_compatible_child(dev->of_node, "apple,panel");

	if (panel_np) {
		const char height_prop[2][16] = { "adj-height-mm", "height-mm" };

		if (of_device_is_available(panel_np)) {
			ret = of_property_read_u32(panel_np, "apple,max-brightness",
						   &dcp->brightness.maximum);
			if (ret)
				dev_err(dev, "Missing property 'apple,max-brightness'\n");
		}

		of_property_read_u32(panel_np, "width-mm", &dcp->panel.width_mm);
		/* use adjusted height as long as the notch is hidden */
		of_property_read_u32(panel_np, height_prop[!dcp->notch_height],
				     &dcp->panel.height_mm);

		of_node_put(panel_np);
		dcp->fixed_connector_type = DRM_MODE_CONNECTOR_eDP;
		dcp->connector_type = DRM_MODE_CONNECTOR_eDP;
	} else if (dcp->hw.panel_max_nits) {
		/*
		 * The J514S boot loader describes the panel on the DCP node
		 * only: its size there, its brightness range and backlight
		 * type from the hardware data.
		 */
		dcp->panel.has_mini_led = dcp->hw.panel_mini_led;
		dcp->brightness.maximum = dcp->hw.panel_max_nits;
		of_property_read_u32(dev->of_node, "apple,panel-width-mm",
				     &dcp->panel.width_mm);
		of_property_read_u32(dev->of_node, "apple,panel-height-mm",
				     &dcp->panel.height_mm);
		dcp->fixed_connector_type = DRM_MODE_CONNECTOR_eDP;
		dcp->connector_type = DRM_MODE_CONNECTOR_eDP;
	}

	ret = dcp_create_piodma_iommu_dev(dcp);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to create PIODMA iommu child device\n");
	if (!dcp->iommu_dom) {
		ret = dev_err_probe(dev, -EPROBE_DEFER,
				    "PIODMA iommu domain is unavailable\n");
		goto err_piodma;
	}

	ret = dcp_get_disp_regs(dcp);
	if (ret) {
		dev_err(dev, "failed to find display registers\n");
		goto err_piodma;
	}

	if (dcp->hw.optional_clock)
		dcp->clk = devm_clk_get_optional(dev, NULL);
	else
		dcp->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dcp->clk)) {
		ret = dev_err_probe(dev, PTR_ERR(dcp->clk),
				    "Unable to find clock\n");
		goto err_piodma;
	}
	dcp->clk_194 = devm_clk_get_optional(dev, "clock-194");
	if (IS_ERR(dcp->clk_194)) {
		ret = dev_err_probe(dev, PTR_ERR(dcp->clk_194),
				    "Unable to find clock 0x194\n");
		goto err_piodma;
	}

	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	    (!dcp->clk_194 || !clk_get_rate(dcp->clk) ||
	     !clk_get_rate(dcp->clk_194))) {
		ret = dev_err_probe(dev, -EINVAL,
				    "External display clocks are unavailable\n");
		goto err_piodma;
	}

	bitmap_zero(dcp->memdesc_map, DCP_MAX_MAPPINGS);
	// TDOD: mem_desc IDs start at 1, for simplicity just skip '0' entry
	set_bit(0, dcp->memdesc_map);

	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		/* Pin before the first write or mailbox wake can expose DMA. */
		ret = dcp_pin_live_session(dcp);
		if (ret)
			goto err_piodma;
		if (!dcp_external_is_idle(dcp)) {
			ret = dev_err_probe(dev, -EBUSY,
					    "External DCP changed state before startup\n");
			goto err_piodma;
		}
	}

	/* The external coprocessor stays cold until its first sink. */
	if (!dcp->hw.adopt_live_session && !iomfb_is_v14_7_external(dcp)) {
		cpu_ctrl =
			readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
		writel_relaxed(cpu_ctrl | APPLE_DCP_COPROC_CPU_CONTROL_RUN,
			       dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	}

	/*
	 * The J514S PMP must have acknowledged the DISP request before the
	 * running DCP is adopted; the acknowledgement also starts the display
	 * memory-bandwidth floor.
	 */
	if (iomfb_is_v14_7_panel(dcp)) {
		ret = dcp_t6030_display_get(dev);
		if (ret) {
			dev_err(dev, "PMP DISP acknowledgement missing: %d\n", ret);
			goto err_piodma;
		}
		dev_info(dev, "PMP DISP acknowledged; starting warm RTKit adoption (no reset)\n");
	}

	/* Registering the mailbox can already expose RTKit buffer requests. */
	ret = dcp_pin_live_session(dcp);
	if (ret)
		goto err_piodma;
	/* External H17P supplies shared-buffer addresses during RTKit startup. */
	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL)
		dcp->rtk = apple_rtkit_init(dev, dcp, "mbox", 0,
					    &external_h17p_rtkit_ops);
	else
		dcp->rtk = apple_rtkit_init(dev, dcp, "mbox", 0, &rtkit_ops);
	if (IS_ERR(dcp->rtk)) {
		ret = dev_err_probe(dev, PTR_ERR(dcp->rtk),
				    "Failed to initialize RTKit\n");
		goto err_piodma;
	}
	ret = devm_add_action_or_reset(dev, dcp_release_rtkit, dcp);
	if (ret)
		goto err_piodma;

	if (dcp->hw.adopt_live_session)
		dev_dbg(dev, "negotiating RTKit with the running DCP\n");

	/* Announce host readiness before awaiting external IOP readiness. */
	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL)
		ret = apple_rtkit_wake_early_ap(dcp->rtk);
	else if (!iomfb_is_v14_7_external(dcp))
		ret = apple_rtkit_wake(dcp->rtk);
	if (ret) {
		ret = dev_err_probe(dev, ret, "Failed to boot RTKit: %d\n", ret);
		goto err_piodma;
	}
	dcp_enable_typec_work(dcp);
	return 0;

err_piodma:
	dcp_release_piodma_iommu_dev(dcp);
	return ret;
}

/*
 * We need to shutdown DCP before tearing down the display subsystem. Otherwise
 * the DCP will crash and briefly flash a green screen of death.
 */
VISIBLE_IF_KUNIT void dcp_comp_unbind(struct device *dev, struct device *main, void *data)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (!dcp)
		return;

	if (iomfb_is_v14_7_external(dcp))
		dcp_ext_v14_7_unbind(dcp);
	if (dcp->retain_dma) {
		/* No firmware stop is acknowledged: detach host users, never DMA. */
		scoped_guard(mutex, &dcp->iomfb.lock)
			WRITE_ONCE(dcp->quiescing, true);
		cancel_work_sync(&dcp->external_stop_work);
		cancel_delayed_work_sync(&dcp->external_stop_retry_wq);
		if (dcp->crtc && !dcp->drm_retained) {
			drm_dev_get(dcp->crtc->base.dev);
			dcp->drm_retained = true;
		}
		iomfb_queue_stop(dcp);
		afk_quiesce(dcp->avep);
		afk_quiesce(dcp->dptxep);
		afk_quiesce(dcp->dptx_controller_ep);
		afk_quiesce(dcp->ibootep);
		afk_quiesce(dcp->systemep);
		afk_quiesce(dcp->dcpavservep);
		cancel_delayed_work_sync(&dcp->bl_fallback_wq);
		dcp_backlight_stop(dcp);
		if (dcp->connector_type == DRM_MODE_CONNECTOR_eDP) {
			cancel_work_sync(&dcp->bl_register_wq);
			cancel_work_sync(&dcp->bl_update_wq);
		}
		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		/*
		 * Stop the announcement before draining the hotplug work: that
		 * work arms the timeout work, which in turn queues it.
		 */
		scoped_guard(mutex, &dcp->hpd_mutex)
			dcp->announce.stopped = true;
		disable_delayed_work_sync(&dcp->announce_wq);
		dcp_test_point(dcp, DCP_TEST_UNBIND_ANNOUNCE_STOPPED);
		cancel_work_sync(&dcp->vblank_wq);
		if (dcp->connector)
			cancel_work_sync(&dcp->connector->hotplug_wq);
		if (dcp->typec_connector && dcp->typec_connector != dcp->connector)
			cancel_work_sync(&dcp->typec_connector->hotplug_wq);
		dev_warn(dev, "retaining live DCP DMA resources after unbind; reboot required\n");
		return;
	}

	if (dcp->hdmi_hpd_irq)
		disable_irq(dcp->hdmi_hpd_irq);

	dcp_disable_typec_work(dcp, true);

	if (dcp->avep) {
		av_service_disconnect(dcp);
		afk_shutdown(dcp->avep);
		dcp->avep = NULL;
	}

	if (dcp->dptxep) {
		afk_shutdown(dcp->dptxep);
		dcp->dptxep = NULL;
	}

	if (!IS_ERR_OR_NULL(dcp->dptx_controller_ep)) {
		afk_shutdown(dcp->dptx_controller_ep);
		dcp->dptx_controller_ep = NULL;
	}

	if (dcp->ibootep) {
		afk_shutdown(dcp->ibootep);
		dcp->ibootep = NULL;
	}

	if (dcp->systemep) {
		afk_shutdown(dcp->systemep);
		dcp->systemep = NULL;
	}

	if (dcp->dcpavservep) {
		afk_shutdown(dcp->dcpavservep);
		dpavservep_detach(dcp);
		dcp->dcpavservep = NULL;
	}

	if (dcp->shmem)
		iomfb_shutdown(dcp);

	iomfb_queue_stop(dcp);
	dcp_release_piodma_iommu_dev(dcp);

	cancel_delayed_work_sync(&dcp->bl_fallback_wq);
	dcp_backlight_stop(dcp);
	if (dcp->connector_type == DRM_MODE_CONNECTOR_eDP) {
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
	}
	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);
	cancel_work_sync(&dcp->vblank_wq);

	devm_clk_put(dev, dcp->clk);
	dcp->clk = NULL;
	/* optional and possibly NULL, released with the bind's devres group */
	dcp->clk_194 = NULL;
}

static const struct component_ops dcp_comp_ops = {
	.bind	= dcp_comp_bind,
	.unbind	= dcp_comp_unbind,
};

/* The J514S (T6030) handoff this driver adopts. */
#define J514S_DCP_UUID		"DDF38191-93B3-324A-BC8F-643006F5AC82"
#define J514S_DCP_COPROC	0x28ec00000ULL
#define J514S_NOTCH_HEIGHT	74
#define J514S_BOOT_STRIDE	12096
#define J514S_INHERITED		"apple,j514s-inherited-mappings"

/*
 * The boot loader leaves one simple framebuffer on the panel: the full panel
 * or the panel below the notch, at the stride D101 reports.
 */
static int dcp_t6030_boot_framebuffer(struct device *dev, u32 *stride)
{
	struct device_node *np;
	u32 width, height, pitch;
	int found = 0;

	for_each_compatible_node(np, NULL, "simple-framebuffer") {
		if (!of_device_is_available(np))
			continue;
		if (found++ ||
		    of_property_read_u32(np, "width", &width) ||
		    of_property_read_u32(np, "height", &height) ||
		    of_property_read_u32(np, "stride", &pitch) ||
		    width != J514S_PANEL_WIDTH || pitch != J514S_BOOT_STRIDE ||
		    (height != J514S_PANEL_HEIGHT &&
		     height != J514S_PANEL_HEIGHT - J514S_NOTCH_HEIGHT)) {
			of_node_put(np);
			dev_err(dev, "unexpected boot framebuffer %pOF\n", np);
			return -EINVAL;
		}
	}
	if (!found) {
		dev_err(dev, "no boot framebuffer\n");
		return -ENODEV;
	}
	*stride = J514S_BOOT_STRIDE;
	return 0;
}

/* An enabled handoff node at @path that carries the inherited-mapping marker. */
static bool dcp_t6030_handoff_node(struct device_node *np, const char *path,
				   const char *compatible)
{
	struct device_node *alias = of_find_node_by_path(path);
	bool ok = np && alias == np && of_device_is_available(np) &&
		  of_property_read_bool(np, J514S_INHERITED) &&
		  (!compatible || of_device_is_compatible(np, compatible));

	of_node_put(alias);
	return ok;
}

/*
 * J514S admission: board and firmware identity, the DCP's coprocessor
 * window and DMA domain, the boot loader's display handoff (scanout and
 * PIODMA markers, inherited mappings, the chosen handoff note) and the boot
 * framebuffer.  The DCP is only adopted from this exact handoff.
 */
static int dcp_t6030_admit(struct platform_device *pdev, u32 *stride)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *piodma, *disp, *chosen;
	struct resource *res;
	const char *uuid;
	u32 compat[3];
	bool handoff;
	int ret;

	if (!of_machine_is_compatible("apple,j514s") ||
	    !of_machine_is_compatible("apple,t6030")) {
		dev_info(dev, "not a J514S; not binding\n");
		return -ENODEV;
	}
	if (of_property_count_u32_elems(np, "apple,firmware-compat") != 3 ||
	    of_property_read_u32_array(np, "apple,firmware-compat", compat, 3) ||
	    compat[0] != 14 || compat[1] != 7 || compat[2] != 0 ||
	    of_property_read_string(np, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, J514S_DCP_UUID)) {
		dev_info(dev, "DCP firmware is not the qualified 14.7.0 image; not binding\n");
		return -ENODEV;
	}
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc");
	if (!res || res->start != J514S_DCP_COPROC || resource_size(res) != SZ_16K) {
		dev_info(dev, "unexpected coprocessor window; not binding\n");
		return -ENODEV;
	}
	if (!iommu_get_domain_for_dev(dev))
		return dev_err_probe(dev, -EPROBE_DEFER, "DCP DMA domain not ready\n");

	piodma = of_get_child_by_name(np, "piodma");
	disp = of_find_node_by_path("disp0");
	chosen = of_find_node_by_path("/chosen");
	handoff = of_property_read_bool(np, "apple,j514s-native-scanout") &&
		  of_property_read_bool(np, "apple,j514s-native-piodma") &&
		  dcp_t6030_handoff_node(piodma, "disp0_piodma", NULL) &&
		  dcp_t6030_handoff_node(disp, "disp0",
					 "apple,t6030-display-diagnostics") &&
		  chosen &&
		  of_property_present(chosen, "aurora,j514s-display-handoff");
	of_node_put(chosen);
	of_node_put(disp);
	of_node_put(piodma);
	if (!handoff) {
		dev_info(dev, "no qualified boot loader display handoff; not binding\n");
		return -ENODEV;
	}

	ret = dcp_t6030_boot_framebuffer(dev, stride);
	if (ret)
		return ret;

	dev_info(dev, "J514S DCP firmware 14.7.0 handoff admitted\n");
	return 0;
}

static int dcp_platform_probe(struct platform_device *pdev)
{
	const struct apple_dcp_hw_data *hw = of_device_get_match_data(&pdev->dev);
	enum dcp_firmware_version fw_compat;
	struct device *dev = &pdev->dev;
	struct apple_dcp *dcp;
	u32 boot_stride = 0;
	int ret, surf, num_surfs;
	u32 surf_en;
	u32 mux_index;

	if (dcp_session_retained(dev))
		return dev_err_probe(dev, -EBUSY, "Previous live DCP session requires a reboot\n");

	/* Reject unsupported external firmware before replacing the boot display. */
	if (of_device_is_compatible(dev->of_node, "apple,t8140-dcpext") &&
	    !dcp_h17p_external_supported(dev->of_node)) {
		dev_err(dev, "Unsupported external display hardware or firmware handoff\n");
		return -ENODEV;
	}

	fw_compat = dcp_check_firmware_version(dev);
	if (fw_compat == DCP_FIRMWARE_UNKNOWN)
		return -ENODEV;

	/* J514S: the panel or a DCPEXT, each only from its qualified handoff. */
	if (IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030) && fw_compat == DCP_FIRMWARE_V_14_7) {
		ret = IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_EXT) && hw->v14_7_external ?
		      dcp_t6030_ext_admit(pdev) : dcp_t6030_admit(pdev, &boot_stride);
		if (ret)
			return ret;
	}

	/* Check for "apple,bw-scratch" to avoid probing appledrm with outdated
	 * device trees. This prevents replacing simpledrm and ending up without
	 * display.
	 */
	if (!of_property_present(dev->of_node, "apple,bw-scratch") &&
	    !hw->firmware_clock && !hw->no_rt_bandwidth)
		return dev_err_probe(dev, -ENODEV, "Incompatible devicetree! "
			"Use devicetree matching this kernel.\n");

	dcp = kzalloc_obj(*dcp);
	if (!dcp)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, dcp_release_context, dcp);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&dcp->swapped_out_fbs);
	mutex_init(&dcp->swapped_out_fbs_lock);
	ret = dcp_backlight_init(dcp, dev);
	if (ret)
		return ret;
	iomfb_queue_init(dcp);

	dcp->fw_compat = fw_compat;
	dcp->dev = dev;
	/*
	 * Type-C and Thunderbolt routes can be activated as soon as they are
	 * registered below, before the DRM device binds.
	 */
	mutex_init(&dcp->hpd_mutex);
	mutex_init(&dcp->external_lock);
	INIT_WORK(&dcp->external_stop_work, dcp_external_stop_work);
	INIT_DELAYED_WORK(&dcp->external_stop_retry_wq, dcp_external_stop_retry);
	mutex_init(&dcp->tb_lock);
	spin_lock_init(&dcp->mode_state.lock);
	spin_lock_init(&dcp->metadata.lock);
	mutex_init(&dcp->dcpavserv.lock);
	dcp->hw = *(struct apple_dcp_hw_data *)of_device_get_match_data(dev);
	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		if (!dcp->hw.firmware_clock ||
		    !apple_dcp_external_bw_valid(resource_size(dcp->hw.firmware_clock),
					 dcp->hw.firmware_scratch,
					 dcp->hw.firmware_request))
			return dev_err_probe(dev, -EINVAL,
					     "Invalid external profile bandwidth resources\n");
		iomfb_pause_presentation(dcp, true);
	}


	if (IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030) &&
	    fw_compat == DCP_FIRMWARE_V_14_7) {
		ret = iomfb_v14_7_session_init(dcp);
		if (ret)
			return ret;
		dcp->v14_7->boot_stride = boot_stride;
	}

	/* The domain use must survive driver-core DMA cleanup for a live session. */
	ret = iommu_device_use_default_domain(dev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, dcp_release_dma_domain, dcp);
	if (ret)
		return ret;
	dcp->fixed_connector_type = dcp_connector_type_from_dt(dev->of_node);
	dcp->connector_type = dcp->fixed_connector_type;
	of_property_read_u32(dev->of_node, "apple,dcp-index", &dcp->index);
	of_property_read_u32(dev->of_node, "apple,dptx-phy", &dcp->dptx_phy);
	of_property_read_u32(dev->of_node, "apple,dptx-die", &dcp->dptx_die);
	dcp->fixed_dptx_phy = dcp->dptx_phy;
	init_completion(&dcp->typec_iomfb_hpd_ready);
	INIT_DELAYED_WORK(&dcp->typec_reconnect_wq,
			  dcp_typec_reconnect_work);
	INIT_DELAYED_WORK(&dcp->placeholder_edid_wq,
			  dcp_placeholder_edid_work);
	INIT_DELAYED_WORK(&dcp->announce_wq, dcp_announce_work);
	INIT_DELAYED_WORK(&dcp->bl_fallback_wq, dcp_work_backlight_fallback);
	/* System sleep and shutdown cancel these even if no bind succeeded. */
	INIT_WORK(&dcp->vblank_wq, dcp_delayed_vblank);
	INIT_DELAYED_WORK(&dcp->swap_watchdog_wq, dcp_swap_watchdog);
	INIT_WORK(&dcp->bl_register_wq, dcp_work_register_backlight);
	INIT_WORK(&dcp->bl_update_wq, dcp_work_update_backlight);
	mutex_init(&dcp->bl_register_mutex);
	/* Balanced by enable at successful component bind. */
	disable_delayed_work(&dcp->typec_reconnect_wq);
	disable_delayed_work(&dcp->placeholder_edid_wq);
	disable_delayed_work(&dcp->announce_wq);
	dcp->announce.stopped = true;

	platform_set_drvdata(pdev, dcp);

	dcp->phy = devm_phy_optional_get(dev, "dp-phy");
	if (IS_ERR(dcp->phy)) {
		dev_err(dev, "Failed to get dp-phy: %ld\n", PTR_ERR(dcp->phy));
		return PTR_ERR(dcp->phy);
	}
	dcp->fixed_phy = dcp->phy;

	bitmap_zero(dcp->iomfb_surfaces, DCP_MAX_PLANES);
	if (!of_property_present(dev->of_node, "apple,iomfb-surfaces"))
		num_surfs = 0;
	else
		num_surfs = of_property_count_elems_of_size(dev->of_node,
						    "apple,iomfb-surfaces",
						    sizeof(u32));

	if (dcp->hw.primary_only && (num_surfs == 0 || num_surfs == -ENODATA)) {
		/* One primary plane: a second surface changes the panel's colour. */
		set_bit(0, dcp->iomfb_surfaces);
	} else if (num_surfs == 0 || num_surfs == -ENODATA) {
		set_bit(0, dcp->iomfb_surfaces);
		set_bit(1, dcp->iomfb_surfaces);
	} else if (num_surfs < 0) {
		return num_surfs;
	} else if (num_surfs > DCP_MAX_PLANES) {
		dev_err(dev, "Number of iomfb-surfaces (%d) exceeds DCP_MAX_PLANES\n",
			num_surfs);
		return -EINVAL;
	}

	surf = 0;
	of_property_for_each_u32(dev->of_node, "apple,iomfb-surfaces", surf_en) {
		if (surf_en)
			set_bit(surf, dcp->iomfb_surfaces);
		surf++;
	}
	/* The J700 loader names surface zero rather than enabling it. */
	if (of_machine_is_compatible("apple,j700") &&
	    of_device_is_compatible(dev->of_node, "apple,t8140-dcp") &&
	    num_surfs == 1 &&
	    !of_property_read_u32(dev->of_node, "apple,iomfb-surfaces", &surf_en) &&
	    surf_en == 0)
		set_bit(0, dcp->iomfb_surfaces);

	if (dcp->phy) {
		int ret;
		/*
		 * Request DP2HDMI related GPIOs as optional for DP-altmode
		 * compatibility. J180D misses a dp2hdmi-pwren GPIO in the
		 * template ADT. TODO: check device ADT
		 */
		dcp->hdmi_hpd = devm_gpiod_get_optional(dev, "hdmi-hpd", GPIOD_IN);
		if (IS_ERR(dcp->hdmi_hpd))
			return PTR_ERR(dcp->hdmi_hpd);
		if (dcp->hdmi_hpd) {
			int irq = gpiod_to_irq(dcp->hdmi_hpd);
			if (irq < 0) {
				dev_err(dev, "failed to translate HDMI hpd GPIO to IRQ\n");
				return irq;
			}
			dcp->hdmi_hpd_irq = irq;

			ret = devm_request_threaded_irq(dev, dcp->hdmi_hpd_irq, NULL,
						iomfb_is_v14_7_external(dcp) ? dcp_ext_v14_7_hpd_irq : dcp_dp2hdmi_hpd,
						IRQF_ONESHOT | IRQF_NO_AUTOEN |
						IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						"dp2hdmi-hpd-irq", dcp);
			if (ret < 0) {
				dev_err(dev, "failed to request HDMI hpd irq %d: %d\n",
					irq, ret);
				return ret;
			}
		}

		/*
		 * Power DP2HDMI on as it is required for the HPD irq.
		 * TODO: check if one is sufficient for the hpd to save power
		 *       on battery powered Macbooks.
		 */
		dcp->hdmi_pwren = devm_gpiod_get_optional(dev, "hdmi-pwren", GPIOD_OUT_HIGH);
		if (IS_ERR(dcp->hdmi_pwren))
			return PTR_ERR(dcp->hdmi_pwren);

		dcp->dp2hdmi_pwren = devm_gpiod_get_optional(dev, "dp2hdmi-pwren", GPIOD_OUT_HIGH);
		if (IS_ERR(dcp->dp2hdmi_pwren))
			return PTR_ERR(dcp->dp2hdmi_pwren);

		/*
		 * A DCP may have both a fixed HDMI/DP route and allocatable Type-C
		 * routes. Keep the fixed route selected until the allocator borrows
		 * this otherwise-idle pipeline for a Type-C display.
		 */
		ret = dcp->fixed_phy ?
			of_property_read_u32(dev->of_node, "mux-index", &mux_index) :
			-ENODATA;
		if (!ret) {
			dcp->fixed_mux_index = mux_index;
			dcp->xbar = devm_mux_control_get(dev, "dp-xbar");
			if (IS_ERR(dcp->xbar)) {
				dev_err(dev, "Failed to get dp-xbar: %ld\n", PTR_ERR(dcp->xbar));
				return PTR_ERR(dcp->xbar);
			}
			ret = devm_add_action_or_reset(dev, dcp_release_fixed_route, dcp);
			if (ret)
				return ret;
			ret = iomfb_is_v14_7_external(dcp) ? 0 : mux_control_select(dcp->xbar, mux_index);
			if (ret)
				dev_warn(dev, "mux_control_select failed: %d\n", ret);
			else	/* 14.7 external: selected with the sink's route (dcpext-t6030.c) */
				dcp->fixed_route_selected = !iomfb_is_v14_7_external(dcp);

			/*
			 * Switch atcphy to DP-only. should move to a Macbook Pro
			 * 14-/16-inch specific DP-to-HDMI drm_bridge.
			 */
			dcp->typec_mux = fwnode_typec_mux_get(dev_fwnode(dcp->dev));
			if (!IS_ERR_OR_NULL(dcp->typec_mux)) {
				struct typec_altmode alt = {
					.svid = USB_TYPEC_DP_SID,
				};
				struct typec_mux_state state = {
					.alt = &alt,
					.mode = TYPEC_DP_STATE_C,
				};
				int ret = typec_mux_set(dcp->typec_mux, &state);
				dev_info(dev, "typec_mux_set() returned: %d\n", ret);
				if (!ret)
					dcp->phy_managed_by_typec = true;
			} else {
				dev_info(dev, "fwnode_typec_mux_get() returned: %ld\n",
						IS_ERR(dcp->typec_mux) ? PTR_ERR(dcp->typec_mux) : 0);
				dcp->typec_mux = NULL;
			}
		}
	}

	ret = dcp_register_typec_routes(dcp);
	if (ret)
		return ret;

	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		/* Complete optional startup before the DRM master can depend on it. */
		ret = dcp_comp_bind(dev, NULL, NULL);
		if (ret) {
			dcp_disable_typec_work(dcp, true);
			return ret;
		}
		WRITE_ONCE(dcp->external_prepared, true);
	}

	/* Last fallible step: the J514S panel DCP is powered through the PMP. */
	if (IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030) &&
	    fw_compat == DCP_FIRMWARE_V_14_7 && !iomfb_is_v14_7_external(dcp)) {
		ret = dcp_t6030_display_prepare(dev);
		if (ret)
			return dev_err_probe(dev, ret, "PMP display supplier not ready\n");
	}

	ret = component_add(&pdev->dev, &dcp_comp_ops);
	if (ret && dcp->external_prepared) {
		dcp_disable_typec_work(dcp, true);
	}
	return ret;
}

static void dcp_platform_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dcp_comp_ops);
}

VISIBLE_IF_KUNIT void dcp_platform_shutdown(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp && dcp->retain_dma && dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		dcp_comp_unbind(&pdev->dev, NULL, NULL);
		return;
	}

	if (dcp && dcp->hw.adopt_live_session && dcp_fail_stop(dcp) &&
	    dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G) {
		/*
		 * This firmware keeps scanning across soft DPMS and has no
		 * known stop sequence. Component unbind releases the RTKit
		 * devres group and the piodma domain while firmware may still
		 * access them. Stop host submissions but keep the component and
		 * its DMA resources alive until the system resets.
		 */
		iomfb_queue_stop(dcp);
		cancel_delayed_work_sync(&dcp->bl_fallback_wq);
		dcp_backlight_stop(dcp);
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
		return;
	}

	component_del(&pdev->dev, &dcp_comp_ops);
}

/*
 * The display stays powered across system sleep, and the blanking present
 * queued by the DRM and backlight suspend handlers must reach the firmware
 * before its mailbox interrupt is suspended.
 */
static void dcp_drain_for_sleep(struct apple_dcp *dcp)
{
	int pass;

	if (!dcp->hw.adopt_live_session || !dcp_presents_nits(dcp))
		return;

	/*
	 * A completed present can schedule one more brightness present, and a
	 * rejected one a delayed retry; run a pending retry now instead of
	 * letting it fire after the mailbox has suspended.  Four passes cover
	 * the first attempt and the policy's three retries.  A fade still
	 * running ends at its target first.
	 */
	if (dcp_has_panel(dcp))
		dcp_backlight_finish_fade(dcp);
	for (pass = 0; pass < 4; pass++) {
		flush_delayed_work(&dcp->iomfb.backlight_retry);
		if (dcp_has_panel(dcp))
			flush_work(&dcp->bl_update_wq);
		if (!iomfb_queue_drain(dcp, msecs_to_jiffies(500))) {
			dev_warn(dcp->dev, "display updates still pending at suspend\n");
			return;
		}
		/*
		 * A crashed or stopped DCP takes no more presents, so a level
		 * still pending there can never be sent and is not waited for.
		 * Checking after the drain, which returns at once for such a
		 * DCP, also covers a crash during the pass.
		 */
		if (READ_ONCE(dcp->crashed) || READ_ONCE(dcp->iomfb.stopped))
			return;
		/*
		 * Nor does a panel whose display controller is off: the
		 * backlight restore at power-on presents the pending level.
		 */
		if (dcp->panel_hw_off)
			return;
		if (!dcp_backlight_active(dcp) || !dcp_backlight_pending(dcp))
			return;
	}
	dev_warn(dcp->dev, "brightness change still pending at suspend\n");
}

/*
 * DPMS off leaves the internal H17P pipe scanning out (see dcp_poweroff()),
 * and while it scans out the firmware publishes a property to the AP on every
 * frame.  The mailbox interrupt is not suspended, so each of those messages
 * wakes a CPU out of suspend-to-idle about 60 times a second.  Hold the
 * messages in the mailbox for system sleep and process them at resume.
 * apple-drm is a consumer of this device, so its suspend handler has blanked
 * the panel before this runs and its resume handler restores the CRTC after.
 */
static void dcp_hold_for_sleep(struct apple_dcp *dcp)
{
	/* A failed component bind leaves no coprocessor session to hold. */
	if (IS_ERR_OR_NULL(dcp->rtk) ||
	    !dcp_uses_soft_dpms(dcp) || !dcp_presents_nits(dcp) ||
	    READ_ONCE(dcp->crashed))
		return;

	iomfb_pause_presentation(dcp, true);
	if (iomfb_queue_settle(dcp, msecs_to_jiffies(500))) {
		iomfb_pause_presentation(dcp, false);
		return;
	}
	apple_rtkit_hold_rx(dcp->rtk);
	dcp->rx_held = true;
}

static void dcp_release_after_sleep(struct apple_dcp *dcp)
{
	if (!dcp->rx_held)
		return;
	dcp->rx_held = false;
	apple_rtkit_resume_rx(dcp->rtk);
	iomfb_pause_presentation(dcp, false);
}

static int dcp_platform_suspend(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (iomfb_is_v14_7_external(dcp))
		return dcp_ext_v14_7_suspend(dcp);
	if (dcp_is_external_h17p(dcp)) {
		int ret;

		dcp_disable_typec_work(dcp, false);
		ret = dcp_external_disconnect(dcp, false);
		if (ret) {
			dcp_enable_typec_work(dcp);
			return ret;
		}
		device_set_wakeup_path(dev);
		return 0;
	}

	dcp_drain_for_sleep(dcp);
	/*
	 * The Type-C route reports cable removal through
	 * dcp_dptx_disconnect_oob(). A DP tunnel kept through the sleep stays
	 * connected, and resume powers the CRTC back up through dcp_poweron()
	 * as after DPMS off.
	 */
	dcp_disable_typec_work(dcp, false);
	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);

	if (dcp->avep)
		av_service_disconnect(dcp);

	if (dcp->hdmi_hpd_irq) {
		disable_irq(dcp->hdmi_hpd_irq);
		if (!dcp->active_typec_route) {
			disconnected_hpd_event(dcp->connector);
			dcp_dptx_disconnect(dcp, 0);
		}
	}
	/*
	 * Set the device as a wakeup device, which forces its power
	 * domains to stay on. We need this as we do not support full
	 * shutdown properly yet.
	 */
	device_set_wakeup_path(dev);
	/* Last: the steps above still talk to the coprocessor. */
	dcp_hold_for_sleep(dcp);

	return 0;
}

static int dcp_platform_resume(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (iomfb_is_v14_7_external(dcp))
		return dcp_ext_v14_7_resume(dcp);
	dcp_release_after_sleep(dcp);
	dcp_enable_typec_work(dcp);
	if (dcp->hdmi_hpd_irq)
		enable_irq(dcp->hdmi_hpd_irq);

	if (dcp->avep)
		av_service_connect(dcp);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(dcp_platform_pm_ops,
				dcp_platform_suspend, dcp_platform_resume);


static const struct apple_dcp_hw_data apple_dcp_hw_t6020 = {
	.num_dptx_ports = 1,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t8112 = {
	.num_dptx_ports = 2,
};

/* The internal T8140 endpoint uses the H17P method profile. */
static const struct resource t8140_firmware_clock =
	DEFINE_RES_MEM(0x302800000, 0xbc000);

static const struct apple_dcp_hw_data apple_dcp_hw_t8140 = {
	.num_dptx_ports = 0,
	.adopt_live_session = true,
	.firmware_compat = DCP_FIRMWARE_H17P,
	.firmware_clock = &t8140_firmware_clock,
	.firmware_display_windows = 5,
	.firmware_scratch = 0x20000,
	.firmware_request = 0x68000,
};

/* Separate from the established internal-panel H17P profile. */
static const struct apple_dcp_hw_data apple_dcp_hw_t8140_external = {
	.num_dptx_ports = 1,
	.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL,
	.adopt_live_session = false,
	.firmware_compat = DCP_FIRMWARE_H17P,
	.firmware_clock = &t8140_firmware_clock,
	.firmware_display_windows = 4,
	/* Native PMGR BWR agent 2 and boost agent 3 for DISPEXT0. */
	.firmware_scratch = 0x28000,
	.firmware_request = 0x70000,
};

/*
 * M5 (T8142) runs H17-generation DCP firmware with the H17G method numbering
 * and is left running by the bootloader.
 */
static const struct apple_dcp_hw_data apple_dcp_hw_t8142 = {
	.num_dptx_ports = 0,
	.iomfb_method_profile = DCP_IOMFB_METHODS_H17G,
	.adopt_live_session = true,
	.firmware_compat = DCP_FIRMWARE_H17P,
};

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030)
/*
 * MacBook Pro M3 Pro (J514S, T6030) internal panel.  The boot loader leaves
 * the DCP running DCP firmware 14.7.0 with the panel lit; Linux adopts that
 * session with an RTKit wake and never stops or restarts the ASC.
 */
static const struct apple_dcp_hw_data apple_dcp_hw_t6030 = {
	.num_dptx_ports = 0,
	.adopt_live_session = true,
	.firmware_compat = DCP_FIRMWARE_V_14_7,
	.primary_only = true,
	.optional_clock = true,
	.no_rt_bandwidth = true,
	.panel_mini_led = true,
	.panel_max_nits = 500,
	.panel_default_nits = 140,
};
#endif

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_EXT)
/*
 * MacBook Pro M3 Pro (J514S) external display controllers (DCPEXT0/1): the
 * panel's 14.7.0 firmware image in its external role, started cold by
 * Linux, with one DPTX port that the Type-C routes connect to a USB-C port.
 */
static const struct apple_dcp_hw_data apple_dcp_hw_t6030_dcpext = {
	.num_dptx_ports = 1,
	.firmware_compat = DCP_FIRMWARE_V_14_7,
	.primary_only = true,
	.optional_clock = true,
	.no_rt_bandwidth = true,
	.v14_7_external = true,
};
#endif

static const struct apple_dcp_hw_data apple_dcp_hw_dcp = {
	.num_dptx_ports = 0,
};

static const struct apple_dcp_hw_data apple_dcp_hw_dcpext = {
	.num_dptx_ports = 2,
};

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,t6020-dcp", .data = &apple_dcp_hw_t6020,  },
	{ .compatible = "apple,t8112-dcp", .data = &apple_dcp_hw_t8112,  },
	{ .compatible = "apple,t8140-dcp", .data = &apple_dcp_hw_t8140,  },
	{ .compatible = "apple,t8140-dcpext", .data = &apple_dcp_hw_t8140_external, },
	{ .compatible = "apple,t8142-dcp", .data = &apple_dcp_hw_t8142,  },
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030)
	{ .compatible = "apple,t6030-dcp", .data = &apple_dcp_hw_t6030,  },
#endif
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_EXT)
	{ .compatible = "apple,t6030-dcpext", .data = &apple_dcp_hw_t6030_dcpext, },
#endif
	{ .compatible = "apple,dcp",       .data = &apple_dcp_hw_dcp,    },
	{ .compatible = "apple,dcpext",    .data = &apple_dcp_hw_dcpext, },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
	.driver_managed_dma = true,
	.probe		= dcp_platform_probe,
	.remove		= dcp_platform_remove,
	.shutdown	= dcp_platform_shutdown,
	.driver	= {
		.name = "apple-dcp",
		.of_match_table	= of_match,
		.pm = pm_sleep_ptr(&dcp_platform_pm_ops),
	},
};

void __init dcp_register(void)
{
	platform_driver_register(&apple_platform_driver);
}

void __exit dcp_unregister(void)
{
	platform_driver_unregister(&apple_platform_driver);
}

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_EXT)
/*
 * J514S DCPEXT start: its DMA domain, buffers and this module are pinned
 * until reboot before the firmware can see a buffer.
 * A cold coprocessor then gets RUN alone, the reset vector state preserved.
 * One the boot loader left running (RUN set, e.g. with a sink attached at
 * power-on) keeps running untouched: only its RTKit communication restarts,
 * with the INIT power request of the RTKit wake. Anything else is refused.
 */
int dcp_ext_v14_7_run(struct apple_dcp *dcp)
{
	struct dcp_session_ref *session;
	bool inherited;
	u32 ctrl;

	ctrl = readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	if (ctrl && ctrl != APPLE_DCP_COPROC_CPU_CONTROL_RUN) {
		dev_err(dcp->dev, "DCPEXT in an unknown state (CPU control %#x); not starting\n",
			ctrl);
		return -EBUSY;
	}
	inherited = ctrl == APPLE_DCP_COPROC_CPU_CONTROL_RUN;
	if (!dcp->retain_dma) {
		session = kzalloc_obj(*session);
		if (!session)
			return -ENOMEM;
		session->dev = get_device(dcp->dev);
		session->group = iommu_group_get(dcp->dev);
		__module_get(THIS_MODULE);
		WRITE_ONCE(dcp->retain_dma, true);
		mutex_lock(&dcp_retained_sessions_lock);
		list_add_tail(&session->link, &dcp_retained_sessions);
		mutex_unlock(&dcp_retained_sessions_lock);
	}
	if (inherited) {
		dev_info(dcp->dev, "DCPEXT left running by the boot loader; restarting its RTKit session, DMA retained until reboot\n");
		return 0;
	}
	writel(APPLE_DCP_COPROC_CPU_CONTROL_RUN, dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	ctrl = readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	if (ctrl != APPLE_DCP_COPROC_CPU_CONTROL_RUN) {
		dev_err(dcp->dev, "DCPEXT RUN not taken (CPU control %#x)\n", ctrl);
		return -EIO;
	}
	dev_info(dcp->dev, "DCPEXT cold RUN; DMA retained until reboot\n");
	return 0;
}
#endif

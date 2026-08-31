// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <linux/align.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/string.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/workqueue.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_module.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "afk.h"
#include "av.h"
#include "dcp.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "parser.h"
#include "trace.h"

#define APPLE_DCP_COPROC_CPU_CONTROL	 0x44
#define APPLE_DCP_COPROC_CPU_CONTROL_RUN BIT(4)

#define DCP_BOOT_TIMEOUT msecs_to_jiffies(1000)

static bool show_notch;
module_param(show_notch, bool, 0644);
MODULE_PARM_DESC(show_notch, "Use the full display height and shows the notch");

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
	/* DRM connector for this physical port, driven by whichever DCP owns it */
	struct apple_connector *connector;
	/* last mux state acted on, to collapse the per-candidate notifications */
	struct typec_altmode *applied_alt;
	unsigned long applied_mode;
	u32 applied_status;
	u32 applied_conf;
	bool applied_valid;
	bool hpd;
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
	return !route->dcp->active_typec_route &&
	       !dcp_typec_route_fixed_output_busy(route);
}

static int dcp_typec_route_activate(struct apple_dcp_typec_route *route);
static int dcp_typec_route_deactivate(struct apple_dcp_typec_route *route);

/*
 * Pipelines are ranked by CRTC index so the fabric's choice is a pure function
 * of the topology rather than of plug order.  A pipeline whose fixed output is
 * live is not a candidate at all, so a hybrid is only ever ranked here when it
 * is genuinely free.
 */
static unsigned int dcp_typec_route_score(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;

	if (!dcp->crtc)
		return UINT_MAX - 1;

	return drm_crtc_index(&dcp->crtc->base);
}

static int dcp_typec_route_activate(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;
	int ret;

	if (dcp->fixed_route_selected) {
		ret = mux_control_deselect(dcp->xbar);
		if (ret)
			return ret;
		dcp->fixed_route_selected = false;
	}

	ret = mux_control_select(route->xbar, route->mux_index);
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
	if (route->port->connector) {
		route->port->connector->dcp = to_platform_device(dcp->dev);
		dcp->typec_connector = route->port->connector;
		dcp->connector = route->port->connector;

		/*
		 * Narrow the port to the pipeline now driving it.  The encoder
		 * spans every pipeline that could, which is what lets userspace
		 * see the port as usable at all -- but only one of them is
		 * routed to the display, and userspace has no way to tell which.
		 * Offering it the choice makes it pair the port with a pipeline
		 * holding a different monitor's mode list, and the modeset is
		 * rejected with no way for it to recover.  The hotplug that
		 * follows makes it re-read this.
		 */
		if (route->port->connector->port_encoder && dcp->crtc)
			route->port->connector->port_encoder->possible_crtcs =
				drm_crtc_mask(&dcp->crtc->base);
	}
	dcp->active_typec_route = route;
	route->selected = true;

	dev_info(dcp->dev, "allocated Type-C DPTX PHY %u\n", route->dptx_phy);
	return 0;
}

static int dcp_typec_route_deactivate(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;
	int ret;

	ret = mux_control_deselect(route->xbar);
	if (ret)
		return ret;

	route->selected = false;
	if (dcp->active_typec_route == route)
		dcp->active_typec_route = NULL;

	if (route->port->connector &&
	    route->port->connector->dcp == to_platform_device(dcp->dev)) {
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
		WRITE_ONCE(route->port->connector->connected, false);
		route->port->connector->dcp = NULL;

		/* Unrouted: the port could go to any of its pipelines again. */
		if (route->port->connector->port_encoder)
			route->port->connector->port_encoder->possible_crtcs =
				route->port->connector->candidate_crtcs;
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

			if (dcp->xbar) {
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

static void dcp_typec_retrain_work(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     typec_fabric_retrain_wq);

	if (READ_ONCE(dcp->active_typec_route) && dcp->typec_connector)
		dcp_retrain_oob(dcp->typec_connector);
}

static void dcp_typec_retrain_active_routes(void)
{
	struct apple_dcp_typec_port *port;

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (!port->owner)
			continue;
		mod_delayed_work(system_freezable_wq,
				 &port->owner->dcp->typec_fabric_retrain_wq,
				 msecs_to_jiffies(200));
	}
}

static int dcp_typec_route_set(struct typec_mux_dev *mux,
			       struct typec_mux_state *state)
{
	struct apple_dcp_typec_route *route = typec_mux_get_drvdata(mux);
	struct apple_dcp_typec_port *port = route->port;
	struct apple_dcp_typec_route *candidate, *best = NULL;
	bool is_dp = dcp_typec_route_is_dp(state);
	struct typec_displayport_data *dp_data = is_dp ? state->data : NULL;
	u32 dp_status = dp_data ? dp_data->status : 0;
	u32 dp_conf = dp_data ? dp_data->conf : 0;
	unsigned int best_score = UINT_MAX;
	bool hpd;
	int ret = 0;

	guard(mutex)(&dcp_typec_fabric_lock);

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
	    port->applied_status == dp_status && port->applied_conf == dp_conf)
		return 0;

	port->applied_alt = state->alt;
	port->applied_mode = state->mode;
	port->applied_status = dp_status;
	port->applied_conf = dp_conf;
	port->applied_valid = true;

	if (!is_dp) {
		if (port->owner) {
			struct apple_dcp *dcp = port->owner->dcp;

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
		}

		if (state->mode == TYPEC_MODE_USB4)
			dcp_typec_retrain_active_routes();
		return 0;
	}

	if (!port->owner) {
		list_for_each_entry(candidate, &port->routes, port_link) {
			unsigned int score;

			if (!dcp_typec_route_available(candidate))
				continue;
			score = dcp_typec_route_score(candidate);
			if (score < best_score) {
				best = candidate;
				best_score = score;
			}
		}

		if (!best)
			return -EBUSY;

		ret = dcp_typec_route_activate(best);
		if (ret)
			return ret;
		port->owner = best;
	}


	hpd = dp_data && (dp_data->status & DP_STATUS_HPD_STATE);
	if (!hpd && port->hpd) {
		dcp_dptx_disconnect_oob(to_platform_device(port->owner->dcp->dev), 0);
	} else if (hpd && !port->hpd) {
		struct apple_dcp *dcp = port->owner->dcp;

		WRITE_ONCE(dcp->typec_cable_connected, true);
		if (dcp->typec_connector)
			dcp_dptx_connect_oob(to_platform_device(dcp->dev), 0);
	} else if (hpd && dp_data && (dp_data->status & DP_STATUS_IRQ_HPD)) {
		struct apple_dcp *dcp = port->owner->dcp;

		if (dcp->typec_connector)
			dcp_retrain_oob(dcp->typec_connector);
	}
	port->hpd = hpd;

	return 0;
}

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

void dcp_typec_port_set_connector(unsigned int idx,
				  struct apple_connector *connector)
{
	struct apple_dcp_typec_port *port;

	guard(mutex)(&dcp_typec_fabric_lock);

	port = dcp_typec_port_by_index(idx);
	if (!port)
		return;

	port->connector = connector;

	/*
	 * The port may already have been routed, either before DRM bound or
	 * while these connectors were being created.  Adopt that owner now,
	 * otherwise its display would be reported on the pipeline's fixed
	 * connector instead of the port it is actually plugged into.
	 */
	if (port->owner) {
		struct apple_dcp *dcp = port->owner->dcp;

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
	if (port->owner == route) {
		struct apple_dcp *dcp = route->dcp;

		if (port->hpd || dcp->typec_cable_connected)
			dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
		port->hpd = false;
		dcp_typec_route_deactivate(route);
		port->owner = NULL;
	}
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

void dcp_set_dimensions(struct apple_dcp *dcp)
{
	int i;
	int width_mm = dcp->width_mm;
	int height_mm = dcp->height_mm;

	if (width_mm == 0 || height_mm == 0) {
		width_mm = dcp->panel.width_mm;
		height_mm = dcp->panel.height_mm;
	}

	/* Set the connector info */
	if (dcp->connector) {
		struct drm_connector *connector = &dcp->connector->base;

		mutex_lock(&connector->dev->mode_config.mutex);
		connector->display_info.width_mm = width_mm;
		connector->display_info.height_mm = height_mm;
		mutex_unlock(&connector->dev->mode_config.mutex);
	}

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

bool dcp_has_panel(struct apple_dcp *dcp)
{
	return dcp->panel.width_mm > 0;
}

int dcp_set_crc(struct drm_crtc *crtc, bool enabled)
{
	struct apple_crtc *ac = to_apple_crtc(crtc);
	struct apple_dcp *dcp = platform_get_drvdata(ac->dcp);

	dcp->crc_enabled = enabled;

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

	dcp = container_of(work, struct apple_dcp, vblank_wq);
	mdelay(5);
	dcp_drm_crtc_vblank(dcp->crtc);
}

static void dcp_recv_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;

	trace_dcp_recv_msg(dcp, endpoint, message);

	switch (endpoint) {
	case IOMFB_ENDPOINT:
		return iomfb_recv_msg(dcp, message);
	case AV_ENDPOINT:
		afk_receive_message(dcp->avep, message);
		return;
	case SYSTEM_ENDPOINT:
		afk_receive_message(dcp->systemep, message);
		return;
	case DISP0_ENDPOINT:
		afk_receive_message(dcp->ibootep, message);
		return;
	case DPAVSERV_ENDPOINT:
		afk_receive_message(dcp->dcpavservep, message);
		return;
	case DPTX_ENDPOINT:
		afk_receive_message(dcp->dptxep, message);
		return;
	default:
		WARN(endpoint, "unknown DCP endpoint %hhu\n", endpoint);
	}
}

static void dcp_rtk_crashed(void *cookie, const void *crashlog, size_t crashlog_size)
{
	struct apple_dcp *dcp = cookie;

	dcp->crashed = true;
	dev_err(dcp->dev, "DCP has crashed\n");
	if (dcp->connector) {
		dcp->connector->connected = 0;
		drm_edid_free(dcp->connector->drm_edid);
		dcp->connector->drm_edid = NULL;
		schedule_work(&dcp->connector->hotplug_wq);
	}
	complete(&dcp->start_done);
}

static int dcp_rtk_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

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

		// TODO: verify phy_addr, cache attribute
		bfr->buffer = memremap(phy_addr, bfr->size, MEMREMAP_WB);
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

void dcp_send_message(struct apple_dcp *dcp, u8 endpoint, u64 message)
{
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

	needs_modeset = drm_atomic_crtc_needs_modeset(crtc_state) || !dcp->valid_mode;
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
#define DPTX_RECONNECT_DELAY msecs_to_jiffies(1000)
#define DPTX_RECONNECT_RETRIES 5

static int dcp_dptx_connect(struct apple_dcp *dcp, u32 port)
{
	int ret = 0;

	if (!dcp->phy) {
		dev_warn(dcp->dev, "dcp_dptx_connect: missing phy\n");
		return -ENODEV;
	}
	dev_info(dcp->dev,
		 "%s(port=%d) target=%u:%u typec=%d route=%s conn_type=%d connected=%d\n",
		 __func__, port, dcp->dptx_die, dcp->dptx_phy,
		 dcp_is_typec_output(dcp),
		 dcp->active_typec_route ? "borrowed" : "fixed",
		 dcp->connector_type, dcp->dptxport[port].connected);

	mutex_lock(&dcp->hpd_mutex);
	if (!dcp->dptxport[port].enabled) {
		dev_warn(dcp->dev, "dcp_dptx_connect: dptx service for port %d not enabled\n", port);
		ret = -ENODEV;
		goto out_unlock;
	}

	if (dcp->dptxport[port].connected)
		goto out_unlock;

	reinit_completion(&dcp->dptxport[port].linkcfg_completion);
	dcp->dptxport[port].atcphy = dcp->phy;
	ret = dptxport_validate_connection(dcp->dptxport[port].service, 0,
					   dcp->dptx_phy, dcp->dptx_die);
	if (ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: failed to validate DPTX target %u:%u: %d\n",
			dcp->dptx_die, dcp->dptx_phy, ret);
		goto out_unlock;
	}

	ret = dptxport_connect(dcp->dptxport[port].service, 0,
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
	ret = wait_for_completion_timeout(&dcp->dptxport[port].linkcfg_completion,
				    DPTX_CONNECT_TIMEOUT);
	if (!ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: timed out waiting for port %u link configuration\n",
			port);
		ret = -ETIMEDOUT;
		goto out_disconnect;
	}

	dev_dbg(dcp->dev, "dcp_dptx_connect: waited %d ms for link\n",
		jiffies_to_msecs(DPTX_CONNECT_TIMEOUT - ret));

	usleep_range(5, 10);

	if (dcp->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
		dptxport_set_hpd(dcp->dptxport[port].service, true);

	if (dcp->avep)
		av_service_connect(dcp);

	return 0;

out_disconnect:
	mutex_lock(&dcp->hpd_mutex);
	dcp->dptxport[port].connected = false;
out_release:
	dptxport_release_display(dcp->dptxport[port].service);

out_unlock:
	mutex_unlock(&dcp->hpd_mutex);
	return ret;
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

	if (++dcp->typec_reconnect_tries < DPTX_RECONNECT_RETRIES) {
		mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq,
				 DPTX_RECONNECT_DELAY);
		return;
	}

	dev_err(dcp->dev, "Type-C DPTX reconnect failed after %u retries: %d\n",
		dcp->typec_reconnect_tries, ret);
}

static void disconnected_hpd_event(struct apple_connector *con)
{
	if (con && con->connected) {
		con->connected = 0;
		drm_edid_free(con->drm_edid);
		con->drm_edid = NULL;
		drm_kms_helper_connector_hotplug_event(&con->base);
	}
}

static int dcp_dptx_disconnect(struct apple_dcp *dcp, u32 port)
{
	dev_info(dcp->dev, "%s(port=%d)\n", __func__, port);

	mutex_lock(&dcp->hpd_mutex);
	if (dcp->dptxport[port].enabled && dcp->dptxport[port].connected) {
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

	if (dcp_is_typec_output(dcp)) {
		WRITE_ONCE(dcp->typec_cable_connected, true);
		dcp->typec_reconnect_tries = 0;
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

	if (dcp_is_typec_output(dcp)) {
		WRITE_ONCE(dcp->typec_cable_connected, false);
		cancel_delayed_work(&dcp->typec_reconnect_wq);
	}

	disconnected_hpd_event(dcp->connector);

	if (dcp->avep)
		av_service_disconnect(dcp);

	if (dcp->dptxport[port].enabled)
		dptxport_set_hpd(dcp->dptxport[port].service, false);

	return dcp_dptx_disconnect(dcp, port);
}

static irqreturn_t dcp_dp2hdmi_hpd(int irq, void *data)
{
	struct apple_dcp *dcp = data;
	bool connected;

	guard(mutex)(&dcp_typec_fabric_lock);

	if (READ_ONCE(dcp->active_typec_route))
		return IRQ_HANDLED;
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

	dcp->crtc = crtc;

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

	init_completion(&dcp->start_done);

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
		} else if (dcp->dptxport[0].enabled) {
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

int dcp_wait_ready(struct platform_device *pdev, u64 timeout)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

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
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

void dcp_poweron(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (dcp_is_typec_output(dcp)) {
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
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}

	if (dcp->avep)
		av_service_connect(dcp);
}

void dcp_poweroff(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (dcp->avep)
		av_service_disconnect(dcp);

	_dcp_poweroff(dcp);

	if (dcp_is_typec_output(dcp)) {
		/*
		 * DCP owns a synthetic HPD for Type-C. Release it with the CRTC,
		 * then recreate the session while the cable remains present.
		 */
		if (dcp->dptxport[0].enabled && dcp->dptxport[0].connected) {
			ret = dptxport_set_hpd(dcp->dptxport[0].service, false);
			if (ret)
				dev_warn(dcp->dev,
					 "failed to deassert Type-C DPTX HPD: %d\n", ret);
			dcp_dptx_disconnect(dcp, 0);

			if (READ_ONCE(dcp->typec_cable_connected)) {
				dcp->typec_reconnect_tries = 0;
				mod_delayed_work(system_freezable_wq,
						 &dcp->typec_reconnect_wq,
						 DPTX_RECONNECT_DELAY);
			}
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
	if (ret) {
		dev_err(dcp->dev, "Unable to register backlight device\n");
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

static int dcp_create_piodma_iommu_dev(struct apple_dcp *dcp)
{
	int ret;
	struct device_node *node __free(device_node) = of_get_child_by_name(dcp->dev->of_node, "piodma");

	if (!node)
		return dev_err_probe(dcp->dev, -ENODEV,
				     "Failed to get piodma child DT node\n");

	dcp->piodma = of_platform_device_create(node, NULL, dcp->dev);
	if (!dcp->piodma)
		return dev_err_probe(dcp->dev, -ENODEV, "Failed to create piodma pdev for %pOF\n", node);

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
	of_platform_device_destroy(&dcp->piodma->dev, NULL);
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
	int count = pdev->num_resources - 1;
	int i, ret;

	if (count <= 0 || count > MAX_DISP_REGISTERS)
		return -EINVAL;

	for (i = 0; i < count; ++i) {
		dcp->disp_registers[i] =
			platform_get_resource(pdev, IORESOURCE_MEM, 1 + i);
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
	char compat_str[DCP_FW_VERSION_STR_LEN];
	char fw_str[DCP_FW_VERSION_STR_LEN];
	int ret;

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

static int dcp_comp_bind(struct device *dev, struct device *main, void *data)
{
	struct device_node *panel_np;
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	u32 cpu_ctrl;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;

	dcp->coproc_reg = devm_platform_ioremap_resource_byname(to_platform_device(dev), "coproc");
	if (IS_ERR(dcp->coproc_reg))
		return PTR_ERR(dcp->coproc_reg);

	if (dcp->index || dcp->dptx_phy || dcp->dptx_die)
		dev_info(dev, "DCP index:%u dptx target phy: %u dptx die: %u\n",
			 dcp->index, dcp->dptx_phy, dcp->dptx_die);
	mutex_init(&dcp->hpd_mutex);

	if (!show_notch)
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
		INIT_WORK(&dcp->bl_register_wq, dcp_work_register_backlight);
		mutex_init(&dcp->bl_register_mutex);
		INIT_WORK(&dcp->bl_update_wq, dcp_work_update_backlight);
	}

	ret = dcp_create_piodma_iommu_dev(dcp);
	if (ret || !dcp->iommu_dom)
		return dev_err_probe(dev, ret,
				"Failed to created PIODMA iommu child device");

	ret = dcp_get_disp_regs(dcp);
	if (ret) {
		dev_err(dev, "failed to find display registers\n");
		return ret;
	}

	dcp->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dcp->clk))
		return dev_err_probe(dev, PTR_ERR(dcp->clk),
				     "Unable to find clock\n");

	bitmap_zero(dcp->memdesc_map, DCP_MAX_MAPPINGS);
	// TDOD: mem_desc IDs start at 1, for simplicity just skip '0' entry
	set_bit(0, dcp->memdesc_map);

	INIT_WORK(&dcp->vblank_wq, dcp_delayed_vblank);

	dcp->swapped_out_fbs =
		(struct list_head)LIST_HEAD_INIT(dcp->swapped_out_fbs);

	cpu_ctrl =
		readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	writel_relaxed(cpu_ctrl | APPLE_DCP_COPROC_CPU_CONTROL_RUN,
		       dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);

	dcp->rtk = devm_apple_rtkit_init(dev, dcp, "mbox", 0, &rtkit_ops);
	if (IS_ERR(dcp->rtk))
		return dev_err_probe(dev, PTR_ERR(dcp->rtk),
				     "Failed to initialize RTKit\n");

	ret = apple_rtkit_wake(dcp->rtk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to boot RTKit: %d\n", ret);
	return ret;
}

/*
 * We need to shutdown DCP before tearing down the display subsystem. Otherwise
 * the DCP will crash and briefly flash a green screen of death.
 */
static void dcp_comp_unbind(struct device *dev, struct device *main, void *data)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (!dcp)
		return;

	if (dcp->hdmi_hpd_irq)
		disable_irq(dcp->hdmi_hpd_irq);

	typec_mux_put(dcp->typec_mux);

	if (dcp->avep) {
		av_service_disconnect(dcp);
		afk_shutdown(dcp->avep);
		dcp->avep = NULL;
	}

	if (dcp->dptxep) {
		afk_shutdown(dcp->dptxep);
		dcp->dptxep = NULL;
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
		dcp->dcpavservep = NULL;
	}

	if (dcp->shmem)
		iomfb_shutdown(dcp);

	if (dcp->piodma) {
		dcp->iommu_dom = NULL;
		of_platform_device_destroy(&dcp->piodma->dev, NULL);
		dcp->piodma = NULL;
	}

	if (dcp->connector_type == DRM_MODE_CONNECTOR_eDP) {
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
	}
	cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
	cancel_delayed_work_sync(&dcp->typec_fabric_retrain_wq);
	cancel_work_sync(&dcp->vblank_wq);

	devm_clk_put(dev, dcp->clk);
	dcp->clk = NULL;
}

static const struct component_ops dcp_comp_ops = {
	.bind	= dcp_comp_bind,
	.unbind	= dcp_comp_unbind,
};

static int dcp_platform_probe(struct platform_device *pdev)
{
	enum dcp_firmware_version fw_compat;
	struct device *dev = &pdev->dev;
	struct apple_dcp *dcp;
	int ret, surf, num_surfs;
	u32 surf_en;
	u32 mux_index;

	fw_compat = dcp_check_firmware_version(dev);
	if (fw_compat == DCP_FIRMWARE_UNKNOWN)
		return -ENODEV;

	/* Check for "apple,bw-scratch" to avoid probing appledrm with outdated
	 * device trees. This prevents replacing simpledrm and ending up without
	 * display.
	 */
	if (!of_property_present(dev->of_node, "apple,bw-scratch"))
		return dev_err_probe(dev, -ENODEV, "Incompatible devicetree! "
			"Use devicetree matching this kernel.\n");

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;

	dcp->fw_compat = fw_compat;
	dcp->dev = dev;
	dcp->hw = *(struct apple_dcp_hw_data *)of_device_get_match_data(dev);
	dcp->fixed_connector_type = dcp_connector_type_from_dt(dev->of_node);
	dcp->connector_type = dcp->fixed_connector_type;
	of_property_read_u32(dev->of_node, "apple,dcp-index", &dcp->index);
	of_property_read_u32(dev->of_node, "apple,dptx-phy", &dcp->dptx_phy);
	of_property_read_u32(dev->of_node, "apple,dptx-die", &dcp->dptx_die);
	dcp->fixed_dptx_phy = dcp->dptx_phy;
	INIT_DELAYED_WORK(&dcp->typec_reconnect_wq,
			  dcp_typec_reconnect_work);
	INIT_DELAYED_WORK(&dcp->typec_fabric_retrain_wq,
			  dcp_typec_retrain_work);

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

	if (num_surfs == 0 || num_surfs == -ENODATA) {
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

			ret = devm_request_threaded_irq(dev, dcp->hdmi_hpd_irq,
						NULL, dcp_dp2hdmi_hpd,
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
			ret = mux_control_select(dcp->xbar, mux_index);
			if (ret)
				dev_warn(dev, "mux_control_select failed: %d\n", ret);
			else
				dcp->fixed_route_selected = true;

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

	return component_add(&pdev->dev, &dcp_comp_ops);
}

static void dcp_platform_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dcp_comp_ops);
}

static void dcp_platform_shutdown(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dcp_comp_ops);
}

static int dcp_platform_suspend(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

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

	return 0;
}

static int dcp_platform_resume(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

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

static const struct apple_dcp_hw_data apple_dcp_hw_dcp = {
	.num_dptx_ports = 0,
};

static const struct apple_dcp_hw_data apple_dcp_hw_dcpext = {
	.num_dptx_ports = 2,
};

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,t6020-dcp", .data = &apple_dcp_hw_t6020,  },
	{ .compatible = "apple,t8112-dcp", .data = &apple_dcp_hw_t8112,  },
	{ .compatible = "apple,dcp",       .data = &apple_dcp_hw_dcp,    },
	{ .compatible = "apple,dcpext",    .data = &apple_dcp_hw_dcpext, },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
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

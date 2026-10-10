// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* "Copyright" 2021 Alyssa Rosenzweig */

#ifndef __APPLE_CONNECTOR_H__
#define __APPLE_CONNECTOR_H__

#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include "drm/drm_connector.h"
#include "drm/drm_edid.h"

struct neo_apple_connector;

#include "dcp-internal.h"

void neo_dcp_hotplug(struct work_struct *work);
void neo_dcp_retrain_oob(struct neo_apple_connector *connector);

/*
 * How many DCP pipelines may offer a route to one Type-C port.  The Type-C mux
 * class caps the mode-switch providers per connector (TYPEC_MUX_MAX_DEVS) and
 * the ATC PHY already claims one of those slots, so this is generous.
 */
struct neo_apple_connector {
	struct drm_connector base;
	bool connected;

	/* The pipeline currently driving this connector, NULL if unrouted. */
	struct platform_device *neo_dcp;

	/*
	 * A Type-C connector is a physical port rather than a fixed pipeline,
	 * and it has one encoder covering every pipeline that can drive it.
	 * One encoder per pipeline would describe the same hardware, but
	 * userspace takes the CRTCs a connector can use to be what all of its
	 * encoders have in common, and single-pipeline encoders have nothing
	 * in common.
	 */
	struct drm_encoder *port_encoder;

	/* the CRTCs of every pipeline that can drive the port */
	u32 candidate_crtcs;

	const struct drm_edid *drm_edid;

	/* Workqueue for sending hotplug events to the associated device */
	struct work_struct hotplug_wq;

	struct mutex chunk_lock;

	struct neo_dcp_chunks color_elements;
	struct neo_dcp_chunks timing_elements;
	struct neo_dcp_chunks display_attributes;
	struct neo_dcp_chunks transport;
};

#define to_apple_connector(x) container_of(x, struct neo_apple_connector, base)

void neo_apple_connector_debugfs_init(struct drm_connector *connector, struct dentry *root);

void neo_dcp_connector_update_dict(struct neo_apple_connector *connector, const char *key,
			       struct neo_dcp_chunks *chunks);
#endif

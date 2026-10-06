// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* "Copyright" 2021 Alyssa Rosenzweig */

#ifndef __APPLE_CONNECTOR_H__
#define __APPLE_CONNECTOR_H__

#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include "drm/drm_connector.h"
#include "drm/drm_edid.h"

struct apple_connector;

#include "dcp-internal.h"

void dcp_hotplug(struct work_struct *work);
void dcp_retrain_oob(struct apple_connector *connector);
enum drm_connector_status apple_connector_detect(struct drm_connector *connector, bool force);

void apple_connector_backlight_init(struct apple_connector *connector);
void apple_connector_backlight_sync(struct apple_connector *connector);
int apple_connector_backlight_late_register(struct apple_connector *connector);
void apple_connector_backlight_early_unregister(struct apple_connector *connector);

/*
 * How many DCP pipelines may offer a route to one Type-C port.  The Type-C mux
 * class caps the mode-switch providers per connector (TYPEC_MUX_MAX_DEVS) and
 * the ATC PHY already claims one of those slots, so this is generous.
 */
struct apple_connector {
	struct drm_connector base;
	bool connected;

	/* The pipeline currently driving this connector, NULL if unrouted. */
	struct platform_device *dcp;

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
	/* Serializes cache ownership and fetch invalidation across sink changes. */
	struct mutex edid_lock;
	u64 edid_generation;
	bool edid_live;

	/* Workqueue for sending hotplug events to the associated device */
	struct work_struct hotplug_wq;

	struct mutex chunk_lock;

	struct dcp_chunks color_elements;
	struct dcp_chunks timing_elements;
	struct dcp_chunks display_attributes;
	struct dcp_chunks transport;

	/*
	 * Backlight of an external display that DCP can drive, registered
	 * while such a display is connected.  bl_lock serialises
	 * registration against the connector's sysfs lifetime.
	 */
	struct mutex bl_lock;
	struct work_struct bl_sync_wq;	/* apple_connector_backlight_sync() */
	struct work_struct bl_commit_wq; /* sends a new level */
	struct backlight_device *bl_dev;
	bool bl_allowed;	/* between late_register and early_unregister */
	/*
	 * Kept across replug and applied on every power-on, once userspace has
	 * chosen one; -1 until then, and nothing is sent.
	 */
	int bl_level;
};

#define to_apple_connector(x) container_of(x, struct apple_connector, base)

void apple_connector_edid_init(struct apple_connector *connector);
void apple_connector_invalidate_edid(struct apple_connector *connector);
void apple_connector_edid_set_live(struct apple_connector *connector, bool live);
void apple_connector_set_pipeline(struct apple_connector *connector,
				  struct platform_device *pdev);
bool apple_connector_edid_begin(struct apple_connector *connector,
				struct platform_device *pdev, u64 *generation);
bool apple_connector_edid_install(struct apple_connector *connector,
				  struct platform_device *pdev, u64 generation,
				  const struct drm_edid *edid);
const struct drm_edid *apple_connector_edid_dup(struct apple_connector *connector,
						struct platform_device *pdev);

void apple_connector_debugfs_init(struct drm_connector *connector, struct dentry *root);

void dcp_connector_update_dict(struct apple_connector *connector, const char *key,
			       struct dcp_chunks *chunks);
#endif

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
bool dcp_connector_connected(struct apple_connector *connector);
void dcp_queue_hotplug(struct apple_connector *connector);
void dcp_route_failure_notify(struct apple_connector *connector);
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

	/* Last successfully sent static metadata, invalidated with the link. */
	spinlock_t hdr_lock;
	u64 hdr_generation;
	bool hdr_valid;
	u8 hdr_data[24];
	struct apple_dcp *hdr_dcp;
	u64 hdr_service_generation;

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
	bool drm_edid_firmware_derived;
	bool edid_invalidation_pending;
	/* The sink is live on this pipeline; a change invalidates its EDID. */
	bool edid_live;
	u64 drm_edid_generation;
	u64 drm_edid_service_epoch;

	/* Workqueue for sending hotplug events to the associated device */
	struct work_struct hotplug_wq;
	atomic_t hotplug_reasons;

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

u64 apple_connector_hdr_generation(struct apple_connector *connector);
void apple_connector_hdr_invalidate(struct apple_connector *connector);
void apple_connector_edid_init(struct apple_connector *connector);
void apple_connector_invalidate_edid(struct apple_connector *connector);
void apple_connector_edid_set_live(struct apple_connector *connector, bool live);
void apple_connector_set_pipeline(struct apple_connector *connector,
				  struct platform_device *pdev);

void apple_connector_debugfs_init(struct drm_connector *connector, struct dentry *root);

void dcp_connector_update_dict(struct apple_connector *connector, const char *key,
			       struct dcp_chunks *chunks);
#endif

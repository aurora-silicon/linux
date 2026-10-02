/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCPEXT_CONNECTOR_H
#define M3_DCPEXT_CONNECTOR_H

#include <linux/mutex.h>
#include <drm/drm_connector.h>
#include "m3_dcpext_modes.h"

struct drm_edid;
struct drm_encoder;

/* One native HDMI port, independent of the assigned DCPEXT controller.
 * The route owner serializes invalidate/publish against hardware commits.
 * Lock order: route owner -> mode_config.mutex -> lock. DRM callbacks take
 * only lock (mode_config.mutex is already held during probing).
 * No firmware operations or userspace notifications run with lock held.
 */
struct m3_dcpext_connector {
	struct drm_connector base;
	struct mutex lock;
	u64 generation;
	bool connected;
	const struct drm_edid *edid;
	u8 timings[64 * 24];
	u32 timings_count;
	struct m3_dcpext_native_mode *native_modes;
	u32 native_modes_count;
	u64 native_generation;
	struct drm_display_mode *modes;
	u32 modes_count;
};

int m3_dcpext_connector_init(struct drm_device *drm,
			   struct m3_dcpext_connector *connector,
			   struct drm_encoder *encoder, int connector_type);
/* Call for every HPD/route change, before dispatching new firmware queries.
 * Returns the generation to attach to all asynchronous query results.
 * Callers must not hold DRM locks. Hotplug notification is explicit below.
 */
u64 m3_dcpext_connector_invalidate(struct m3_dcpext_connector *connector,
				 bool connected);
/* Publish one coherent EDID + firmware timing snapshot. Stale replies cannot
 * repopulate a disconnected/reassigned port. Original EDID bytes are retained.
 */
int m3_dcpext_connector_publish(struct m3_dcpext_connector *connector,
			      u64 generation, const u8 *edid, u32 bytes,
			      const u8 *timings, u32 count);
int m3_dcpext_connector_publish_native(struct m3_dcpext_connector *connector,
				     u64 generation, const u8 *edid, u32 bytes,
				     const struct m3_dcpext_native_mode *modes,
				     u32 count, u64 native_generation);
/* Owner calls after releasing its route lock, with the DRM device registered. */
void m3_dcpext_connector_hotplug(struct m3_dcpext_connector *connector);
/* Recheck a checked atomic state's generation immediately before hardware
 * submission, under the route owner's lock. Returns a complete firmware record.
 * This does not implement scanout or imply presentation/retirement completion.
 */
int m3_dcpext_connector_commit_mode(struct m3_dcpext_connector *connector,
				  const struct drm_connector_state *state,
				  const struct drm_display_mode *mode,
				  struct m3_dcpext_mode *geometry,
				  u8 timing[24]);
int m3_dcpext_connector_commit_native(struct m3_dcpext_connector *connector,
				     const struct drm_connector_state *state,
				     const struct drm_display_mode *mode,
				     struct m3_dcpext_native_mode *selected,
				     u64 *native_generation);
#endif

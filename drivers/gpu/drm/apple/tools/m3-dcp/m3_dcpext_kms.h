/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCPEXT_KMS_H
#define M3_DCPEXT_KMS_H
#include "m3_dcpext_connector.h"
struct device;
struct iosys_map;
struct drm_framebuffer;
struct sg_table;
struct m3_dcpext_kms;

/* The route mutex serializes callbacks/hotplug and hardware commits. For direct scanout, fb/sgt describe pinned GEM pages; the owner retains
 * its own framebuffer reference while a DART slot maps them. Otherwise pixels
 * are CPU-readable until present returns and copied into retained slots. Success requires matching firmware completion.
 * Disable has mode/pixels NULL; an active mode with pixels NULL presents only
 * the background. Neither operation may free uncertain DMA resources. */
struct m3_dcpext_kms_ops {
	int (*present)(void *cookie, const struct m3_dcpext_native_mode *mode,
		       u64 generation, const struct iosys_map *pixels,
		       u32 source_pitch, bool opaque,
		       struct drm_framebuffer *fb, struct sg_table *sgt);
};
struct m3_dcpext_kms *m3_dcpext_kms_create(struct device *dev,
		const struct m3_dcpext_kms_ops *ops, void *cookie,
		struct mutex *route_lock, int connector_type);
struct m3_dcpext_connector *m3_dcpext_kms_connector(struct m3_dcpext_kms *kms);
int m3_dcpext_kms_register(struct m3_dcpext_kms *kms);
void m3_dcpext_kms_unplug(struct m3_dcpext_kms *kms);
#endif

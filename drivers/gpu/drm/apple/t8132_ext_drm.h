/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* J713: the adopted T8132 external DCPs as outputs of the DRM device. */
#ifndef __APPLE_T8132_EXT_DRM_H__
#define __APPLE_T8132_EXT_DRM_H__

#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct drm_connector;
struct drm_crtc;
struct drm_device;
struct drm_plane;
struct drm_atomic_commit;

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T8132)
int t8132_ext_drm_init(struct drm_device *drm);
/* The objects of these outputs are not apple_crtc/apple_connector/apple_plane. */
bool t8132_ext_crtc(const struct drm_crtc *crtc);
bool t8132_ext_connector(const struct drm_connector *connector);
bool t8132_ext_plane(const struct drm_plane *plane);
/* From the DCP: the sink of @dcpext changed (state or modes). Any context. */
void t8132_ext_drm_hotplug(struct device *dcpext);
#else
static inline int t8132_ext_drm_init(struct drm_device *drm) { return 0; }
static inline bool t8132_ext_crtc(const struct drm_crtc *crtc) { return false; }
static inline bool t8132_ext_connector(const struct drm_connector *connector) { return false; }
static inline bool t8132_ext_plane(const struct drm_plane *plane) { return false; }
static inline void t8132_ext_drm_hotplug(struct device *dcpext) { }
#endif

#endif

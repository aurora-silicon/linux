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
/* First in the DRM device's teardown: the PM notifier and the work stopped. */
void t8132_ext_drm_fini(struct drm_device *drm);
/* The objects of these outputs are not apple_crtc/apple_connector/apple_plane. */
bool t8132_ext_crtc(const struct drm_crtc *crtc);
bool t8132_ext_connector(const struct drm_connector *connector);
bool t8132_ext_plane(const struct drm_plane *plane);
/* From the DCP: the sink of @dcpext changed (state or modes). Any context. */
void t8132_ext_drm_hotplug(struct device *dcpext);
/* The swap of @dcpext's last direct frame is done (shown or lost). */
void t8132_ext_drm_flip_done(struct device *dcpext);
/* Whether @state has one of these CRTCs, and wait for their flips. */
bool t8132_ext_drm_in_commit(struct drm_atomic_commit *state);
void t8132_ext_drm_wait_flips(struct drm_atomic_commit *state);
#else
static inline int t8132_ext_drm_init(struct drm_device *drm) { return 0; }
static inline void t8132_ext_drm_fini(struct drm_device *drm) { }
static inline bool t8132_ext_crtc(const struct drm_crtc *crtc) { return false; }
static inline bool t8132_ext_connector(const struct drm_connector *connector) { return false; }
static inline bool t8132_ext_plane(const struct drm_plane *plane) { return false; }
static inline void t8132_ext_drm_hotplug(struct device *dcpext) { }
static inline void t8132_ext_drm_flip_done(struct device *dcpext) { }
static inline bool t8132_ext_drm_in_commit(struct drm_atomic_commit *state) { return false; }
static inline void t8132_ext_drm_wait_flips(struct drm_atomic_commit *state) { }
#endif

#endif

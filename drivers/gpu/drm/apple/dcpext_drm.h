/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCPEXT_DRM_H__
#define __APPLE_DCPEXT_DRM_H__
#include <linux/types.h>
#include <linux/atomic.h>
struct apple_dcp;
struct drm_device;
/* Caller retains the already-scanning buffer and serializes one registration. */
int dcpext_drm_register(struct apple_dcp *dcp, void *pixels, size_t size, u32 stride,
			atomic_t *terminal_error, struct drm_device **retained_drm);
#endif

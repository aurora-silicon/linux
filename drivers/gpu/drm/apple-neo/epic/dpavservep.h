// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#ifndef _DRM_APPLE_EPIC_DPAVSERV_H
#define _DRM_APPLE_EPIC_DPAVSERV_H

#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct drm_edid;
struct neo_apple_epic_service;
struct neo_apple_dcp;

struct dcpavserv {
	spinlock_t lock; /* Protects the service pointer and its owner pin. */
	bool enabled;
	struct completion enable_completion;
	u32 unit;
	struct neo_apple_epic_service *service;
};

const struct drm_edid *dcpavserv_copy_edid(struct neo_apple_dcp *neo_dcp);
void dpavservep_detach(struct neo_apple_dcp *neo_dcp);

#endif /* _DRM_APPLE_EPIC_DPAVSERV_H */

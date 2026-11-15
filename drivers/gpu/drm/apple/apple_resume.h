/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */

#ifndef __APPLE_RESUME_H__
#define __APPLE_RESUME_H__

#include <linux/types.h>

struct drm_atomic_commit;

u32 apple_drm_drop_stale_crtcs(struct drm_atomic_commit *state);

#endif /* __APPLE_RESUME_H__ */

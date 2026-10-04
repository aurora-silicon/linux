/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCP_KMS_H
#define M3_DCP_KMS_H
#include <linux/types.h>
struct m3_dcp_native;
struct drm_display_mode;
int m3_dcp_preferred_mode(const void *blob, u32 size, struct drm_display_mode *mode);
int m3_dcp_kms_register(struct m3_dcp_native *native, bool takeover);
#endif

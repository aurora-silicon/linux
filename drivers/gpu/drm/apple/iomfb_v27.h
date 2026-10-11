/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright The Asahi Linux Contributors */
/* Copyright 2026 Aurora Silicon */

#ifndef APPLE_IOMFB_V27_H
#define APPLE_IOMFB_V27_H

#include "iomfb_h17p.h"

struct dcp_swap_v27 {

	u64 timestamp[7];
	u64 unk_38;

	u64 debug_info[2];

	u64 event_wait[SWAP_SURFACES];
	u64 event_signal_on_glass;
	u64 event_signal[SWAP_SURFACES];

	u32 swap_id;

	u32 surf_ids[SWAP_SURFACES];
	struct dcp_rect src_rect[SWAP_SURFACES];
	u32 surf_flags[SWAP_SURFACES];
	u32 surf_unk[SWAP_SURFACES];
	struct dcp_rect dst_rect[SWAP_SURFACES];
	u32 swap_enabled;
	u32 swap_completed;

	u32 bg_color;
	u8 reserved_158[0x1fc];

	u8 brightness_update;
	u8 reserved_355[9];
	u64 brightness_nits;
	u8 reserved_366[0x37a];
} __packed;

struct dcp_surface_v27 {
	struct dcp_surface base;
	u8 padding[47];
} __packed;

struct dcp_swap_submit_req_v27 {
	struct dcp_swap_v27 swap;
	struct dcp_surface_v27 surf[SWAP_SURFACES];
	u64 surf_iova[SWAP_SURFACES];
	u64 unk_u64_a[SWAP_SURFACES];

	u8 unkbool;
	u64 unkdouble;
	u64 unkU64;
	u8 unkbool2;
	u32 clear;
	u32 unkU32Ptr;
	u8 swap_null;
	u8 surf_null[SWAP_SURFACES];
	u8 unkoutbool_null;
	u8 unknown_pointer_null;
	u8 unknown_output_null;
	u8 padding[2];
} __packed;

struct dcp_swap_submit_resp_v27 {
	u8 unkoutbool;
	u32 unkU32out;
	u32 ret;
	u8 padding[3];
} __packed;

static_assert(sizeof(struct dcp_swap_v27) == 0x6e0);
static_assert(sizeof(struct dcp_surface_v27) == 0x22c);
static_assert(sizeof(struct dcp_swap_submit_req_v27) == 0xff4);
static_assert(offsetof(struct dcp_swap_submit_req_v27, surf) == 0x6e0);
static_assert(offsetof(struct dcp_swap_submit_req_v27, surf_iova) == 0xf90);
static_assert(offsetof(struct dcp_swap_submit_req_v27, swap_null) == 0xfea);
static_assert(offsetof(struct dcp_swap_v27, brightness_update) == 0x354);
static_assert(offsetof(struct dcp_swap_v27, brightness_nits) == 0x35e);

void iomfb_serialize_present_v27(struct dcp_swap_submit_req_v27 *wire,
				const struct dcp_swap_submit_req_h17p *request);
bool iomfb_v27_callback_size_valid(unsigned int tag, u32 input, u32 output);
void iomfb_firmware_state_cleanup_v27(void *data);
int iomfb_modeset_v27(struct apple_dcp *dcp, struct drm_crtc_state *state);
void iomfb_flush_v27(struct apple_dcp *dcp, struct drm_crtc *crtc,
		     struct drm_atomic_state *state);
void iomfb_start_v27(struct apple_dcp *dcp);
void iomfb_shutdown_v27(struct apple_dcp *dcp);
void iomfb_poweron_v27(struct apple_dcp *dcp);
void iomfb_poweroff_v27(struct apple_dcp *dcp);
void iomfb_sleep_v27(struct apple_dcp *dcp);
void iomfb_present_backlight_v27(struct apple_dcp *dcp);

#endif

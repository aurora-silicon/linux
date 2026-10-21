/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

/*
 * DCP firmware 14.7.0 interface: the J514S (MacBook Pro 14-inch, M3 Pro,
 * T6030) internal panel, whose DCP the boot loader hands over running.
 *
 * The wire family is the 13.x one (surf2 block, 0x22c-byte surfaces, D589
 * swap completion), with the H17-style 16/8-byte swap_start, a 0x48-byte
 * gap before the swap ID and nits carried inside the swap.
 */

#ifndef __APPLE_IOMFB_V14_7_H__
#define __APPLE_IOMFB_V14_7_H__

#include <linux/sizes.h>
#include <linux/types.h>

#include "version_utils.h"

#define DCP_FW v14_7
#define DCP_FW_VER DCP_FW_VERSION(14, 7, 0)

#include "iomfb_template.h"

/* 14.7 wire layouts. */
/* A407 swap_submit: 0x1b58 bytes in, 12 bytes out (status at +5). */
static_assert(sizeof(struct dcp_swap_v14_7) == 0x508);
static_assert(offsetof(struct dcp_swap_v14_7, flags1) == 0x40);
static_assert(offsetof(struct dcp_swap_v14_7, swap_id) == 0x98);
static_assert(offsetof(struct dcp_swap_v14_7, surf_ids) == 0x9c);
static_assert(offsetof(struct dcp_swap_v14_7, src_rect) == 0xac);
static_assert(offsetof(struct dcp_swap_v14_7, surf_flags) == 0xec);
static_assert(offsetof(struct dcp_swap_v14_7, dst_rect) == 0x10c);
static_assert(offsetof(struct dcp_swap_v14_7, swap_enabled) == 0x14c);
static_assert(offsetof(struct dcp_swap_v14_7, swap_completed) == 0x150);
static_assert(offsetof(struct dcp_swap_v14_7, bg_color) == 0x154);
static_assert(offsetof(struct dcp_swap_v14_7, bl_nits_present) == 0x354);
static_assert(offsetof(struct dcp_swap_v14_7, bl_nits) == 0x35c);
static_assert(sizeof(struct dcp_surface_v14_7) == 0x22c);
static_assert(sizeof(struct dcp_swap_submit_req_v14_7) == 0x1b58);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, surf) == 0x508);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, surf_iova) == 0xdb8);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, unk_u64_a) == 0xdd8);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, surf2) == 0xdf8);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, surf2_iova) == 0x1b00);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, unkbool) == 0x1b30);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, clear) == 0x1b42);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, swap_null) == 0x1b4a);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, surf_null) == 0x1b4b);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, surf2_null) == 0x1b4f);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, unkoutbool_null) == 0x1b55);
static_assert(offsetof(struct dcp_swap_submit_req_v14_7, unknown_output_null) == 0x1b57);
static_assert(sizeof(struct dcp_swap_submit_resp_v14_7) == 0xc);
static_assert(offsetof(struct dcp_swap_submit_resp_v14_7, ret) == 5);
/* A406 swap_start: 16/8, scalar client handle and swap-ID null byte. */
static_assert(sizeof(struct dcp_swap_start_req_v14_7) == 0x10);
static_assert(offsetof(struct dcp_swap_start_req_v14_7, swap_id_null) == 12);
static_assert(sizeof(struct dcp_swap_start_resp_v14_7) == 0x8);
/* A472 set_power_state: 12/8, output-null byte at +11. */
static_assert(sizeof(struct dcp_set_power_state_req_v14_7) == 0xc);
static_assert(offsetof(struct dcp_set_power_state_req_v14_7, unkint_null) == 11);
/* D201 map_buf reply: AP address, DVA, status. */
static_assert(sizeof(struct dcp_map_buf_resp_v14_7) == 0x14);
/* D589 swap_complete_ap_gated: 0x6f0 bytes, swap ID first. */
static_assert(sizeof(struct dc_swap_complete_resp_v14_7) == 0x6f0);
static_assert(offsetof(struct dc_swap_complete_resp_v14_7, swap_id) == 0);
static_assert(offsetof(struct dc_swap_complete_resp_v14_7, swap_info_count) == 0x6e7);
static_assert(offsetof(struct dc_swap_complete_resp_v14_7, swap_data_null) == 0x6ec);

bool iomfb_check_callback_v14_7(int tag, u32 in_len, u32 out_len);

#undef DCP_FW_VER
#undef DCP_FW

struct apple_dcp;
struct dcp_property_store;

/* The J514S panel and the DCP's DMA window (DART SID5, 64 GiB at 1 TiB). */
#define J514S_PANEL_WIDTH	3024
#define J514S_PANEL_HEIGHT	1964
#define DCP_V14_7_DVA_BASE	SZ_1T
#define DCP_V14_7_DVA_SIZE	SZ_64G

/*
 * Firmware buffers of the adopted 14.7 session (D451/D201/D454): zeroed,
 * physically contiguous, mapped at the same DVA for the DCP (SID5) and
 * PIODMA (SID4), and retained until reboot.  IDs are 1-based and never
 * reused.
 */
#define DCP_V14_7_OBJECTS	64
#define DCP_V14_7_OBJECT_MAX	SZ_16M
#define DCP_V14_7_OBJECT_TOTAL	SZ_64M

struct dcp_v14_7_object {
	u64 phys, dva, size;
	bool mapped, retired;
};

/* Session state of a 14.7 DCP, owned by the IOMFB receive path. */
struct dcp_v14_7_session {
	struct dcp_property_store *props;

	struct dcp_v14_7_object object[DCP_V14_7_OBJECTS];
	u32 objects;
	u64 object_bytes;

	/* RTKit system buffers (borrowed and allocated), bounded like D451. */
	size_t rtkit_bytes;

	/* Stride of the inherited boot framebuffer, checked at admission. */
	u32 boot_stride;

	/* IOMFB READY word; startup progress. */
	u64 ready_word;
	bool opened;
	bool modes_ready;
	u32 callbacks;

	/* D300: id 19 is the logical nits (16.16), id 18 the correction. */
	u32 pr_nits;
	u32 pr_correction;
	bool pr_nits_valid;

	/* D576: flags of the last hotplug notification. */
	u64 hotplug_flags;
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_EXT)
	/* The external role's state (dcpext-t6030.c); NULL for the panel. */
	struct dcp_ext_v14_7 *ext;
#endif
};

int iomfb_v14_7_session_init(struct apple_dcp *dcp);
/* Latch a protocol failure of the adopted session; nothing is ACKed after. */
void iomfb_v14_7_fail(struct apple_dcp *dcp, const char *what);
/* Callbacks whose reply needs a nested AP call (a context-0 continuation). */
bool iomfb_v14_7_needs_continuation(int tag);
void iomfb_v14_7_try_activate(struct apple_dcp *dcp);
/* A472 hardware power of the internal panel's display controller. */
int iomfb_v14_7_panel_power(struct apple_dcp *dcp, bool on);

/*
 * The external role (DRM_APPLE_DCP_T6030_EXT): the client opens after the
 * first HPD reply, modes come from the sink's TimingElements, and mode and
 * power are A411 and A472 (iomfb_v14_7.c, external block).
 */
struct drm_crtc_state;
void iomfb_v14_7_ext_ready(struct apple_dcp *dcp);
bool iomfb_v14_7_ext_a444(struct apple_dcp *dcp, void *out);
int iomfb_v14_7_ext_open(struct apple_dcp *dcp, unsigned long timeout);
int iomfb_v14_7_ext_modeset(struct apple_dcp *dcp, struct drm_crtc_state *crtc_state);
int iomfb_v14_7_ext_power(struct apple_dcp *dcp, bool on);

#endif /* __APPLE_IOMFB_V14_7_H__ */

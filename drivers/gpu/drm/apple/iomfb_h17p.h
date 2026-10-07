/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

#ifndef __APPLE_IOMFB_H17P_H__
#define __APPLE_IOMFB_H17P_H__

#include "version_utils.h"

#define DCP_FW h17p
#define DCP_FW_VER DCP_FW_VERSION(26, 6, 0)

#include "iomfb_template.h"

static_assert(sizeof(struct dcp_swap_h17p) == 0x58a);
static_assert(sizeof(struct dcp_surface_h17p) == 0x22c);
static_assert(sizeof(struct dcp_swap_submit_req_h17p) == 0xe9c);
static_assert(sizeof(struct dcp_swap_submit_resp_h17p) == 0x0c);

/* Owned by the RTKit receive thread, independently of DRM state lifetime. */
struct dcp_present_state_h17p {
	u32 swap_id;
	bool pending;
	bool accepted;
};

static inline bool
dcp_present_begin_h17p(struct dcp_present_state_h17p *state, u32 swap_id)
{
	if (state->pending)
		return false;
	state->swap_id = swap_id;
	state->pending = true;
	state->accepted = false;
	return true;
}

static inline bool
dcp_present_submit_h17p(struct dcp_present_state_h17p *state, u32 swap_id,
			bool accepted)
{
	if (!state->pending || state->accepted || state->swap_id != swap_id)
		return false;
	state->accepted = accepted;
	if (!accepted)
		state->pending = false;
	return true;
}

static inline bool
dcp_present_complete_h17p(struct dcp_present_state_h17p *state, u32 swap_id)
{
	if (!state->pending || !state->accepted || state->swap_id != swap_id)
		return false;
	state->pending = false;
	state->accepted = false;
	return true;
}

/* The H17P swap boundary is independent of the template's native layout. */
struct dcp_present_h17p {
	u8 swap[0x588];
	struct dcp_surface_h17p surf[SWAP_SURFACES];
	u8 tail[0x64];
} __packed;

static_assert(sizeof(struct dcp_present_h17p) == 0xe9c);
static_assert(offsetof(struct dcp_present_h17p, surf) == 0x588);
static_assert(offsetof(struct dcp_present_h17p, tail) == 0xe38);

void iomfb_encode_backlight_h17p(struct dcp_present_h17p *wire, u32 nits,
				 u32 maximum, bool update);

void iomfb_serialize_present_h17p(struct dcp_present_h17p *wire,
				  const struct dcp_swap_submit_req_h17p *request);

#undef DCP_FW_VER
#undef DCP_FW

#endif /* __APPLE_IOMFB_H17P_H__ */

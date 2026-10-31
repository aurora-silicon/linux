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

/* Retired presents kept, with their scanouts, until a later one completes. */
#define IOMFB_DROPPED_MAX	8

/* Owned by the RTKit receive thread, independently of DRM state lifetime. */
struct dcp_present_state_h17p {
	u32 swap_id;
	bool pending;
	bool accepted;
	/* The firmware aborted the present before its submit reply. */
	bool aborted;
	/* The most recent aborted present, whose late completion is ignored. */
	bool has_last_aborted;
	u32 last_aborted;
	/*
	 * Presents retired without a completion or an abort, until the firmware
	 * completes a later one: a late completion of any of them is ignored.
	 */
	unsigned int nr_retired;
	u32 retired[IOMFB_DROPPED_MAX];
};

static inline bool
dcp_present_begin_h17p(struct dcp_present_state_h17p *state, u32 swap_id)
{
	if (state->pending)
		return false;
	state->swap_id = swap_id;
	state->pending = true;
	state->accepted = false;
	state->aborted = false;
	return true;
}

static inline bool
dcp_present_submit_h17p(struct dcp_present_state_h17p *state, u32 swap_id,
			bool accepted)
{
	if (!state->pending || state->accepted || state->swap_id != swap_id)
		return false;
	state->accepted = accepted;
	if (!accepted || state->aborted)
		state->pending = false;
	return true;
}

/*
 * An aborted present never completes.  Before its submit reply, the abort is
 * recorded and the reply ends the present instead.
 */
static inline bool
dcp_present_abort_h17p(struct dcp_present_state_h17p *state, u32 swap_id)
{
	if (!state->pending || state->swap_id != swap_id)
		return false;
	if (state->accepted) {
		state->pending = false;
		state->accepted = false;
	} else {
		state->aborted = true;
	}
	state->has_last_aborted = true;
	state->last_aborted = swap_id;
	return true;
}

/* Whether the firmware aborted @swap_id; a completion for it is stale. */
static inline bool
dcp_present_was_aborted_h17p(const struct dcp_present_state_h17p *state,
			     u32 swap_id)
{
	return state->has_last_aborted && state->last_aborted == swap_id;
}

/*
 * The swap watchdog retires an accepted present that neither completed nor
 * was aborted in time.  Nothing from the firmware ended it, so unlike an
 * aborted one it may still complete: it is kept until a later present
 * completes.  Without room for it, the present stays in flight.
 */
static inline bool
dcp_present_retire_h17p(struct dcp_present_state_h17p *state, u32 swap_id)
{
	if (!state->pending || !state->accepted || state->swap_id != swap_id ||
	    state->nr_retired >= IOMFB_DROPPED_MAX)
		return false;
	state->pending = false;
	state->accepted = false;
	state->retired[state->nr_retired++] = swap_id;
	return true;
}

/* Whether @swap_id was retired; a completion for it is late, not unexpected. */
static inline bool
dcp_present_was_retired_h17p(const struct dcp_present_state_h17p *state,
			     u32 swap_id)
{
	unsigned int i;

	for (i = 0; i < state->nr_retired; i++)
		if (state->retired[i] == swap_id)
			return true;
	return false;
}

static inline bool
dcp_present_complete_h17p(struct dcp_present_state_h17p *state, u32 swap_id)
{
	if (!state->pending || !state->accepted || state->swap_id != swap_id)
		return false;
	state->pending = false;
	state->accepted = false;
	/* The firmware is past every present before this one. */
	state->nr_retired = 0;
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

struct dcp_apply_property_h17p {
	u32 property;
	u32 value;
} __packed;

static_assert(sizeof(struct dcp_apply_property_h17p) == 0x8);

static inline struct dcp_apply_property_h17p dcp_opaque_x_property_h17p(void)
{
	return (struct dcp_apply_property_h17p) {
		.property = 0x49,
		.value = 0,
	};
}

void iomfb_blank_h17p(struct apple_dcp *dcp);
void iomfb_encode_backlight_swap_h17p(u8 *swap, u32 nits, u32 maximum,
				      bool update);
void iomfb_encode_backlight_h17p(struct dcp_present_h17p *wire, u32 nits,
				 u32 maximum, bool update);

void iomfb_serialize_present_h17p(struct dcp_present_h17p *wire,
				  const struct dcp_swap_submit_req_h17p *request);

#if IS_ENABLED(CONFIG_DRM_APPLE_KUNIT_TEST)
int iomfb_h17p_first_unbounded_callback(void);
int iomfb_h17p_deliver_swap_complete(struct apple_dcp *dcp, u32 swap_id);
#endif

#undef DCP_FW_VER
#undef DCP_FW

#endif /* __APPLE_IOMFB_H17P_H__ */

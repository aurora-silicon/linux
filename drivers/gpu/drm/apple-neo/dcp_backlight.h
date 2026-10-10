/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */

#ifndef __APPLE_DCP_BACKLIGHT_H__
#define __APPLE_DCP_BACKLIGHT_H__

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/types.h>

/* Commanded nits, independent of firmware encoding. No fade policy. */
struct neo_dcp_backlight_state {
	u32 maximum;
	u32 target;
	u32 actual;
	u32 sent_nits;
	u64 sequence;
	u8 retries;
	bool ready;
	bool controlled;
	bool dpms_off;
	bool core_blank;
	bool suspended;
	bool dirty;
	bool in_flight;
	/*
	 * The panel shows a level Linux knows: the loader's reported level, or
	 * one a completed present carried.  A default used for registration
	 * without a loader report is not known until a present carries a level.
	 */
	bool level_known;
};

struct neo_dcp_backlight_present {
	u64 sequence;
	u32 nits;
};

/*
 * Public powerlog reports use millinits.  A report slightly above the panel
 * ceiling describes the brightest level the panel can show, so clamp it; one
 * more than twice the ceiling is not a credible panel level.
 */
static inline int neo_dcp_bl_takeover_nits(u32 maximum, u32 millinits, u32 *nits)
{
	if (!maximum || maximum > INT_MAX / 2000)
		return -EINVAL;
	if (millinits / 1000 > 2 * maximum)
		return -ERANGE;
	*nits = min(millinits / 1000, maximum);
	return 0;
}

/*
 * An inherited level comes from a loader report.  A default level is only
 * reported to userspace; it is never presented on its own (see level_known).
 */
static inline int neo_dcp_bl_init(struct neo_dcp_backlight_state *state, u32 maximum,
			      bool inherited_valid, u32 inherited,
			      bool default_valid, u32 default_nits)
{
	u32 nits;

	if (!maximum || maximum > INT_MAX)
		return -EINVAL;
	if (!inherited_valid && !default_valid)
		return -ENODATA;
	nits = inherited_valid ? inherited : default_nits;
	if (nits > maximum)
		return -ERANGE;

	*state = (struct neo_dcp_backlight_state) {
		.maximum = maximum,
		.target = nits,
		.actual = nits,
		.ready = true,
		.level_known = inherited_valid,
	};
	return 0;
}

/*
 * A decoded powerlog sample can seed takeover only before Linux control.
 * It reports the level the panel shows, so the level is then known.
 */
static inline bool neo_dcp_bl_seed(struct neo_dcp_backlight_state *state, u32 nits)
{
	if (!state->ready || state->controlled || nits > state->maximum)
		return false;
	state->target = nits;
	state->actual = nits;
	state->level_known = true;
	return true;
}

static inline u32 neo_dcp_bl_effective(const struct neo_dcp_backlight_state *state)
{
	if (state->dpms_off || state->core_blank || state->suspended)
		return 0;
	return state->target;
}

static inline int neo_dcp_bl_request(struct neo_dcp_backlight_state *state, u32 nits,
				 bool core_blank, bool suspended)
{
	if (!state->ready)
		return -ENODATA;
	if (nits > state->maximum)
		return -ERANGE;
	state->controlled = true;
	state->retries = 0;
	/*
	 * Until a present carries a level, the panel does not show the target,
	 * so even a request for the reported default has to be presented.
	 */
	if (state->target != nits || state->core_blank != core_blank ||
	    state->suspended != suspended || !state->level_known) {
		state->target = nits;
		state->core_blank = core_blank;
		state->suspended = suspended;
		state->dirty = true;
	}
	return 0;
}

static inline int neo_dcp_bl_dpms(struct neo_dcp_backlight_state *state, bool on)
{
	if (!state->ready)
		return -ENODATA;
	state->controlled = true;
	state->retries = 0;
	if (state->dpms_off != !on) {
		state->dpms_off = !on;
		state->dirty = true;
	}
	return 0;
}

/*
 * The firmware asked for the level again.  Re-send the effective level once
 * Linux controls it and knows it, unless a present already carries or will
 * carry it.  A default level is never sent this way.
 */
static inline bool neo_dcp_bl_resend(struct neo_dcp_backlight_state *state)
{
	if (!state->ready || !state->controlled || !state->level_known ||
	    state->dirty || state->in_flight)
		return false;
	state->dirty = true;
	return true;
}

/*
 * Called by the single outbound queue, with a pinned, accepted scanout or a
 * prepared replacement. Soft-off never releases it. A brightness-only present
 * must use the last accepted surface, not a rejected replacement's DRM state.
 */
static inline int neo_dcp_bl_prepare(struct neo_dcp_backlight_state *state,
				 bool have_surface,
				 struct neo_dcp_backlight_present *present)
{
	if (!state->ready || !have_surface)
		return -ENODATA;
	if (state->in_flight)
		return -EBUSY;
	if (!state->dirty)
		return -EALREADY;

	state->controlled = true;
	state->in_flight = true;
	state->dirty = false;
	state->sent_nits = neo_dcp_bl_effective(state);
	present->nits = state->sent_nits;
	present->sequence = ++state->sequence;
	return 0;
}

/*
 * accepted means both accepted submit and its matching completed present.
 * Rejection/serialization failure cancels the reservation without changing
 * actual. This result is NEVER a framebuffer retirement authorization.
 */
static inline bool neo_dcp_bl_complete(struct neo_dcp_backlight_state *state,
				   u64 sequence, bool accepted)
{
	if (!state->in_flight || sequence != state->sequence)
		return false;
	state->in_flight = false;
	if (accepted) {
		state->actual = state->sent_nits;
		state->retries = 0;
		state->level_known = true;
	} else {
		state->dirty = true;
		if (state->retries < 4)
			state->retries++;
	}
	return true;
}

/* Three deferred retries per user request; a permanent rejection stays dirty. */
static inline unsigned int neo_dcp_bl_retry_delay(const struct neo_dcp_backlight_state *state)
{
	if (!state->ready || !state->dirty || state->in_flight ||
	    !state->retries || state->retries > 3)
		return 0;
	return 100U << (state->retries - 1);
}

struct neo_apple_dcp;

/*
 * kick only queues work; it must not submit a top-level RPC from the caller.
 * The central IOMFB queue owns surface references, retries, and wire encoding.
 * Configure before registration and keep the callback valid until teardown.
 */
bool neo_dcp_backlight_active(struct neo_apple_dcp *neo_dcp);
int neo_dcp_backlight_takeover(struct neo_apple_dcp *neo_dcp, u32 millinits);
int neo_dcp_backlight_configure(struct neo_apple_dcp *neo_dcp, u32 maximum,
			    bool inherited_valid, u32 inherited,
			    bool default_valid, u32 default_nits,
			    void (*kick)(struct neo_apple_dcp *neo_dcp));
bool neo_dcp_backlight_seed(struct neo_apple_dcp *neo_dcp, u32 nits);
int neo_dcp_backlight_dpms(struct neo_apple_dcp *neo_dcp, bool on);
int neo_dcp_backlight_prepare(struct neo_apple_dcp *neo_dcp, bool have_surface,
			  struct neo_dcp_backlight_present *present);
bool neo_dcp_backlight_complete(struct neo_apple_dcp *neo_dcp, u64 sequence, bool accepted);
bool neo_dcp_backlight_pending(struct neo_apple_dcp *neo_dcp);
bool neo_dcp_backlight_resend(struct neo_apple_dcp *neo_dcp);
unsigned int neo_dcp_backlight_retry_delay(struct neo_apple_dcp *neo_dcp);

#endif /* __APPLE_DCP_BACKLIGHT_H__ */

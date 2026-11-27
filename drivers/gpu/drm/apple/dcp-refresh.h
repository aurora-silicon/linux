/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */
#ifndef __APPLE_DCP_REFRESH_H__
#define __APPLE_DCP_REFRESH_H__

#include <linux/types.h>

/* Accept nominal 120 Hz and its 1000/1001 fractional timing. */
static inline bool dcp_refresh_is_promotion(u32 rate)
{
	return rate >= (120U << 16) - (120U << 16) / 500 &&
		rate <= (120U << 16) + (120U << 16) / 500;
}

/* All rates are unsigned 16.16 Hz, including fractional fixed timings. */
static inline bool dcp_refresh_range(u32 rate, u32 minimum, u32 maximum,
				     bool internal, u32 *low, u32 *high)
{
	if (!minimum && !maximum && internal && dcp_refresh_is_promotion(rate)) {
		minimum = 24U << 16;
		maximum = rate;
	}
	/* A partial, inverted or out-of-mode range leaves a fixed timing. */
	if (minimum < (1U << 16) || minimum >= maximum ||
	    maximum > rate || maximum > (65535U << 16))
		return false;
	*low = minimum;
	*high = maximum;
	return true;
}

/*
 * Whether swaps of a mode carry a presentation deadline: VRR modes and an
 * internal ProMotion 120 Hz timing do. A panel that scans out on its 120 Hz
 * grid at every timing (every_internal) needs it for all of its modes: a
 * swap without one completes only at the second vblank, so a fixed 48 or
 * 50 Hz timing would run at 60 Hz.
 */
static inline bool dcp_refresh_needs_deadline(u32 rate, bool vrr, bool internal,
					      bool every_internal)
{
	return vrr || (internal && (every_internal || dcp_refresh_is_promotion(rate)));
}

/*
 * The adaptive sync minimum rate (16.16 Hz) of a VRR timing on a panel that
 * takes VRR_ENABLED on its live mode: @on, the mode's own minimum, lets the
 * panel follow the frames; @off, the nominal rate, holds it fixed; @sent is
 * the one the firmware was last given.  @on is zero while adaptive sync was
 * not armed that way.
 */
struct dcp_vrr_floor {
	u32 on, off, sent;
};

/*
 * A mode set armed adaptive sync at the timing's @minimum.  Fixed refresh
 * holds the minimum at the timing's @nominal rate, never above its @maximum
 * (a 280.001 Hz timing reports a maximum just over 280 Hz).
 */
static inline void dcp_vrr_floor_arm(struct dcp_vrr_floor *floor, u32 minimum,
				     u32 maximum, u32 nominal)
{
	floor->on = minimum;
	floor->off = maximum && maximum < nominal ? maximum : nominal;
	floor->sent = minimum;
}

/*
 * The minimum VRR_ENABLED (@enabled) asks for, if the firmware has not been
 * given it yet: then true, with it in @minimum, and recorded as sent.
 */
static inline bool dcp_vrr_floor_next(struct dcp_vrr_floor *floor, bool enabled,
				      u32 *minimum)
{
	u32 next = enabled ? floor->on : floor->off;

	if (!floor->on || next == floor->sent)
		return false;
	floor->sent = next;
	*minimum = next;
	return true;
}

/*
 * Desktop 26.6 uses its own presentation clock. A deadline long past asks
 * for the next permitted refresh. ARM counter values can stall the swap.
 */
static inline u64 dcp_desktop_present_deadline(bool have_surface, bool required)
{
	return have_surface && required ? 120 : 0;
}

#endif

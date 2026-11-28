/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_PEER_FRAMES_V27_H
#define APPLE_DCP_PEER_FRAMES_V27_H

#include <linux/types.h>

/* Protected by dcp_peer_lock. Sequence comparisons cover less than 2^31
 * outstanding swaps, as in the existing single-pipe retirement path.
 */
struct dcp_peer_frames_v27 {
	u32 timing;
	u32 color;
	u32 min_vrr;
	bool output_hdr;
	u32 first, submitted, engine[2], retired, shown[2];
	bool active, armed, failed, have[2], have_retired, have_shown[2];
};

static inline bool dcp_peer_vrr_only_v27(const struct dcp_peer_frames_v27 *frames,
					 u32 timing, u32 color, u32 min_vrr,
					 bool output_hdr)
{
	return frames->active && !frames->failed && frames->timing == timing &&
	       frames->color == color && frames->min_vrr != min_vrr &&
	       frames->output_hdr == output_hdr;
}

static inline void dcp_peer_frame_submit_v27(struct dcp_peer_frames_v27 *s, u32 id)
{
	if (!s->armed) {
		s->first = id;
		s->armed = true;
	}
	s->submitted = id;
}

/*
 * *shown: both engines' firmware reported the retired swap itself as
 * displayed, from completions of that swap ID.
 */
static inline bool dcp_peer_frame_complete_v27(struct dcp_peer_frames_v27 *s,
					      unsigned int engine, u32 id,
					      bool shown_now, u32 *watermark,
					      bool *shown)
{
	u32 done;

	if (!s->active || s->failed || !s->armed || engine > 1 ||
	    (s32)(id - s->first) < 0 || (s32)(s->submitted - id) < 0 ||
	    (s->have[engine] && (s32)(id - s->engine[engine]) <= 0))
		return false;
	s->engine[engine] = id;
	s->have[engine] = true;
	if (shown_now) {
		s->shown[engine] = id;
		s->have_shown[engine] = true;
	}
	if (!s->have[0] || !s->have[1])
		return false;
	done = (s32)(s->engine[0] - s->engine[1]) < 0 ? s->engine[0] : s->engine[1];
	if (s->have_retired && (s32)(done - s->retired) <= 0)
		return false;
	s->retired = *watermark = done;
	s->have_retired = true;
	*shown = s->have_shown[0] && s->shown[0] == done &&
		 s->have_shown[1] && s->shown[1] == done;
	return true;
}
#endif

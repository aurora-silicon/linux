/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef DCP_SWAP_PACE_H
#define DCP_SWAP_PACE_H

#include <linux/minmax.h>
#include <linux/types.h>

#include "dcp-refresh.h"

/* Values of the swap_ts_mode module parameter. */
enum dcp_swap_ts_mode {
	/* Clock values: now, and the previous swap (or now after idle). */
	DCP_SWAP_TS_CURRENT,
	/* The desktop 26.6 constant in all three, as the M3 and M4 paths send. */
	DCP_SWAP_TS_DESKTOP,
	/* As current, but one 120 Hz frame before now for a swap after idle. */
	DCP_SWAP_TS_LONE_FRAME,
	/* No timestamps. */
	DCP_SWAP_TS_OFF,
};

/* Types 1, 2 and 7 of IOMobileFramebuffer: swap.timestamp[0], [1] and [6]. */
struct dcp_swap_ts {
	u64 ts0;
	u64 ts1;
	u64 ts6;
};

/*
 * prev is the previous accepted swap's time, or now when there is none or it
 * is stale (dcp_swap_prev_time()). A swap whose prev is now follows idle.
 */
static inline struct dcp_swap_ts dcp_swap_ts_pick(unsigned int mode, u64 now,
						  u64 prev, u64 frame_ticks)
{
	struct dcp_swap_ts ts = { .ts0 = now, .ts1 = prev, .ts6 = now };

	switch (mode) {
	case DCP_SWAP_TS_DESKTOP:
		ts.ts0 = ts.ts1 = ts.ts6 = dcp_desktop_present_deadline(true, true);
		break;
	case DCP_SWAP_TS_LONE_FRAME:
		if (prev == now)
			ts.ts1 = now > frame_ticks ? now - frame_ticks : 0;
		break;
	case DCP_SWAP_TS_OFF:
		ts = (struct dcp_swap_ts) {};
		break;
	}
	return ts;
}

#define DCP_KEEPALIVE_MAX_MS 1000

/* The keepalive_ms module parameter as a delay: 0 is off, large values are bounded. */
static inline unsigned int dcp_keepalive_delay_ms(unsigned int param)
{
	return min_t(unsigned int, param, DCP_KEEPALIVE_MAX_MS);
}

#endif

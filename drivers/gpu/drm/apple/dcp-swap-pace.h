/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef DCP_SWAP_PACE_H
#define DCP_SWAP_PACE_H

#include <linux/minmax.h>
#include <linux/time64.h>
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
	/*
	 * The four types macOS 26.6.2 sets (QuartzCore finish_update): a target
	 * of max(target - frame/8, update start + 100 us), the compose start,
	 * the update start and the submit time. The commit time stands in for
	 * update and compose start, and the submit time for the target.
	 */
	DCP_SWAP_TS_MACOS,
};

/* Types 1, 2, 4, 5 and 7 of IOMobileFramebuffer: swap.timestamp[0], [1], [3], [4] and [6]. */
struct dcp_swap_ts {
	u64 ts0;
	u64 ts1;
	u64 ts3;
	u64 ts4;
	u64 ts6;
};

/*
 * prev is the previous accepted swap's time, or now when there is none or it
 * is stale (dcp_swap_prev_time()). A swap whose prev is now follows idle.
 * start is when the driver got the commit; mode 4 clamps it to now. hz is
 * the timebase of all of them.
 */
/*
 * swap_target_lead_us, in firmware ticks at hz: added to every clock target
 * the pick sets. 0 keeps the old past targets; a lead moves the target into
 * the future the way macOS does.
 */
static inline u64 dcp_swap_lead_ticks(unsigned int lead_us, u64 hz)
{
	return div_u64(lead_us * hz, 1000000U);
}

static inline struct dcp_swap_ts dcp_swap_ts_pick(unsigned int mode, u64 now,
						  u64 prev, u64 start, u64 hz,
						  unsigned int lead_us)
{
	struct dcp_swap_ts ts = { .ts0 = now, .ts1 = prev, .ts6 = now };
	u64 frame_ticks = hz / 120;
	u64 lead = dcp_swap_lead_ticks(lead_us, hz);

	ts.ts0 += lead;
	ts.ts6 += lead;

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
	case DCP_SWAP_TS_MACOS:
		start = min(start, now);
		ts = (struct dcp_swap_ts) {
			.ts0 = max(now > frame_ticks / 8 ? now - frame_ticks / 8 : 0,
				   start + hz / 10000) + lead,
			.ts1 = start,
			.ts3 = start,
			.ts4 = now,
		};
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

/*
 * An idle M5 internal panel scans out on a grid of about 100 ms. A swap that
 * reaches the firmware 3-8 ms before the next grid frame gets no frame of its
 * own (receipts/2026-11-14-m5-lone-print-cal-ab.md: lost leads 3.1-7.5 ms,
 * shown leads up to 3.5 ms and from 7.2 ms). The hold runs before the swap is
 * stamped, up to 0.6 ms ahead of the submit, so the band ends at 8.5 ms.
 */
#define DCP_BAND_FRAMES 4
#define DCP_BAND_IDLE_MIN_NS (90 * NSEC_PER_MSEC)
#define DCP_BAND_IDLE_MAX_NS (110 * NSEC_PER_MSEC)
#define DCP_BAND_LEAD_MIN_NS (3 * NSEC_PER_MSEC)
#define DCP_BAND_LEAD_MAX_NS (8 * NSEC_PER_MSEC + NSEC_PER_MSEC / 2)
#define DCP_BAND_MARGIN_NS (1 * NSEC_PER_MSEC)

/*
 * frames[] are the last frame times, oldest first, 0 when unknown. Returns how
 * long a swap at now waits so it reaches the firmware after the grid frame.
 */
static inline u64 dcp_band_hold_ns(const u64 *frames, u64 now)
{
	u64 iv[DCP_BAND_FRAMES - 1], period, last = frames[DCP_BAND_FRAMES - 1], lead;
	int i;

	for (i = 0; i < DCP_BAND_FRAMES - 1; i++) {
		if (!frames[i] || frames[i + 1] <= frames[i])
			return 0;
		iv[i] = frames[i + 1] - frames[i];
		if (iv[i] < DCP_BAND_IDLE_MIN_NS || iv[i] > DCP_BAND_IDLE_MAX_NS)
			return 0;
	}
	period = max(min(iv[0], iv[1]), min(max(iv[0], iv[1]), iv[2]));
	if (now < last || now - last >= period)
		return 0;
	lead = period - (now - last);
	if (lead < DCP_BAND_LEAD_MIN_NS || lead >= DCP_BAND_LEAD_MAX_NS)
		return 0;
	return lead + DCP_BAND_MARGIN_NS;
}

/* Boot values of the parameters. A change here flips the default; the parameters stay as the runtime rollback. */
#define DCP_SWAP_TS_MODE_DEFAULT DCP_SWAP_TS_MACOS
#define DCP_KEEPALIVE_MS_DEFAULT 25
#define DCP_SWAP_BAND_HOLD_DEFAULT true

#endif

/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ANE_JOB_GAP_H_
#define _ANE_JOB_GAP_H_
#include <linux/types.h>

/* Preserve legacy one-shot clamping; periodic mode is explicitly bounded. */
static inline bool ane_job_gap_valid(u32 us, u32 at, u32 every,
                                    bool pending, bool ack_pending, bool quiet)
{
	return !every || (us && us <= 2000 && every <= 128 && !at &&
			 !pending && !ack_pending && !quiet);
}

/* ordinal1 is the second execution of the warmed identity, before publish. */
static inline bool ane_job_gap_selected(u32 ordinal, u32 at, u32 every)
{
	if (!ordinal)
		return false;
	if (every)
		return !(ordinal % every);
	return !at || ordinal == (at > 128 ? 128 : at);
}
/* Private drain-and-hold discriminator: only every-call busy hold is allowed. */
static inline bool ane_job_drain_valid(u32 drain, u32 hold, u32 at, u32 every)
{
	return !drain || (drain <= 2000 && hold && hold <= 100 && !at && every == 1);
}

static inline u32 ane_job_gap_duration(u32 ordinal, u32 hold, u32 drain)
{
	return ordinal == 1 && drain ? drain : (hold > 2000 ? 2000 : hold);
}
#endif

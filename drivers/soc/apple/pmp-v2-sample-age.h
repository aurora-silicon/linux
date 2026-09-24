/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_PMP_V2_SAMPLE_AGE_H
#define APPLE_PMP_V2_SAMPLE_AGE_H
#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/overflow.h>

/* Maximum age when observed, not a continuously renewed frequency lease. */
#define PMP_SAMPLE_MAX_AGE_NS 5000000ULL

static inline int pmp_sample_age(u64 counter, u64 timestamp, u64 *age)
{
	u64 scaled;

	/* AP1GHz/PTD24MHz candidate fixed for the admitted boot; no fitted offset. */
	if (check_mul_overflow(timestamp, 125ULL, &scaled))
		return -ERANGE;
	scaled = div_u64(scaled, 3);
	if (scaled > counter || counter - scaled >= PMP_SAMPLE_MAX_AGE_NS)
		return -ETIME;
	*age = counter - scaled;
	return 0;
}

#endif

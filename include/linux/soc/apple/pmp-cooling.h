/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_COOLING_H
#define _LINUX_SOC_APPLE_PMP_COOLING_H

#include <linux/types.h>

/* J713 development backstop, shared by CPU QoS and the G16 firmware ceiling.
 * The thermal governor runs separately. Inputs are validated package mC and
 * monotonic ns. Sampling more often must never accelerate prediction/release.
 * None of these thresholds are claimed as Apple's hardware safety limits.
 */
/* Existing provisional J713 development policy. macOS reached
 * 115.25 C normally; that observation does not identify its emergency trip.
 * Begin ordinary regulation below the target and strengthen the independent
 * cooling envelope near it. Emergency shutdown is a separate, provisional
 * policy in pmp-emergency-policy.h; none of these values establishes it.
 */
#define APPLE_PMP_PASSIVE_MC 106000
#define APPLE_PMP_TARGET_MC 110000
#define APPLE_PMP_BACKSTOP_MC 108000
#define APPLE_PMP_FULL_COOLING_MC 115000
#define APPLE_PMP_RELEASE_MC 106000
/*
 * The firmware publishes a die sensor only while its cluster has power, so an idle
 * machine can have none valid for a moment (0.46 s the longest measured). Go on from the
 * last reading for this long, if it was this cool. Longer or hotter is a failed sensor.
 */
#define APPLE_PMP_GATED_HOLD_NS 2000000000ULL
#define APPLE_PMP_GATED_HOLD_MC 90000
/* Below this hotspot, with no fault, recovery or cooling in force, nothing
 * acts until prediction starts at 105 C, so the slow cadence is enough.
 */
#define APPLE_PMP_FAST_POLL_MC 95000

struct apple_pmp_cooling {
	unsigned long long last_ns, good_ns, anchor_ns, release_ns;
	unsigned long long dt_ns;
	long anchor_mc, previous_mc, projected_mc;
	unsigned int good_reads, prediction, level;
	bool hard, anchor_valid;
	int reason; /* 0 startup, 1 sensor, 2 measured heat, 3 prediction, 4 governor, 5 recovery */
};

static inline void apple_pmp_cooling_init(struct apple_pmp_cooling *s)
{
	*s = (struct apple_pmp_cooling) { .level = 8, .hard = true };
}

static inline unsigned int apple_pmp_cooling_level(const struct apple_pmp_cooling *s)
{
	return s->level;
}

/* Sample at the fast cadence whenever a decision could depend on it. */
static inline bool apple_pmp_cooling_fast(const struct apple_pmp_cooling *s,
					  long mc, int error)
{
	return error || s->hard || s->level || mc >= APPLE_PMP_FAST_POLL_MC;
}

/* Retain a governor restriction in the release envelope. Otherwise a trip
 * crossing could drop the governor from maximum cooling to zero in one tick,
 * bypassing the backstop's gradual recovery entirely.
 */
static inline void apple_pmp_cooling_observe(struct apple_pmp_cooling *s,
		unsigned long long now, unsigned int level)
{
	if (level > 8)
		level = 8;
	if (level > s->level) {
		s->level = level;
		s->release_ns = now;
		s->reason = 4;
	}
}

/* A measured rise uses intermediate states: 108 C -> 1, 112 C -> 5, 115 C -> 8.
 * This remains effective even before the thermal governor has run.
 */
static inline unsigned int apple_pmp_heat_level(long mc)
{
	return mc < APPLE_PMP_BACKSTOP_MC ? 0 :
		mc >= APPLE_PMP_FULL_COOLING_MC ? 8 :
		(mc - APPLE_PMP_BACKSTOP_MC) / 1000 + 1;
}

static inline void apple_pmp_cooling_sample(struct apple_pmp_cooling *s,
		unsigned long long now, long mc, int error)
{
	unsigned long long dt = now - s->anchor_ns;
	unsigned int target;
	bool trend_due = false;

	s->last_ns = now;
	if (error || mc >= APPLE_PMP_FULL_COOLING_MC) {
		s->level = 8;
		s->hard = true;
		s->reason = error ? 1 : 2;
		s->good_reads = 0;
		s->good_ns = now;
		s->release_ns = now;
		s->anchor_valid = false;
		s->prediction = 0;
		s->dt_ns = 0;
		s->projected_mc = error ? 0 : mc;
		return;
	}

	if (s->hard) {
		/* Startup/fault recovery needs 200 ms of cool, valid observations.
		 * Repeated sysfs or submission reads cannot count faster than time.
		 */
		if (mc < APPLE_PMP_BACKSTOP_MC) {
			if (now - s->good_ns >= 20000000ULL) {
				s->good_ns = now;
				if (s->good_reads < 10)
					s->good_reads++;
			}
			if (s->good_reads == 10)
				s->hard = false;
		} else {
			s->good_reads = 0;
			s->good_ns = now;
		}
		/* Exit protection gradually, not minimum -> maximum in one call. */
		s->release_ns = now;
		if (s->hard)
			return;
	}

	if (!s->anchor_valid || dt >= 250000000ULL) {
		/* No slope across idle gaps, faults or power-gated observations. */
		s->anchor_valid = true;
		s->anchor_ns = now;
		s->anchor_mc = mc;
		s->previous_mc = mc;
		s->dt_ns = 0;
		s->projected_mc = mc;
		s->prediction = 0;
	} else if (dt >= 50000000ULL) {
		long rise = 0;

		s->previous_mc = s->anchor_mc;
		s->dt_ns = dt;
		if (mc > s->anchor_mc)
			rise = (mc - s->anchor_mc) * 100000000LL / dt;
		/* A short excursion is not a measured 170 C die. Forecasts only
		 * request a gradual step, with at most 2 C of anticipatory headroom.
		 */
		if (rise > 2000)
			rise = 2000;
		s->projected_mc = mc + rise;
		s->prediction = mc >= APPLE_PMP_BACKSTOP_MC - 3000 ? apple_pmp_heat_level(s->projected_mc) : 0;
		s->anchor_ns = now;
		s->anchor_mc = mc;
		trend_due = true;
	}

	target = apple_pmp_heat_level(mc);
	if (target > s->level) {
		s->level = target;
		s->reason = 2;
		s->release_ns = now;
	} else if (trend_due && s->prediction > s->level) {
		s->level++;
		s->reason = 3;
		s->release_ns = now;
	} else if (mc < APPLE_PMP_RELEASE_MC && s->prediction == 0 &&
		   now - s->release_ns >= 200000000ULL) {
		if (s->level)
			s->level--;
		s->release_ns = now;
		s->reason = s->level ? 5 : 4;
	} else if (mc >= APPLE_PMP_RELEASE_MC) {
		/* Require a continuous cool interval before releasing a step. */
		s->release_ns = now;
	}
}
#endif

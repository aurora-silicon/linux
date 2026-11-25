/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_PMP_HOLD_H
#define APPLE_PMP_HOLD_H

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/soc/apple/pmp-cooling.h>
#include <linux/types.h>

/*
 * Minimum window over which CPU idle counters are integrated before declaring
 * the package idle. Chosen as half the 100 ms slow poll cadence (matching the
 * 50 ms minimum slope window in apple_pmp_cooling_sample()) so every slow poll
 * advances the window despite timer jitter, while brief wakeups during 5 ms
 * fast polling are amortized instead of looking like sustained load.
 */
#define APPLE_PMP_IDLE_WINDOW_NS	50000000ULL
/*
 * Summed busy permille across all online CPUs below which the package is
 * considered idle (250 permille = 25% of one core across the whole SoC).
 * Heuristic chosen well below a single-core sustained workload (1000 permille)
 * so any real CPU load immediately starts the 2 s APPLE_PMP_GATED_HOLD_NS
 * fail-closed clock, while still allowing background timer/IRQ jitter across
 * all cores (~2.5% per core on a 10-core T8132) to count as idle.
 */
#define APPLE_PMP_IDLE_BUSY_PERMILLE	250U
/*
 * Absolute fail-closed cap on time without a real PMP SRAM temperature sample,
 * even while CPUs appear idle (~4x the longest observed 3.8 s benign idle
 * all-dark stretch at 100 ms polling on T8132). Bounds non-CPU workloads
 * (GPU, ANE, media engines with gated CPU clusters) so thermal fault detection
 * can never be suppressed indefinitely.
 */
#define APPLE_PMP_GATED_IDLE_CAP_NS	15000000000ULL

struct apple_pmp_gated_hold {
	u64 valid_ns;
	u64 real_valid_ns;
	long valid_mc;
};

static inline void apple_pmp_gated_hold_reset(struct apple_pmp_gated_hold *h)
{
	h->valid_ns = 0;
	h->real_valid_ns = 0;
	h->valid_mc = 0;
}

/*
 * Carry a cool valid reading across system sleep (deep sleep or s2idle) so
 * suspended time does not spend the gated-hold allowance or idle cap before
 * the first post-wake sample runs.
 */
static inline void apple_pmp_gated_hold_resume(struct apple_pmp_gated_hold *h,
					       u64 now)
{
	if (h->valid_ns && h->real_valid_ns &&
	    h->valid_mc < APPLE_PMP_GATED_HOLD_MC) {
		h->valid_ns = now;
		h->real_valid_ns = now;
	}
}

/*
 * Convert accumulated busy and wall time across @cpus online CPUs into summed
 * single-core permille (1000 = one fully busy core across the SoC) with a
 * single division so per-CPU truncation cannot hide spread-out load.
 */
static inline unsigned int
apple_pmp_cpus_busy_permille(u64 busy, u64 wall, unsigned int cpus)
{
	u64 scale;

	if (!cpus || !wall)
		return 1000;
	if (busy >= wall)
		return cpus * 1000;
	scale = (u64)cpus * 1000ULL;
	if (busy > U64_MAX / scale)
		return cpus * 1000;
	return (unsigned int)div64_u64(busy * scale, wall);
}

struct apple_pmp_cpu_sample {
	u64 idle;
	u64 wall;
};

struct apple_pmp_idle_tracker {
	u64 window_ns;
	bool cpus_idle;
};

struct apple_pmp_idle_acc {
	u64 busy;
	u64 wall;
	unsigned int seen;
	bool valid;
};

static inline void
apple_pmp_idle_tracker_reset(struct apple_pmp_idle_tracker *t)
{
	t->window_ns = 0;
	t->cpus_idle = false;
}

static inline bool
apple_pmp_idle_window_due(const struct apple_pmp_idle_tracker *t, u64 now)
{
	return !(t->window_ns && now >= t->window_ns &&
		 now - t->window_ns < APPLE_PMP_IDLE_WINDOW_NS);
}

static inline void apple_pmp_idle_begin(struct apple_pmp_idle_acc *acc,
					bool primed)
{
	acc->busy = 0;
	acc->wall = 0;
	acc->seen = 0;
	acc->valid = primed;
}

static inline void apple_pmp_idle_add_cpu(struct apple_pmp_idle_acc *acc,
					  struct apple_pmp_cpu_sample *prev,
					  bool online, u64 idle, u64 wall)
{
	u64 prev_wall, prev_idle, dw, di;

	if (!online) {
		prev->wall = 0;
		return;
	}
	prev_wall = prev->wall;
	prev_idle = prev->idle;
	prev->wall = wall;
	prev->idle = idle;
	if (!acc->valid || !prev_wall || wall <= prev_wall) {
		acc->valid = false;
		return;
	}
	dw = wall - prev_wall;
	if (idle <= prev_idle) {
		acc->busy += dw;
	} else {
		di = idle - prev_idle;
		if (di < dw)
			acc->busy += dw - di;
	}
	acc->wall += dw;
	acc->seen++;
}

static inline bool apple_pmp_idle_finish(struct apple_pmp_idle_tracker *t,
					 const struct apple_pmp_idle_acc *acc,
					 u64 now)
{
	t->window_ns = now;
	t->cpus_idle = acc->valid && acc->seen > 0 &&
		       apple_pmp_cpus_busy_permille(acc->busy, acc->wall,
						    acc->seen) <
		       APPLE_PMP_IDLE_BUSY_PERMILLE;
	return t->cpus_idle;
}

/*
 * Resolve a raw PMP hotspot sample against the power-gated sensor hold.
 *
 * PMP marks a cluster's die sensors invalid (-ENODATA) while the cluster is
 * power-gated in WFI. While the last real valid reading was cool
 * (< APPLE_PMP_GATED_HOLD_MC) and within APPLE_PMP_GATED_IDLE_CAP_NS:
 * - When the CPUs are idle, hold the last valid reading and advance valid_ns
 *   so the APPLE_PMP_GATED_HOLD_NS timeout starts when CPU work begins.
 * - When the CPUs are busy (or idle state is unconfirmed), hold only for up
 *   to APPLE_PMP_GATED_HOLD_NS.
 *
 * Any non-ENODATA fault, hot reading, or expired hold/cap clears both valid_ns
 * and real_valid_ns so fault recovery always requires a fresh valid sensor
 * sample from PMP SRAM.
 */
static inline int apple_pmp_gated_hold_sample(struct apple_pmp_gated_hold *h,
					      u64 now, long *mc, int ret,
					      bool idle)
{
	if (!ret) {
		h->valid_ns = now;
		h->real_valid_ns = now;
		h->valid_mc = *mc;
		return 0;
	}
	if (ret == -ENODATA && h->valid_ns && h->real_valid_ns &&
	    h->valid_mc < APPLE_PMP_GATED_HOLD_MC &&
	    now >= h->real_valid_ns &&
	    now - h->real_valid_ns < APPLE_PMP_GATED_IDLE_CAP_NS &&
	    (idle || (now >= h->valid_ns &&
		      now - h->valid_ns < APPLE_PMP_GATED_HOLD_NS))) {
		*mc = h->valid_mc;
		if (idle)
			h->valid_ns = now;
		return 0;
	}
	h->valid_ns = 0;
	h->real_valid_ns = 0;
	return ret;
}

/*
 * A light load runs its cluster for only part of each slow period, and a load
 * with a matching period can keep every slow sample in the gated part until the
 * hold expires. Look for a real reading at the fast cadence while the sensors
 * are dark (@ret is the raw sample's) and the CPUs have work.
 */
static inline bool apple_pmp_dark_fast(int ret, bool idle)
{
	return ret == -ENODATA && !idle;
}

enum apple_pmp_zone_action {
	APPLE_PMP_ZONE_NONE,
	APPLE_PMP_ZONE_UPDATE,
	APPLE_PMP_ZONE_REENABLE,
};

struct apple_pmp_zone_notify {
	u64 governor_ns;
	bool in_error;
	bool reenable_pending;
};

static inline void
apple_pmp_zone_notify_reset(struct apple_pmp_zone_notify *z)
{
	z->governor_ns = 0;
	z->in_error = false;
	z->reenable_pending = false;
}

/*
 * Decide how apple_pmp_poll_work() should notify thermal_core:
 * - On fault entry, notify once so thermal_core runs its own exponential
 *   recheck backoff (~330 s before thermal_zone_broken_disable() on
 *   non-EAGAIN faults; -EAGAIN rechecks every 250 ms without backing off)
 *   instead of burning through all backoff steps at our 100 ms governor rate.
 * - On recovery, if thermal_zone_broken_disable() disabled the zone during the
 *   fault, re-enable it cleanly (keeping the 200 ms cool recovery ramp from
 *   guard level 8) and push an immediate zone update on the next good poll.
 * - Otherwise throttle normal updates to @interval_ns.
 */
static inline enum apple_pmp_zone_action
apple_pmp_zone_notify_step(struct apple_pmp_zone_notify *z, u64 now, int ret,
			   bool fault_disabled, u64 interval_ns)
{
	if (ret) {
		z->reenable_pending = false;
		if (!z->in_error) {
			z->in_error = true;
			z->governor_ns = now;
			return APPLE_PMP_ZONE_UPDATE;
		}
		return APPLE_PMP_ZONE_NONE;
	}
	if (fault_disabled) {
		if (!z->reenable_pending || now < z->governor_ns ||
		    now - z->governor_ns >= interval_ns) {
			z->in_error = true;
			z->reenable_pending = true;
			z->governor_ns = now;
			return APPLE_PMP_ZONE_REENABLE;
		}
		return APPLE_PMP_ZONE_NONE;
	}
	z->reenable_pending = false;
	if (z->in_error || now < z->governor_ns ||
	    now - z->governor_ns >= interval_ns) {
		z->in_error = false;
		z->governor_ns = now;
		return APPLE_PMP_ZONE_UPDATE;
	}
	return APPLE_PMP_ZONE_NONE;
}
#endif

/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ASAHI_COOLING_POLICY_H
#define ASAHI_COOLING_POLICY_H

#define ASAHI_GPU_COOLING_MAX 100U
/* Provisional until the minimum useful absolute power is qualified per board. */
#define ASAHI_GPU_MIN_PERCENT 25U

enum asahi_gpu_cap_source {
	ASAHI_GPU_CAP_THERMAL,
	ASAHI_GPU_CAP_FAST_DIE,
	ASAHI_GPU_CAP_SYSTEM,
	ASAHI_GPU_CAP_MANUAL,
	ASAHI_GPU_CAP_SOURCES,
};

struct asahi_gpu_cap {
	unsigned int max_mw;
	unsigned int floor_mw;
	unsigned int requests[ASAHI_GPU_CAP_SOURCES];
	unsigned int requested_state;
	unsigned int applied_mw;
};

static inline int asahi_gpu_cap_percent(unsigned long state)
{
	if (state > ASAHI_GPU_COOLING_MAX)
		return -1;
	return ASAHI_GPU_COOLING_MAX - state;
}

static inline unsigned int asahi_gpu_cap_scale(unsigned int max_mw,
					       unsigned int percent)
{
	/* Divide first so even UINT_MAX never overflows. Round down. */
	return max_mw / 100 * percent + max_mw % 100 * percent / 100;
}

static inline int asahi_gpu_cap_init(struct asahi_gpu_cap *cap,
				     unsigned int max_mw)
{
	unsigned int i;

	if (!max_mw)
		return -1;
	cap->max_mw = max_mw;
	cap->floor_mw = asahi_gpu_cap_scale(max_mw, ASAHI_GPU_MIN_PERCENT);
	if (!cap->floor_mw)
		cap->floor_mw = 1;
	for (i = 0; i < ASAHI_GPU_CAP_SOURCES; i++)
		cap->requests[i] = max_mw;
	/* Start restrictive, before INIT. The thermal governor may subsequently
	 * relax its own request; it cannot relax another requester's ceiling.
	 */
	cap->requests[ASAHI_GPU_CAP_THERMAL] = cap->floor_mw;
	cap->requested_state = ASAHI_GPU_COOLING_MAX;
	cap->applied_mw = 0; /* No publication yet; never a firmware zero request. */
	return 0;
}

static inline unsigned int asahi_gpu_cap_effective(const struct asahi_gpu_cap *cap)
{
	unsigned int i, limit = cap->max_mw;

	for (i = 0; i < ASAHI_GPU_CAP_SOURCES; i++)
		if (cap->requests[i] < limit)
			limit = cap->requests[i];
	return limit;
}

/* Publisher validates before any store and cannot partially fail. Commit only
 * after successful publication. A weaker requester never overrides another.
 * Absolute requests below the provisional floor are rejected, never rounded up.
 */
static inline int asahi_gpu_cap_request(struct asahi_gpu_cap *cap,
	unsigned int source, unsigned int mw,
	int (*publish)(void *context, unsigned int mw), void *context)
{
	struct asahi_gpu_cap next = *cap;
	unsigned int effective;
	int ret;

	if (source >= ASAHI_GPU_CAP_SOURCES || mw < cap->floor_mw || mw > cap->max_mw)
		return -1;
	next.requests[source] = mw;
	effective = asahi_gpu_cap_effective(&next);
	ret = publish(context, effective);
	if (ret)
		return ret;
	next.applied_mw = effective;
	*cap = next;
	return 0;
}

static inline int asahi_gpu_cap_update(struct asahi_gpu_cap *cap,
	unsigned long state,
	int (*publish)(void *context, unsigned int mw), void *context)
{
	int percent = asahi_gpu_cap_percent(state);
	unsigned int mw;
	int ret;

	if (percent < 0)
		return -1;
	mw = asahi_gpu_cap_scale(cap->max_mw, percent);
	if (mw < cap->floor_mw)
		mw = cap->floor_mw;
	ret = asahi_gpu_cap_request(cap, ASAHI_GPU_CAP_THERMAL, mw, publish, context);
	if (!ret)
		cap->requested_state = state;
	return ret;
}
/* Restore the committed restriction before queues reopen after recovery. */
static inline int asahi_gpu_cap_rearm(const struct asahi_gpu_cap *cap,
	int (*publish)(void *context, unsigned int mw), void *context)
{
	return publish(context, cap->applied_mw);
}
#endif

/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ASAHI_COOLING_POLICY_H
#define ASAHI_COOLING_POLICY_H

/* Firmware interface units, not an OPP index or a measured power in watts. */
#define ASAHI_GPU_COOLING_MAX 100U
#define ASAHI_GPU_PREARM_PERCENT 25U
#define ASAHI_GPU_UNPUBLISHED 101U

struct asahi_gpu_cap {
	unsigned int requested_state;
	unsigned int published_percent;
};

static inline void asahi_gpu_cap_init(struct asahi_gpu_cap *cap)
{
	cap->requested_state = 0;
	cap->published_percent = ASAHI_GPU_UNPUBLISHED;
}

/* State 0 permits 100%, state 100 requests 0%. Invalid states never wrap. */
static inline int asahi_gpu_cap_percent(unsigned long state)
{
	if (state > ASAHI_GPU_COOLING_MAX)
		return -1;
	return ASAHI_GPU_COOLING_MAX - state;
}

/*
 * Keep the qualification backstop permanently armed. There is deliberately no
 * userspace switch to lift it: an achieved pstate is not an acknowledgement of
 * this percent ceiling, and source refresh cannot prove fast-die freshness.
 */
static inline unsigned int asahi_gpu_cap_effective(unsigned long state,
						 unsigned int budget_percent)
{
	int percent = asahi_gpu_cap_percent(state);
	unsigned int limit = ASAHI_GPU_PREARM_PERCENT;

	if (percent < 0 || budget_percent > 100)
		return 0;
	if ((unsigned int)percent < limit)
		limit = percent;
	if (budget_percent < limit)
		limit = budget_percent;
	return limit;
}

/* Commit host publication only on success; never call it applied/achieved. */
static inline int asahi_gpu_cap_update(struct asahi_gpu_cap *cap,
	unsigned long state, unsigned int budget_percent,
	int (*publish)(void *context, unsigned int percent), void *context)
{
	unsigned int percent;
	int ret;

	if (asahi_gpu_cap_percent(state) < 0 || budget_percent > 100)
		return -1;
	percent = asahi_gpu_cap_effective(state, budget_percent);
	ret = publish(context, percent);
	if (ret)
		return ret;
	cap->requested_state = state;
	cap->published_percent = percent;
	return 0;
}
#endif

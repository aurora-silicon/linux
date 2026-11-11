/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCPEXT26_LIFECYCLE_H
#define APPLE_DCPEXT26_LIFECYCLE_H

#include <linux/types.h>

/* A protocol control completion grants no permission to retire DMA backing. */
struct dcpext26_exchange {
	u32 id;
	bool replied;
	bool completed;
	bool failed;
};

static inline bool dcpext26_exchange_begin(struct dcpext26_exchange *x, u32 id)
{
	if (!id || x->id)
		return false;
	x->id = id;
	return true;
}

static inline bool dcpext26_exchange_complete(struct dcpext26_exchange *x, u32 id)
{
	if (!x->id || id != x->id)
		return false;
	x->completed = true;
	return true;
}

static inline bool dcpext26_exchange_finished(const struct dcpext26_exchange *x)
{
	return x->failed || (x->id && x->replied && x->completed);
}
/* Admission only: already exposed backing must always survive teardown. */
static inline bool dcpext26_backing_fits(u64 retained, u64 incoming)
{
	const u64 limit = (1ULL << 30) - (64ULL << 20);

	return retained <= limit && incoming <= limit - retained;
}
#endif

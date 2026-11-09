/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_AVE_RECOVERY_H_
#define _APPLE_AVE_RECOVERY_H_

#include <linux/jiffies.h>
#include <linux/types.h>

#define AVE_RECOVERY_ATTEMPTS	3
#define AVE_RECOVERY_WINDOW	(10 * 60 * HZ)

struct ave_recovery_history {
	unsigned long attempts[AVE_RECOVERY_ATTEMPTS];
	unsigned int next;
	unsigned int count;
};

/* A sliding window, including across jiffies wrap. Caller serialises access. */
static inline bool ave_recovery_allow(struct ave_recovery_history *h, unsigned long now)
{
	if (h->count == AVE_RECOVERY_ATTEMPTS &&
	    time_before(now, h->attempts[h->next] + AVE_RECOVERY_WINDOW))
		return false;
	h->attempts[h->next] = now;
	h->next = (h->next + 1) % AVE_RECOVERY_ATTEMPTS;
	if (h->count < AVE_RECOVERY_ATTEMPTS)
		h->count++;
	return true;
}

#endif

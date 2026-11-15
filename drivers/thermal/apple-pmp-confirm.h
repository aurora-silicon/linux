/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_PMP_CONFIRM_H
#define APPLE_PMP_CONFIRM_H

#include <linux/soc/apple/pmp-emergency-policy.h>

/* Raw critical samples, never an average. Call under the policy mutex. */
struct apple_pmp_confirmation {
	unsigned int count;
	unsigned long long last_ns;
	bool confirmed;
};

static inline void apple_pmp_confirmation_sample(struct apple_pmp_confirmation *s,
						long mc, int error,
						unsigned long long now)
{
	/* Once confirmed, retain the shutdown request until it is serviced. */
	if (s->confirmed)
		return;
	if (error || mc < APPLE_PMP_CRITICAL_MC) {
		s->count = 0;
		s->last_ns = 0;
		return;
	}
	/* sysfs reads cannot turn one firmware record into three confirmations. */
	if (s->count && now - s->last_ns < 5000000ULL)
		return;
	s->last_ns = now;
	if (++s->count == 3)
		s->confirmed = true;
}
#endif

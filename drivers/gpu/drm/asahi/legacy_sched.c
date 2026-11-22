// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <linux/sched.h>
#include <uapi/linux/sched/types.h>

#include "legacy_sched.h"

int asahi_legacy_set_current_util_min(int util_min)
{
	struct sched_attr attr = {
		.sched_policy = current->policy,
		.sched_priority = current->rt_priority,
		.sched_flags = SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP_MIN,
		.sched_util_min = util_min,
	};

	if (util_min < -1 || util_min > SCHED_CAPACITY_SCALE)
		return -EINVAL;

	return sched_setattr_nocheck(current, &attr);
}

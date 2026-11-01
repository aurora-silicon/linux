/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ANE_FW_PENDING_H_
#define _ANE_FW_PENDING_H_

#include <linux/errno.h>
#include <linux/types.h>

#define ANE_FW_PENDING_MAX 256
#define ANE_FW_PENDING_ACTIVE_US 200

struct ane_fw_pending_sample {
	u64 before, after;
	u32 value, before_cpu, after_cpu, valid;
};

struct ane_fw_pending_ops {
	u64 (*now_ns)(void *ctx);
	u64 (*counter)(void *ctx);
	u32 (*cpu)(void *ctx);
	u32 (*read_pending)(void *ctx);
	void (*delay_us)(void *ctx, u32 us);
};

static inline bool ane_fw_pending_config_valid(u32 mode, u32 gap_us, u32 gap_at)
{
	return mode <= 2 && (!mode || (gap_us == 2000 && gap_at && gap_at <= 128));
}

/* Read-only callback; no IRQ/preemption changes, transport service or writes.
 * Cap bounds work even if the clock stops advancing. A sample begun before the
 * deadline may finish after it. Interrupt/preemption overshoot remains visible.
 * Control mode keeps the same loop/counters/delay but performs no pending read.
 */
static inline int ane_fw_pending_capture(void *ctx, const struct ane_fw_pending_ops *ops,
		struct ane_fw_pending_sample *samples, u32 capacity, u64 until_ns,
		bool read_pending, u32 *count)
{
	u32 n = 0;

	if (!count)
		return -EINVAL;
	*count = 0;
	if (!ops || !samples || !capacity || capacity > ANE_FW_PENDING_MAX ||
	    !ops->now_ns || !ops->counter || !ops->cpu || !ops->delay_us ||
	    (read_pending && !ops->read_pending))
		return -EINVAL;
	while (n < capacity && ops->now_ns(ctx) < until_ns) {
		struct ane_fw_pending_sample *s = &samples[n++];

		s->before_cpu = ops->cpu(ctx);
		s->before = ops->counter(ctx);
		s->value = read_pending ? ops->read_pending(ctx) : 0;
		s->after = ops->counter(ctx);
		s->after_cpu = ops->cpu(ctx);
		s->valid = read_pending;
		ops->delay_us(ctx, 1);
	}
	*count = n;
	return n == capacity && ops->now_ns(ctx) < until_ns ? -ENOSPC : 0;
}

#endif

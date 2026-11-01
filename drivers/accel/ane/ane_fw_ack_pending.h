/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ANE_FW_ACK_PENDING_H_
#define _ANE_FW_ACK_PENDING_H_

#include <linux/types.h>

struct ane_fw_ack_pending_trace {
	u64 counters[6]; /* before-read pair, existing-write pair, after-read pair */
	u32 before_value, after_value, before_cpu, after_cpu;
	u32 mode, calls, bit, valid;
};

struct ane_fw_ack_pending_ops {
	u64 (*counter)(void *ctx);
	u32 (*cpu)(void *ctx);
	u32 (*read_pending)(void *ctx);
	void (*write_doorbell)(void *ctx, u32 value);
};

/* Caller validates mode0..2 and scopes this to a selected consume's original
 * doorbell. There is exactly ONE write in every mode. No delay, retry, IRQ mask,
 * transport action or pointer lifetime is introduced here. Private mode1 keeps
 * bookkeeping but never interprets its zero values as pending observations.
 */
static inline void ane_fw_ack_pending_observe(void *ctx,
		const struct ane_fw_ack_pending_ops *ops, struct ane_fw_ack_pending_trace *t,
		u32 mode, u32 bit, u32 value)
{
	if (!mode || !t) {
		ops->write_doorbell(ctx, value);
		return;
	}
	t->calls++;
	t->mode = mode;
	t->bit = bit;
	t->valid = mode == 2;
	t->before_cpu = ops->cpu(ctx);
	t->counters[0] = ops->counter(ctx);
	t->before_value = mode == 2 ? ops->read_pending(ctx) : 0;
	t->counters[1] = ops->counter(ctx);
	t->counters[2] = ops->counter(ctx);
	ops->write_doorbell(ctx, value);
	t->counters[3] = ops->counter(ctx);
	t->counters[4] = ops->counter(ctx);
	t->after_value = mode == 2 ? ops->read_pending(ctx) : 0;
	t->counters[5] = ops->counter(ctx);
	t->after_cpu = ops->cpu(ctx);
}

#endif

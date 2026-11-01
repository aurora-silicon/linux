/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ANE_FW_IRQ_COALESCE_H_
#define _ANE_FW_IRQ_COALESCE_H_
#include <linux/errno.h>
#include <linux/types.h>

#define ANE_FW_IRQ_COALESCE_MAX_US 25
struct ane_fw_irq_coalesce_ops {
	u64 (*now_ns)(void *ctx);
	int (*error)(void *ctx);
	void (*delay_us)(void *ctx, u32 us);
};
struct ane_fw_irq_coalesce_result {
	u64 elapsed_ns;
	u32 iterations;
	bool clipped;
};
struct ane_fw_irq_coalesce_stats {
	u64 calls, total_ns, min_ns, max_ns, iterations, clipped, errors;
};

/* Worker only, after a condition-observed IRQ wake. Never extend the original
 * deadline or restart the window for another notification. IRQs/preemption stay
 * enabled; their overshoot remains visible. Cap work if a diagnostic clock stalls.
 */
static inline int ane_fw_irq_coalesce(void *ctx, const struct ane_fw_irq_coalesce_ops *ops,
		u32 us, u64 deadline, struct ane_fw_irq_coalesce_result *out)
{
	u64 began, now, until;
	int error;

	if (!out || !ops || !ops->now_ns || !ops->error || !ops->delay_us ||
	    !us || us > ANE_FW_IRQ_COALESCE_MAX_US)
		return -EINVAL;
	*out = (struct ane_fw_irq_coalesce_result) {};
	began = now = ops->now_ns(ctx);
	until = began + (u64)us * 1000;
	if (until < began)
		return -EOVERFLOW;
	out->clipped = deadline < until;
	if (out->clipped)
		until = deadline;
	while (!(error = ops->error(ctx)) && now < until) {
		if (out->iterations == 128) {
			error = -EIO;
			break;
		}
		ops->delay_us(ctx, 1);
		out->iterations++;
		now = ops->now_ns(ctx);
	}
	now = ops->now_ns(ctx); /* include preemption during the final error check */
	out->elapsed_ns = now - began;
	if (error)
		return error;
	return now >= deadline ? -ETIMEDOUT : 0;
}
#endif

/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ANE_FW_REPORT_FIFO_H_
#define _ANE_FW_REPORT_FIFO_H_
#include <linux/kfifo.h>
#include <linux/gfp.h>
#include <linux/errno.h>
#include <linux/minmax.h>

#define ANE_FW_REPORT_FIFO_CAPACITY 256
struct ane_fw_report {
	size_t size;
	u64 ns, counter;
	u8 data[0x80];
};

/* Process-context owner only. IRQ handler never reads or mutates this FIFO. */
struct ane_fw_report_fifo {
	STRUCT_KFIFO_PTR(struct ane_fw_report) packets;
	u32 captured, peak, full_errors;
};

static inline int ane_fw_report_fifo_init(struct ane_fw_report_fifo *q)
{
	q->captured = q->peak = q->full_errors = 0;
	return kfifo_alloc(&q->packets, ANE_FW_REPORT_FIFO_CAPACITY, GFP_KERNEL);
}

static inline u32 ane_fw_report_fifo_count(struct ane_fw_report_fifo *q)
{
	return kfifo_initialized(&q->packets) ? kfifo_len(&q->packets) : 0;
}

static inline void ane_fw_report_fifo_free(struct ane_fw_report_fifo *q)
{
	kfifo_free(&q->packets);
}

static inline int ane_fw_report_fifo_pop(struct ane_fw_report_fifo *q,
					struct ane_fw_report *packet)
{
	if (!kfifo_initialized(&q->packets))
		return -EAGAIN;
	return kfifo_out(&q->packets, packet, 1) == 1 ? 0 : -EAGAIN;
}

static inline int ane_fw_report_fifo_complete(struct ane_fw_report_fifo *q,
					      unsigned int outstanding)
{
	return ane_fw_report_fifo_count(q) || outstanding ? -EPROTO : 0;
}

/* capture copies the payload BEFORE transport ACK; the private packet then
 * survives FW buffer recycling. Check capacity before any capture/ACK and never
 * overwrite/drop. A small budget lets caller recheck CMD and original deadline.
 */
static inline int ane_fw_report_fifo_prefetch(struct ane_fw_report_fifo *q,
		int (*capture)(void *, struct ane_fw_report *), void *ctx,
		unsigned int budget, bool *progress)
{
	unsigned int i;

	*progress = false;
	if (!kfifo_initialized(&q->packets) || !capture || !budget)
		return -EINVAL;
	for (i = 0; i < budget; i++) {
		struct ane_fw_report packet = {};
		int ret;

		if (kfifo_is_full(&q->packets)) {
			q->full_errors++;
			return -ENOSPC;
		}
		ret = capture(ctx, &packet);
		if (ret == -EAGAIN)
			return 0;
		if (ret)
			return ret;
		if (packet.size < 8 || packet.size > sizeof(packet.data))
			return -EPROTO;
		if (kfifo_in(&q->packets, &packet, 1) != 1)
			return -EIO;
		q->captured++;
		q->peak = max(q->peak, ane_fw_report_fifo_count(q));
		*progress = true;
	}
	return 0;
}
#endif

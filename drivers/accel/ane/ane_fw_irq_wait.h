/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ANE_FW_IRQ_WAIT_H_
#define _ANE_FW_IRQ_WAIT_H_

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>

/* Capture sequence BEFORE checking/draining queues. A notification arriving
 * between that check and wait preparation must make the wait a no-op.
 * IRQ context owns notification/error only; transport state stays with worker.
 */
struct ane_fw_irq_event {
	wait_queue_head_t queue;
	atomic_t sequence;
	atomic_t error;
};

static inline void ane_fw_irq_event_init(struct ane_fw_irq_event *event)
{
	init_waitqueue_head(&event->queue);
	atomic_set(&event->sequence, 0);
	atomic_set(&event->error, 0);
}

static inline unsigned int ane_fw_irq_event_sequence(struct ane_fw_irq_event *event)
{
	return atomic_read(&event->sequence);
}

static inline int ane_fw_irq_event_error(struct ane_fw_irq_event *event)
{
	return atomic_read(&event->error);
}

static inline void ane_fw_irq_event_notify(struct ane_fw_irq_event *event, int error)
{
	if (error)
		atomic_cmpxchg(&event->error, 0, error);
	atomic_inc(&event->sequence);
	smp_mb__after_atomic();
	wake_up_all(&event->queue);
}

/* Return 1 for a notification,0 for bounded fallback,or the sticky error.
 * Caller caps relative timeout against its ORIGINAL absolute deadline.
 */
static inline int ane_fw_irq_event_wait(struct ane_fw_irq_event *event,
					unsigned int sequence, ktime_t timeout)
{
	int ret, error;

	ret = wait_event_hrtimeout(event->queue,
		ane_fw_irq_event_sequence(event) != sequence ||
		ane_fw_irq_event_error(event), timeout);
	error = ane_fw_irq_event_error(event);
	return error ? error : ret == -ETIME ? 0 : 1;
}

#endif

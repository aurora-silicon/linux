// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/freezer.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include "shim.h"

/* Sequence sampling before the Rust state check closes the wake-before-wait race. */
struct sep_key_event {
	wait_queue_head_t wait;
	atomic64_t sequence;
};

void *sep_key_event_new(void)
{
	struct sep_key_event *event = kzalloc(sizeof(*event), GFP_KERNEL_ACCOUNT);

	if (event) {
		init_waitqueue_head(&event->wait);
		atomic64_set(&event->sequence, 0);
	}
	return event;
}

void sep_key_event_free(void *ptr)
{
	kfree(ptr);
}

u64 sep_key_event_sequence(void *ptr)
{
	struct sep_key_event *event = ptr;

	return atomic64_read(&event->sequence);
}

void sep_key_event_wake(void *ptr)
{
	struct sep_key_event *event = ptr;

	atomic64_inc(&event->sequence);
	wake_up_all(&event->wait);
}

int sep_key_event_wait(void *ptr, u64 sequence, u32 timeout_ms)
{
	struct sep_key_event *event = ptr;
	long left = msecs_to_jiffies(timeout_ms);
	DEFINE_WAIT(wait);
	int ret = 0;

	for (;;) {
		ret = prepare_to_wait_event(&event->wait, &wait,
					   TASK_KILLABLE | TASK_FREEZABLE);
		if (atomic64_read(&event->sequence) != sequence || !left)
			break;
		if (ret)
			break;
		left = schedule_timeout(left);
	}
	finish_wait(&event->wait, &wait);
	return ret ? -EINTR : 0;
}

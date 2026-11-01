// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/module.h>

#include "ane_fw_irq_wait.h"

static void ane_irq_signal_before_prepare_test(struct kunit *test)
{
	struct ane_fw_irq_event event;
	unsigned int sequence;

	ane_fw_irq_event_init(&event);
	sequence = ane_fw_irq_event_sequence(&event);
	/* The worker found its ring empty; IRQ lands before wait preparation. */
	ane_fw_irq_event_notify(&event, 0);
	KUNIT_EXPECT_EQ(test, ane_fw_irq_event_wait(&event, sequence,
					 ns_to_ktime(NSEC_PER_MSEC)), 1);
}

struct ane_irq_wait_thread {
	struct ane_fw_irq_event event;
	struct completion entered, exited;
	int result;
};

static int ane_irq_wait_thread_fn(void *data)
{
	struct ane_irq_wait_thread *wait = data;
	unsigned int sequence = ane_fw_irq_event_sequence(&wait->event);

	complete(&wait->entered);
	wait->result = ane_fw_irq_event_wait(&wait->event, sequence,
					    ns_to_ktime(NSEC_PER_SEC));
	complete(&wait->exited);
	return 0;
}

static void ane_irq_signal_waiter_test(struct kunit *test)
{
	struct ane_irq_wait_thread wait;
	struct task_struct *task;
	unsigned long entered, exited;

	ane_fw_irq_event_init(&wait.event);
	init_completion(&wait.entered);
	init_completion(&wait.exited);
	wait.result = -EINVAL;
	task = kthread_run(ane_irq_wait_thread_fn, &wait, "ane-irq-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	entered = wait_for_completion_timeout(&wait.entered, HZ);
	ane_fw_irq_event_notify(&wait.event, 0);
	exited = wait_for_completion_timeout(&wait.exited, 2 * HZ);
	/* Always wake/stop before stack storage goes out of scope. */
	ane_fw_irq_event_notify(&wait.event, -ECANCELED);
	kthread_stop(task);
	KUNIT_EXPECT_NE(test, entered, 0UL);
	KUNIT_EXPECT_NE(test, exited, 0UL);
	KUNIT_EXPECT_EQ(test, wait.result, 1);
}

static void ane_irq_error_sticky_test(struct kunit *test)
{
	struct ane_fw_irq_event event;
	unsigned int sequence;

	ane_fw_irq_event_init(&event);
	sequence = ane_fw_irq_event_sequence(&event);
	ane_fw_irq_event_notify(&event, -EPROTO);
	ane_fw_irq_event_notify(&event, -EIO);
	KUNIT_EXPECT_EQ(test, ane_fw_irq_event_wait(&event, sequence,
					 ns_to_ktime(NSEC_PER_MSEC)), -EPROTO);
	sequence = ane_fw_irq_event_sequence(&event);
	KUNIT_EXPECT_EQ(test, ane_fw_irq_event_wait(&event, sequence,
					 ns_to_ktime(NSEC_PER_MSEC)), -EPROTO);
}

static void ane_irq_bounded_fallback_test(struct kunit *test)
{
	struct ane_fw_irq_event event;

	ane_fw_irq_event_init(&event);
	KUNIT_EXPECT_EQ(test, ane_fw_irq_event_wait(&event, 0,
					 ns_to_ktime(NSEC_PER_MSEC)), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_irq_event_error(&event), 0);
}

static struct kunit_case ane_irq_wait_cases[] = {
	KUNIT_CASE(ane_irq_signal_before_prepare_test),
	KUNIT_CASE(ane_irq_signal_waiter_test),
	KUNIT_CASE(ane_irq_error_sticky_test),
	KUNIT_CASE(ane_irq_bounded_fallback_test),
	{}
};

static struct kunit_suite ane_irq_wait_suite = {
	.name = "ane-fw-irq-wait",
	.test_cases = ane_irq_wait_cases,
};
kunit_test_suite(ane_irq_wait_suite);
MODULE_LICENSE("Dual MIT/GPL");

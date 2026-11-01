// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include "ane_fw_report_fifo.h"
#include "ane_fw_irq_wait.h"

struct fifo_source {
	unsigned int next, available, calls;
	int error;
	bool reverse;
	struct ane_fw_irq_event *notify;
	u8 source[0x30];
};

static int fifo_capture(void *ctx, struct ane_fw_report *p)
{
	struct fifo_source *f = ctx;

	f->calls++;
	if (f->next == f->available)
		return f->error ?: -EAGAIN;
	if (f->reverse) {
		memset(f->source, 0, sizeof(f->source));
		put_unaligned_le16(0x300, f->source + 4);
		put_unaligned_le64(2 - f->next, f->source + 8);
		put_unaligned_le32(1, f->source + 0x1c);
	} else {
		put_unaligned_le64(f->next, f->source);
	}
	p->size = sizeof(f->source);
	memcpy(p->data, f->source, p->size);
	p->counter = 1000 + f->next;
	p->ns = 100 + f->next;
	f->next++;
	/* Model immediate firmware reuse after the private copy/ACK. */
	memset(f->source, 0xa5, sizeof(f->source));
	if (f->notify) {
		ane_fw_irq_event_notify(f->notify, 0);
		f->notify = NULL;
	}
	return 0;
}

static void fifo_copy_budget_and_order_test(struct kunit *test)
{
	struct ane_fw_report_fifo q = {};
	struct fifo_source f = { .available = 6 };
	struct ane_fw_report p;
	bool progress;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_init(&q), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_prefetch(&q, fifo_capture, &f, 4, &progress), 0);
	KUNIT_EXPECT_TRUE(test, progress);
	KUNIT_EXPECT_EQ(test, f.calls, 4U);
	KUNIT_EXPECT_EQ(test, q.peak, 4U);
	for (i = 0; i < 4; i++) {
		KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_pop(&q, &p), 0);
		KUNIT_EXPECT_EQ(test, get_unaligned_le64(p.data), (u64)i);
		KUNIT_EXPECT_EQ(test, p.counter, 1000ULL + i);
		KUNIT_EXPECT_EQ(test, p.ns, 100ULL + i);
	}
	/* FIFO reads themselves must never touch the still-live source. */
	KUNIT_EXPECT_EQ(test, f.calls, 4U);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_prefetch(&q, fifo_capture, &f, 4, &progress), 0);
	KUNIT_EXPECT_EQ(test, f.calls, 7U); /* two reports plus EAGAIN */
	for (i = 4; i < 6; i++) {
		KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_pop(&q, &p), 0);
		KUNIT_EXPECT_EQ(test, get_unaligned_le64(p.data), (u64)i);
	}
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_pop(&q, &p), -EAGAIN);
	KUNIT_EXPECT_EQ(test, q.captured, 6U);
	ane_fw_report_fifo_free(&q);
}

static void fifo_full_never_captures_or_overwrites_test(struct kunit *test)
{
	struct ane_fw_report_fifo q = {};
	struct fifo_source f = { .available = ANE_FW_REPORT_FIFO_CAPACITY + 1 };
	struct ane_fw_report p;
	bool progress;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_init(&q), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_prefetch(&q, fifo_capture, &f,
			ANE_FW_REPORT_FIFO_CAPACITY, &progress), 0);
	KUNIT_EXPECT_EQ(test, q.peak, (u32)ANE_FW_REPORT_FIFO_CAPACITY);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_prefetch(&q, fifo_capture, &f, 1, &progress), -ENOSPC);
	KUNIT_EXPECT_EQ(test, f.calls, (unsigned int)ANE_FW_REPORT_FIFO_CAPACITY);
	KUNIT_EXPECT_EQ(test, q.full_errors, 1U);
	for (i = 0; i < ANE_FW_REPORT_FIFO_CAPACITY; i++) {
		KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_pop(&q, &p), 0);
		KUNIT_EXPECT_EQ(test, get_unaligned_le64(p.data), (u64)i);
	}
	/* Exercise wrap after consuming the full allocation. */
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_prefetch(&q, fifo_capture, &f, 1, &progress), 0);
	KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_pop(&q, &p), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p.data), (u64)ANE_FW_REPORT_FIFO_CAPACITY);
	ane_fw_report_fifo_free(&q);
}

static void fifo_error_and_completion_guard_test(struct kunit *test)
{
	struct ane_fw_report_fifo q = {};
	struct fifo_source f = { .available = 1, .error = -EIO };
	struct ane_fw_report p;
	bool progress;

	KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_init(&q), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_prefetch(&q, fifo_capture, &f, 4, &progress), -EIO);
	KUNIT_EXPECT_TRUE(test, progress);
	/* An early report survives a subsequent command/capture error. */
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_complete(&q, 0), -EPROTO);
	KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_pop(&q, &p), 0);
	KUNIT_EXPECT_EQ(test, p.counter, 1000ULL);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_complete(&q, 1), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_complete(&q, 0), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_pop(&q, &p), -EAGAIN);
	ane_fw_report_fifo_free(&q);
}

static void fifo_irq_during_prefetch_prevents_lost_wait_test(struct kunit *test)
{
	struct ane_fw_report_fifo q = {};
	struct ane_fw_irq_event event;
	struct fifo_source f = { .available = 1, .notify = &event };
	unsigned int sequence;
	bool progress;

	KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_init(&q), 0);
	ane_fw_irq_event_init(&event);
	sequence = ane_fw_irq_event_sequence(&event);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_prefetch(&q, fifo_capture, &f, 4, &progress), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_irq_event_wait(&event, sequence, ms_to_ktime(1)), 1);
	ane_fw_report_fifo_free(&q);
}

static void fifo_uninitialized_never_captures_test(struct kunit *test)
{
	struct ane_fw_report_fifo q = {};
	struct fifo_source f = { .available = 1 };
	struct ane_fw_report p;
	bool progress;

	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_pop(&q, &p), -EAGAIN);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_prefetch(&q, fifo_capture, &f, 4, &progress), -EINVAL);
	KUNIT_EXPECT_EQ(test, f.calls, 0U);
	ane_fw_report_fifo_free(&q);
}

/* Opaque FIFO preserves out-of-order terminal identities; actual UUID retirement
 * remains the backend dispatcher's job and is covered separately on hardware.
 */
static void fifo_out_of_order_terminal_identity_test(struct kunit *test)
{
	struct ane_fw_report_fifo q = {};
	struct fifo_source f = { .available = 2, .reverse = true };
	struct ane_fw_report p;
	bool progress;

	KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_init(&q), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_prefetch(&q, fifo_capture, &f, 4, &progress), 0);
	KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_pop(&q, &p), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p.data + 8), 2ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(p.data + 0x1c), 1U);
	KUNIT_EXPECT_EQ(test, p.counter, 1000ULL);
	KUNIT_ASSERT_EQ(test, ane_fw_report_fifo_pop(&q, &p), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p.data + 8), 1ULL);
	KUNIT_EXPECT_EQ(test, p.counter, 1001ULL);
	KUNIT_EXPECT_EQ(test, ane_fw_report_fifo_complete(&q, 0), 0);
	ane_fw_report_fifo_free(&q);
}

static struct kunit_case fifo_cases[] = {
	KUNIT_CASE(fifo_copy_budget_and_order_test),
	KUNIT_CASE(fifo_full_never_captures_or_overwrites_test),
	KUNIT_CASE(fifo_error_and_completion_guard_test),
	KUNIT_CASE(fifo_irq_during_prefetch_prevents_lost_wait_test),
	KUNIT_CASE(fifo_uninitialized_never_captures_test),
	KUNIT_CASE(fifo_out_of_order_terminal_identity_test),
	{}
};
static struct kunit_suite fifo_suite = { .name = "ane-fw-report-fifo", .test_cases = fifo_cases };
kunit_test_suite(fifo_suite);
MODULE_LICENSE("Dual MIT/GPL");

// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <kunit/test.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>

#include "dcp.h"
#include "dcp-internal.h"

/*
 * The 14.7 panel's display controller off (A472) goes through the IOMFB
 * queue, and a present that reaches the stopped controller is accepted but
 * never completed.  Lid-close sleep captured it: a present queued behind the
 * A472 off held the queue, so the A472 on at resume timed out and the queue
 * timeout declared the coprocessor crashed.  These cases check admission
 * through the real queue; each transaction completes at start, as one the
 * firmware answers at once.
 */

struct hold_transaction {
	struct iomfb_transaction transaction;
	unsigned int *order;
	unsigned int started;
};

struct hold_test {
	struct apple_dcp dcp;
	struct hold_transaction t[4];
	unsigned int order;
};

static void hold_test_noop(struct work_struct *work)
{
}

static void hold_test_start(struct apple_dcp *dcp, struct iomfb_transaction *transaction)
{
	struct hold_transaction *t = container_of(transaction, struct hold_transaction,
						  transaction);

	t->started = ++*t->order;
}

static void hold_test_release(struct iomfb_transaction *transaction)
{
}

static void hold_test_exit(void *data)
{
	struct hold_test *h = data;

	iomfb_queue_stop(&h->dcp);
}

static struct hold_test *hold_test_init(struct kunit *test)
{
	struct hold_test *h = kunit_kzalloc(test, sizeof(*h), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, h);
	h->dcp.fw_compat = DCP_FIRMWARE_V_14_7;
	iomfb_queue_init(&h->dcp);
	/* Each completion asks the backlight policy for a pending level. */
	spin_lock_init(&h->dcp.backlight.lock);
	INIT_WORK(&h->dcp.vblank_wq, hold_test_noop);
	for (i = 0; i < ARRAY_SIZE(h->t); i++) {
		h->t[i].order = &h->order;
		h->t[i].transaction.start = hold_test_start;
		h->t[i].transaction.release = hold_test_release;
	}
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, hold_test_exit, h), 0);
	return h;
}

static void hold_test_present(struct hold_transaction *t, bool brightness_only)
{
	t->transaction.presentation = true;
	t->transaction.brightness_only = brightness_only;
}

/* Held: presents are refused, a command such as A472 still goes out. */
static void panel_hold_refuses_presents_test(struct kunit *test)
{
	struct hold_test *h = hold_test_init(test);
	struct apple_dcp *dcp = &h->dcp;

	if (!iomfb_is_v14_7(dcp))
		kunit_skip(test, "needs CONFIG_DRM_APPLE_DCP_T6030");

	iomfb_hold_panel_presents(dcp, true);
	hold_test_present(&h->t[0], false);
	hold_test_present(&h->t[1], true);
	KUNIT_EXPECT_EQ(test, iomfb_queue(dcp, &h->t[0].transaction), -EAGAIN);
	KUNIT_EXPECT_EQ(test, iomfb_queue(dcp, &h->t[1].transaction), -EAGAIN);
	/* A refused brightness present leaves no queued level behind. */
	KUNIT_EXPECT_FALSE(test, dcp->iomfb.backlight_queued);

	KUNIT_ASSERT_EQ(test, iomfb_queue(dcp, &h->t[2].transaction), 0);
	KUNIT_EXPECT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(1000)), 0);
	KUNIT_EXPECT_EQ(test, h->t[2].started, 1U);
	KUNIT_EXPECT_EQ(test, h->t[0].started, 0U);
	KUNIT_EXPECT_EQ(test, h->t[1].started, 0U);

	iomfb_hold_panel_presents(dcp, false);
	KUNIT_ASSERT_EQ(test, iomfb_queue(dcp, &h->t[0].transaction), 0);
	KUNIT_EXPECT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(1000)), 0);
	KUNIT_EXPECT_EQ(test, h->t[0].started, 2U);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->crashed));
}

/*
 * A present admitted before the hold still goes out, ahead of the A472 off
 * queued after it; one admitted after the hold does not.  A pending present
 * keeps the queue from starting anything until the test lets it go.
 */
static void panel_hold_keeps_order_test(struct kunit *test)
{
	struct hold_test *h = hold_test_init(test);
	struct apple_dcp *dcp = &h->dcp;

	if (!iomfb_is_v14_7(dcp))
		kunit_skip(test, "needs CONFIG_DRM_APPLE_DCP_T6030");

	scoped_guard(mutex, &dcp->iomfb.lock)
		dcp->present_state_h17p.pending = true;
	hold_test_present(&h->t[0], false);
	KUNIT_ASSERT_EQ(test, iomfb_queue(dcp, &h->t[0].transaction), 0);

	iomfb_hold_panel_presents(dcp, true);
	/* The A472 off. */
	KUNIT_ASSERT_EQ(test, iomfb_queue(dcp, &h->t[1].transaction), 0);
	hold_test_present(&h->t[2], true);
	KUNIT_EXPECT_EQ(test, iomfb_queue(dcp, &h->t[2].transaction), -EAGAIN);

	scoped_guard(mutex, &dcp->iomfb.lock)
		dcp->present_state_h17p.pending = false;
	schedule_work(&dcp->iomfb.work);
	KUNIT_EXPECT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(1000)), 0);
	KUNIT_EXPECT_EQ(test, h->t[0].started, 1U);
	KUNIT_EXPECT_EQ(test, h->t[1].started, 2U);
	KUNIT_EXPECT_EQ(test, h->t[2].started, 0U);
}

/*
 * System sleep pauses and resumes presentation around the mailbox hold.  The
 * resume must not release presents for a controller that is still off: the
 * backlight core restores its level before DRM powers the panel on.
 */
static void panel_hold_outlasts_pause_test(struct kunit *test)
{
	struct hold_test *h = hold_test_init(test);
	struct apple_dcp *dcp = &h->dcp;

	if (!iomfb_is_v14_7(dcp))
		kunit_skip(test, "needs CONFIG_DRM_APPLE_DCP_T6030");

	iomfb_hold_panel_presents(dcp, true);
	iomfb_pause_presentation(dcp, true);
	iomfb_pause_presentation(dcp, false);
	hold_test_present(&h->t[0], true);
	KUNIT_EXPECT_EQ(test, iomfb_queue(dcp, &h->t[0].transaction), -EAGAIN);

	iomfb_hold_panel_presents(dcp, false);
	KUNIT_EXPECT_EQ(test, iomfb_queue(dcp, &h->t[0].transaction), 0);
	KUNIT_EXPECT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(1000)), 0);
	KUNIT_EXPECT_EQ(test, h->t[0].started, 1U);
}

static struct kunit_case panel_hold_cases[] = {
	KUNIT_CASE(panel_hold_refuses_presents_test),
	KUNIT_CASE(panel_hold_keeps_order_test),
	KUNIT_CASE(panel_hold_outlasts_pause_test),
	{}
};

static struct kunit_suite panel_hold_suite = {
	.name = "apple-dcp-panel-hold",
	.test_cases = panel_hold_cases,
};

kunit_test_suite(panel_hold_suite);

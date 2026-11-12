/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* CPU-only transport fault injection: normal RAM stands in for the registers.
 * The receive FIFO never empties and the send FIFO never gains space.
 */
#include <kunit/test.h>

static void apple_g17_test_rx(struct apple_mbox *mbox,
                              struct apple_mbox_msg msg, void *cookie)
{
	unsigned int *count = cookie;

	(*count)++;
}

static void apple_g17_rx_flood_bounded_test(struct kunit *test)
{
	const struct apple_mbox_hw hw = {
		.i2a_control = 0, .i2a_recv0 = 8, .i2a_recv1 = 16,
		.control_empty = BIT(17),
	};
	struct apple_mbox mbox = { .hw = &hw, .rx = apple_g17_test_rx };
	unsigned int count = 0;
	void *regs = kunit_kzalloc(test, 32, GFP_KERNEL);
	int n;

	KUNIT_ASSERT_NOT_NULL(test, regs);
	mbox.regs = (void __iomem *)regs;
	mbox.ascwrap_v6.role = APPLE_MBOX_ASCWRAP_V6_ROLE_GFX;
	mbox.cookie = &count;
	spin_lock_init(&mbox.rx_lock);
	for (n = 0; n < 64; n++)
		KUNIT_ASSERT_EQ(test, apple_mbox_poll(&mbox), APPLE_G17_MBOX_RX_BUDGET);
	KUNIT_EXPECT_EQ(test, count, 64U * APPLE_G17_MBOX_RX_BUDGET);
}

static void apple_g17_tx_stall_bounded_test(struct kunit *test)
{
	const struct apple_mbox_hw hw = { .control_full = BIT(16) };
	struct apple_mbox mbox = { .hw = &hw, .irq_send_empty = -1 };
	struct apple_mbox_msg msg = { 0 };
	void *regs = kunit_kzalloc(test, 32, GFP_KERNEL);
	int n;

	KUNIT_ASSERT_NOT_NULL(test, regs);
	mbox.regs = (void __iomem *)regs;
	mbox.ascwrap_v6.role = APPLE_MBOX_ASCWRAP_V6_ROLE_GFX1;
	writel(BIT(16), mbox.regs);
	spin_lock_init(&mbox.tx_lock);
	for (n = 0; n < 8; n++)
		KUNIT_ASSERT_EQ(test, apple_mbox_send(&mbox, msg, true), -ETIMEDOUT);
	/* The G17 no-IRQ, non-atomic branch must sleep off-lock and expire. */
	KUNIT_EXPECT_EQ(test, apple_mbox_send(&mbox, msg, false), -ETIMEDOUT);
	mbox.removing = true;
	KUNIT_EXPECT_EQ(test, apple_mbox_send(&mbox, msg, false), -ENODEV);
}

/* The receive thread's minimum utilization: request validation, and applying
 * it to an RT thread (the test thread stands in) without losing its policy.
 */
static void apple_g17_rx_util_min_test(struct kunit *test)
{
	struct apple_mbox mbox = {
		.rx_util_min = -1, .rx_util_min_applied = -1,
	};

	KUNIT_EXPECT_EQ(test, apple_mbox_set_rx_util_min(&mbox, 0), -EOPNOTSUPP);
	mbox.ascwrap_v6.role = APPLE_MBOX_ASCWRAP_V6_ROLE_GFX;
	if (!IS_ENABLED(CONFIG_UCLAMP_TASK)) {
		KUNIT_EXPECT_EQ(test, apple_mbox_set_rx_util_min(&mbox, 0), -EOPNOTSUPP);
		kunit_skip(test, "needs CONFIG_UCLAMP_TASK");
	}
	KUNIT_EXPECT_EQ(test, apple_mbox_set_rx_util_min(&mbox, -2), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_mbox_set_rx_util_min(&mbox, SCHED_CAPACITY_SCALE + 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, mbox.rx_util_min, -1);
#ifdef CONFIG_UCLAMP_TASK
	sched_set_fifo(current);
	KUNIT_ASSERT_EQ(test, apple_mbox_set_rx_util_min(&mbox, 256), 0);
	apple_mbox_apply_rx_util_min(&mbox);
	KUNIT_EXPECT_EQ(test, mbox.rx_util_min_applied, 256);
	KUNIT_EXPECT_EQ(test, current->policy, SCHED_FIFO);
	KUNIT_EXPECT_EQ(test, current->rt_priority, MAX_RT_PRIO / 2);
	KUNIT_EXPECT_EQ(test, (unsigned int)current->uclamp_req[UCLAMP_MIN].value, 256U);
	KUNIT_EXPECT_TRUE(test, current->uclamp_req[UCLAMP_MIN].user_defined);

	/* -1 hands the clamp back to the scheduler's RT default. */
	KUNIT_ASSERT_EQ(test, apple_mbox_set_rx_util_min(&mbox, -1), 0);
	apple_mbox_apply_rx_util_min(&mbox);
	KUNIT_EXPECT_EQ(test, mbox.rx_util_min_applied, -1);
	KUNIT_EXPECT_EQ(test, current->policy, SCHED_FIFO);
	KUNIT_EXPECT_EQ(test, current->rt_priority, MAX_RT_PRIO / 2);
	KUNIT_EXPECT_FALSE(test, current->uclamp_req[UCLAMP_MIN].user_defined);
	sched_set_normal(current, 0);
#endif
}

struct apple_g17_fast_rx {
	struct apple_mbox_priv priv;
	struct completion ticked;
};

static enum hrtimer_restart apple_g17_fast_rx_tick(struct hrtimer *timer)
{
	struct apple_g17_fast_rx *f = container_of(timer, struct apple_g17_fast_rx,
						   priv.rx_fast_timer);

	complete(&f->ticked);
	return HRTIMER_NORESTART;
}

static struct apple_g17_fast_rx *apple_g17_fast_rx_new(struct kunit *test)
{
	struct apple_g17_fast_rx *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	init_completion(&f->ticked);
	hrtimer_setup(&f->priv.rx_fast_timer, apple_g17_fast_rx_tick,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	return f;
}

/* Only the kick that takes rx_fast_armed from 0 to 1 starts the timer. */
static void apple_g17_fast_rx_kick_arms_once_test(struct kunit *test)
{
	struct apple_g17_fast_rx *f = apple_g17_fast_rx_new(test);
	ktime_t first;

	apple_mbox_fast_rx_kick(&f->priv);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->priv.rx_fast_armed), 1);
	KUNIT_EXPECT_GT(test, wait_for_completion_timeout(&f->ticked, HZ), 0UL);

	/* The test tick does not restart, as the real tick does while armed. */
	reinit_completion(&f->ticked);
	first = f->priv.rx_fast_until;
	apple_mbox_fast_rx_kick(&f->priv);
	KUNIT_EXPECT_EQ(test, wait_for_completion_timeout(&f->ticked,
							  msecs_to_jiffies(5)), 0UL);
	KUNIT_EXPECT_TRUE(test, ktime_after(f->priv.rx_fast_until, first));

	/* The real tick clears the flag on every NORESTART exit; model that. */
	atomic_set(&f->priv.rx_fast_armed, 0);
	apple_mbox_fast_rx_kick(&f->priv);
	KUNIT_EXPECT_GT(test, wait_for_completion_timeout(&f->ticked, HZ), 0UL);
	apple_mbox_fast_rx_cancel(&f->priv);
}

/*
 * A kick that lands after the tick read its expired deadline but before the
 * tick dropped the flag sees the flag set and starts nothing. The tick must
 * then notice the new deadline instead of going idle with the kick lost.
 */
static void apple_g17_fast_rx_keeps_racing_kick_test(struct kunit *test)
{
	struct apple_g17_fast_rx *f = apple_g17_fast_rx_new(test);
	ktime_t before;

	/* Deadline passed, no kick: the tick stops and the next kick re-arms. */
	atomic_set(&f->priv.rx_fast_armed, 1);
	f->priv.rx_fast_until = 0;
	KUNIT_EXPECT_FALSE(test, apple_mbox_fast_rx_keep_armed(&f->priv));
	KUNIT_EXPECT_EQ(test, atomic_read(&f->priv.rx_fast_armed), 0);

	/* The racing kick: the flag is still set, so the kick starts no timer. */
	atomic_set(&f->priv.rx_fast_armed, 1);
	f->priv.rx_fast_until = 0;
	before = ktime_get();
	apple_mbox_fast_rx_kick(&f->priv);
	KUNIT_EXPECT_TRUE(test, ktime_after(f->priv.rx_fast_until, before));
	KUNIT_EXPECT_EQ(test, wait_for_completion_timeout(&f->ticked, 0), 0UL);
	/* Pin the deadline live so a slow runner cannot expire the kick's own. */
	f->priv.rx_fast_until = ktime_add_ms(ktime_get(), 1000);
	KUNIT_EXPECT_TRUE(test, apple_mbox_fast_rx_keep_armed(&f->priv));
	KUNIT_EXPECT_EQ(test, atomic_read(&f->priv.rx_fast_armed), 1);
	apple_mbox_fast_rx_cancel(&f->priv);
}

/*
 * A kick that took the flag but has not called hrtimer_start() yet must keep
 * it across a cancel: the timer is not queued, so nothing was cancelled.
 */
static void apple_g17_fast_rx_cancel_keeps_inflight_kick_test(struct kunit *test)
{
	struct apple_g17_fast_rx *f = apple_g17_fast_rx_new(test);

	atomic_set(&f->priv.rx_fast_armed, 1);
	apple_mbox_fast_rx_cancel(&f->priv);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->priv.rx_fast_armed), 1);

	/* A queued timer is cancelled with its flag, so the next kick re-arms. */
	hrtimer_start(&f->priv.rx_fast_timer, ms_to_ktime(1000), HRTIMER_MODE_REL);
	apple_mbox_fast_rx_cancel(&f->priv);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->priv.rx_fast_armed), 0);
}

static struct kunit_case apple_g17_stall_cases[] = {
	KUNIT_CASE(apple_g17_rx_flood_bounded_test),
	KUNIT_CASE(apple_g17_tx_stall_bounded_test),
	KUNIT_CASE(apple_g17_rx_util_min_test),
	KUNIT_CASE(apple_g17_fast_rx_kick_arms_once_test),
	KUNIT_CASE(apple_g17_fast_rx_keeps_racing_kick_test),
	KUNIT_CASE(apple_g17_fast_rx_cancel_keeps_inflight_kick_test),
	{ }
};
static struct kunit_suite apple_g17_stall_suite = {
	.name = "apple_mbox_g17_stall",
	.test_cases = apple_g17_stall_cases,
};
kunit_test_suite(apple_g17_stall_suite);

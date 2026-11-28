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

static void apple_g17_rx_hardirq_dispatch_test(struct kunit *test)
{
	const struct apple_mbox_hw hw = {
		.i2a_control = 0, .i2a_recv0 = 8, .i2a_recv1 = 16,
		.control_empty = BIT(17),
	};
	struct apple_mbox mbox = { .hw = &hw, .rx = apple_g17_test_rx };
	unsigned int count = 0;
	void *regs = kunit_kzalloc(test, 32, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, regs);
	mbox.regs = (void __iomem *)regs;
	const struct apple_mbox_hw hw_v8 = {
		.i2a_control = 0, .i2a_recv0 = 8, .i2a_recv1 = 16,
		.control_empty = BIT(17),
		.is_ascwrap_v8 = true,
	};
	struct apple_mbox mbox_v8 = { .hw = &hw_v8 };

	mbox.ascwrap_v6.role = APPLE_MBOX_ASCWRAP_V6_ROLE_GFX;
	mbox.cookie = &count;
	spin_lock_init(&mbox.rx_lock);

	/* ASCWrap v6 GFX/GFX1 use the hard-IRQ split; ASCWrap v8 stays excluded
	 * so apple_mbox_recv_irq() keeps running apple_mbox_v8_rx_irq_guard().
	 */
	KUNIT_EXPECT_TRUE(test, apple_mbox_is_g17(&mbox));
	mbox_v8.ascwrap_v6.role = APPLE_MBOX_ASCWRAP_V6_ROLE_GFX;
	KUNIT_EXPECT_FALSE(test, apple_mbox_is_g17(&mbox_v8));
	mbox_v8.ascwrap_v6.role = APPLE_MBOX_ASCWRAP_V6_ROLE_GFX1;
	KUNIT_EXPECT_FALSE(test, apple_mbox_is_g17(&mbox_v8));

	/* Empty FIFO finishes directly in hard IRQ without waking the thread. */
	writel(BIT(17), mbox.regs);
	KUNIT_EXPECT_EQ(test, apple_mbox_g17_recv_irq(0, &mbox), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, count, 0U);

	/* Saturated budget wakes the oneshot throttle thread. */
	writel(0, mbox.regs);
	KUNIT_EXPECT_EQ(test, apple_mbox_g17_recv_irq(0, &mbox), IRQ_WAKE_THREAD);
	KUNIT_EXPECT_EQ(test, count, (unsigned int)APPLE_G17_MBOX_RX_BUDGET);
}

static struct kunit_case apple_g17_stall_cases[] = {
	KUNIT_CASE(apple_g17_rx_flood_bounded_test),
	KUNIT_CASE(apple_g17_rx_hardirq_dispatch_test),
	KUNIT_CASE(apple_g17_tx_stall_bounded_test),
	KUNIT_CASE(apple_g17_rx_util_min_test),
	{ }
};
static struct kunit_suite apple_g17_stall_suite = {
	.name = "apple_mbox_g17_stall",
	.test_cases = apple_g17_stall_cases,
};
kunit_test_suite(apple_g17_stall_suite);

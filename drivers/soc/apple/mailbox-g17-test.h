/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* CPU-only transport fault injection: normal RAM stands in for the registers.
 * The receive FIFO never empties and the send FIFO never gains space.
 */
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/irq.h>

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

/* T6050 receive until the recv interrupt is proven. The interrupt is
 * requested as the driver requests it, on a descriptor with no controller
 * behind it: the tests enable and disable it and call the receive handler as
 * its thread would. The registers are RAM;
 * apple_g17_rx_fifo() makes the receive FIFO never empty or always empty.
 * Whether a poller runs is shown by what it receives: the test thread itself
 * receives only through the calls it makes, and snapshots state after
 * cancelling the pollers or under rx_lock.
 */
static const struct apple_mbox_hw apple_g17_rx_until_irq_hw = {
	.i2a_control = 0, .i2a_recv0 = 8, .i2a_recv1 = 16,
	.a2i_control = 32, .a2i_send0 = 40, .a2i_send1 = 48,
	.control_empty = BIT(17), .control_full = BIT(16),
	.rx_poll_until_irq = true,
};

/* Generous: a poller that runs receives within a jiffy or a fast-poll period. */
#define APPLE_G17_TEST_RX_WAIT msecs_to_jiffies(5000)

struct apple_g17_rx_fixture {
	struct apple_mbox_priv *priv;
	/* rx_lock: messages received, by any path. */
	unsigned int count;
	/* Completed by every message received. */
	struct completion rx;
};

static void apple_g17_fixture_rx(struct apple_mbox *mbox,
				 struct apple_mbox_msg msg, void *cookie)
{
	struct apple_g17_rx_fixture *f = cookie;

	f->count++;
	complete(&f->rx);
}

static void apple_g17_rx_fixture_exit(void *data)
{
	struct apple_mbox_priv *priv = data;

	cancel_delayed_work_sync(&priv->rx_poll_work);
	hrtimer_cancel(&priv->rx_fast_timer);
	free_irq(priv->mbox.irq_recv_not_empty, &priv->mbox);
	irq_free_desc(priv->mbox.irq_recv_not_empty);
}

/* The fixture outlives the test's stack, like the pollers. */
static struct apple_g17_rx_fixture *apple_g17_rx_fixture(struct kunit *test)
{
	struct apple_g17_rx_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	struct apple_mbox_priv *priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	void *regs = kunit_kzalloc(test, 64, GFP_KERNEL);
	struct apple_mbox *mbox;
	int irq;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	KUNIT_ASSERT_NOT_NULL(test, regs);
	irq = irq_alloc_desc(NUMA_NO_NODE);
	KUNIT_ASSERT_GE(test, irq, 0);
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	init_completion(&f->rx);
	mbox = &priv->mbox;
	mbox->hw = &apple_g17_rx_until_irq_hw;
	mbox->regs = (void __iomem *)regs;
	mbox->ascwrap_v6.role = APPLE_MBOX_ASCWRAP_V6_ROLE_GFX;
	mbox->rx = apple_g17_fixture_rx;
	mbox->cookie = f;
	mbox->irq_recv_not_empty = irq;
	mbox->irq_send_empty = -1;
	mbox->active = true;
	spin_lock_init(&mbox->rx_lock);
	spin_lock_init(&mbox->tx_lock);
	hrtimer_setup(&priv->rx_fast_timer, apple_mbox_fast_rx_poll,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	INIT_DELAYED_WORK(&priv->rx_poll_work, apple_mbox_v8_rx_poll_work);
	if (request_threaded_irq(irq, NULL, apple_mbox_recv_irq,
				 IRQF_ONESHOT | IRQF_NO_AUTOEN, "apple-mbox-test",
				 mbox)) {
		irq_free_desc(irq);
		KUNIT_FAIL_AND_ABORT(test, "request_threaded_irq failed");
	}
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				apple_g17_rx_fixture_exit, priv), 0);
	f->priv = priv;
	return f;
}

/* Under rx_lock, so a running poller sees the FIFO before or after the change. */
static void apple_g17_rx_fifo(struct apple_mbox *mbox, bool full)
{
	unsigned long flags;

	spin_lock_irqsave(&mbox->rx_lock, flags);
	writel(full ? 0 : mbox->hw->control_empty, mbox->regs + mbox->hw->i2a_control);
	spin_unlock_irqrestore(&mbox->rx_lock, flags);
}

/*
 * Fill the FIFO and forget earlier receives, both under rx_lock: whatever
 * completes f->rx afterwards received from this full FIFO.
 */
static void apple_g17_rx_fill(struct apple_g17_rx_fixture *f)
{
	struct apple_mbox *mbox = &f->priv->mbox;
	unsigned long flags;

	spin_lock_irqsave(&mbox->rx_lock, flags);
	reinit_completion(&f->rx);
	writel(0, mbox->regs + mbox->hw->i2a_control);
	spin_unlock_irqrestore(&mbox->rx_lock, flags);
}

static bool apple_g17_rx_received(struct apple_g17_rx_fixture *f)
{
	return wait_for_completion_timeout(&f->rx, APPLE_G17_TEST_RX_WAIT);
}

static unsigned int apple_g17_rx_count(struct apple_g17_rx_fixture *f)
{
	struct apple_mbox *mbox = &f->priv->mbox;
	unsigned long flags;
	unsigned int count;

	spin_lock_irqsave(&mbox->rx_lock, flags);
	count = f->count;
	spin_unlock_irqrestore(&mbox->rx_lock, flags);
	return count;
}

static bool apple_g17_rx_irq_enabled(struct apple_mbox *mbox)
{
	return !irqd_irq_disabled(irq_get_irq_data(mbox->irq_recv_not_empty));
}

/*
 * A send arms the fast poller: the armed poller fires and receives from the
 * full FIFO. Call with the jiffy poller cancelled, so that nothing else
 * receives.
 */
static bool apple_g17_send_fast_polls(struct kunit *test,
				      struct apple_g17_rx_fixture *f)
{
	struct apple_mbox_msg msg = { 0 };
	bool received;

	apple_g17_rx_fill(f);
	KUNIT_EXPECT_EQ(test, apple_mbox_send(&f->priv->mbox, msg, true), 0);
	received = apple_g17_rx_received(f);
	hrtimer_cancel(&f->priv->rx_fast_timer);
	apple_g17_rx_fifo(&f->priv->mbox, false);
	return received;
}

/* A send that does not arm the fast poller sets no deadline and starts no timer. */
static bool apple_g17_send_leaves_fast_poller(struct kunit *test,
					      struct apple_mbox_priv *priv)
{
	struct apple_mbox_msg msg = { 0 };
	bool left;

	WRITE_ONCE(priv->rx_fast_until, 0);
	KUNIT_EXPECT_EQ(test, apple_mbox_send(&priv->mbox, msg, true), 0);
	left = !READ_ONCE(priv->rx_fast_until) &&
	       !hrtimer_active(&priv->rx_fast_timer);
	hrtimer_cancel(&priv->rx_fast_timer);
	return left;
}

/* Proof: both pollers run until the live interrupt delivers a message. */
static void apple_g17_rx_irq_proof_test(struct kunit *test)
{
	struct apple_g17_rx_fixture *f = apple_g17_rx_fixture(test);
	struct apple_mbox_priv *priv = f->priv;
	struct apple_mbox *mbox = &priv->mbox;
	unsigned int before;

	priv->rx_irq_live = true;

	/* Not proven: the fast poller restarts and a send arms it. Receiving
	 * moves the deadline, so the restart does not depend on the clock.
	 */
	apple_g17_rx_fifo(mbox, true);
	KUNIT_EXPECT_EQ(test, apple_mbox_fast_rx_poll(&priv->rx_fast_timer),
			HRTIMER_RESTART);
	KUNIT_EXPECT_EQ(test, f->count, APPLE_G17_MBOX_RX_BUDGET);
	KUNIT_EXPECT_TRUE(test, apple_g17_send_fast_polls(test, f));

	/* An interrupt that finds nothing proves nothing; one with a message does. */
	apple_mbox_recv_irq(mbox->irq_recv_not_empty, mbox);
	KUNIT_EXPECT_FALSE(test, priv->rx_irq_proven);
	KUNIT_EXPECT_EQ(test, priv->rx_irq_empty_run, 1U);
	before = f->count;
	apple_g17_rx_fifo(mbox, true);
	apple_mbox_recv_irq(mbox->irq_recv_not_empty, mbox);
	KUNIT_EXPECT_TRUE(test, priv->rx_irq_proven);
	KUNIT_EXPECT_EQ(test, priv->rx_irq_empty_run, 0U);
	KUNIT_EXPECT_EQ(test, f->count, before + APPLE_G17_MBOX_RX_BUDGET);

	/* Proven: the fast poller stops and a send does not arm it. */
	KUNIT_EXPECT_EQ(test, apple_mbox_fast_rx_poll(&priv->rx_fast_timer),
			HRTIMER_NORESTART);
	apple_g17_rx_fifo(mbox, false);
	KUNIT_EXPECT_TRUE(test, apple_g17_send_leaves_fast_poller(test, priv));

	/* A line that is not live (stopped, or cut by the guard) proves nothing. */
	priv->rx_irq_proven = false;
	priv->rx_irq_live = false;
	apple_g17_rx_fifo(mbox, true);
	apple_mbox_recv_irq(mbox->irq_recv_not_empty, mbox);
	KUNIT_EXPECT_FALSE(test, priv->rx_irq_proven);
}

/* After proof the jiffy poller keeps running. A message it finds that the
 * interrupt did not deliver takes the proof back; the interrupt proves the
 * line again.
 */
static void apple_g17_rx_watchdog_test(struct kunit *test)
{
	struct apple_g17_rx_fixture *f = apple_g17_rx_fixture(test);
	struct apple_mbox_priv *priv = f->priv;
	struct apple_mbox *mbox = &priv->mbox;
	unsigned long flags;
	unsigned int before;
	bool proven;

	priv->rx_irq_live = true;
	apple_g17_rx_fifo(mbox, true);
	apple_mbox_recv_irq(mbox->irq_recv_not_empty, mbox);
	KUNIT_ASSERT_TRUE(test, priv->rx_irq_proven);

	/* The interrupt drained the FIFO: the watchdog finds nothing. */
	apple_g17_rx_fifo(mbox, false);
	apple_mbox_v8_rx_poll_work(&priv->rx_poll_work.work);
	spin_lock_irqsave(&mbox->rx_lock, flags);
	proven = priv->rx_irq_proven;
	spin_unlock_irqrestore(&mbox->rx_lock, flags);
	KUNIT_EXPECT_TRUE(test, proven);

	/* It queued itself again. The line goes silent with a message queued:
	 * a later pass takes the message and the proof.
	 */
	before = apple_g17_rx_count(f);
	apple_g17_rx_fill(f);
	KUNIT_EXPECT_TRUE(test, apple_g17_rx_received(f));
	cancel_delayed_work_sync(&priv->rx_poll_work);
	KUNIT_EXPECT_FALSE(test, priv->rx_irq_proven);
	KUNIT_EXPECT_GE(test, f->count, before + APPLE_G17_MBOX_RX_BUDGET);

	/* Back to polling: the fast poller restarts and a send arms it. */
	apple_g17_rx_fifo(mbox, false);
	KUNIT_EXPECT_TRUE(test, apple_g17_send_fast_polls(test, f));
	apple_g17_rx_fifo(mbox, true);
	KUNIT_EXPECT_EQ(test, apple_mbox_fast_rx_poll(&priv->rx_fast_timer),
			HRTIMER_RESTART);

	/* The interrupt delivers again and proves the line again. */
	apple_mbox_recv_irq(mbox->irq_recv_not_empty, mbox);
	KUNIT_EXPECT_TRUE(test, priv->rx_irq_proven);
	apple_g17_rx_fifo(mbox, false);
	KUNIT_EXPECT_TRUE(test, apple_g17_send_leaves_fast_poller(test, priv));
}

/* The storm guard cuts a proven line that fires with nothing to read, starts
 * the jiffy poller and takes the proof back, so a send fast-polls again.
 */
static void apple_g17_rx_storm_guard_test(struct kunit *test)
{
	struct apple_g17_rx_fixture *f = apple_g17_rx_fixture(test);
	struct apple_mbox_priv *priv = f->priv;
	struct apple_mbox *mbox = &priv->mbox;
	unsigned int n;

	enable_irq(mbox->irq_recv_not_empty);
	priv->rx_irq_live = true;
	apple_g17_rx_fifo(mbox, true);
	apple_mbox_recv_irq(mbox->irq_recv_not_empty, mbox);
	KUNIT_ASSERT_TRUE(test, priv->rx_irq_proven);

	apple_g17_rx_fifo(mbox, false);
	for (n = 1; n < APPLE_ASCWRAP_V8_RX_STORM_LIMIT; n++)
		apple_mbox_recv_irq(mbox->irq_recv_not_empty, mbox);
	KUNIT_EXPECT_TRUE(test, priv->rx_irq_live);
	KUNIT_EXPECT_TRUE(test, priv->rx_irq_proven);
	KUNIT_EXPECT_TRUE(test, apple_g17_rx_irq_enabled(mbox));
	/* Nothing queued the jiffy poller yet. */
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&priv->rx_poll_work));

	apple_mbox_recv_irq(mbox->irq_recv_not_empty, mbox);
	KUNIT_EXPECT_FALSE(test, priv->rx_irq_live);
	KUNIT_EXPECT_FALSE(test, priv->rx_irq_proven);
	KUNIT_EXPECT_FALSE(test, apple_g17_rx_irq_enabled(mbox));

	/* The guard queued the jiffy poller: it takes a message left queued. */
	apple_g17_rx_fill(f);
	KUNIT_EXPECT_TRUE(test, apple_g17_rx_received(f));
	cancel_delayed_work_sync(&priv->rx_poll_work);
	apple_g17_rx_fifo(mbox, false);
	KUNIT_EXPECT_TRUE(test, apple_g17_send_fast_polls(test, f));

	/* The cut line proves nothing. */
	apple_g17_rx_fifo(mbox, true);
	apple_mbox_recv_irq(mbox->irq_recv_not_empty, mbox);
	KUNIT_EXPECT_FALSE(test, priv->rx_irq_proven);
}

/* Start enables the line, starts the jiffy poller and clears the proof;
 * stop disables the line, stops the poller and clears the proof.
 */
static void apple_g17_rx_start_stop_test(struct kunit *test)
{
	struct apple_g17_rx_fixture *f = apple_g17_rx_fixture(test);
	struct apple_mbox_priv *priv = f->priv;
	struct apple_mbox *mbox = &priv->mbox;
	unsigned long flags;
	bool proven;

	apple_g17_rx_fifo(mbox, false);
	priv->rx_irq_proven = true;
	apple_mbox_v8_rx_start(mbox);
	KUNIT_EXPECT_TRUE(test, priv->rx_irq_live);
	KUNIT_EXPECT_FALSE(test, priv->rx_irq_proven);
	KUNIT_EXPECT_TRUE(test, apple_g17_rx_irq_enabled(mbox));

	/* Start queued the jiffy poller: it takes a message left queued. */
	apple_g17_rx_fill(f);
	KUNIT_EXPECT_TRUE(test, apple_g17_rx_received(f));
	apple_g17_rx_fifo(mbox, false);

	spin_lock_irqsave(&mbox->rx_lock, flags);
	apple_mbox_rx_irq_guard(mbox, 1);
	proven = priv->rx_irq_proven;
	spin_unlock_irqrestore(&mbox->rx_lock, flags);
	KUNIT_EXPECT_TRUE(test, proven);

	mbox->active = false;
	apple_mbox_v8_rx_stop(mbox);
	KUNIT_EXPECT_FALSE(test, priv->rx_irq_live);
	KUNIT_EXPECT_FALSE(test, priv->rx_irq_proven);
	KUNIT_EXPECT_FALSE(test, apple_g17_rx_irq_enabled(mbox));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&priv->rx_poll_work));
}

static struct kunit_case apple_g17_stall_cases[] = {
	KUNIT_CASE(apple_g17_rx_flood_bounded_test),
	KUNIT_CASE(apple_g17_tx_stall_bounded_test),
	KUNIT_CASE(apple_g17_rx_util_min_test),
	KUNIT_CASE(apple_g17_rx_irq_proof_test),
	KUNIT_CASE(apple_g17_rx_watchdog_test),
	KUNIT_CASE(apple_g17_rx_storm_guard_test),
	KUNIT_CASE(apple_g17_rx_start_stop_test),
	{ }
};
static struct kunit_suite apple_g17_stall_suite = {
	.name = "apple_mbox_g17_stall",
	.test_cases = apple_g17_stall_cases,
};
kunit_test_suite(apple_g17_stall_suite);

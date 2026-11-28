// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple PCIe PIODMA stop handling, included from
 * apple-piodma.c.
 *
 * The engine is a plain memory register file: it is seen stopped once
 * FIFO_STATUS reports the whole FIFO available, and an engine whose FIFO
 * never drains does not stop.
 *
 * Copyright The Aurora Silicon Contributors
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>

/* Up to the last map register */
#define PIODMA_TEST_ENGINE_SIZE		(APPLE_PIODMA_REG_MAP_BASE + \
					 APPLE_PIODMA_ROUTE_COUNT * \
					 APPLE_PIODMA_REG_MAP_STRIDE)
#define PIODMA_TEST_FABRIC_SIZE		0x10
#define PIODMA_TEST_WAIT_MS		2000

struct piodma_test {
	struct apple_piodma piodma;
	atomic_t freed;
	atomic_t callbacks;
	enum dmaengine_tx_result result;
	struct completion synchronized;
	bool sync_started;
};

static struct piodma_test *to_piodma_test(struct virt_dma_desc *vd)
{
	return container_of(to_apple_piodma(vd->tx.chan), struct piodma_test,
			    piodma);
}

static void piodma_test_desc_free(struct virt_dma_desc *vd)
{
	atomic_inc(&to_piodma_test(vd)->freed);
	apple_piodma_desc_free(vd);
}

static void piodma_test_callback(void *param,
				 const struct dmaengine_result *result)
{
	struct piodma_test *t = param;

	t->result = result->result;
	atomic_inc(&t->callbacks);
}

static u32 piodma_test_reg(struct piodma_test *t, u32 reg)
{
	return readl(t->piodma.engine + reg);
}

static void piodma_test_set_stuck(struct piodma_test *t, bool stuck)
{
	writel(stuck ? 0 : APPLE_PIODMA_FIFO_DEPTH,
	       t->piodma.engine + APPLE_PIODMA_REG_FIFO_STATUS);
}

static struct piodma_test *piodma_test_alloc(struct kunit *test)
{
	struct apple_piodma *piodma;
	struct piodma_test *t;
	struct device *dev;

	dev = kunit_device_register(test, "apple-piodma-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	piodma = &t->piodma;
	piodma->dev = dev;
	piodma->engine = (void __iomem *)kunit_kzalloc(test,
			PIODMA_TEST_ENGINE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, (void __force *)piodma->engine);
	piodma->fabric = (void __iomem *)kunit_kzalloc(test,
			PIODMA_TEST_FABRIC_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, (void __force *)piodma->fabric);
	piodma->slot = kunit_kzalloc(test, sizeof(*piodma->slot), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, piodma->slot);
	piodma->irq = 0;

	mutex_init(&piodma->reset_lock);
	INIT_DELAYED_WORK(&piodma->reset_work, apple_piodma_reset_work);
	piodma->vc.desc_free = piodma_test_desc_free;
	piodma->dma.dev = dev;
	INIT_LIST_HEAD(&piodma->dma.channels);
	vchan_init(&piodma->vc, &piodma->dma);
	init_completion(&t->synchronized);

	/* As after runtime resume: configured and running. */
	piodma_test_set_stuck(t, false);
	KUNIT_ASSERT_EQ(test, apple_piodma_hw_init(piodma), 0);
	test->priv = t;
	return t;
}

static void piodma_test_exit(struct kunit *test)
{
	struct piodma_test *t = test->priv;

	if (!t)
		return;
	piodma_test_set_stuck(t, false);
	if (t->sync_started)
		wait_for_completion(&t->synchronized);
	apple_piodma_terminate_all(&t->piodma.vc.chan);
	apple_piodma_synchronize(&t->piodma.vc.chan);
	cancel_delayed_work_sync(&t->piodma.reset_work);
	vchan_free_chan_resources(&t->piodma.vc);
	tasklet_kill(&t->piodma.vc.task);
	mutex_destroy(&t->piodma.reset_lock);
}

static void piodma_test_submit(struct kunit *test, struct piodma_test *t,
			       dma_addr_t dst, dma_addr_t src)
{
	struct dma_async_tx_descriptor *tx;

	tx = apple_piodma_prep_memcpy(&t->piodma.vc.chan, dst, src, SZ_64,
				      DMA_PREP_INTERRUPT);
	KUNIT_ASSERT_NOT_NULL(test, tx);
	tx->callback_result = piodma_test_callback;
	tx->callback_param = t;
	KUNIT_ASSERT_GE(test, dmaengine_submit(tx), 0);
}

static int piodma_test_synchronize_fn(void *data)
{
	struct piodma_test *t = data;

	apple_piodma_synchronize(&t->piodma.vc.chan);
	complete_all(&t->synchronized);
	return 0;
}

static bool piodma_test_synced(struct piodma_test *t, unsigned int ms)
{
	return wait_for_completion_timeout(&t->synchronized,
					   msecs_to_jiffies(ms));
}

static bool piodma_test_wait(atomic_t *counter, int value)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(PIODMA_TEST_WAIT_MS);

	while (atomic_read(counter) < value) {
		if (time_after(jiffies, timeout))
			return false;
		msleep(20);
	}
	return true;
}

/* A stopped engine: synchronize returns and the request is freed. */
static void piodma_terminate_stopped_test(struct kunit *test)
{
	struct piodma_test *t = piodma_test_alloc(test);

	piodma_test_submit(test, t, SZ_8K, SZ_4K);
	apple_piodma_issue_pending(&t->piodma.vc.chan);
	KUNIT_EXPECT_TRUE(test, piodma_test_reg(t, APPLE_PIODMA_REG_COMMAND) &
			  APPLE_PIODMA_COMMAND_RUN);

	apple_piodma_terminate_all(&t->piodma.vc.chan);
	apple_piodma_synchronize(&t->piodma.vc.chan);
	KUNIT_EXPECT_FALSE(test, t->piodma.stopping);
	KUNIT_EXPECT_FALSE(test, t->piodma.halted);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->freed), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->callbacks), 0);
}

/*
 * An engine that does not stop: synchronize waits, and keeps the request,
 * until the engine is seen stopped.
 */
static void piodma_terminate_unconfirmed_test(struct kunit *test)
{
	struct piodma_test *t = piodma_test_alloc(test);
	struct task_struct *task;

	piodma_test_submit(test, t, SZ_8K, SZ_4K);
	apple_piodma_issue_pending(&t->piodma.vc.chan);
	piodma_test_set_stuck(t, true);
	apple_piodma_terminate_all(&t->piodma.vc.chan);
	KUNIT_EXPECT_TRUE(test, t->piodma.stopping);

	task = kthread_run(piodma_test_synchronize_fn, t, "piodma-test-sync");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, task);
	t->sync_started = true;
	/* Several stop attempts, each failing. */
	KUNIT_EXPECT_FALSE(test, piodma_test_synced(t, 5 * APPLE_PIODMA_RESET_RETRY_MS));
	KUNIT_EXPECT_TRUE(test, t->piodma.stopping);
	KUNIT_EXPECT_TRUE(test, t->piodma.halted);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->freed), 0);

	piodma_test_set_stuck(t, false);
	KUNIT_EXPECT_TRUE(test, piodma_test_synced(t, PIODMA_TEST_WAIT_MS));
	KUNIT_EXPECT_FALSE(test, t->piodma.stopping);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->freed), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->callbacks), 0);
}

/*
 * A request that failed on an engine that does not stop: its client is not
 * told before the engine is seen stopped, and the next request waits.
 */
static void piodma_failed_unconfirmed_test(struct kunit *test)
{
	struct piodma_test *t = piodma_test_alloc(test);
	struct apple_piodma *piodma = &t->piodma;
	struct apple_piodma_desc *first;
	u32 packet1;

	piodma_test_submit(test, t, SZ_8K, SZ_4K);
	piodma_test_submit(test, t, SZ_16K, SZ_4K);
	apple_piodma_issue_pending(&piodma->vc.chan);
	first = piodma->active;
	KUNIT_ASSERT_NOT_NULL(test, first);
	packet1 = le32_to_cpu(piodma->slot->packet[1]);

	/* An error interrupt, and the engine does not stop. */
	piodma_test_set_stuck(t, true);
	writel(BIT(1), piodma->engine + APPLE_PIODMA_REG_IRQ_STATUS);
	KUNIT_EXPECT_EQ(test, apple_piodma_irq(0, piodma), IRQ_HANDLED);
	writel(0, piodma->engine + APPLE_PIODMA_REG_IRQ_STATUS);
	KUNIT_EXPECT_PTR_EQ(test, piodma->failed, first);
	KUNIT_EXPECT_NULL(test, piodma->active);
	KUNIT_EXPECT_TRUE(test, piodma->stopping);

	flush_delayed_work(&piodma->reset_work);
	msleep(2 * APPLE_PIODMA_RESET_RETRY_MS);
	KUNIT_EXPECT_TRUE(test, piodma->stopping);
	KUNIT_EXPECT_PTR_EQ(test, piodma->failed, first);
	KUNIT_EXPECT_NULL(test, piodma->active);
	KUNIT_EXPECT_FALSE(test, piodma_test_reg(t, APPLE_PIODMA_REG_COMMAND) &
			   APPLE_PIODMA_COMMAND_RUN);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(piodma->slot->packet[1]), packet1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->callbacks), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->freed), 0);

	/* Seen stopped: the failure is reported and the next request runs. */
	piodma_test_set_stuck(t, false);
	flush_delayed_work(&piodma->reset_work);
	KUNIT_EXPECT_FALSE(test, piodma->stopping);
	KUNIT_EXPECT_NULL(test, piodma->failed);
	KUNIT_EXPECT_NOT_NULL(test, piodma->active);
	KUNIT_EXPECT_PTR_NE(test, piodma->active, first);
	KUNIT_EXPECT_TRUE(test, piodma_test_reg(t, APPLE_PIODMA_REG_COMMAND) &
			  APPLE_PIODMA_COMMAND_RUN);
	KUNIT_EXPECT_TRUE(test, piodma_test_wait(&t->callbacks, 1));
	KUNIT_EXPECT_EQ(test, t->result, DMA_TRANS_ABORTED);
	KUNIT_EXPECT_TRUE(test, piodma_test_wait(&t->freed, 1));
}

/* Terminating a failed request on an engine that does not stop holds it. */
static void piodma_failed_terminate_test(struct kunit *test)
{
	struct piodma_test *t = piodma_test_alloc(test);
	struct apple_piodma *piodma = &t->piodma;
	struct task_struct *task;

	piodma_test_submit(test, t, SZ_8K, SZ_4K);
	apple_piodma_issue_pending(&piodma->vc.chan);
	piodma_test_set_stuck(t, true);
	writel(BIT(1), piodma->engine + APPLE_PIODMA_REG_IRQ_STATUS);
	apple_piodma_irq(0, piodma);
	writel(0, piodma->engine + APPLE_PIODMA_REG_IRQ_STATUS);
	apple_piodma_terminate_all(&piodma->vc.chan);
	KUNIT_EXPECT_NULL(test, piodma->failed);

	task = kthread_run(piodma_test_synchronize_fn, t, "piodma-test-sync");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, task);
	t->sync_started = true;
	KUNIT_EXPECT_FALSE(test, piodma_test_synced(t, 3 * APPLE_PIODMA_RESET_RETRY_MS));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->freed), 0);

	piodma_test_set_stuck(t, false);
	KUNIT_EXPECT_TRUE(test, piodma_test_synced(t, PIODMA_TEST_WAIT_MS));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->freed), 1);
	/* A terminated request's client is not called back. */
	KUNIT_EXPECT_EQ(test, atomic_read(&t->callbacks), 0);
}

static struct kunit_case apple_piodma_stop_test_cases[] = {
	KUNIT_CASE(piodma_terminate_stopped_test),
	KUNIT_CASE_SLOW(piodma_terminate_unconfirmed_test),
	KUNIT_CASE_SLOW(piodma_failed_unconfirmed_test),
	KUNIT_CASE_SLOW(piodma_failed_terminate_test),
	{}
};

static struct kunit_suite apple_piodma_stop_test_suite = {
	.name = "apple-piodma-stop",
	.exit = piodma_test_exit,
	.test_cases = apple_piodma_stop_test_cases,
};

kunit_test_suite(apple_piodma_stop_test_suite);

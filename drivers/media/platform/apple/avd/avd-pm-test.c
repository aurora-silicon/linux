// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/io.h>

#include "avd.h"
#include "avd-regs.h"

struct avd_pm_test {
	struct avd_dev avd;
	bool queue_initialized;
	struct avd_deferred_unmap unmap;
	unsigned int draining;
	unsigned int step;
	unsigned int unmap_step;
	unsigned int boot_step;
	unsigned int shutdown_step;
	unsigned int resubmit_step;
	unsigned int perf_put_step;
	unsigned int shutdowns;
	unsigned int submissions;
	unsigned int perf_gets;
	unsigned int perf_puts;
	unsigned int perf_sync_puts;
	unsigned int perf_suspends;
	int perf_get_error;
	int boot_error;
	int suspend_error;
	bool unmapped_active;
};

static struct avd_pm_test *avd_pm_fixture(struct avd_dev *avd)
{
	return container_of(avd, struct avd_pm_test, avd);
}

static u64 avd_pm_test_now_ns;

static u64 avd_pm_test_clock(void)
{
	return avd_pm_test_now_ns;
}

static int avd_pm_test_perf_get(struct avd_dev *avd)
{
	struct avd_pm_test *fixture = avd_pm_fixture(avd);

	if (fixture->perf_get_error)
		return fixture->perf_get_error;
	fixture->perf_gets++;
	return 0;
}

static void avd_pm_test_perf_put(struct avd_dev *avd, bool sync)
{
	struct avd_pm_test *fixture = avd_pm_fixture(avd);

	fixture->perf_put_step = ++fixture->step;
	fixture->perf_puts++;
	if (sync)
		fixture->perf_sync_puts++;
}

static void avd_pm_test_perf_suspend(struct avd_dev *avd)
{
	avd_pm_fixture(avd)->perf_suspends++;
}

static const struct avd_pm_perf_ops avd_pm_test_perf_ops = {
	.get = avd_pm_test_perf_get,
	.put = avd_pm_test_perf_put,
	.suspend = avd_pm_test_perf_suspend,
};

static void avd_pm_test_enable_perf(struct avd_dev *avd)
{
	avd_pm_test_now_ns = 1000ULL * NSEC_PER_MSEC;
	avd->pm.clock_ns = avd_pm_test_clock;
	avd->pm.perf_ops = &avd_pm_test_perf_ops;
}

static bool avd_pm_test_idle(struct avd_dev *avd)
{
	struct avd_pm_test *fixture = avd_pm_fixture(avd);

	return avd_pm_can_suspend(fixture->avd.frames, fixture->draining);
}

static int avd_pm_test_drain(struct avd_dev *avd)
{
	return avd_pm_test_idle(avd) ? 0 : -EBUSY;
}

static int avd_pm_test_boot(struct avd_dev *avd)
{
	struct avd_pm_test *fixture = avd_pm_fixture(avd);

	fixture->boot_step = ++fixture->step;
	return fixture->boot_error;
}

static void avd_pm_test_shutdown(struct avd_dev *avd)
{
	struct avd_pm_test *fixture = avd_pm_fixture(avd);

	fixture->shutdown_step = ++fixture->step;
	fixture->shutdowns++;
}

static int avd_pm_test_suspend(struct avd_dev *avd)
{
	struct avd_pm_test *fixture = avd_pm_fixture(avd);

	if (fixture->suspend_error)
		return fixture->suspend_error;
	return avd_pm_backend_suspend(avd);
}

static int avd_pm_test_submit(struct avd_dev *avd, struct avd_frame *frame)
{
	struct avd_pm_test *fixture = avd_pm_fixture(avd);

	fixture->resubmit_step = ++fixture->step;
	fixture->submissions++;
	frame->state = AVD_FRAME_SUBMITTED;
	return 0;
}

static void avd_pm_test_resubmit(struct avd_dev *avd)
{
	struct avd_pm_test *fixture = avd_pm_fixture(avd);
	unsigned int i;

	fixture->resubmit_step = ++fixture->step;
	for (i = 0; i < AVD_MAX_FRAMES; i++)
		if (fixture->avd.frames[i].state == AVD_FRAME_RESERVED)
			avd->backend->submit(avd, &fixture->avd.frames[i]);
}

static void avd_pm_test_unmap(struct avd_dev *avd,
			      struct avd_deferred_unmap *unmap)
{
	struct avd_pm_test *fixture = avd_pm_fixture(avd);

	fixture->unmap_step = ++fixture->step;
	fixture->unmapped_active = avd->pm.dart_active;
}

static const struct avd_queue_pm_ops avd_pm_test_queue = {
	.idle = avd_pm_test_idle,
	.drain = avd_pm_test_drain,
	.resubmit = avd_pm_test_resubmit,
};

static const struct avd_backend_ops avd_pm_test_backend = {
	.name = "test",
	.boot = avd_pm_test_boot,
	.shutdown = avd_pm_test_shutdown,
	.submit = avd_pm_test_submit,
	.suspend = avd_pm_test_suspend,
	.resume = avd_pm_backend_resume,
};

static int avd_pm_test_init(struct kunit *test)
{
	struct avd_pm_test *fixture;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (!fixture)
		return -ENOMEM;
	test->priv = fixture;
	avd_pm_init(&fixture->avd, &avd_pm_test_backend, &avd_pm_test_queue);
	INIT_LIST_HEAD(&fixture->unmap.node);
	fixture->unmap.unmap = avd_pm_test_unmap;
	return 0;
}

static void avd_pm_suspend_gate_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	unsigned int i, state;

	KUNIT_EXPECT_TRUE(test, avd_pm_test_idle(&fixture->avd));
	fixture->draining = 1;
	KUNIT_EXPECT_EQ(test, avd_pm_power_off(&fixture->avd), -EBUSY);
	fixture->draining = 0;

	/* An occupied slot anywhere vetoes idle, even after completion IRQ. */
	for (i = 0; i < AVD_MAX_FRAMES; i++) {
		for (state = AVD_FRAME_RESERVED; state <= AVD_FRAME_DONE; state++) {
			fixture->avd.frames[i].state = state;
			KUNIT_EXPECT_EQ(test, avd_pm_power_off(&fixture->avd), -EBUSY);
		}
		fixture->avd.frames[i].state = AVD_FRAME_FREE;
	}
	KUNIT_EXPECT_EQ(test, fixture->shutdowns, 0U);
	KUNIT_EXPECT_EQ(test, avd_pm_power_off(&fixture->avd), 0);
	KUNIT_EXPECT_EQ(test, fixture->shutdowns, 1U);
}

static void avd_pm_resume_order_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;

	avd_pm_unmap(avd, &fixture->unmap);
	KUNIT_EXPECT_EQ(test, fixture->unmap_step, 0U);
	fixture->avd.frames[0].state = AVD_FRAME_RESERVED;
	fixture->avd.frames[1].state = AVD_FRAME_SUBMITTED;
	fixture->avd.frames[2].state = AVD_FRAME_DONE;
	fixture->avd.frames[15].state = AVD_FRAME_RESERVED;
	KUNIT_ASSERT_EQ(test, avd_pm_power_on(avd), 0);
	KUNIT_EXPECT_TRUE(test, fixture->unmapped_active);
	KUNIT_EXPECT_LT(test, fixture->unmap_step, fixture->boot_step);
	KUNIT_EXPECT_LT(test, fixture->boot_step, fixture->resubmit_step);
	KUNIT_EXPECT_EQ(test, fixture->submissions, 2U);
	KUNIT_EXPECT_EQ(test, fixture->avd.frames[0].state, AVD_FRAME_SUBMITTED);
	KUNIT_EXPECT_EQ(test, fixture->avd.frames[2].state, AVD_FRAME_DONE);
	KUNIT_EXPECT_TRUE(test, list_empty(&avd->pm.deferred_unmaps));
}

static void avd_pm_resume_failure_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;

	fixture->avd.frames[0].state = AVD_FRAME_RESERVED;
	fixture->boot_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, avd_pm_power_on(avd), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, avd->pm.dart_active);
	KUNIT_EXPECT_EQ(test, fixture->resubmit_step, 0U);
	KUNIT_EXPECT_EQ(test, fixture->shutdowns, 1U);
	KUNIT_EXPECT_EQ(test, fixture->avd.frames[0].state, AVD_FRAME_RESERVED);

	/* A later successful resume retries the still-unposted frame once. */
	fixture->boot_error = 0;
	KUNIT_ASSERT_EQ(test, avd_pm_power_on(avd), 0);
	KUNIT_EXPECT_EQ(test, fixture->submissions, 1U);
}

static void avd_pm_suspend_failure_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;

	KUNIT_ASSERT_EQ(test, avd_pm_power_on(avd), 0);
	fixture->suspend_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, avd_pm_power_off(avd), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, avd->pm.dart_active);
	KUNIT_EXPECT_EQ(test, fixture->shutdowns, 0U);
}

static void avd_pm_deferred_unmap_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	unsigned int step;

	KUNIT_ASSERT_EQ(test, avd_pm_power_on(avd), 0);
	avd_pm_unmap(avd, &fixture->unmap);
	KUNIT_EXPECT_TRUE(test, fixture->unmapped_active);
	KUNIT_EXPECT_TRUE(test, list_empty(&avd->pm.deferred_unmaps));
	KUNIT_ASSERT_EQ(test, avd_pm_power_off(avd), 0);
	step = fixture->unmap_step;
	avd_pm_unmap(avd, &fixture->unmap);
	KUNIT_EXPECT_EQ(test, fixture->unmap_step, step);
	KUNIT_ASSERT_EQ(test, avd_pm_power_on(avd), 0);
	KUNIT_EXPECT_GT(test, fixture->unmap_step, step);
	KUNIT_EXPECT_TRUE(test, list_empty(&avd->pm.deferred_unmaps));
}

static void avd_pm_real_queue_resume_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	struct avd_ctx *ctx;
	unsigned int i;

	avd->dev = kunit_device_register(test, "avd-pm-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, avd->dev);
	KUNIT_ASSERT_EQ(test, avd_queue_init(avd), 0);
	fixture->queue_initialized = true;
	avd->backend = &avd_pm_test_backend;
	avd->pm.queue = &avd_pm_queue_ops;
	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->dev = avd;
	for (i = 0; i < 3; i++) {
		avd->frames[i].ctx = ctx;
		avd->frames[i].seq = i + 1;
		avd->frames[i].deadline = jiffies + msecs_to_jiffies(60000);
		avd->frames[i].state = AVD_FRAME_RESERVED;
	}
	/* This one was posted before the power transition; never replay it. */
	avd->frames[1].state = AVD_FRAME_SUBMITTED;
	avd->inflight = 3;
	KUNIT_ASSERT_EQ(test, avd_pm_power_on(avd), 0);
	flush_delayed_work(&avd->queue_work);
	cancel_delayed_work_sync(&avd->queue_work);
	KUNIT_EXPECT_EQ(test, fixture->submissions, 2U);
	KUNIT_EXPECT_LT(test, fixture->boot_step, fixture->resubmit_step);
	KUNIT_EXPECT_EQ(test, avd->frames[0].state, AVD_FRAME_SUBMITTED);
	KUNIT_EXPECT_EQ(test, avd->frames[2].state, AVD_FRAME_SUBMITTED);
}

static void avd_pm_real_queue_gate_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;

	spin_lock_init(&avd->queue_lock);
	avd->pm.queue = &avd_pm_queue_ops;
	avd_pm_drain_begin(avd);
	KUNIT_EXPECT_EQ(test, avd_pm_power_off(avd), -EBUSY);
	avd_pm_drain_end(avd);
	avd->frames[15].state = AVD_FRAME_DONE;
	KUNIT_EXPECT_EQ(test, avd_pm_power_off(avd), -EBUSY);
	avd->frames[15].state = AVD_FRAME_FREE;
	KUNIT_EXPECT_EQ(test, avd_pm_power_off(avd), 0);
}

static void avd_pm_open_boot_replay_test(struct kunit *test)
{
	static const struct avd_variant variant = { .revision = 3 };
	static const u8 image[] = { 0x12, 0x34, 0x56, 0x78 };
	static const struct firmware fw = { .data = image, .size = sizeof(image) };
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	struct resource code = {
		.name = "code", .flags = IORESOURCE_MEM,
		.start = 0x1000, .end = 0x10ff,
	};
	struct platform_device *pdev;
	u8 observed[sizeof(image)];
	unsigned int i;

	pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdev);
	pdev->resource = &code;
	pdev->num_resources = 1;
	avd->pdev = pdev;
	avd->variant = &variant;
	avd->fw = &fw;
	avd->backend = &avd_open_backend;
	/* Ordinary test RAM, never a mapped target aperture. */
	avd->code = (__force void __iomem *)kunit_kzalloc(test, 0x100, GFP_KERNEL);
	avd->mbox = (__force void __iomem *)kunit_kzalloc(test, 0x100, GFP_KERNEL);
	avd->pm.piodma = (__force void __iomem *)kunit_kzalloc(test, 0x28, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, (__force void *)avd->code);
	KUNIT_ASSERT_NOT_NULL(test, (__force void *)avd->mbox);
	KUNIT_ASSERT_NOT_NULL(test, (__force void *)avd->pm.piodma);
	avd->pm.sram_phys = 0x123456780ULL;
	for (i = 0; i < 2; i++) {
		/* Model loss of both ITCM and PIODMA configuration between boots. */
		memset_io(avd->code, 0, sizeof(image));
		writel(0, avd->pm.piodma + 0x24);
		/* The fixture supplies firmware readiness; no firmware is executed. */
		writel(1, avd->mbox + AVD_REG_FLAG0_SET);
		KUNIT_ASSERT_EQ(test, avd_backend_boot(avd), 0);
		KUNIT_EXPECT_EQ(test, readl(avd->pm.piodma + 0x24), 0x12345678U);
		memcpy_fromio(observed, avd->code, sizeof(observed));
		KUNIT_EXPECT_MEMEQ(test, observed, image, sizeof(image));
		KUNIT_EXPECT_EQ(test, readl(avd->mbox + AVD_REG_RUN_CTRL),
				(u32)AVD_RUN_CTRL_UNK_RUN);
		KUNIT_EXPECT_EQ(test, readl(avd->mbox + AVD_REG_MBOX1_STATUS),
				(u32)AVD_MBOX_ENABLE);
		KUNIT_EXPECT_EQ(test, readl(avd->mbox + AVD_REG_MBOX_IRQ_ENABLE),
				(u32)AVD_MBOX1_NOT_EMPTY);
		avd_shutdown(avd);
		KUNIT_EXPECT_EQ(test, readl(avd->mbox + AVD_REG_RUN_CTRL),
				(u32)(AVD_RUN_CTRL_UNK_STOP));
		KUNIT_EXPECT_EQ(test, readl(avd->mbox + AVD_REG_FLAG0_CLR), 1U);
		KUNIT_EXPECT_EQ(test, readl(avd->mbox + AVD_REG_MBOX_IRQ_ENABLE), 0U);
	}
}

static void avd_pm_recovery_dart_gate_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	struct iommu_domain domain = {};

	/* A stopped device cannot be reset through powered-off registers. */
	KUNIT_EXPECT_EQ(test, avd_pm_recover(avd), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, fixture->shutdowns, 0U);
	avd->pm.dart_active = true;
	avd->domain = &domain;
	/* No empty domain means no valid DART flush: never reboot anyway. */
	KUNIT_EXPECT_EQ(test, avd_pm_recover(avd), -ENODEV);
	KUNIT_EXPECT_EQ(test, fixture->shutdowns, 1U);
	KUNIT_EXPECT_EQ(test, fixture->boot_step, 0U);
}

static void avd_pm_recovery_boot_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;

	avd->pm.dart_active = true;
	fixture->boot_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, avd_pm_recover(avd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, fixture->shutdowns, 2U);
	KUNIT_EXPECT_EQ(test, fixture->resubmit_step, 0U);
	fixture->boot_error = 0;
	KUNIT_EXPECT_EQ(test, avd_pm_recover(avd), 0);
	KUNIT_EXPECT_EQ(test, avd->fw_generation, 1ULL);
	/* Recovery must fail old slots before core reopens the host IRQ. */
	KUNIT_EXPECT_FALSE(test, avd->pm.irq_enabled);
	KUNIT_EXPECT_EQ(test, fixture->resubmit_step, 0U);
}

static void avd_pm_parked_resume_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;

	/* Autosuspend/resume must not boot or reopen a resting/retired engine. */
	avd->pm.dma_parked = true;
	avd->rest_until = jiffies + msecs_to_jiffies(60000);
	KUNIT_EXPECT_EQ(test, avd_pm_power_on(avd), 0);
	KUNIT_EXPECT_EQ(test, fixture->boot_step, 0U);
	KUNIT_EXPECT_EQ(test, fixture->resubmit_step, 0U);
	KUNIT_EXPECT_FALSE(test, avd->pm.irq_enabled);
	KUNIT_EXPECT_EQ(test, avd_pm_power_off(avd), 0);
	KUNIT_EXPECT_TRUE(test, avd->pm.dma_parked);
	avd->rest_until = 0;
	avd->faulted = true;
	KUNIT_EXPECT_EQ(test, avd_pm_power_on(avd), 0);
	KUNIT_EXPECT_EQ(test, fixture->boot_step, 0U);
	KUNIT_EXPECT_EQ(test, fixture->resubmit_step, 0U);
	KUNIT_EXPECT_FALSE(test, avd->pm.irq_enabled);
}

static void avd_pm_perf_low_demand_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	int stream_a;
	unsigned int i;

	/* Without a "perf" domain every call is a no-op. */
	avd_pm_perf_frame_start(avd, &stream_a, 3840, 2160, 1, 1);
	avd_pm_perf_idle(avd);
	avd_pm_perf_suspend(avd);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 0U);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 0U);

	avd_pm_test_enable_perf(avd);

	/* Steady 1080p60 single-stream playback (~125 Mpx/s) stays off floor. */
	for (i = 0; i < 60; i++) {
		avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088, 1, 1);
		KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
		avd_pm_perf_idle(avd);
		avd_pm_test_now_ns += 16666667ULL;
	}
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 0U);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 0U);
}

static void avd_pm_perf_large_frame_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	int stream_a;

	avd_pm_test_enable_perf(avd);

	/* 4K engages on the first frame and does not double-acquire while held. */
	avd_pm_perf_frame_start(avd, &stream_a, 3840, 2160, 1, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 1U);

	avd_pm_test_now_ns += 5ULL * NSEC_PER_MSEC;
	avd_pm_perf_frame_start(avd, &stream_a, 3840, 2160, 2, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 1U);

	/* Idle releases asynchronously so inter-frame gaps reuse autosuspend. */
	avd_pm_perf_idle(avd);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 1U);
	KUNIT_EXPECT_EQ(test, fixture->perf_sync_puts, 0U);

	/* Power-off flushes any pending autosuspend timer synchronously. */
	KUNIT_ASSERT_EQ(test, avd_pm_power_off(avd), 0);
	KUNIT_EXPECT_EQ(test, fixture->perf_suspends, 1U);

	/* 1440p also engages immediately; parking drops it synchronously. */
	avd->pm.dart_active = true;
	avd_pm_perf_frame_start(avd, &stream_a, 2560, 1440, 1, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 2U);
	KUNIT_ASSERT_EQ(test, avd_pm_park(avd), 0);
	KUNIT_EXPECT_LT(test, fixture->shutdown_step, fixture->perf_put_step);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 2U);
	KUNIT_EXPECT_EQ(test, fixture->perf_sync_puts, 1U);
}

static void avd_pm_perf_rate_hysteresis_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	int stream_a;
	unsigned int i;

	avd_pm_test_enable_perf(avd);

	/* Unthrottled 1080p burst (>180 Mpx/s) engages perf mid-window. */
	for (i = 0; i < 24; i++) {
		avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088, 1, 1);
		avd_pm_test_now_ns += 3ULL * NSEC_PER_MSEC;
	}
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_rate_high);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 1U);

	/* Roll over into a 150 Mpx/s window (inside 135..180 Mpx/s hysteresis band). */
	avd_pm_test_now_ns += AVD_PERF_WINDOW_MS * NSEC_PER_MSEC;
	avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088, 1, 1);
	for (i = 0; i < 17; i++) {
		avd_pm_test_now_ns += 13ULL * NSEC_PER_MSEC;
		avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088, 1, 1);
	}
	avd_pm_test_now_ns += 35ULL * NSEC_PER_MSEC;
	avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088, 1, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_rate_high);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);

	/* Drop to 1080p30 (~63 Mpx/s): stays held through hold-down, then releases. */
	for (i = 0; i < 30; i++) {
		avd_pm_test_now_ns += 33333333ULL;
		avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088, 1, 1);
	}
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_rate_high);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 1U);
}

static void avd_pm_perf_concurrency_and_backlog_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	int stream_a, stream_b;

	avd_pm_test_enable_perf(avd);

	/* Simultaneous multi-stream frames engage immediately. */
	avd_pm_perf_frame_start(avd, &stream_a, 1280, 720, 2, 2);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 1U);
	avd_pm_perf_suspend(avd);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_sync_puts, 1U);

	/* Alternating streams on a 1-slot backend also trigger within a window. */
	avd_pm_test_now_ns += 1000ULL * NSEC_PER_MSEC;
	avd_pm_perf_frame_start(avd, &stream_a, 1280, 720, 1, 1);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	avd_pm_test_now_ns += 16ULL * NSEC_PER_MSEC;
	avd_pm_perf_frame_start(avd, &stream_b, 1280, 720, 1, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 2U);
	avd_pm_perf_suspend(avd);

	/* Queue backlog (>= AVD_PERF_INFLIGHT_HIGH) engages immediately. */
	avd_pm_test_now_ns += 1000ULL * NSEC_PER_MSEC;
	avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088,
				AVD_PERF_INFLIGHT_HIGH - 1, 1);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088,
				AVD_PERF_INFLIGHT_HIGH, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 3U);
	avd_pm_perf_idle(avd);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
}

static void avd_pm_perf_clock_and_gap_reset_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	int stream_a, stream_b;
	unsigned int i;

	avd_pm_test_enable_perf(avd);

	/* Engage high-rate and multi-stream demand inside one window. */
	for (i = 0; i < 24; i++) {
		avd_pm_perf_frame_start(avd, (i & 1) ? &stream_b : &stream_a,
					1920, 1088, 1, 1);
		avd_pm_test_now_ns += 3ULL * NSEC_PER_MSEC;
	}
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_rate_high);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_window_multi);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 1U);

	/* A gap >= 2 * window resets rate/multi state and releases on low load. */
	avd_pm_test_now_ns += 2ULL * AVD_PERF_WINDOW_MS * NSEC_PER_MSEC;
	avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088, 1, 1);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_rate_high);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_window_multi);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 1U);

	/* Re-engage high demand, then jump the clock backwards mid-window. */
	for (i = 0; i < 24; i++) {
		avd_pm_perf_frame_start(avd, (i & 1) ? &stream_b : &stream_a,
					1920, 1088, 1, 1);
		avd_pm_test_now_ns += 3ULL * NSEC_PER_MSEC;
	}
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_rate_high);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_window_multi);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 2U);

	avd_pm_test_now_ns = avd->pm.perf_window_start_ns - 50ULL * NSEC_PER_MSEC;
	avd_pm_perf_frame_start(avd, &stream_a, 1920, 1088, 1, 1);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_rate_high);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_window_multi);
	KUNIT_EXPECT_EQ(test, avd->pm.perf_hold_until_ns, 0ULL);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 2U);
}

static void avd_pm_perf_failed_park_and_recover_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	struct iommu_domain domain = {};
	int stream_a;

	avd_pm_test_enable_perf(avd);

	/* Early -EHOSTDOWN return from avd_pm_park() still drops perf_held. */
	avd_pm_perf_frame_start(avd, &stream_a, 3840, 2160, 1, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, avd_pm_park(avd), -EHOSTDOWN);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 1U);
	KUNIT_EXPECT_EQ(test, fixture->perf_sync_puts, 1U);

	/* Failed DART parking (-ENODEV) shuts down first, then drops perf_held. */
	avd->pm.dart_active = true;
	avd->domain = &domain;
	avd_pm_perf_frame_start(avd, &stream_a, 3840, 2160, 1, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, avd_pm_recover(avd), -ENODEV);
	KUNIT_EXPECT_EQ(test, fixture->shutdowns, 1U);
	KUNIT_EXPECT_LT(test, fixture->shutdown_step, fixture->perf_put_step);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 2U);
	KUNIT_EXPECT_EQ(test, fixture->perf_sync_puts, 2U);

	/* Failed recovery reboot re-parks without double-putting perf_held. */
	avd->domain = NULL;
	fixture->boot_error = -ETIMEDOUT;
	avd_pm_perf_frame_start(avd, &stream_a, 3840, 2160, 1, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, avd_pm_recover(avd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, fixture->shutdowns, 3U);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 3U);
	KUNIT_EXPECT_EQ(test, fixture->perf_sync_puts, 3U);
	KUNIT_EXPECT_EQ(test, fixture->perf_suspends, 1U);
}

static void avd_pm_perf_error_handling_test(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;
	struct avd_dev *avd = &fixture->avd;
	int stream_a;

	avd_pm_test_enable_perf(avd);

	/* Failed floor acquisition never sets perf_held or causes an unmatched put. */
	fixture->perf_get_error = -EIO;
	avd_pm_perf_frame_start(avd, &stream_a, 3840, 2160, 1, 1);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 0U);
	avd_pm_perf_idle(avd);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 0U);

	/* Recovery once floor acquisition succeeds again. */
	fixture->perf_get_error = 0;
	avd_pm_perf_frame_start(avd, &stream_a, 3840, 2160, 1, 1);
	KUNIT_EXPECT_TRUE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_gets, 1U);
	avd_pm_perf_suspend(avd);
	KUNIT_EXPECT_FALSE(test, avd->pm.perf_held);
	KUNIT_EXPECT_EQ(test, fixture->perf_puts, 1U);
	KUNIT_EXPECT_EQ(test, fixture->perf_sync_puts, 1U);
}

static void avd_pm_test_exit(struct kunit *test)
{
	struct avd_pm_test *fixture = test->priv;

	if (fixture->queue_initialized)
		cancel_delayed_work_sync(&fixture->avd.queue_work);
}

static struct kunit_case avd_pm_test_cases[] = {
	KUNIT_CASE(avd_pm_suspend_gate_test),
	KUNIT_CASE(avd_pm_resume_order_test),
	KUNIT_CASE(avd_pm_resume_failure_test),
	KUNIT_CASE(avd_pm_suspend_failure_test),
	KUNIT_CASE(avd_pm_deferred_unmap_test),
	KUNIT_CASE(avd_pm_real_queue_resume_test),
	KUNIT_CASE(avd_pm_real_queue_gate_test),
	KUNIT_CASE(avd_pm_open_boot_replay_test),
	KUNIT_CASE(avd_pm_recovery_dart_gate_test),
	KUNIT_CASE(avd_pm_recovery_boot_test),
	KUNIT_CASE(avd_pm_parked_resume_test),
	KUNIT_CASE(avd_pm_perf_low_demand_test),
	KUNIT_CASE(avd_pm_perf_large_frame_test),
	KUNIT_CASE(avd_pm_perf_rate_hysteresis_test),
	KUNIT_CASE(avd_pm_perf_concurrency_and_backlog_test),
	KUNIT_CASE(avd_pm_perf_clock_and_gap_reset_test),
	KUNIT_CASE(avd_pm_perf_failed_park_and_recover_test),
	KUNIT_CASE(avd_pm_perf_error_handling_test),
	{}
};

static struct kunit_suite avd_pm_test_suite = {
	.name = "apple-avd-pm",
	.init = avd_pm_test_init,
	.exit = avd_pm_test_exit,
	.test_cases = avd_pm_test_cases,
};

kunit_test_suite(avd_pm_test_suite);

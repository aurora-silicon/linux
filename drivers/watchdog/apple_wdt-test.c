// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple SoC watchdog driver: the timeout to counter
 * tick conversion and its clamp to the 32-bit counter, the time left, and
 * which registers start, stop, ping, restart, suspend and resume write.
 *
 * The watchdog operations run on a register block in kernel memory with the
 * clock rate set directly, so the tests need no Apple hardware.
 *
 * Included by apple_wdt.c.
 */

#include <kunit/device.h>
#include <kunit/test.h>

#define APPLE_WDT_TEST_REGS	0x30
#define APPLE_WDT_TEST_RATE	24000000UL
/* What every register holds before the operation under test. */
#define APPLE_WDT_TEST_FILL(reg)	(0xa5000000 | (reg))

struct apple_wdt_test {
	struct apple_wdt wdt;
	struct device *dev;
	u32 expect[APPLE_WDT_TEST_REGS / sizeof(u32)];
};

static void apple_wdt_test_set_rate(struct apple_wdt_test *ctx, unsigned long rate)
{
	ctx->wdt.clk_rate = rate;
	/* as apple_wdt_probe() */
	ctx->wdt.wdd.max_hw_heartbeat_ms = U32_MAX / rate * 1000;
}

static int apple_wdt_test_init(struct kunit *test)
{
	struct apple_wdt_test *ctx;
	unsigned int reg;
	void *regs;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;

	regs = kunit_kzalloc(test, APPLE_WDT_TEST_REGS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, regs);
	ctx->wdt.regs = (void __iomem __force *)regs;

	ctx->dev = kunit_device_register(test, "apple-wdt-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->dev);
	dev_set_drvdata(ctx->dev, &ctx->wdt);

	ctx->wdt.wdd.parent = ctx->dev;
	ctx->wdt.wdd.ops = &apple_wdt_ops;
	ctx->wdt.wdd.info = &apple_wdt_info;
	ctx->wdt.wdd.timeout = APPLE_WDT_TIMEOUT_DEFAULT;
	apple_wdt_test_set_rate(ctx, APPLE_WDT_TEST_RATE);

	for (reg = 0; reg < APPLE_WDT_TEST_REGS; reg += sizeof(u32)) {
		writel(APPLE_WDT_TEST_FILL(reg), ctx->wdt.regs + reg);
		ctx->expect[reg / sizeof(u32)] = APPLE_WDT_TEST_FILL(reg);
	}

	return 0;
}

static void apple_wdt_test_set(struct apple_wdt_test *ctx, unsigned int reg, u32 val)
{
	writel(val, ctx->wdt.regs + reg);
	ctx->expect[reg / sizeof(u32)] = val;
}

/* Note what the operation under test has to leave in a register. */
static void apple_wdt_test_expect(struct apple_wdt_test *ctx, unsigned int reg, u32 val)
{
	ctx->expect[reg / sizeof(u32)] = val;
}

/* Every register holds what was noted for it; none other was written. */
static void apple_wdt_test_check(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;
	unsigned int reg;

	for (reg = 0; reg < APPLE_WDT_TEST_REGS; reg += sizeof(u32))
		KUNIT_EXPECT_EQ_MSG(test, readl(ctx->wdt.regs + reg),
				    ctx->expect[reg / sizeof(u32)],
				    "register %#x", reg);
}

/* Start rewinds the counter and arms the reset of WD1 only. */
static void apple_wdt_test_start(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, apple_wdt_ops.start(&ctx->wdt.wdd), 0);

	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CUR_TIME, 0);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CTRL, APPLE_WDT_CTRL_RESET_EN);
	apple_wdt_test_check(test);
}

/* Stop disarms the reset and nothing else. */
static void apple_wdt_test_stop(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, apple_wdt_ops.stop(&ctx->wdt.wdd), 0);

	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CTRL, 0);
	apple_wdt_test_check(test);
}

/* A ping rewinds the counter; the bite time and the control stay. */
static void apple_wdt_test_ping(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CTRL, APPLE_WDT_CTRL_RESET_EN);
	KUNIT_ASSERT_EQ(test, apple_wdt_ops.ping(&ctx->wdt.wdd), 0);

	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CUR_TIME, 0);
	apple_wdt_test_check(test);
}

struct apple_wdt_test_timeout {
	const char *name;
	unsigned long rate;
	unsigned int timeout;
	unsigned int max_hw_s;
	u32 bite;
};

static const struct apple_wdt_test_timeout apple_wdt_test_timeouts[] = {
	{
		.name = "24 MHz, 30 s",
		.rate = 24000000, .timeout = 30,
		.max_hw_s = 178, .bite = 720000000,
	}, {
		.name = "24 MHz, 1 s",
		.rate = 24000000, .timeout = 1,
		.max_hw_s = 178, .bite = 24000000,
	}, {
		/* the last timeout whose ticks fit 32 bits */
		.name = "24 MHz, 178 s",
		.rate = 24000000, .timeout = 178,
		.max_hw_s = 178, .bite = 4272000000U,
	}, {
		/* 179 s are 4296000000 ticks, which would wrap to 1032704 */
		.name = "24 MHz, 179 s is clamped",
		.rate = 24000000, .timeout = 179,
		.max_hw_s = 178, .bite = 4272000000U,
	}, {
		.name = "24 MHz, 600 s is clamped",
		.rate = 24000000, .timeout = 600,
		.max_hw_s = 178, .bite = 4272000000U,
	}, {
		/* the longest timeout the watchdog core accepts */
		.name = "24 MHz, UINT_MAX / 1000 s is clamped",
		.rate = 24000000, .timeout = UINT_MAX / 1000,
		.max_hw_s = 178, .bite = 4272000000U,
	}, {
		.name = "32768 Hz, 30 s",
		.rate = 32768, .timeout = 30,
		.max_hw_s = 131071, .bite = 983040,
	}, {
		.name = "32768 Hz, 131071 s",
		.rate = 32768, .timeout = 131071,
		.max_hw_s = 131071, .bite = 4294934528U,
	}, {
		.name = "32768 Hz, 131072 s is clamped",
		.rate = 32768, .timeout = 131072,
		.max_hw_s = 131071, .bite = 4294934528U,
	}, {
		/* one tick short of a 32-bit rate: only 1 s fits */
		.name = "4 GHz, 2 s is clamped",
		.rate = 4000000000UL, .timeout = 2,
		.max_hw_s = 1, .bite = 4000000000U,
	},
};

static void apple_wdt_test_timeout_desc(const struct apple_wdt_test_timeout *t, char *desc)
{
	strscpy(desc, t->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(apple_wdt_test_timeout, apple_wdt_test_timeouts,
		  apple_wdt_test_timeout_desc);

/*
 * The bite time is the timeout in ticks, clamped to what the 32-bit counter
 * can hold (the watchdog core pings for the rest of a longer timeout); the
 * timeout that is reported is the one asked for. The counter is rewound.
 */
static void apple_wdt_test_set_timeout(struct kunit *test)
{
	const struct apple_wdt_test_timeout *t = test->param_value;
	struct apple_wdt_test *ctx = test->priv;

	apple_wdt_test_set_rate(ctx, t->rate);
	KUNIT_ASSERT_EQ(test, ctx->wdt.wdd.max_hw_heartbeat_ms / 1000, t->max_hw_s);
	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CTRL, APPLE_WDT_CTRL_RESET_EN);

	KUNIT_ASSERT_EQ(test, apple_wdt_ops.set_timeout(&ctx->wdt.wdd, t->timeout), 0);

	KUNIT_EXPECT_EQ(test, ctx->wdt.wdd.timeout, t->timeout);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CUR_TIME, 0);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_BITE_TIME, t->bite);
	apple_wdt_test_check(test);

	/* Right after it, the whole (hardware) timeout is left. */
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(&ctx->wdt.wdd),
			min(t->timeout, t->max_hw_s));
}

/* Armed, the time left is the distance to the bite time in whole seconds. */
static void apple_wdt_test_timeleft(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;
	struct watchdog_device *wdd = &ctx->wdt.wdd;

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CTRL, APPLE_WDT_CTRL_RESET_EN);
	apple_wdt_test_set(ctx, APPLE_WDT_WD1_BITE_TIME, 30 * APPLE_WDT_TEST_RATE);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 0);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 30);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 1);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 29);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 10 * APPLE_WDT_TEST_RATE);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 20);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 29 * APPLE_WDT_TEST_RATE);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 1);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 29 * APPLE_WDT_TEST_RATE + 1);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 30 * APPLE_WDT_TEST_RATE);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	/* A bite time in the upper half of the counter is not a negative one. */
	apple_wdt_test_set(ctx, APPLE_WDT_WD1_BITE_TIME, 178 * APPLE_WDT_TEST_RATE);
	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 0);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 178);

	/* Reading the time left writes nothing. */
	apple_wdt_test_check(test);
}

/*
 * Armed, the counter can read past the bite time while the reset is on
 * its way. No time is left then; the distance must not wrap to minutes.
 */
static void apple_wdt_test_timeleft_past_bite(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;
	struct watchdog_device *wdd = &ctx->wdt.wdd;

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CTRL, APPLE_WDT_CTRL_RESET_EN);
	apple_wdt_test_set(ctx, APPLE_WDT_WD1_BITE_TIME, 30 * APPLE_WDT_TEST_RATE);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 30 * APPLE_WDT_TEST_RATE + 1);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 31 * APPLE_WDT_TEST_RATE);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, U32_MAX);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	apple_wdt_test_check(test);
}

/*
 * Not armed (a stopped watchdog, or one that was never started), the
 * counter runs freely and wraps past zero every 2^32 ticks. No reset is
 * coming, so no time is left, whether the counter is past the bite time
 * or has wrapped back below it.
 */
static void apple_wdt_test_timeleft_disarmed(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;
	struct watchdog_device *wdd = &ctx->wdt.wdd;

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CTRL, 0);
	apple_wdt_test_set(ctx, APPLE_WDT_WD1_BITE_TIME, 30 * APPLE_WDT_TEST_RATE);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 31 * APPLE_WDT_TEST_RATE);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, U32_MAX);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	/* Wrapped back below the bite time. */
	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 0);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 10 * APPLE_WDT_TEST_RATE);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	/* A pending interrupt status alone does not arm the reset. */
	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CTRL,
			   APPLE_WDT_CTRL_IRQ_EN | APPLE_WDT_CTRL_IRQ_STATUS);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);

	/* Stopping a started watchdog takes its time left away. */
	KUNIT_ASSERT_EQ(test, apple_wdt_ops.start(wdd), 0);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 30);
	KUNIT_ASSERT_EQ(test, apple_wdt_ops.stop(wdd), 0);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 0);
}

/* A ping gives the whole timeout back. */
static void apple_wdt_test_ping_restores_timeleft(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;
	struct watchdog_device *wdd = &ctx->wdt.wdd;

	KUNIT_ASSERT_EQ(test, apple_wdt_ops.set_timeout(wdd, 60), 0);
	KUNIT_ASSERT_EQ(test, apple_wdt_ops.start(wdd), 0);

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 45 * APPLE_WDT_TEST_RATE);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 15);

	KUNIT_ASSERT_EQ(test, apple_wdt_ops.ping(wdd), 0);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(wdd), 60);
	KUNIT_EXPECT_EQ(test, wdd->timeout, 60);

	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CUR_TIME, 0);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_BITE_TIME, 60 * APPLE_WDT_TEST_RATE);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CTRL, APPLE_WDT_CTRL_RESET_EN);
	apple_wdt_test_check(test);
}

/*
 * A restart arms the reset with a bite time the counter has already
 * reached, whatever the timeout and state were, on WD1 only.
 */
static void apple_wdt_test_restart(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;

	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CTRL, 0);
	KUNIT_ASSERT_EQ(test, apple_wdt_ops.restart(&ctx->wdt.wdd, 0, NULL), 0);

	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CTRL, APPLE_WDT_CTRL_RESET_EN);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_BITE_TIME, 0);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CUR_TIME, 0);
	apple_wdt_test_check(test);
}

/* An idle watchdog is left alone across a system sleep. */
static void apple_wdt_test_sleep_idle(struct kunit *test)
{
	struct apple_wdt_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, apple_wdt_suspend(ctx->dev), 0);
	apple_wdt_test_check(test);
	KUNIT_ASSERT_EQ(test, apple_wdt_resume(ctx->dev), 0);
	apple_wdt_test_check(test);
}

static void apple_wdt_test_sleep_running(struct kunit *test, unsigned long status_bit)
{
	struct apple_wdt_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, apple_wdt_ops.set_timeout(&ctx->wdt.wdd, 30), 0);
	KUNIT_ASSERT_EQ(test, apple_wdt_ops.start(&ctx->wdt.wdd), 0);
	set_bit(status_bit, &ctx->wdt.wdd.status);
	apple_wdt_test_set(ctx, APPLE_WDT_WD1_CUR_TIME, 20 * APPLE_WDT_TEST_RATE);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_BITE_TIME, 30 * APPLE_WDT_TEST_RATE);

	/* Suspend disarms it, so the sleep cannot end in a reset. */
	KUNIT_ASSERT_EQ(test, apple_wdt_suspend(ctx->dev), 0);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CTRL, 0);
	apple_wdt_test_check(test);

	/* Resume arms it with the whole timeout ahead. */
	KUNIT_ASSERT_EQ(test, apple_wdt_resume(ctx->dev), 0);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CTRL, APPLE_WDT_CTRL_RESET_EN);
	apple_wdt_test_expect(ctx, APPLE_WDT_WD1_CUR_TIME, 0);
	apple_wdt_test_check(test);
	KUNIT_EXPECT_EQ(test, apple_wdt_ops.get_timeleft(&ctx->wdt.wdd), 30);
}

/* A watchdog that userspace opened is stopped and started again. */
static void apple_wdt_test_sleep_active(struct kunit *test)
{
	apple_wdt_test_sleep_running(test, WDOG_ACTIVE);
}

/* So is one that the bootloader left running and the core pings. */
static void apple_wdt_test_sleep_hw_running(struct kunit *test)
{
	apple_wdt_test_sleep_running(test, WDOG_HW_RUNNING);
}

static struct kunit_case apple_wdt_test_cases[] = {
	KUNIT_CASE(apple_wdt_test_start),
	KUNIT_CASE(apple_wdt_test_stop),
	KUNIT_CASE(apple_wdt_test_ping),
	KUNIT_CASE_PARAM(apple_wdt_test_set_timeout, apple_wdt_test_timeout_gen_params),
	KUNIT_CASE(apple_wdt_test_timeleft),
	KUNIT_CASE(apple_wdt_test_timeleft_past_bite),
	KUNIT_CASE(apple_wdt_test_timeleft_disarmed),
	KUNIT_CASE(apple_wdt_test_ping_restores_timeleft),
	KUNIT_CASE(apple_wdt_test_restart),
	KUNIT_CASE(apple_wdt_test_sleep_idle),
	KUNIT_CASE(apple_wdt_test_sleep_active),
	KUNIT_CASE(apple_wdt_test_sleep_hw_running),
	{}
};

static struct kunit_suite apple_wdt_test_suite = {
	.name = "apple_wdt",
	.init = apple_wdt_test_init,
	.test_cases = apple_wdt_test_cases,
};

kunit_test_suite(apple_wdt_test_suite);

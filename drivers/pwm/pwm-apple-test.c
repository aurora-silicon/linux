// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * KUnit tests for the Apple SoC PWM driver: the period and duty cycle to
 * ON/OFF cycle register conversion, its readback and the power reference
 * held for a running output.
 *
 * The chip is a real pwm_chip registered on a KUnit device and driven
 * through the PWM core by a consumer found through a lookup table. Its
 * registers are plain kernel memory and the clock rate is set directly, so
 * the tests need no Apple hardware.
 *
 * Included by pwm-apple.c.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/slab.h>

#define APPLE_PWM_TEST_REGS	0x20
#define APPLE_PWM_TEST_RATE	24000000ULL
#define APPLE_PWM_TEST_CTRL_ON	(APPLE_PWM_CTRL_ENABLE | \
				 APPLE_PWM_CTRL_OUTPUT_ENABLE | \
				 APPLE_PWM_CTRL_UPDATE)

struct apple_pwm_test {
	struct device *dev;
	struct pwm_chip *chip;
	struct apple_pwm *fpwm;
	struct pwm_device *pwm;
	struct pwm_lookup lookup;
};

static void apple_pwm_test_put(void *data)
{
	pwm_put(data);
}

static void apple_pwm_test_remove_table(void *data)
{
	pwm_remove_table(data, 1);
}

static void apple_pwm_test_remove_chip(void *data)
{
	pwmchip_remove(data);
}

static void apple_pwm_test_pm_disable(void *data)
{
	pm_runtime_disable(data);
}

static int apple_pwm_test_init(struct kunit *test)
{
	struct apple_pwm_test *ctx;
	struct device *consumer;
	void *regs;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;

	ctx->dev = kunit_device_register(test, "apple-pwm-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->dev);
	consumer = kunit_device_register(test, "apple-pwm-test-consumer");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, consumer);

	ctx->chip = devm_pwmchip_alloc(ctx->dev, 1, sizeof(*ctx->fpwm));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->chip);
	ctx->fpwm = to_apple_pwm(ctx->chip);

	regs = kunit_kzalloc(test, APPLE_PWM_TEST_REGS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, regs);
	ctx->fpwm->base = (void __iomem __force *)regs;
	ctx->fpwm->clkrate = APPLE_PWM_TEST_RATE;
	ctx->chip->ops = &apple_pwm_ops;
	dev_set_drvdata(ctx->dev, ctx->chip);

	KUNIT_ASSERT_EQ(test, pm_runtime_set_active(ctx->dev), 0);
	pm_runtime_enable(ctx->dev);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, apple_pwm_test_pm_disable,
							ctx->dev), 0);

	KUNIT_ASSERT_EQ(test, pwmchip_add(ctx->chip), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, apple_pwm_test_remove_chip,
							ctx->chip), 0);

	ctx->lookup.provider = dev_name(ctx->dev);
	ctx->lookup.dev_id = dev_name(consumer);
	ctx->lookup.con_id = "test";
	ctx->lookup.polarity = PWM_POLARITY_NORMAL;
	pwm_add_table(&ctx->lookup, 1);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, apple_pwm_test_remove_table,
							&ctx->lookup), 0);

	ctx->pwm = pwm_get(consumer, "test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->pwm);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, apple_pwm_test_put,
							ctx->pwm), 0);

	return 0;
}

static u32 apple_pwm_test_reg(struct apple_pwm_test *ctx, unsigned int reg)
{
	return readl(ctx->fpwm->base + reg);
}

static void apple_pwm_test_set_regs(struct apple_pwm_test *ctx, u32 ctrl,
				    u32 on_cycles, u32 off_cycles)
{
	writel(ctrl, ctx->fpwm->base + APPLE_PWM_CTRL);
	writel(on_cycles, ctx->fpwm->base + APPLE_PWM_ON_CYCLES);
	writel(off_cycles, ctx->fpwm->base + APPLE_PWM_OFF_CYCLES);
}

static int apple_pwm_test_apply(struct apple_pwm_test *ctx, u64 period,
				u64 duty_cycle, bool enabled)
{
	struct pwm_state state = {
		.period = period,
		.duty_cycle = duty_cycle,
		.polarity = PWM_POLARITY_NORMAL,
		.enabled = enabled,
	};

	return pwm_apply_might_sleep(ctx->pwm, &state);
}

static int apple_pwm_test_usage(struct apple_pwm_test *ctx)
{
#ifdef CONFIG_PM
	return atomic_read(&ctx->dev->power.usage_count);
#else
	return 0;
#endif
}

struct apple_pwm_test_conv {
	const char *name;
	u64 clkrate;
	u64 period;
	u64 duty_cycle;
	u32 on_cycles;
	u32 off_cycles;
};

static const struct apple_pwm_test_conv apple_pwm_test_convs[] = {
	{
		.name = "24 MHz, 1 ms period, 25 % duty",
		.clkrate = 24000000, .period = 1000000, .duty_cycle = 250000,
		.on_cycles = 6000, .off_cycles = 18000,
	}, {
		/* 2.4 cycles of duty round down to 2, the period is exact */
		.name = "24 MHz, duty rounds down",
		.clkrate = 24000000, .period = 1000, .duty_cycle = 100,
		.on_cycles = 2, .off_cycles = 22,
	}, {
		/* 1999 ns are 47.976 cycles */
		.name = "24 MHz, period rounds down",
		.clkrate = 24000000, .period = 1999, .duty_cycle = 1000,
		.on_cycles = 24, .off_cycles = 23,
	}, {
		/* 41 ns are less than one cycle of 41.67 ns */
		.name = "24 MHz, period below one cycle",
		.clkrate = 24000000, .period = 41, .duty_cycle = 41,
		.on_cycles = 0, .off_cycles = 0,
	}, {
		.name = "24 MHz, no duty",
		.clkrate = 24000000, .period = 1000000, .duty_cycle = 0,
		.on_cycles = 0, .off_cycles = 24000,
	}, {
		.name = "24 MHz, full duty",
		.clkrate = 24000000, .period = 1000000, .duty_cycle = 1000000,
		.on_cycles = 24000, .off_cycles = 0,
	}, {
		/* 100 s and 150 s fit 32 bits each, their sum does not */
		.name = "24 MHz, period above 32 bits of cycles",
		.clkrate = 24000000, .period = 250ULL * NSEC_PER_SEC,
		.duty_cycle = 100ULL * NSEC_PER_SEC,
		.on_cycles = 2400000000U, .off_cycles = 3600000000U,
	}, {
		/* 200 s of duty are 4.8e9 cycles: ON saturates, OFF keeps the rest */
		.name = "24 MHz, duty saturates, period kept",
		.clkrate = 24000000, .period = 300ULL * NSEC_PER_SEC,
		.duty_cycle = 200ULL * NSEC_PER_SEC,
		.on_cycles = U32_MAX, .off_cycles = 2905032705U,
	}, {
		.name = "24 MHz, duty and period saturate",
		.clkrate = 24000000, .period = 500ULL * NSEC_PER_SEC,
		.duty_cycle = 400ULL * NSEC_PER_SEC,
		.on_cycles = U32_MAX, .off_cycles = U32_MAX,
	}, {
		/* the period times the rate does not fit 64 bits */
		.name = "24 MHz, longest period",
		.clkrate = 24000000, .period = U64_MAX, .duty_cycle = 1000000,
		.on_cycles = 24000, .off_cycles = U32_MAX,
	}, {
		.name = "24 MHz, longest period and duty",
		.clkrate = 24000000, .period = U64_MAX, .duty_cycle = U64_MAX,
		.on_cycles = U32_MAX, .off_cycles = U32_MAX,
	}, {
		/* the fastest clock the probe accepts: one cycle per ns */
		.name = "1 GHz, one cycle per ns",
		.clkrate = NSEC_PER_SEC, .period = 4000000000ULL,
		.duty_cycle = 123456789,
		.on_cycles = 123456789, .off_cycles = 3876543211U,
	}, {
		.name = "1 GHz, longest period and duty",
		.clkrate = NSEC_PER_SEC, .period = U64_MAX, .duty_cycle = U64_MAX,
		.on_cycles = U32_MAX, .off_cycles = U32_MAX,
	}, {
		/* 1 s is 32768 cycles, 100 us are 3.28 */
		.name = "32768 Hz",
		.clkrate = 32768, .period = NSEC_PER_SEC, .duty_cycle = 100000,
		.on_cycles = 3, .off_cycles = 32765,
	},
};

static void apple_pwm_test_conv_desc(const struct apple_pwm_test_conv *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(apple_pwm_test_conv, apple_pwm_test_convs, apple_pwm_test_conv_desc);

/* An enabled state is converted to ON and OFF cycles and latched. */
static void apple_pwm_test_apply_cycles(struct kunit *test)
{
	const struct apple_pwm_test_conv *c = test->param_value;
	struct apple_pwm_test *ctx = test->priv;

	ctx->fpwm->clkrate = c->clkrate;
	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, c->period, c->duty_cycle, true), 0);

	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_ON_CYCLES), c->on_cycles);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_OFF_CYCLES), c->off_cycles);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_CTRL), APPLE_PWM_TEST_CTRL_ON);
}

/*
 * What the hardware reads back is never longer than what was asked for, is
 * less than one clock cycle shorter unless a register saturated, and
 * programs the same registers when it is applied again.
 */
static void apple_pwm_test_readback(struct kunit *test)
{
	const struct apple_pwm_test_conv *c = test->param_value;
	struct apple_pwm_test *ctx = test->priv;
	u64 cycle_ns = DIV64_U64_ROUND_UP(NSEC_PER_SEC, c->clkrate);
	struct pwm_state hw;

	ctx->fpwm->clkrate = c->clkrate;
	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, c->period, c->duty_cycle, true), 0);
	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);

	KUNIT_EXPECT_TRUE(test, hw.enabled);
	KUNIT_EXPECT_EQ(test, hw.polarity, PWM_POLARITY_NORMAL);
	KUNIT_EXPECT_LE(test, hw.duty_cycle, hw.period);
	KUNIT_EXPECT_LE(test, hw.duty_cycle, c->duty_cycle);
	KUNIT_EXPECT_LE(test, hw.period, c->period);
	if (c->on_cycles != U32_MAX)
		KUNIT_EXPECT_LT(test, c->duty_cycle - hw.duty_cycle, cycle_ns);
	if (c->on_cycles != U32_MAX && c->off_cycles != U32_MAX)
		KUNIT_EXPECT_LT(test, c->period - hw.period, cycle_ns);

	/* Scribble over the registers so that the second apply shows. */
	apple_pwm_test_set_regs(ctx, 0, ~c->on_cycles, ~c->off_cycles);
	KUNIT_ASSERT_EQ(test, apple_pwm_apply(ctx->chip, ctx->pwm, &hw), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_ON_CYCLES), c->on_cycles);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_OFF_CYCLES), c->off_cycles);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_CTRL), APPLE_PWM_TEST_CTRL_ON);
}

/* Readback rounds up to the next nanosecond, duty and period separately. */
static void apple_pwm_test_get_state_rounds_up(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;
	struct pwm_state hw;

	/* 1 cycle is 41.67 ns, 2 are 83.33 ns, 3 are exactly 125 ns */
	apple_pwm_test_set_regs(ctx, APPLE_PWM_TEST_CTRL_ON, 1, 1);
	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
	KUNIT_EXPECT_EQ(test, hw.duty_cycle, 42);
	KUNIT_EXPECT_EQ(test, hw.period, 84);

	apple_pwm_test_set_regs(ctx, APPLE_PWM_TEST_CTRL_ON, 3, 0);
	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
	KUNIT_EXPECT_EQ(test, hw.duty_cycle, 125);
	KUNIT_EXPECT_EQ(test, hw.period, 125);
}

/* Both registers at their maximum: the 33-bit sum must not be truncated. */
static void apple_pwm_test_get_state_max(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;
	struct pwm_state hw;

	apple_pwm_test_set_regs(ctx, APPLE_PWM_TEST_CTRL_ON, U32_MAX, U32_MAX);
	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
	KUNIT_EXPECT_TRUE(test, hw.enabled);
	KUNIT_EXPECT_EQ(test, hw.duty_cycle, 178956970625ULL);
	KUNIT_EXPECT_EQ(test, hw.period, 357913941250ULL);

	/* The slowest clock: 2^33 - 2 cycles of one second each. */
	ctx->fpwm->clkrate = 1;
	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
	KUNIT_EXPECT_EQ(test, hw.duty_cycle, (u64)U32_MAX * NSEC_PER_SEC);
	KUNIT_EXPECT_EQ(test, hw.period, 2ULL * U32_MAX * NSEC_PER_SEC);
}

/* The output only runs with both the enable and the output enable bit. */
static void apple_pwm_test_get_state_enabled(struct kunit *test)
{
	static const struct {
		u32 ctrl;
		bool enabled;
	} cases[] = {
		{ 0, false },
		{ APPLE_PWM_CTRL_ENABLE, false },
		{ APPLE_PWM_CTRL_OUTPUT_ENABLE, false },
		{ APPLE_PWM_CTRL_UPDATE, false },
		{ APPLE_PWM_CTRL_ENABLE | APPLE_PWM_CTRL_OUTPUT_ENABLE, true },
		{ APPLE_PWM_TEST_CTRL_ON, true },
		{ ~(u32)APPLE_PWM_CTRL_ENABLE, false },
		{ ~(u32)APPLE_PWM_CTRL_OUTPUT_ENABLE, false },
	};
	struct apple_pwm_test *ctx = test->priv;
	struct pwm_state hw;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		apple_pwm_test_set_regs(ctx, cases[i].ctrl, 6000, 18000);
		KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
		KUNIT_EXPECT_EQ_MSG(test, hw.enabled, cases[i].enabled,
				    "ctrl %#x", cases[i].ctrl);
		KUNIT_EXPECT_EQ(test, hw.polarity, PWM_POLARITY_NORMAL);
		KUNIT_EXPECT_EQ(test, hw.duty_cycle, 250000);
		KUNIT_EXPECT_EQ(test, hw.period, 1000000);
	}
}

/* Disabling clears the control register and leaves the cycle registers. */
static void apple_pwm_test_disable(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;
	struct pwm_state hw;

	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 250000, true), 0);
	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 2000000, 1000000, false), 0);

	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_CTRL), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_ON_CYCLES), 6000);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_OFF_CYCLES), 18000);

	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
	KUNIT_EXPECT_FALSE(test, hw.enabled);
}

/* A disabled state never has its period or duty cycle looked at. */
static void apple_pwm_test_disable_ignores_timing(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;

	apple_pwm_test_set_regs(ctx, APPLE_PWM_TEST_CTRL_ON, 11, 22);
	/* duty above period, which is only valid while disabled */
	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 5, U64_MAX, false), 0);

	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_CTRL), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_ON_CYCLES), 11);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_OFF_CYCLES), 22);
}

static void apple_pwm_test_expect_untouched(struct kunit *test, int usage)
{
	struct apple_pwm_test *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_CTRL), APPLE_PWM_TEST_CTRL_ON);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_ON_CYCLES), 6000);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_OFF_CYCLES), 18000);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage);
	KUNIT_EXPECT_TRUE(test, ctx->fpwm->enabled);
}

/* The hardware cannot invert: refused before anything is written. */
static void apple_pwm_test_inversed_refused(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;
	struct pwm_state state = {
		.period = 2000000,
		.duty_cycle = 500000,
		.polarity = PWM_POLARITY_INVERSED,
		.enabled = true,
	};
	int usage;

	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 250000, true), 0);
	usage = apple_pwm_test_usage(ctx);

	KUNIT_EXPECT_EQ(test, pwm_apply_might_sleep(ctx->pwm, &state), -EINVAL);
	apple_pwm_test_expect_untouched(test, usage);

	/* Also when it is asked for together with a disable. */
	state.enabled = false;
	KUNIT_EXPECT_EQ(test, pwm_apply_might_sleep(ctx->pwm, &state), -EINVAL);
	apple_pwm_test_expect_untouched(test, usage);

	/* And when the driver is called without the core in front. */
	KUNIT_EXPECT_EQ(test, apple_pwm_apply(ctx->chip, ctx->pwm, &state), -EINVAL);
	apple_pwm_test_expect_untouched(test, usage);
}

/* A duty cycle above the period never reaches the registers. */
static void apple_pwm_test_duty_above_period_refused(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;
	int usage;

	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 250000, true), 0);
	usage = apple_pwm_test_usage(ctx);

	KUNIT_EXPECT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 1000001, true), -EINVAL);
	apple_pwm_test_expect_untouched(test, usage);
}

/* Nor does an enabled state without a period. */
static void apple_pwm_test_zero_period_refused(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;
	int usage;

	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 250000, true), 0);
	usage = apple_pwm_test_usage(ctx);

	KUNIT_EXPECT_EQ(test, apple_pwm_test_apply(ctx, 0, 0, true), -EINVAL);
	apple_pwm_test_expect_untouched(test, usage);
}

/*
 * A running output holds exactly one runtime PM reference, however often it
 * is reprogrammed, and gives it back when it is disabled.
 */
static void apple_pwm_test_enable_holds_power(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;
	struct pwm_state off = { .polarity = PWM_POLARITY_NORMAL };
	int usage = apple_pwm_test_usage(ctx);

	if (!IS_ENABLED(CONFIG_PM))
		kunit_skip(test, "needs CONFIG_PM");

	KUNIT_EXPECT_FALSE(test, ctx->fpwm->enabled);

	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 250000, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage + 1);
	KUNIT_EXPECT_TRUE(test, ctx->fpwm->enabled);

	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 500000, true), 0);
	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 2000000, 500000, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage + 1);

	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 2000000, 500000, false), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage);
	KUNIT_EXPECT_FALSE(test, ctx->fpwm->enabled);

	/* A second disable, which the core would filter, gives nothing back. */
	KUNIT_ASSERT_EQ(test, apple_pwm_apply(ctx->chip, ctx->pwm, &off), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage);

	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 250000, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage + 1);
	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 0, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage);
	KUNIT_EXPECT_FALSE(test, ctx->fpwm->enabled);

	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 250000, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage + 1);
	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 250000, false), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage);
}

/* Reading the state back takes no lasting power reference. */
static void apple_pwm_test_get_state_power_balanced(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;
	int usage = apple_pwm_test_usage(ctx);
	struct pwm_state hw;

	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage);
}

/*
 * The power domain may drop the registers of a stopped PWM: runtime suspend
 * saves all three and runtime resume writes them back.
 */
static void apple_pwm_test_runtime_pm_keeps_registers(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;

	apple_pwm_test_set_regs(ctx, APPLE_PWM_CTRL_MODE, 0x12345678, 0x9abcdef0);
	KUNIT_ASSERT_EQ(test, apple_pwm_runtime_suspend(ctx->dev), 0);

	apple_pwm_test_set_regs(ctx, 0, 0, 0);
	KUNIT_ASSERT_EQ(test, apple_pwm_runtime_resume(ctx->dev), 0);

	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_CTRL), APPLE_PWM_CTRL_MODE);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_ON_CYCLES), 0x12345678);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_reg(ctx, APPLE_PWM_OFF_CYCLES), 0x9abcdef0);
}

/*
 * An enabled zero-duty state (or a duty that rounds down to zero cycles)
 * releases the output power hold while preserving readback across register loss.
 */
static void apple_pwm_test_zero_duty_runtime_pm_readback(struct kunit *test)
{
	struct apple_pwm_test *ctx = test->priv;
	struct pwm_state hw;
	int usage = apple_pwm_test_usage(ctx);

	if (!IS_ENABLED(CONFIG_PM))
		kunit_skip(test, "needs CONFIG_PM");

	/* Explicit zero duty */
	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 0, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage);
	KUNIT_EXPECT_FALSE(test, ctx->fpwm->enabled);
	KUNIT_ASSERT_EQ(test, apple_pwm_runtime_suspend(ctx->dev), 0);
	apple_pwm_test_set_regs(ctx, 0, 0, 0);
	KUNIT_ASSERT_EQ(test, apple_pwm_runtime_resume(ctx->dev), 0);
	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
	KUNIT_EXPECT_TRUE(test, hw.enabled);
	KUNIT_EXPECT_EQ(test, hw.duty_cycle, 0);
	KUNIT_EXPECT_EQ(test, hw.period, 1000000);

	/* Non-zero duty that rounds down to 0 cycles at 24 MHz (1 cycle = 41.67 ns) */
	KUNIT_ASSERT_EQ(test, apple_pwm_test_apply(ctx, 1000000, 10, true), 0);
	KUNIT_EXPECT_EQ(test, apple_pwm_test_usage(ctx), usage);
	KUNIT_EXPECT_FALSE(test, ctx->fpwm->enabled);
	KUNIT_ASSERT_EQ(test, apple_pwm_runtime_suspend(ctx->dev), 0);
	apple_pwm_test_set_regs(ctx, 0, 0, 0);
	KUNIT_ASSERT_EQ(test, apple_pwm_runtime_resume(ctx->dev), 0);
	KUNIT_ASSERT_EQ(test, pwm_get_state_hw(ctx->pwm, &hw), 0);
	KUNIT_EXPECT_TRUE(test, hw.enabled);
	KUNIT_EXPECT_EQ(test, hw.duty_cycle, 0);
	KUNIT_EXPECT_EQ(test, hw.period, 1000000);
}

static struct kunit_case apple_pwm_test_cases[] = {
	KUNIT_CASE_PARAM(apple_pwm_test_apply_cycles, apple_pwm_test_conv_gen_params),
	KUNIT_CASE_PARAM(apple_pwm_test_readback, apple_pwm_test_conv_gen_params),
	KUNIT_CASE(apple_pwm_test_get_state_rounds_up),
	KUNIT_CASE(apple_pwm_test_get_state_max),
	KUNIT_CASE(apple_pwm_test_get_state_enabled),
	KUNIT_CASE(apple_pwm_test_disable),
	KUNIT_CASE(apple_pwm_test_disable_ignores_timing),
	KUNIT_CASE(apple_pwm_test_inversed_refused),
	KUNIT_CASE(apple_pwm_test_duty_above_period_refused),
	KUNIT_CASE(apple_pwm_test_zero_period_refused),
	KUNIT_CASE(apple_pwm_test_enable_holds_power),
	KUNIT_CASE(apple_pwm_test_get_state_power_balanced),
	KUNIT_CASE(apple_pwm_test_runtime_pm_keeps_registers),
	KUNIT_CASE(apple_pwm_test_zero_duty_runtime_pm_readback),
	{}
};

static struct kunit_suite apple_pwm_test_suite = {
	.name = "pwm-apple",
	.init = apple_pwm_test_init,
	.test_cases = apple_pwm_test_cases,
};

kunit_test_suite(apple_pwm_test_suite);

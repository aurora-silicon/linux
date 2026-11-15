// SPDX-License-Identifier: GPL-2.0
/* Included by gov_power_allocator.c to exercise the actual PID controller. */

#include <kunit/test.h>

struct power_allocator_test_ctx {
	struct thermal_zone_device tz;
	struct thermal_zone_params tzp;
	struct power_allocator_params params;
};

static int power_allocator_test_init(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->tz.tzp = &ctx->tzp;
	ctx->tz.governor_data = &ctx->params;
	ctx->tz.passive_delay_jiffies = msecs_to_jiffies(500);
	ctx->tzp.sustainable_power = 1500;
	ctx->params.sustainable_power = 1500;
	/* Isolate the integral term: one mW per millidegree of error. */
	ctx->tzp.k_i = int_to_frac(1);
	test->priv = ctx;
	return 0;
}

static u32 power_allocator_test_sample(struct kunit *test, int temperature)
{
	struct power_allocator_test_ctx *ctx = test->priv;

	ctx->tz.temperature = temperature;
	return pid_controller(&ctx->tz, 95000, 10000);
}

static void power_allocator_test_no_positive_windup(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;
	int n;

	for (n = 0; n < 10; n++)
		KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 94000), 1500U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, 0LL);
}

static void power_allocator_test_hot_integrates(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 95100), 1400U);
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 95100), 1300U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, -(s64)int_to_frac(200));
}

static void power_allocator_test_partial_recovery(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, power_allocator_test_sample(test, 95100), 1400U);
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 94950), 1450U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, -(s64)int_to_frac(50));
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 94950), 1500U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, 0LL);
}

static void power_allocator_test_recovery_stops_at_zero(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, power_allocator_test_sample(test, 95100), 1400U);
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 94000), 1500U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, 0LL);
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 94000), 1500U);
	/* Recovery does not require crossing the switch-on trip or resetting. */
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 95100), 1400U);
}

static void power_allocator_test_positive_cutoff(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;

	ctx->tzp.integral_cutoff = 1000;
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 94500), 2000U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, (s64)int_to_frac(500));
	/* Error at the cutoff still cannot grow a positive integral. */
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 94000), 2000U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, (s64)int_to_frac(500));
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 96000), 1000U);
	/* Above the cutoff, recovery is permitted only as far as zero. */
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 93000), 1500U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, 0LL);
}

static void power_allocator_test_negative_cutoff(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;

	ctx->tzp.integral_cutoff = -100;
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 95050), 1500U);
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 95100), 1500U);
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 95200), 1300U);
	/* A negative error outside the cutoff must not unwind anything. */
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 95050), 1300U);
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 94800), 1500U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, 0LL);
}

static void power_allocator_test_integral_bound(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;

	/* The existing strict power bound still rejects saturated updates. */
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 105000), 1500U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, 0LL);
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 104000), 0U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, -(s64)int_to_frac(9000));
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 96000), 0U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, -(s64)int_to_frac(9000));
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 85000), 1500U);
	KUNIT_EXPECT_EQ(test, ctx->params.err_integral, 0LL);
}

static void power_allocator_test_legacy_gain(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;
	struct thermal_trip switch_on = { .temperature = 58000 };

	/* Without an explicit time constant the per-poll estimate is unchanged. */
	ctx->tz.passive_delay_jiffies = msecs_to_jiffies(500);
	estimate_pid_constants(&ctx->tz, 1500, &switch_on, 95000);
	KUNIT_EXPECT_EQ(test, ctx->tzp.k_i, 8);
	ctx->tz.passive_delay_jiffies = msecs_to_jiffies(250);
	estimate_pid_constants(&ctx->tz, 1500, &switch_on, 95000);
	KUNIT_EXPECT_EQ(test, ctx->tzp.k_i, 8);
}

static void power_allocator_test_timed_gain(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;
	struct thermal_trip switch_on = { .temperature = 58000 };
	u32 slow_budget;

	ctx->tzp.integral_time_constant_ms = 5000;
	ctx->tz.passive_delay_jiffies = msecs_to_jiffies(500);
	estimate_pid_constants(&ctx->tz, 1500, &switch_on, 95000);
	KUNIT_ASSERT_EQ(test, ctx->tzp.k_i, 8);
	slow_budget = power_allocator_test_sample(test, 96000);

	reset_pid_controller(&ctx->params);
	ctx->tz.passive_delay_jiffies = msecs_to_jiffies(250);
	estimate_pid_constants(&ctx->tz, 1500, &switch_on, 95000);
	KUNIT_EXPECT_EQ(test, ctx->tzp.k_po, 41);
	KUNIT_EXPECT_EQ(test, ctx->tzp.k_pu, 83);
	KUNIT_ASSERT_EQ(test, ctx->tzp.k_i, 4);
	/* Two fast samples integrate the same error as one slow sample. */
	power_allocator_test_sample(test, 96000);
	KUNIT_EXPECT_EQ(test, power_allocator_test_sample(test, 96000), slow_budget);
}

static void power_allocator_test_timed_gain_bound(struct kunit *test)
{
	struct power_allocator_test_ctx *ctx = test->priv;
	struct thermal_trip switch_on = { .temperature = 58000 };

	ctx->tzp.integral_time_constant_ms = 1;
	ctx->tz.passive_delay_jiffies = msecs_to_jiffies(30000000);
	estimate_pid_constants(&ctx->tz, 1500, &switch_on, 95000);
	KUNIT_EXPECT_EQ(test, ctx->tzp.k_i, S32_MAX);
}

static struct kunit_case power_allocator_test_cases[] = {
	KUNIT_CASE(power_allocator_test_no_positive_windup),
	KUNIT_CASE(power_allocator_test_hot_integrates),
	KUNIT_CASE(power_allocator_test_partial_recovery),
	KUNIT_CASE(power_allocator_test_recovery_stops_at_zero),
	KUNIT_CASE(power_allocator_test_positive_cutoff),
	KUNIT_CASE(power_allocator_test_negative_cutoff),
	KUNIT_CASE(power_allocator_test_integral_bound),
	KUNIT_CASE(power_allocator_test_legacy_gain),
	KUNIT_CASE(power_allocator_test_timed_gain),
	KUNIT_CASE(power_allocator_test_timed_gain_bound),
	{}
};

static struct kunit_suite power_allocator_test_suite = {
	.name = "thermal-power-allocator",
	.init = power_allocator_test_init,
	.test_cases = power_allocator_test_cases,
};

kunit_test_suite(power_allocator_test_suite);

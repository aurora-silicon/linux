// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by mcc.c to exercise allocation failure and cache handoff. */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/sizes.h>

struct apple_mcc_test {
	struct apple_mcc mcc;
	unsigned int reject_plane;
	unsigned long attempted;
	unsigned long disabled;
	unsigned long flushed;
	struct t8112_dram_config live[T8112_DCS_CHANNELS];
	unsigned int config_writes;
	unsigned int reject_config_once;
	bool config_rejected;
};

static struct t8112_dram_config apple_mcc_test_read_config(struct apple_mcc *mcc,
							   unsigned int channel)
{
	struct kunit *test = kunit_get_current_test();
	struct apple_mcc_test *ctx = test->priv;

	return ctx->live[channel];
}

static void apple_mcc_test_write_config(struct apple_mcc *mcc, unsigned int channel,
					const struct t8112_dram_config *config)
{
	struct kunit *test = kunit_get_current_test();
	struct apple_mcc_test *ctx = test->priv;

	ctx->config_writes++;
	if (channel == ctx->reject_config_once && !ctx->config_rejected) {
		ctx->config_rejected = true;
		return;
	}
	ctx->live[channel] = *config;
}

static void apple_mcc_test_write(struct apple_mcc *mcc, unsigned int plane,
				 u32 val, u32 offset)
{
	struct kunit *test = kunit_get_current_test();
	struct apple_mcc_test *ctx = test->priv;

	if (offset == MCC_ALLOC_DISABLE) {
		if (val) {
			ctx->disabled |= BIT(plane);
		} else {
			ctx->attempted |= BIT(plane);
			if (plane == ctx->reject_plane)
				return;
		}
	}
	if (offset == MCC_CACHE_FLUSH) {
		ctx->flushed |= BIT(plane);
		/* Model a cache whose flush completes immediately. */
		val = 0;
	}
	writel(val, mcc->planes[plane] + offset);
}

static struct apple_mcc_test *apple_mcc_test_alloc(struct kunit *test)
{
	struct apple_mcc_test *ctx;
	struct apple_mcc *mcc;
	unsigned int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	mcc = &ctx->mcc;
	mcc->dev = kunit_device_register(test, "apple-mcc-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mcc->dev);
	mcc->nr_planes = 3;
	ctx->reject_plane = MCC_MAX_PLANES;
	ctx->reject_config_once = T8112_DCS_CHANNELS;
	for (i = 0; i < mcc->nr_planes; i++) {
		mcc->planes[i] = (__force void __iomem *)kunit_kzalloc(test, SZ_16K, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, mcc->planes[i]);
		writel(1, mcc->planes[i] + MCC_ALLOC_DISABLE);
		writel(1, mcc->planes[i] + MCC_CACHE_WAYS);
		writel(FIELD_PREP(MCC_CACHE_WAYS_POWERED, 1), mcc->planes[i] + MCC_CACHE_POWER);
	}
	test->priv = ctx;
	kunit_activate_static_stub(test, apple_mcc_writel, apple_mcc_test_write);
	kunit_activate_static_stub(test, apple_mcc_t8112_read_config,
				   apple_mcc_test_read_config);
	kunit_activate_static_stub(test, apple_mcc_t8112_write_config,
				   apple_mcc_test_write_config);
	return ctx;
}

static void apple_mcc_test_partial_enable(struct kunit *test)
{
	struct apple_mcc_test *ctx = apple_mcc_test_alloc(test);
	struct apple_mcc *mcc = &ctx->mcc;
	unsigned int i;

	ctx->reject_plane = 1;
	KUNIT_EXPECT_EQ(test, apple_mcc_enable(mcc), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->attempted, BIT(0) | BIT(1));
	/* The failed plane's state is uncertain, so include it in cleanup. */
	KUNIT_EXPECT_EQ(test, ctx->disabled, BIT(0) | BIT(1));
	KUNIT_EXPECT_EQ(test, ctx->flushed, BIT(0) | BIT(1));
	KUNIT_EXPECT_FALSE(test, mcc->enabled);
	for (i = 0; i < mcc->nr_planes; i++)
		KUNIT_EXPECT_EQ(test, readl(mcc->planes[i] + MCC_ALLOC_DISABLE), 1U);
}

static void apple_mcc_test_quiesce(struct kunit *test)
{
	struct apple_mcc_test *ctx = apple_mcc_test_alloc(test);
	struct apple_mcc *mcc = &ctx->mcc;

	KUNIT_ASSERT_EQ(test, apple_mcc_enable(mcc), 0);
	mcc->enabled = true;
	apple_mcc_quiesce(mcc);
	KUNIT_EXPECT_EQ(test, ctx->attempted, 7UL);
	KUNIT_EXPECT_EQ(test, ctx->disabled, 7UL);
	KUNIT_EXPECT_EQ(test, ctx->flushed, 7UL);
	KUNIT_EXPECT_FALSE(test, mcc->enabled);
}

static void apple_mcc_test_not_ready(struct kunit *test)
{
	struct apple_mcc_test *ctx = apple_mcc_test_alloc(test);
	struct apple_mcc *mcc = &ctx->mcc;

	writel(0, mcc->planes[1] + MCC_CACHE_WAYS);
	KUNIT_EXPECT_EQ(test, apple_mcc_enable(mcc), -EBUSY);
	KUNIT_EXPECT_EQ(test, ctx->attempted, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->disabled, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->flushed, 0UL);
}

static void apple_mcc_test_t8112_dram_restore(struct kunit *test)
{
	struct apple_mcc_test *ctx = apple_mcc_test_alloc(test);
	struct apple_mcc *mcc = &ctx->mcc;
	unsigned int i;

	for (i = 0; i < T8112_DCS_CHANNELS; i++) {
		mcc->boot_config[i] = (struct t8112_dram_config) {
			.config0 = 0x100 + i,
			.config1 = 0x200 + i,
		};
		ctx->live[i] =
			(struct t8112_dram_config) { .config0 = i, .config1 = i + 1 };
		mcc->pmp_config[i] = ctx->live[i];
	}

	KUNIT_ASSERT_EQ(test, apple_mcc_t8112_restore_config(mcc), 0);
	KUNIT_EXPECT_EQ(test, ctx->config_writes, T8112_DCS_CHANNELS);
	for (i = 0; i < T8112_DCS_CHANNELS; i++)
		KUNIT_EXPECT_TRUE(test, apple_mcc_t8112_config_equal(&ctx->live[i],
								     &mcc->boot_config[i]));
}

static void apple_mcc_test_t8112_dram_rollback(struct kunit *test)
{
	struct apple_mcc_test *ctx = apple_mcc_test_alloc(test);
	struct apple_mcc *mcc = &ctx->mcc;
	unsigned int i;

	for (i = 0; i < T8112_DCS_CHANNELS; i++) {
		mcc->boot_config[i] =
			(struct t8112_dram_config) { .config0 = 0x10 + i, .config1 = 0x20 + i };
		ctx->live[i] =
			(struct t8112_dram_config) { .config0 = 0x30 + i, .config1 = 0x40 + i };
		mcc->pmp_config[i] = ctx->live[i];
	}
	ctx->reject_config_once = 3;

	KUNIT_ASSERT_EQ(test, apple_mcc_t8112_restore_config(mcc), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->config_writes, 2 * T8112_DCS_CHANNELS);
	for (i = 0; i < T8112_DCS_CHANNELS; i++)
		KUNIT_EXPECT_TRUE(test, apple_mcc_t8112_config_equal(&ctx->live[i],
								     &mcc->pmp_config[i]));
}

static struct kunit_case apple_mcc_test_cases[] = {
	KUNIT_CASE(apple_mcc_test_partial_enable),
	KUNIT_CASE(apple_mcc_test_quiesce),
	KUNIT_CASE(apple_mcc_test_not_ready),
	KUNIT_CASE(apple_mcc_test_t8112_dram_restore),
	KUNIT_CASE(apple_mcc_test_t8112_dram_rollback),
	{}
};

static struct kunit_suite apple_mcc_test_suite = {
	.name = "apple-mcc",
	.test_cases = apple_mcc_test_cases,
};

kunit_test_suite(apple_mcc_test_suite);

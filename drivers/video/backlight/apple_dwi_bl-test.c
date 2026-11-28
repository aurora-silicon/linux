// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * KUnit tests for the Apple DWI backlight driver: the brightness command
 * encoding, the send bits, reading the brightness back from the command
 * register, and blanking through bl_power and the backlight core's
 * suspend/resume.
 *
 * A real backlight device is registered on a KUnit device with the driver's
 * ops; its register block is kernel memory, so the tests need no Apple
 * hardware.
 *
 * Included by apple_dwi_bl.c.
 */

#include <kunit/device.h>
#include <kunit/test.h>

#define DWI_BL_TEST_REGS	0x10
/* What every register holds before the operation under test. */
#define DWI_BL_TEST_FILL	0xdeadbeef
/* SEND1 | SEND2 | SEND3 | LE_DATA | SEND4 */
#define DWI_BL_TEST_CTL_SEND	0x1071

struct dwi_bl_test {
	struct apple_dwi_bl dwi_bl;
	struct backlight_device *bl;
	void *regs;
};

static u32 dwi_bl_test_reg(struct dwi_bl_test *ctx, unsigned int reg)
{
	return readl(ctx->dwi_bl.base + reg);
}

static int dwi_bl_test_init(struct kunit *test)
{
	struct backlight_properties props = {
		/* as dwi_bl_probe() */
		.type = BACKLIGHT_PLATFORM,
		.max_brightness = DWI_BL_MAX_BRIGHTNESS,
		.scale = BACKLIGHT_SCALE_LINEAR,
	};
	struct dwi_bl_test *ctx;
	struct device *dev;
	unsigned int reg;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;

	ctx->regs = kunit_kzalloc(test, DWI_BL_TEST_REGS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->regs);
	ctx->dwi_bl.base = (void __iomem __force *)ctx->regs;
	for (reg = 0; reg < DWI_BL_TEST_REGS; reg += sizeof(u32))
		writel(DWI_BL_TEST_FILL, ctx->dwi_bl.base + reg);

	dev = kunit_device_register(test, "apple-dwi-bl-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	ctx->bl = devm_backlight_device_register(dev, "apple-dwi-bl-test", dev,
						 &ctx->dwi_bl, &dwi_bl_ops,
						 &props);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->bl);

	return 0;
}

static u32 dwi_bl_test_cmd(u32 brightness)
{
	return 0xa0000000 | brightness;
}

/* The command word for a brightness: type 0xa in [31:28], level in [10:0]. */
static void dwi_bl_test_encode(struct kunit *test)
{
	static const u32 levels[] = { 1, 2, 0x155, 0x2aa, 1023, 1024, 2046 };
	struct dwi_bl_test *ctx = test->priv;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(levels); i++) {
		KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, levels[i]), 0);
		KUNIT_EXPECT_EQ_MSG(test, dwi_bl_test_reg(ctx, DWI_BL_CMD),
				    dwi_bl_test_cmd(levels[i]), "level %u", levels[i]);
	}
}

/* Every update sets all five send bits, SEND4 (bit 12) included. */
static void dwi_bl_test_send_bits(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, 100), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CTL), DWI_BL_TEST_CTL_SEND);
}

/* An update writes CTL and CMD and nothing beyond them. */
static void dwi_bl_test_other_regs_untouched(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, 7), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, 0x8), DWI_BL_TEST_FILL);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, 0xc), DWI_BL_TEST_FILL);
}

/* The maximum is the full 11-bit level field. */
static void dwi_bl_test_max(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, DWI_BL_MAX_BRIGHTNESS, (int)FIELD_MAX(DWI_BL_CMD_DATA));
	KUNIT_EXPECT_EQ(test, ctx->bl->props.max_brightness, 2047);

	KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, 2047), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(2047));
	KUNIT_EXPECT_EQ(test, dwi_bl_get_brightness(ctx->bl), 2047);
}

/* The core refuses a level above the maximum and the hardware keeps the last one. */
static void dwi_bl_test_over_max(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, 500), 0);
	KUNIT_EXPECT_EQ(test, backlight_device_set_brightness(ctx->bl, 2048), -EINVAL);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(500));
	KUNIT_EXPECT_EQ(test, ctx->bl->props.brightness, 500);
}

/* Brightness 0 is a real command, not a skipped write. */
static void dwi_bl_test_zero(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, 0), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(0));
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CTL), DWI_BL_TEST_CTL_SEND);
}

/* Every level written by update_status reads back through get_brightness. */
static void dwi_bl_test_round_trip(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;
	u32 level;

	for (level = 0; level <= DWI_BL_MAX_BRIGHTNESS; level++) {
		KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, level), 0);
		KUNIT_ASSERT_EQ_MSG(test, ctx->bl->ops->get_brightness(ctx->bl), level,
				    "level %u", level);
	}
}

/* get_brightness takes only the level field of the command register. */
static void dwi_bl_test_get_masks_type(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;

	/* What the bootloader left: another type and junk above the field. */
	writel(0x5ffff800 | 0x3c4, ctx->dwi_bl.base + DWI_BL_CMD);
	KUNIT_EXPECT_EQ(test, dwi_bl_get_brightness(ctx->bl), 0x3c4);

	writel(0xfffff800, ctx->dwi_bl.base + DWI_BL_CMD);
	KUNIT_EXPECT_EQ(test, dwi_bl_get_brightness(ctx->bl), 0);

	writel(dwi_bl_test_cmd(2047), ctx->dwi_bl.base + DWI_BL_CMD);
	KUNIT_EXPECT_EQ(test, dwi_bl_get_brightness(ctx->bl), 2047);
}

/* A level above the field is truncated to it, never into the type nibble. */
static void dwi_bl_test_field_overflow(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;

	/* Bypass the core's max check, as a caller writing props directly does. */
	ctx->bl->props.brightness = 0x800 | 0x12;
	KUNIT_ASSERT_EQ(test, backlight_update_status(ctx->bl), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD) & DWI_BL_CMD_TYPE,
			0xa0000000);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD) & ~DWI_BL_CMD_TYPE,
			0x12);
}

/* bl_power off sends level 0 and keeps the set level; on sends it again. */
static void dwi_bl_test_power_off_on(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, 1200), 0);

	KUNIT_ASSERT_EQ(test, backlight_disable(ctx->bl), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(0));
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CTL), DWI_BL_TEST_CTL_SEND);
	KUNIT_EXPECT_EQ(test, ctx->bl->props.brightness, 1200);

	writel(0, ctx->dwi_bl.base + DWI_BL_CTL);
	KUNIT_ASSERT_EQ(test, backlight_enable(ctx->bl), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(1200));
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CTL), DWI_BL_TEST_CTL_SEND);
}

/* bl_power off alone (no fb blank state) also blanks. */
static void dwi_bl_test_power_only(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, 300), 0);

	mutex_lock(&ctx->bl->update_lock);
	ctx->bl->props.power = BACKLIGHT_POWER_OFF;
	mutex_unlock(&ctx->bl->update_lock);
	KUNIT_ASSERT_EQ(test, backlight_update_status(ctx->bl), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(0));

	/* A level set while off is kept but not sent. */
	KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, 400), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(0));

	mutex_lock(&ctx->bl->update_lock);
	ctx->bl->props.power = BACKLIGHT_POWER_ON;
	mutex_unlock(&ctx->bl->update_lock);
	KUNIT_ASSERT_EQ(test, backlight_update_status(ctx->bl), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(400));
}

/* The driver asks the core to blank on suspend and restore on resume. */
static void dwi_bl_test_suspend_resume(struct kunit *test)
{
	struct dwi_bl_test *ctx = test->priv;
	const struct dev_pm_ops *pm = ctx->bl->dev.class->pm;

	KUNIT_EXPECT_TRUE(test, dwi_bl_ops.options & BL_CORE_SUSPENDRESUME);

	if (!IS_ENABLED(CONFIG_PM_SLEEP) || !pm || !pm->suspend || !pm->resume)
		kunit_skip(test, "backlight class has no sleep ops (CONFIG_PM_SLEEP=n)");

	KUNIT_ASSERT_EQ(test, backlight_device_set_brightness(ctx->bl, 900), 0);

	KUNIT_ASSERT_EQ(test, pm->suspend(&ctx->bl->dev), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(0));

	KUNIT_ASSERT_EQ(test, pm->resume(&ctx->bl->dev), 0);
	KUNIT_EXPECT_EQ(test, dwi_bl_test_reg(ctx, DWI_BL_CMD), dwi_bl_test_cmd(900));
}

static struct kunit_case dwi_bl_test_cases[] = {
	KUNIT_CASE(dwi_bl_test_encode),
	KUNIT_CASE(dwi_bl_test_send_bits),
	KUNIT_CASE(dwi_bl_test_other_regs_untouched),
	KUNIT_CASE(dwi_bl_test_max),
	KUNIT_CASE(dwi_bl_test_over_max),
	KUNIT_CASE(dwi_bl_test_zero),
	KUNIT_CASE(dwi_bl_test_round_trip),
	KUNIT_CASE(dwi_bl_test_get_masks_type),
	KUNIT_CASE(dwi_bl_test_field_overflow),
	KUNIT_CASE(dwi_bl_test_power_off_on),
	KUNIT_CASE(dwi_bl_test_power_only),
	KUNIT_CASE(dwi_bl_test_suspend_resume),
	{}
};

static struct kunit_suite dwi_bl_test_suite = {
	.name = "apple-dwi-bl",
	.init = dwi_bl_test_init,
	.test_cases = dwi_bl_test_cases,
};

kunit_test_suite(dwi_bl_test_suite);

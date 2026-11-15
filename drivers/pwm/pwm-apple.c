// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Driver for the Apple SoC PWM controller
 *
 * Copyright The Asahi Linux Contributors
 *
 * Limitations:
 * - The writes to cycle registers are shadowed until a write to
 *   the control register.
 * - If both OFF_CYCLES and ON_CYCLES are set to 0, the output
 *   is a constant off signal.
 * - When APPLE_PWM_CTRL is set to 0, the output is constant low
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/math64.h>
#include <linux/pm_runtime.h>

#define APPLE_PWM_CTRL        0x00
#define APPLE_PWM_ON_CYCLES   0x1c
#define APPLE_PWM_OFF_CYCLES  0x18

#define APPLE_PWM_CTRL_ENABLE        BIT(0)
#define APPLE_PWM_CTRL_MODE          BIT(2)
#define APPLE_PWM_CTRL_UPDATE        BIT(5)
#define APPLE_PWM_CTRL_TRIGGER       BIT(9)
#define APPLE_PWM_CTRL_INVERT        BIT(10)
#define APPLE_PWM_CTRL_OUTPUT_ENABLE BIT(14)

struct apple_pwm {
	void __iomem *base;
	struct clk *clk;
	u64 clkrate;
	u32 on_cycles;
	u32 off_cycles;
	u32 ctrl;
	bool enabled;
};

static inline struct apple_pwm *to_apple_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int apple_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			   const struct pwm_state *state)
{
	struct apple_pwm *fpwm;
	struct device *dev = pwmchip_parent(chip);
	int ret;

	if (state->polarity == PWM_POLARITY_INVERSED)
		return -EINVAL;

	fpwm = to_apple_pwm(chip);
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	if (state->enabled) {
		u64 on_cycles, off_cycles;

		on_cycles = mul_u64_u64_div_u64(fpwm->clkrate,
						state->duty_cycle, NSEC_PER_SEC);
		if (on_cycles > 0xFFFFFFFF)
			on_cycles = 0xFFFFFFFF;

		off_cycles = mul_u64_u64_div_u64(fpwm->clkrate,
						 state->period, NSEC_PER_SEC) - on_cycles;
		if (off_cycles > 0xFFFFFFFF)
			off_cycles = 0xFFFFFFFF;

		writel(on_cycles, fpwm->base + APPLE_PWM_ON_CYCLES);
		writel(off_cycles, fpwm->base + APPLE_PWM_OFF_CYCLES);
		writel(APPLE_PWM_CTRL_ENABLE | APPLE_PWM_CTRL_OUTPUT_ENABLE | APPLE_PWM_CTRL_UPDATE,
		       fpwm->base + APPLE_PWM_CTRL);
	} else {
		writel(0, fpwm->base + APPLE_PWM_CTRL);
	}

	/* Keep the controller powered for as long as it drives an output. */
	if (state->enabled && !fpwm->enabled)
		pm_runtime_get_noresume(dev);
	else if (!state->enabled && fpwm->enabled)
		pm_runtime_put_noidle(dev);
	fpwm->enabled = state->enabled;
	pm_runtime_put(dev);
	return 0;
}

static int apple_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			   struct pwm_state *state)
{
	struct apple_pwm *fpwm;
	u32 on_cycles, off_cycles, ctrl;
	struct device *dev = pwmchip_parent(chip);
	int ret;

	fpwm = to_apple_pwm(chip);
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ctrl = readl(fpwm->base + APPLE_PWM_CTRL);
	on_cycles = readl(fpwm->base + APPLE_PWM_ON_CYCLES);
	off_cycles = readl(fpwm->base + APPLE_PWM_OFF_CYCLES);

	state->enabled = (ctrl & APPLE_PWM_CTRL_ENABLE) && (ctrl & APPLE_PWM_CTRL_OUTPUT_ENABLE);
	state->polarity = PWM_POLARITY_NORMAL;
	// on_cycles + off_cycles is 33 bits, NSEC_PER_SEC is 30, there is no overflow
	state->duty_cycle = DIV64_U64_ROUND_UP((u64)on_cycles * NSEC_PER_SEC, fpwm->clkrate);
	state->period = DIV64_U64_ROUND_UP(((u64)off_cycles + (u64)on_cycles) *
					    NSEC_PER_SEC, fpwm->clkrate);

	pm_runtime_put(dev);
	return 0;
}

static const struct pwm_ops apple_pwm_ops = {
	.apply = apple_pwm_apply,
	.get_state = apple_pwm_get_state,
};

static int apple_pwm_runtime_suspend(struct device *dev)
{
	struct apple_pwm *fpwm = to_apple_pwm(dev_get_drvdata(dev));

	/* A disabled PWM can lose its register state when the domain is off. */
	fpwm->on_cycles = readl(fpwm->base + APPLE_PWM_ON_CYCLES);
	fpwm->off_cycles = readl(fpwm->base + APPLE_PWM_OFF_CYCLES);
	fpwm->ctrl = readl(fpwm->base + APPLE_PWM_CTRL);
	clk_disable_unprepare(fpwm->clk);
	return 0;
}

static int apple_pwm_runtime_resume(struct device *dev)
{
	struct apple_pwm *fpwm = to_apple_pwm(dev_get_drvdata(dev));
	int ret;

	ret = clk_prepare_enable(fpwm->clk);
	if (ret)
		return ret;

	writel(fpwm->on_cycles, fpwm->base + APPLE_PWM_ON_CYCLES);
	writel(fpwm->off_cycles, fpwm->base + APPLE_PWM_OFF_CYCLES);
	writel(fpwm->ctrl, fpwm->base + APPLE_PWM_CTRL);
	return 0;
}

static void apple_pwm_runtime_disable(void *data)
{
	struct device *dev = data;
	struct apple_pwm *fpwm = to_apple_pwm(dev_get_drvdata(dev));

	pm_runtime_disable(dev);
	if (fpwm->enabled)
		pm_runtime_put_noidle(dev);
	if (!pm_runtime_status_suspended(dev))
		clk_disable_unprepare(fpwm->clk);
	pm_runtime_set_suspended(dev);
}

static const struct dev_pm_ops apple_pwm_pm_ops = {
	RUNTIME_PM_OPS(apple_pwm_runtime_suspend, apple_pwm_runtime_resume, NULL)
};

static int apple_pwm_probe(struct platform_device *pdev)
{
	struct pwm_chip *chip;
	struct apple_pwm *fpwm;
	struct device *dev = &pdev->dev;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, 1, sizeof(*fpwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	fpwm = to_apple_pwm(chip);

	fpwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fpwm->base))
		return PTR_ERR(fpwm->base);

	fpwm->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(fpwm->clk))
		return dev_err_probe(dev, PTR_ERR(fpwm->clk), "unable to get the clock");

	/*
	 * Uses the 24MHz system clock on all existing devices, can only
	 * happen if the device tree is broken
	 *
	 * This check is done to prevent an overflow in .apply
	 */
	fpwm->clkrate = clk_get_rate(fpwm->clk);
	if (!fpwm->clkrate || fpwm->clkrate > NSEC_PER_SEC)
		return dev_err_probe(&pdev->dev, -EINVAL, "pwm clock out of range");

	chip->ops = &apple_pwm_ops;
	platform_set_drvdata(pdev, chip);

	ret = clk_prepare_enable(fpwm->clk);
	if (ret)
		return ret;
	ret = pm_runtime_set_active(dev);
	if (ret) {
		clk_disable_unprepare(fpwm->clk);
		return ret;
	}

	/* Preserve an output that the bootloader left running. */
	fpwm->ctrl = readl(fpwm->base + APPLE_PWM_CTRL);
	fpwm->enabled = (fpwm->ctrl & APPLE_PWM_CTRL_ENABLE) &&
			(fpwm->ctrl & APPLE_PWM_CTRL_OUTPUT_ENABLE);
	pm_runtime_get_noresume(dev); /* probe reference */
	if (fpwm->enabled)
		pm_runtime_get_noresume(dev); /* output reference */
	pm_runtime_enable(dev);
	ret = devm_add_action_or_reset(dev, apple_pwm_runtime_disable, dev);
	if (ret) {
		pm_runtime_put_noidle(dev);
		return ret;
	}

	ret = devm_pwmchip_add(&pdev->dev, chip);
	pm_runtime_put(dev);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "unable to add pwm chip");

	return 0;
}

static const struct of_device_id apple_pwm_of_match[] = {
	{ .compatible = "apple,s5l-fpwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_pwm_of_match);

static struct platform_driver apple_pwm_driver = {
	.probe = apple_pwm_probe,
	.driver = {
		.name = "apple-pwm",
		.of_match_table = apple_pwm_of_match,
		.pm = pm_ptr(&apple_pwm_pm_ops),
	},
};
module_platform_driver(apple_pwm_driver);

MODULE_DESCRIPTION("Apple SoC PWM driver");
MODULE_LICENSE("Dual MIT/GPL");

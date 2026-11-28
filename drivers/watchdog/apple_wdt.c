// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC Watchdog driver
 *
 * Copyright (C) The Asahi Linux Contributors
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/crash_dump.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/sleep.h>
#include <linux/watchdog.h>

/*
 * Apple Watchdog MMIO registers
 *
 * This HW block has three separate watchdogs. WD0 resets the machine to
 * recovery mode and is not used by Linux. On legacy wdt-version 2 platforms
 * WD1 triggers a normal machine reset via APPLE_WDT_CTRL_RESET_EN when
 * CUR_TIME >= BITE_TIME.
 *
 * On platforms that provide the optional "reconfig" routing register
 * (simple-reconfig-wdog-support), WD1/WD2 hardware RESET_EN does not reset
 * reliably; instead, setting bits 5:4 = 01 in the reconfig register routes
 * WD2's bark interrupt (CUR_TIME >= BARK_TIME with APPLE_WDT_CTRL_IRQ_EN
 * set and BITE_TIME = U32_MAX) to a clean cold reset.
 *
 * APPLE_WDT_WDx_CUR_TIME is a simple counter incremented for each tick of the
 * reference clock. It can also be overwritten to any value.
 * Whenever APPLE_WDT_CTRL_IRQ_EN is set and CUR_TIME >= BARK_TIME an interrupt
 * is triggered and APPLE_WDT_CTRL_IRQ_STATUS is set. The interrupt can be
 * cleared by writing 1 to APPLE_WDT_CTRL_IRQ_STATUS.
 */
#define APPLE_WDT_WD0_CUR_TIME		0x00
#define APPLE_WDT_WD0_BITE_TIME		0x04
#define APPLE_WDT_WD0_BARK_TIME		0x08
#define APPLE_WDT_WD0_CTRL		0x0c

#define APPLE_WDT_WD1_CUR_TIME		0x10
#define APPLE_WDT_WD1_BITE_TIME		0x14
#define APPLE_WDT_WD1_CTRL		0x1c

#define APPLE_WDT_WD2_CUR_TIME		0x20
#define APPLE_WDT_WD2_BITE_TIME		0x24
#define APPLE_WDT_WD2_BARK_TIME		0x28
#define APPLE_WDT_WD2_CTRL		0x2c

#define APPLE_WDT_CTRL_IRQ_EN		BIT(0)
#define APPLE_WDT_CTRL_IRQ_STATUS	BIT(1)
#define APPLE_WDT_CTRL_RESET_EN		BIT(2)

/* Optional "reconfig" register: bits 5:4 = 01 routes WD2 bark to cold reset */
#define APPLE_WDT_RECONFIG_MASK		GENMASK(5, 4)
#define APPLE_WDT_RECONFIG_WD2		BIT(4)

/* ~40 ms bark delay before simple-reconfig cold reset */
#define APPLE_WDT_REBOOT_DELAY_MS	40

#define APPLE_WDT_TIMEOUT_DEFAULT	30

struct apple_wdt {
	struct watchdog_device wdd;
	void __iomem *regs;
	void __iomem *reconfig;
	unsigned long clk_rate;
};

static struct apple_wdt *to_apple_wdt(struct watchdog_device *wdd)
{
	return container_of(wdd, struct apple_wdt, wdd);
}

static u32 apple_wdt_timeout_ticks(struct apple_wdt *wdt, unsigned int s)
{
	u32 actual = min(s, wdt->wdd.max_hw_heartbeat_ms / 1000);

	return wdt->clk_rate * actual;
}

static void apple_wdt_enable_reconfig(struct apple_wdt *wdt)
{
	u32 val = readl_relaxed(wdt->reconfig);

	writel_relaxed((val & ~APPLE_WDT_RECONFIG_MASK) | APPLE_WDT_RECONFIG_WD2,
		       wdt->reconfig);
	(void)readl(wdt->reconfig);
}

static void apple_wdt_wd2_arm(struct apple_wdt *wdt, u32 bark_ticks)
{
	/*
	 * Disarm WD2, program safe BITE/CUR/BARK, and clear any latched
	 * IRQ_STATUS before routing WD2 bark to cold reset and enabling IRQ_EN.
	 * Read back WD2_CTRL to order the watchdog-block writes before touching
	 * the separate AON reconfig mapping; apple_wdt_enable_reconfig() reads
	 * back reconfig before the final IRQ_EN write.
	 */
	writel_relaxed(0, wdt->regs + APPLE_WDT_WD2_CTRL);
	writel_relaxed(U32_MAX, wdt->regs + APPLE_WDT_WD2_BITE_TIME);
	writel_relaxed(0, wdt->regs + APPLE_WDT_WD2_CUR_TIME);
	writel_relaxed(bark_ticks, wdt->regs + APPLE_WDT_WD2_BARK_TIME);
	writel_relaxed(APPLE_WDT_CTRL_IRQ_STATUS,
		       wdt->regs + APPLE_WDT_WD2_CTRL);
	(void)readl(wdt->regs + APPLE_WDT_WD2_CTRL);
	apple_wdt_enable_reconfig(wdt);
	writel_relaxed(APPLE_WDT_CTRL_IRQ_EN, wdt->regs + APPLE_WDT_WD2_CTRL);
}

static int apple_wdt_start(struct watchdog_device *wdd)
{
	struct apple_wdt *wdt = to_apple_wdt(wdd);

	if (wdt->reconfig) {
		apple_wdt_wd2_arm(wdt,
				  apple_wdt_timeout_ticks(wdt, wdd->timeout));
		return 0;
	}

	writel_relaxed(0, wdt->regs + APPLE_WDT_WD1_CUR_TIME);
	writel_relaxed(APPLE_WDT_CTRL_RESET_EN, wdt->regs + APPLE_WDT_WD1_CTRL);

	return 0;
}

static int apple_wdt_stop(struct watchdog_device *wdd)
{
	struct apple_wdt *wdt = to_apple_wdt(wdd);
	u32 ctrl_off = wdt->reconfig ? APPLE_WDT_WD2_CTRL : APPLE_WDT_WD1_CTRL;

	writel_relaxed(0, wdt->regs + ctrl_off);

	return 0;
}

static int apple_wdt_ping(struct watchdog_device *wdd)
{
	struct apple_wdt *wdt = to_apple_wdt(wdd);
	u32 cur_off;

	if (wdt->reconfig && apple_sleep_wdt_active(wdd->parent))
		return 0;

	cur_off = wdt->reconfig ? APPLE_WDT_WD2_CUR_TIME : APPLE_WDT_WD1_CUR_TIME;
	writel_relaxed(0, wdt->regs + cur_off);

	return 0;
}

static int apple_wdt_set_timeout(struct watchdog_device *wdd, unsigned int s)
{
	struct apple_wdt *wdt = to_apple_wdt(wdd);
	u32 ticks = apple_wdt_timeout_ticks(wdt, s);

	if (wdt->reconfig) {
		writel_relaxed(0, wdt->regs + APPLE_WDT_WD2_CUR_TIME);
		writel_relaxed(ticks, wdt->regs + APPLE_WDT_WD2_BARK_TIME);
	} else {
		writel_relaxed(0, wdt->regs + APPLE_WDT_WD1_CUR_TIME);
		writel_relaxed(ticks, wdt->regs + APPLE_WDT_WD1_BITE_TIME);
	}

	wdd->timeout = s;

	return 0;
}

static unsigned int apple_wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct apple_wdt *wdt = to_apple_wdt(wdd);
	u32 cur_time, reset_time;

	if (wdt->reconfig) {
		cur_time = readl_relaxed(wdt->regs + APPLE_WDT_WD2_CUR_TIME);
		reset_time = readl_relaxed(wdt->regs + APPLE_WDT_WD2_BARK_TIME);
	} else {
		cur_time = readl_relaxed(wdt->regs + APPLE_WDT_WD1_CUR_TIME);
		reset_time = readl_relaxed(wdt->regs + APPLE_WDT_WD1_BITE_TIME);
	}

	if (cur_time >= reset_time)
		return 0;

	return (reset_time - cur_time) / wdt->clk_rate;
}

static int apple_wdt_restart(struct watchdog_device *wdd, unsigned long mode,
			     void *cmd)
{
	struct apple_wdt *wdt = to_apple_wdt(wdd);

	if (wdt->reconfig) {
		u32 ticks = max_t(u32, 1,
				  div_u64((u64)wdt->clk_rate * APPLE_WDT_REBOOT_DELAY_MS,
					  MSEC_PER_SEC));

		apple_wdt_wd2_arm(wdt, ticks);

		/*
		 * Flush writes and wait for the bark delay (~40 ms) plus SoC
		 * reset latency (~125 ms) before returning from restart.
		 */
		(void)readl(wdt->regs + APPLE_WDT_WD2_CUR_TIME);
		mdelay(200);
		return 0;
	}

	writel_relaxed(APPLE_WDT_CTRL_RESET_EN, wdt->regs + APPLE_WDT_WD1_CTRL);
	writel_relaxed(0, wdt->regs + APPLE_WDT_WD1_BITE_TIME);
	writel_relaxed(0, wdt->regs + APPLE_WDT_WD1_CUR_TIME);

	/*
	 * Flush writes and then wait for the SoC to reset. Even though the
	 * reset is queued almost immediately experiments have shown that it
	 * can take up to ~120-125ms until the SoC is actually reset. Just
	 * wait 150ms here to be safe.
	 */
	(void)readl(wdt->regs + APPLE_WDT_WD1_CUR_TIME);
	mdelay(150);

	return 0;
}

static const struct watchdog_ops apple_wdt_ops = {
	.owner = THIS_MODULE,
	.start = apple_wdt_start,
	.stop = apple_wdt_stop,
	.ping = apple_wdt_ping,
	.set_timeout = apple_wdt_set_timeout,
	.get_timeleft = apple_wdt_get_timeleft,
	.restart = apple_wdt_restart,
};

static const struct watchdog_info apple_wdt_info = {
	.identity = "Apple SoC Watchdog",
	.options = WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT,
};

static void apple_wdt_sleep_restore(void *data)
{
	struct apple_wdt *wdt = data;

	if (watchdog_active(&wdt->wdd) || watchdog_hw_running(&wdt->wdd))
		apple_wdt_start(&wdt->wdd);
}

static void apple_wdt_sleep_unregister(void *data)
{
	apple_sleep_wdt_unregister(data);
}

static int apple_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *wdt_res, *reconfig_res;
	struct apple_wdt *wdt;
	struct clk *clk;
	u32 wdt_ctrl;
	int ret;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &wdt_res);
	if (IS_ERR(wdt->regs))
		return PTR_ERR(wdt->regs);

	reconfig_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "reconfig");
	if (reconfig_res) {
		if (resource_size(reconfig_res) < sizeof(u32))
			return -EINVAL;
		wdt->reconfig = devm_ioremap(dev, reconfig_res->start,
					     resource_size(reconfig_res));
		if (!wdt->reconfig)
			return -ENOMEM;
	}

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	wdt->clk_rate = clk_get_rate(clk);
	if (!wdt->clk_rate)
		return -EINVAL;

	platform_set_drvdata(pdev, wdt);

	wdt->wdd.parent = dev;
	wdt->wdd.ops = &apple_wdt_ops;
	wdt->wdd.info = &apple_wdt_info;
	wdt->wdd.max_hw_heartbeat_ms = U32_MAX / wdt->clk_rate * 1000;
	wdt->wdd.timeout = APPLE_WDT_TIMEOUT_DEFAULT;

	watchdog_set_nowayout(&wdt->wdd, WATCHDOG_NOWAYOUT);
	watchdog_init_timeout(&wdt->wdd, 0, dev);

	if (wdt->reconfig) {
		u32 wd1_ctrl, wd2_ctrl;
		bool armed;

		/*
		 * Register the sleep handoff before touching watchdog registers
		 * so a registration failure cannot leave WD2 armed, and so devm
		 * unwinds watchdog_unregister_device() before unregistering here.
		 */
		ret = apple_sleep_wdt_register(dev, wdt_res,
					       apple_wdt_sleep_restore, wdt);
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(dev, apple_wdt_sleep_unregister, dev);
		if (ret)
			return ret;

		wd1_ctrl = readl_relaxed(wdt->regs + APPLE_WDT_WD1_CTRL);
		wd2_ctrl = readl_relaxed(wdt->regs + APPLE_WDT_WD2_CTRL);
		armed = (wd1_ctrl & APPLE_WDT_CTRL_RESET_EN) ||
			(wd2_ctrl & (APPLE_WDT_CTRL_IRQ_EN | APPLE_WDT_CTRL_RESET_EN));

		if (wd1_ctrl)
			writel_relaxed(0, wdt->regs + APPLE_WDT_WD1_CTRL);

		if (armed && !is_kdump_kernel()) {
			if (wd1_ctrl & APPLE_WDT_CTRL_RESET_EN)
				dev_info(dev, "taking over legacy WD1 watchdog as WD2 simple-reconfig\n");
			if (wd2_ctrl & APPLE_WDT_CTRL_RESET_EN)
				dev_info(dev, "taking over WD2 RESET_EN watchdog as WD2 simple-reconfig\n");
			set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
			apple_wdt_wd2_arm(wdt,
					  apple_wdt_timeout_ticks(wdt, wdt->wdd.timeout));
		} else {
			if (wd2_ctrl)
				writel_relaxed(APPLE_WDT_CTRL_IRQ_STATUS,
					       wdt->regs + APPLE_WDT_WD2_CTRL);
			apple_wdt_set_timeout(&wdt->wdd, wdt->wdd.timeout);
		}
	} else {
		wdt_ctrl = readl_relaxed(wdt->regs + APPLE_WDT_WD1_CTRL);
		if ((wdt_ctrl & APPLE_WDT_CTRL_RESET_EN) && !is_kdump_kernel())
			set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
		apple_wdt_set_timeout(&wdt->wdd, wdt->wdd.timeout);
	}

	/*
	 * A crash kernel only saves the dump and restarts. If it stalls, the
	 * machine must still restart rather than hang: arm the watchdog at its
	 * longest timeout and leave it unpinged, unless userspace opens it.
	 */
	if (is_kdump_kernel()) {
		apple_wdt_set_timeout(&wdt->wdd,
				      wdt->wdd.max_hw_heartbeat_ms / 1000);
		apple_wdt_start(&wdt->wdd);
		dev_info(dev, "crash kernel: restarting in %u s unless done\n",
			 wdt->wdd.timeout);
	}
	watchdog_stop_on_unregister(&wdt->wdd);
	watchdog_set_restart_priority(&wdt->wdd, 128);

	return devm_watchdog_register_device(dev, &wdt->wdd);
}

static int apple_wdt_resume(struct device *dev)
{
	struct apple_wdt *wdt = dev_get_drvdata(dev);

	if (wdt->reconfig && apple_sleep_wdt_active(dev))
		return 0;

	if (watchdog_active(&wdt->wdd) || watchdog_hw_running(&wdt->wdd))
		apple_wdt_start(&wdt->wdd);

	return 0;
}

static int apple_wdt_suspend(struct device *dev)
{
	struct apple_wdt *wdt = dev_get_drvdata(dev);

	if (wdt->reconfig && apple_sleep_wdt_active(dev))
		return 0;

	if (watchdog_active(&wdt->wdd) || watchdog_hw_running(&wdt->wdd))
		apple_wdt_stop(&wdt->wdd);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(apple_wdt_pm_ops, apple_wdt_suspend, apple_wdt_resume);

static const struct of_device_id apple_wdt_of_match[] = {
	{ .compatible = "apple,t8103-wdt" },
	{ .compatible = "apple,wdt" },
	{},
};
MODULE_DEVICE_TABLE(of, apple_wdt_of_match);

static struct platform_driver apple_wdt_driver = {
	.driver = {
		.name = "apple-watchdog",
		.of_match_table = apple_wdt_of_match,
		.pm = pm_sleep_ptr(&apple_wdt_pm_ops),
	},
	.probe = apple_wdt_probe,
};
module_platform_driver(apple_wdt_driver);

MODULE_DESCRIPTION("Apple SoC watchdog driver");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_LICENSE("Dual MIT/GPL");

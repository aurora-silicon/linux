// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * TRIAL ONLY, not for any PR: a kernel console on the J613 debug cable's DockChannel data page,
 * for a RAM trial whose only output is that cable. The same write loop as the sleep driver's
 * trial console, without any sleep code. Each character waits a bounded time for room and is
 * then dropped, so a cable nobody reads never stalls printk.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>

#define J613_DC_DATA		0x2e413c000ULL
#define J613_DC_TX8		0x04
#define J613_DC_TX_FREE		0x14

static void __iomem *j613_dc;

static void j613_dc_write(struct console *con, const char *str, unsigned int n)
{
	while (n--) {
		unsigned char c = *str++;
		unsigned int pass = c == '\n' ? 2 : 1;

		while (pass--) {
			unsigned int tries = 4000;

			while (!readl(j613_dc + J613_DC_TX_FREE) && --tries)
				cpu_relax();
			if (tries)
				writel(pass ? '\r' : c, j613_dc + J613_DC_TX8);
		}
	}
}

static struct console j613_dc_console = {
	.name = "j613dc",
	.write = j613_dc_write,
	.flags = CON_ENABLED | CON_ANYTIME | CON_PRINTBUFFER,
	.index = -1,
};

static int __init j613_trial_console_init(void)
{
	if (!of_machine_is_compatible("apple,j613"))
		return 0;
	j613_dc = ioremap_np(J613_DC_DATA, 0x4000);
	if (!j613_dc)
		return -ENOMEM;
	console_suspend_enabled = false;
	register_console(&j613_dc_console);
	pr_info("J613 trial: kernel console on the debug-cable DockChannel (standalone)\n");
	return 0;
}
arch_initcall(j613_trial_console_init);

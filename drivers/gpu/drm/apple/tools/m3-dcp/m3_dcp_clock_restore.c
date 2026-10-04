// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J514S diagnostic, not a clock driver. Restore only the single observed
 * before/after PMP difference at the ADT-qualified display clock register.
 * Do not infer this bit's semantics or use on another boot state/platform.
 * Run under the initialization watchdog, with native DCP unattached.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

static int __init restore(void)
{
	void __iomem *reg;
	u32 before, after;

	if (!of_machine_is_compatible("apple,j514s") ||
	    !of_machine_is_compatible("apple,t6030"))
		return -ENODEV;
	reg = ioremap_np(0x350040000ULL, 0x4000);
	if (!reg)
		return -ENOMEM;
	before = readl(reg + 0x64);
	if (before != 0x87100000) {
		pr_err("M3 DCP clock restore refuses unexpected %#x\n", before);
		iounmap(reg);
		return -EINVAL;
	}
	writel(0x85100000, reg + 0x64);
	after = readl(reg + 0x64);
	pr_info("M3 DCP clock restore before=%#x immediate=%#x\n", before, after);
	msleep(100);
	pr_info("M3 DCP clock restore after100ms=%#x\n", readl(reg + 0x64));
	iounmap(reg);
	return 0;
}
module_init(restore);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S restore observed pre-PMP display clock bit; reboot to remove");

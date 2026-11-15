// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC CPU cluster power gates
 *
 * Each CPU cluster has a PMGR word whose top bit lets its cores power down
 * in WFI. A core PMGR powers on comes up allowed to, but the boot CPU was
 * started by the boot ROM, so it only follows the word, which the boot
 * loader may leave clear. Set it for every cluster the device tree lists.
 */

#include <linux/bits.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>

#define CLUSTER_PWRGATE_EN	BIT(31)

static const struct of_device_id apple_cluster_pwrgate_of_match[] __initconst = {
	{ .compatible = "apple,cluster-pwrgate" },
	{ }
};

/* Before the secondaries start. A core picks the word up at its next WFI. */
static int __init apple_cluster_pwrgate_init(void)
{
	struct device_node *np;

	for_each_matching_node(np, apple_cluster_pwrgate_of_match) {
		void __iomem *gate;
		int i;

		if (!of_device_is_available(np))
			continue;

		/* One word per cluster. Shared with the sleep code, so not claimed. */
		for (i = 0; (gate = of_iomap(np, i)); i++) {
			u32 was = readl(gate);

			writel(was | CLUSTER_PWRGATE_EN, gate);
			pr_debug("%pOF: cluster %d power gate %#x, now %#x\n", np, i, was,
				 readl(gate));
			iounmap(gate);
		}
	}

	return 0;
}
early_initcall(apple_cluster_pwrgate_init);

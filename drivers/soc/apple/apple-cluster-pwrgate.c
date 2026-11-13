// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC CPU cluster power gates
 *
 * PMGR has power-gate words whose top bit lets a core power down in WFI. A
 * core PMGR powers on comes up allowed to, but the boot CPU was started by
 * the boot ROM, so it only follows its word, which the boot loader may leave
 * clear. Set it for every word the device tree lists. On T8140 the words are
 * described per cluster; on T6050 the page holds one word per core, and the
 * boot core's is the one left clear.
 *
 * Why it matters for suspend to RAM, as measured on T6050:
 *
 * - With its word clear the boot core never powers off, in idle or with its
 *   stop bit set, however its own registers are programmed. Every other core
 *   in the same state powers off within microseconds. Nothing is pending and
 *   nothing wakes it: it just stays on.
 *
 * - A cluster can only power down once all of its cores are off, so the boot
 *   cluster never goes down, at runtime or at sleep entry. Its request is
 *   released but never granted again, and its status word keeps the value
 *   the firmware left.
 *
 * - The SoC's sleep sequencer powers the CPU clusters down itself after the
 *   OS commits to sleep, and it runs the memory's sleep-entry step on the
 *   same engine, behind the boot cluster's power-down. Those waits have no
 *   timeout. With the boot core still on, the sequencer sits there, memory
 *   never enters self-refresh, and nothing reports an error.
 *
 * - The only symptom is the watchdog: the machine resets about five seconds
 *   after the commit, with every other sleep step looking correct. Stopping
 *   coprocessors, clocks or devices differently changes nothing, because the
 *   sequencer never gets past the cluster.
 *
 * So this must run before anything depends on the boot core gating: at boot,
 * before the secondaries start, which is also where macOS does it.
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

		/* Shared with the sleep code, so not claimed. */
		for (i = 0; (gate = of_iomap(np, i)); i++) {
			u32 was = readl(gate);

			writel(was | CLUSTER_PWRGATE_EN, gate);
			pr_debug("%pOF: power gate %d %#x, now %#x\n", np, i, was,
				 readl(gate));
			iounmap(gate);
		}
	}

	return 0;
}
early_initcall(apple_cluster_pwrgate_init);

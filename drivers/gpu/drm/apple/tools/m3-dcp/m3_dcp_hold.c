// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Explicit J514S experiment: retain inherited display domains across PMP boot.
 * No unload: keep the power floor until reboot, including on probe failure.
 */
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/delay.h>
#include <linux/of.h>

static int __init hold(void)
{
	const u32 offsets[] = {0x1c0, 0x258, 0x10000};
	struct regmap *pmgr;
	struct device_node *node;
	int ret;
	u32 before[ARRAY_SIZE(offsets)];

	if (!of_machine_is_compatible("apple,j514s"))
		return -ENODEV;
	node = of_find_node_by_path("/soc/power-management@350700000");
	if (!node)
		return -ENODEV;
	pmgr = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(pmgr))
		return PTR_ERR(pmgr);
	for (unsigned int i = 0; i < ARRAY_SIZE(offsets); i++) {
		pr_info("M3 DCP hold reading PMGR +%#x through syscon\n", offsets[i]);
		msleep(100);
		ret = regmap_read(pmgr, offsets[i], &before[i]);
		if (ret)
			return ret;
		pr_info("M3 DCP hold before PMGR +%#x = %#x\n", offsets[i], before[i]);
		if ((before[i] & 0xff) != 0xff || (before[i] & BIT(31))) {
			return -EBUSY;
		}
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(offsets); i++) {
		/* Same PS_MIN field used by apple,min-state in pmgr-pwrstate.c.
		 * Preserve automatic control and clear write-one-to-clear history.
		 */
		pr_info("M3 DCP hold raising PMGR +%#x minimum to active\n", offsets[i]);
		msleep(100);
		ret = regmap_update_bits(pmgr, offsets[i], GENMASK(19, 16) | GENMASK(9, 8),
					 GENMASK(19, 16));
		if (ret)
			return ret;
		ret = regmap_read(pmgr, offsets[i], &before[i]);
		if (ret)
			return ret;
		pr_info("M3 DCP hold after PMGR +%#x = %#x\n", offsets[i], before[i]);
	}
	return 0;
}
module_init(hold);
MODULE_LICENSE("Dual MIT/GPL");

// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Isolate the existing J514S DISP request from native DCP attachment.
 * Initialization-only diagnostic: use the bounded lifecycle watchdog.
 * Keep the vote until reboot; no unload or display submissions.
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/apple/pmp.h>
#include <linux/unaligned.h>

static int __init vote(void)
{
	struct device_node *node;
	const u8 *table;
	int length, ret = -EINVAL;

	if (!of_machine_is_compatible("apple,j514s") ||
	    !of_machine_is_compatible("apple,t6030"))
		return -ENODEV;
	node = of_find_node_by_path("/soc/pmp@350500000");
	if (!node)
		return -ENODEV;
	table = of_get_property(node, "apple,tunable-soc-device", &length);
	if (table && length == 21 * 0x7c &&
	    get_unaligned_le32(table + 7 * 0x7c) == 8 &&
	    !memcmp(table + 7 * 0x7c + 0x74, "DISP\0\0\0\0", 8))
		ret = apple_pmp_set_device_power(0x0f, 8, 1);
	of_node_put(node);
	pr_info("M3 DCP isolated PMP DISP request returned %d\n", ret);
	if (!ret)
		msleep(20);
	return ret;
}
module_init(vote);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S isolated PMP display power request; reboot to remove");

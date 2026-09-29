// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J514S PMP v2 display request, qualified by macOS pmgr-76 and ADT.
 * Initialization diagnostic only, under the lifecycle watchdog. Reboot to
 * discard the request. Never submit display or GPU work from this module.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/unaligned.h>

#ifndef M3_PTD_INSTANCE
#define M3_PTD_INSTANCE 0
#endif
static bool external = M3_PTD_INSTANCE != 0;
module_param(external, bool, 0400);
MODULE_PARM_DESC(external, "Also request qualified J514S DISPEXT0; retain until reboot");

static int __init vote(void)
{
	struct device_node *node;
	const u8 *table;
	void __iomem *ptd;
	u64 request, ack, mask, ack_mask = BIT_ULL(7);
	int length, i, found = 0;

	pr_info("M3 DCP PTD diagnostic entered; no register access yet\n");
	if (!of_machine_is_compatible("apple,j514s") ||
	    !of_machine_is_compatible("apple,t6030"))
		return -ENODEV;
	node = of_find_node_by_path("/soc/pmp@350500000");
	if (!node)
		return -ENODEV;
	table = of_get_property(node, "apple,tunable-soc-device", &length);
	if (!table || length != 21 * 0x7c ||
	    get_unaligned_le32(table + 7 * 0x7c) != 8 ||
	    memcmp(table + 7 * 0x7c + 0x74, "DISP\0\0\0\0", 8))
		goto invalid;
	/* macOS also votes for the in-use ANS controller (bit16). Without
	 * that dependency, firmware DVFS may stop storage before logs flush.
	 */
	if (get_unaligned_le32(table + 16 * 0x7c) != 17 ||
	    memcmp(table + 16 * 0x7c + 0x74, "ANS\0\0\0\0\0", 8))
		goto invalid;
	if (external) {
		/* J514S retained boot ADT, distinct from DISP at index7. The
		 * power protocol is shared; do not copy macOS26's device indices. */
		unsigned int index = 8 + M3_PTD_INSTANCE;
		if (get_unaligned_le32(table + index * 0x7c) != index + 1 ||
		    memcmp(table + index * 0x7c + 0x74, M3_PTD_INSTANCE ? "DISPEXT1" : "DISPEXT0", 8))
			goto invalid;
		ack_mask |= BIT_ULL(index);
	}
	table = of_get_property(node, "apple,tunable-ptd-range", &length);
	if (!table || length % 32)
		goto invalid;
	for (i = 0; i < length; i += 32) {
		u32 id = get_unaligned_le32(table + i);
		if (id != 10 && id != 11)
			continue;
		if (get_unaligned_le32(table + i + 4) != (id == 10 ? 280 : 284) ||
		    get_unaligned_le32(table + i + 8) != 4)
			goto invalid;
		found |= id == 10 ? 1 : 2;
	}
	if (found != 3)
		goto invalid;
	of_node_put(node);
	pr_info("M3 DCP PTD display/ANS and range tables validated\n");
	ptd = ioremap_np(0x3503c0000ULL, 0x14000);
	if (!ptd)
		return -ENOMEM;
	pr_info("M3 DCP PTD mapping ready; reading status\n");
	msleep(100);
	for (i = 0; i < 100 && readq(ptd + 16) != 1; i++)
		usleep_range(1000, 2000);
	if (readq(ptd + 16) != 1) {
		iounmap(ptd);
		return -EAGAIN;
	}
	pr_info("M3 DCP PTD status ready; reading request/ack\n");
	msleep(100);
	request = readq(ptd + 280 * 16);
	ack = readq(ptd + 284 * 16);
	pr_info("M3 DCP PTD display before request=%#llx ack=%#llx\n", request, ack);
	/* Preserve every other client bit. No other diagnostic writer may run. */
	/* DISP flags0xb require acknowledgement; ANS flags0 do not. Match
	 * ApplePMGR's flags-bit1 check, rather than requiring ANS in the ACK.
	 */
	if ((request ^ ack) & ack_mask) {
		iounmap(ptd);
		return -EBUSY;
	}
	mask = ack_mask | BIT_ULL(16);
	pr_info("M3 DCP PTD writing display/ANS request through non-posted mapping\n");
	msleep(100);
	writeq(request | mask, ptd + 0x10000 + 280 * 8);
	for (i = 0; i < 100; i++) {
		ack = readq(ptd + 284 * 16);
		if ((ack & ack_mask) == ack_mask)
			break;
		usleep_range(1000, 2000);
	}
	pr_info("M3 DCP PTD display after request=%#llx ack=%#llx polls=%d\n",
		readq(ptd + 280 * 16), ack, i);
	iounmap(ptd);
	return (ack & ack_mask) == ack_mask ? 0 : -ETIMEDOUT;
invalid:
	of_node_put(node);
	return -EINVAL;
}
module_init(vote);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S isolated PMP v2 PTD display request; reboot to remove");

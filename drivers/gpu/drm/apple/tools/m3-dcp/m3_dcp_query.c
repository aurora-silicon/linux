// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Repeat the already-qualified A410 query under an external watchdog. */
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/rtkit.h>
#include "../../dcp-link.h"
#include "m3_dcp_state.h"
static bool dump;
module_param(dump, bool, 0400);

static int __init query(void)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct m3_dcp_rtkit *dcp;
	int ret;

	if (!of_machine_is_compatible("apple,j514s"))
		return -ENODEV;
	node = of_find_node_by_path("/soc/dcp@28ec00000");
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -ENODEV;
	if (!pdev->dev.driver || strcmp(pdev->dev.driver->name, "m3-dcp-rtkit")) {
		ret = -ENODEV;
		goto out;
	}
	dcp = platform_get_drvdata(pdev);
	if (dump && dcp && dcp->rpc) {
		for (unsigned int offset = 0; offset < 0x80000; offset += 0x20000) {
			pr_info("M3 DCP RPC snapshot offset=%#x\n", offset);
			print_hex_dump(KERN_INFO, "M3 RPC ", DUMP_PREFIX_OFFSET, 16, 1,
				       dcp->rpc + offset, 256, false);
		}
		ret = 0;
		goto out;
	}
	if (!dcp || dcp->result || dcp->awaiting_rpc || !dcp->rpc) {
		ret = -EBUSY;
		goto out;
	}
	reinit_completion(&dcp->ready);
	apple_dcp_link_rpc_header_encode(dcp->rpc, 0x41343130, 0, 4);
	*(__le32 *)(dcp->rpc + 12) = cpu_to_le32(0xa5);
	dcp->result = -ETIMEDOUT;
	dcp->awaiting_rpc = true;
	dma_wmb();
	ret = apple_rtkit_send_message(dcp->rtkit, APPLE_DCP_LINK_ENDPOINT,
				      (16ULL << 32) | APPLE_DCP_LINK_MSG_RPC, NULL, false);
	if (!ret)
		ret = wait_for_completion_timeout(&dcp->ready, msecs_to_jiffies(3000)) ?
			dcp->result : -ETIMEDOUT;
	pr_info("M3 repeated A410 query ret=%d value=%u\n", ret,
		le32_to_cpup((__le32 *)(dcp->rpc + 12)));
out:
	put_device(&pdev->dev);
	return ret;
}
static void __exit done(void) {}
module_init(query);
module_exit(done);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S one-shot query of an existing DCP diagnostic session");

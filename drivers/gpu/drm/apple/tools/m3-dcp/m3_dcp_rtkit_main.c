// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Explicit-load J514S DCP bring-up client using the native DMA and RTKit APIs. */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/unaligned.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/rtkit.h>

#include "../../dcp-link.h"

#include "m3_dcp_state.h"
#include "m3_dcp_bridge.h"
#include "m3_dcp_native.h"
#include "m3_dcp_kms.h"
#include <linux/debugfs.h>
#include <linux/mutex.h>

static bool kernel_client;
module_param(kernel_client, bool, 0400);
static bool defer_client_open;
module_param(defer_client_open, bool, 0400);
MODULE_PARM_DESC(defer_client_open, "Pause native startup before A455 for staged takeover diagnosis");

static struct m3_dcp_rtkit *active;
static DEFINE_MUTEX(control_lock);
static bool kms_registered;
static bool client_opened;
static void save_properties(struct m3_dcp_rtkit *dcp);
static int control_set(const char *value, const struct kernel_param *param)
{
	int ret;

	guard(mutex)(&control_lock);
	if (!active || IS_ERR_OR_NULL(active->native) || active->result)
		return -EBUSY;
	if (sysfs_streq(value, "client-open")) {
		if (client_opened)
			return -EALREADY;
		ret = m3_dcp_native_open(active->native);
		if (!ret) {
			client_opened = true;
			save_properties(active);
		}
	} else if (!client_opened) {
		return -EAGAIN;
	} else if (sysfs_streq(value, "background")) {
		ret = m3_dcp_native_swap(active->native, NULL, 0, 0, 0, -1);
	} else if (sysfs_streq(value, "background-green")) {
		ret = m3_dcp_native_background(active->native, 0xff00ff00);
	} else if (sysfs_streq(value, "panel-on")) {
		ret = m3_dcp_native_panel(active->native, 1);
	} else if (sysfs_streq(value, "panel-mode")) {
		ret = m3_dcp_native_panel(active->native, 2);
	} else if (sysfs_streq(value, "panel-state")) {
		ret = m3_dcp_native_panel(active->native, 3);
	} else if (sysfs_streq(value, "panel-no-idle")) {
		ret = m3_dcp_native_panel(active->native, 4);
	} else if (sysfs_streq(value, "panel-identity")) {
		ret = m3_dcp_native_panel(active->native, 5);
	} else if (sysfs_streq(value, "panel-plane2")) {
		ret = m3_dcp_native_panel(active->native, 6);
	} else if (sysfs_streq(value, "panel-plane0")) {
		ret = m3_dcp_native_panel(active->native, 7);
	} else if (sysfs_streq(value, "panel-flat")) {
		ret = m3_dcp_native_panel(active->native, 8);
	} else if (sysfs_streq(value, "panel-planar")) {
		ret = m3_dcp_native_panel(active->native, 9);
	} else if (sysfs_streq(value, "kms")) {
		if (kms_registered)
			return -EALREADY;
		ret = m3_dcp_kms_register(active->native, true);
		if (!ret)
			kms_registered = true;
	} else {
		return -EINVAL;
	}
	active->result = ret;
	return ret;
}
static const struct kernel_param_ops control_ops = { .set = control_set };
module_param_cb(control, &control_ops, NULL, 0200);
MODULE_PARM_DESC(control, "Explicit guarded diagnostics after native startup: background or kms");

static void save_properties(struct m3_dcp_rtkit *dcp)
{
	const char *names[] = { "PreferredTimingElements", "DisplayAttributes" };
	struct dentry *dir = debugfs_create_dir("m3-dcp-properties", NULL);

	for (unsigned int i = 0; i < ARRAY_SIZE(names); i++) {
		struct debugfs_blob_wrapper *blob = devm_kzalloc(dcp->dev, sizeof(*blob), GFP_KERNEL);
		u32 size;
		void *data;

		if (!blob)
			continue;
		data = m3_dcp_native_property(dcp->native, names[i], &size);
		if (!data)
			continue;
		/* Immutable startup snapshots; this diagnostic remains pinned. */
		blob->data = data;
		blob->size = size;
		debugfs_create_blob(names[i], 0400, dir, blob);
	}
}

static bool start_link;
module_param(start_link, bool, 0400);
MODULE_PARM_DESC(start_link, "Also negotiate DCPLink, without issuing display RPCs");
static bool query_display;
module_param(query_display, bool, 0400);
MODULE_PARM_DESC(query_display, "Issue the M3 A410 read-only main-display query after READY");
static bool power_vote = true;
module_param(power_vote, bool, 0400);
MODULE_PARM_DESC(power_vote, "Require PMP DISP vote; false permits only the watchdog-bounded startup/query diagnostic before PMP");
static void crashed(void *cookie, const void *log, size_t size)
{
	struct m3_dcp_rtkit *dcp = cookie;

	dcp->result = -EIO;
	dev_err(dcp->dev, "DCP firmware crashed; retain all DMA allocations until reboot\n");
	if (dcp->bridge)
		m3_dcp_bridge_crashed(dcp->bridge);
	complete(&dcp->ready);
}

static void receive(void *cookie, u8 ep, u64 message)
{
	struct m3_dcp_rtkit *dcp = cookie;
	u32 alignment;

	dev_dbg(dcp->dev, "DCP app rx ep=%#x message=%#llx\n", ep, message);
	if (ep != APPLE_DCP_LINK_ENDPOINT)
		return;
	if (dcp->bridge) {
		m3_dcp_bridge_receive(dcp->bridge, message);
		return;
	}
	if (dcp->awaiting_rpc) {
		u32 output;

		dma_rmb();
		output = le32_to_cpup((__le32 *)(dcp->rpc + 12));
		if (message != APPLE_DCP_LINK_MSG_RPC_REPLY || output > 1) {
			dcp->result = -EPROTO;
		} else {
			dcp->result = 0;
			dev_info(dcp->dev, "M3 A410 reply: is_main_display=%u\n", output);
		}
		dcp->awaiting_rpc = false;
		complete(&dcp->ready);
		return;
	}
	if ((message & 3) == APPLE_DCP_LINK_MSG_FIRMWARE_INIT)
		return;
	/* J514S TEXT 0x1178b8: READY encodes alignment at bit 16,
	 * with no firmware hash. 0x1161c0 supplies the firmware side bit.
	 */
	alignment = message >> 16;
	if (message != ((64ULL << 16) | 0x101)) {
		dcp->result = -EPROTO;
	} else {
		dcp->result = 0;
		dev_info(dcp->dev, "M3 DCPLink READY alignment=%u\n", alignment);
	}
	complete(&dcp->ready);
}

static int shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct m3_dcp_rtkit *dcp = cookie;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dcp->dev);
	phys_addr_t phys;

	if (!bfr->size || bfr->size > SZ_16M || !domain)
		return -EINVAL;
	if (!bfr->iova) {
		bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size, &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;
		memset(bfr->buffer, 0, bfr->size);
		dma_wmb();
		dev_info(dcp->dev, "DCP shmem allocated size=%#zx dva=%pad\n",
			 bfr->size, &bfr->iova);
		return 0;
	}
	phys = iommu_iova_to_phys(domain, bfr->iova);
	/* Restrict firmware-supplied addresses to a declared reserved region. */
	for (int i = 0; ; i++) {
		struct device_node *node = of_parse_phandle(dcp->dev->of_node, "memory-region", i);
		struct resource res;
		bool oslog, valid;

		if (!node)
			return -ERANGE;
		oslog = of_property_read_bool(node, "apple,dcp-os-log");
		valid = !of_address_to_resource(node, 0, &res);
		of_node_put(node);
		if (!valid)
			continue;
		/* The OS-log carveout is a host physical buffer, not a DART alias. */
		if (oslog && bfr->iova == res.start)
			phys = res.start;
		if (phys < res.start || phys > res.end || bfr->size - 1 > res.end - phys)
			continue;
		if (!oslog) {
			for (size_t off = 0; off < bfr->size; off += SZ_16K)
				if (iommu_iova_to_phys(domain, bfr->iova + off) != phys + off)
					return -ERANGE;
			if (iommu_iova_to_phys(domain, bfr->iova + bfr->size - 1) != phys + bfr->size - 1)
				return -ERANGE;
		}
		bfr->buffer = memremap(phys, bfr->size, MEMREMAP_WB);
		if (!bfr->buffer)
			return -ENOMEM;
		bfr->is_mapped = true;
		dev_info(dcp->dev, "DCP shmem inherited size=%#zx dva=%pad phys=%pap\n",
			 bfr->size, &bfr->iova, &phys);
		return 0;
	}
}

static void shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct m3_dcp_rtkit *dcp = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(dcp->dev, bfr->size, bfr->buffer, bfr->iova);
}

static const struct apple_rtkit_ops ops = {
	.crashed = crashed,
	.recv_message = receive,
	.shmem_setup = shmem_setup,
	.shmem_destroy = shmem_destroy,
};

/* PMP v2's legacy device-power mailbox only acknowledges the message on
 * this firmware. The lifecycle must seed the PTD request before PMP starts.
 * Verify the request and firmware acknowledgement without changing votes.
 */
static int keep_display_power(struct device *dev)
{
	struct device_node *node = of_find_node_by_path("/soc/pmp@350500000");
	const u8 *table;
	void __iomem *ptd;
	u64 status, request, ack;
	int length, i, found = 0;

	if (!node)
		return -ENODEV;
	table = of_get_property(node, "apple,tunable-soc-device", &length);
	if (!table || length != 21 * 0x7c ||
	    get_unaligned_le32(table + 7 * 0x7c) != 8 ||
	    !(get_unaligned_le32(table + 7 * 0x7c + 8) & BIT(1)) ||
	    memcmp(table + 7 * 0x7c + 0x74, "DISP\0\0\0\0", 8))
		goto invalid;
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
	ptd = ioremap_np(0x3503c0000ULL, 0x4000);
	if (!ptd)
		return -ENOMEM;
	status = readq(ptd + 16);
	request = readq(ptd + 280 * 16);
	ack = readq(ptd + 284 * 16);
	iounmap(ptd);
	dev_info(dev, "PMP PTD display status=%#llx request=%#llx ack=%#llx\n",
		 status, request, ack);
	return status == 1 && (request & ack & BIT_ULL(7)) ? 0 : -EAGAIN;
invalid:
	of_node_put(node);
	return -EINVAL;
}

static int probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct m3_dcp_rtkit *dcp;
	void __iomem *cpu;
	const char *uuid;
	u64 message;
	int ret;

	if (kernel_client && (!power_vote || query_display))
		return -EINVAL;
	if (defer_client_open && !kernel_client)
		return -EINVAL;
	if (!of_machine_is_compatible("apple,j514s") ||
	    of_property_read_string(dev->of_node, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, "DDF38191-93B3-324A-BC8F-643006F5AC82"))
		return -ENODEV;
	if (!iommu_get_domain_for_dev(dev))
		return -ENODEV;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;
	cpu = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cpu))
		return PTR_ERR(cpu);
	dev_info(dev, "M3 DCP inherited CPU control=%#x status=%#x\n", readl(cpu + 0x44), readl(cpu + 0x48));
	if (!(readl(cpu + 0x44) & BIT(4)))
		return dev_err_probe(dev, -EBUSY, "DCP CPU stopped during handoff; do not resume interrupted firmware\n");
	if (power_vote) {
		ret = keep_display_power(dev);
		if (ret)
			return dev_err_probe(dev, ret, "DISP power vote failed\n");
	}
	if (!(readl(cpu + 0x44) & BIT(4)))
		return dev_err_probe(dev, -EIO, "DCP CPU stopped after DISP power vote\n");
	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;
	dcp->dev = dev;
	init_completion(&dcp->ready);
	platform_set_drvdata(pdev, dcp);
	dcp->rtkit = apple_rtkit_init(dev, dcp, "mbox", 0, &ops);
	if (IS_ERR(dcp->rtkit))
		return dev_err_probe(dev, PTR_ERR(dcp->rtkit), "RTKit init failed\n");
	/* This bring-up client has no qualified hot teardown. Retain its DMA and
	 * RTKit objects even on startup failure; recovery is a guarded reboot.
	 */
	__module_get(THIS_MODULE);
	ret = apple_rtkit_wake(dcp->rtkit);
	for (int n = 0; ret == -ETIME && n < 4; n++)
		ret = apple_rtkit_boot(dcp->rtkit);
	if (ret)
		goto retained;
	dev_info(dev, "M3 native RTKit boot complete; running=%d\n", apple_rtkit_is_running(dcp->rtkit));
	if (!start_link && !query_display && !kernel_client)
		goto retained;
	dcp->rpc = dma_alloc_coherent(dev, APPLE_DCP_LINK_RPC_MEMORY_SIZE,
				      &dcp->rpc_dva, GFP_KERNEL);
	if (!dcp->rpc) {
		ret = -ENOMEM;
		goto retained;
	}
	memset(dcp->rpc, 0, APPLE_DCP_LINK_RPC_MEMORY_SIZE);
	apple_dcp_link_init_desc_encode(dcp->rpc, 0, false);
	/* The imported descriptor names this field version; M3 uses alignment. */
	((struct apple_dcp_link_init_desc *)dcp->rpc)->version = cpu_to_le32(64);
	ret = apple_dcp_link_init_message(dcp->rpc_dva, &message);
	if (ret)
		goto retained;
	dcp->result = -ETIMEDOUT;
	ret = apple_rtkit_start_ep(dcp->rtkit, APPLE_DCP_LINK_ENDPOINT);
	if (ret)
		goto retained;
	dma_wmb();
	ret = apple_rtkit_send_message(dcp->rtkit, APPLE_DCP_LINK_ENDPOINT, message, NULL, false);
	if (ret)
		goto retained;
	if (!wait_for_completion_timeout(&dcp->ready, msecs_to_jiffies(3000)))
		ret = -ETIMEDOUT;
	else
		ret = dcp->result;
	if (!ret && kernel_client) {
		dcp->bridge = m3_dcp_bridge_create(dev, dcp->rtkit, dcp->rpc, true);
		if (IS_ERR(dcp->bridge)) {
			ret = PTR_ERR(dcp->bridge);
			dcp->bridge = NULL;
			goto retained;
		}
		m3_dcp_bridge_activate(dcp->bridge);
		dcp->native = m3_dcp_native_start(dev, dcp->bridge, defer_client_open);
		ret = PTR_ERR_OR_ZERO(dcp->native);
		if (!ret) {
			client_opened = !defer_client_open;
			if (client_opened)
				save_properties(dcp);
			active = dcp;
		}
	}
	if (!ret && query_display) {
		/* __TEXT 0x13f84c: no input; bool result serialized in four bytes.
		 * One request on AP stream 0 at offset 0, already 64-byte aligned.
		 */
		reinit_completion(&dcp->ready);
		apple_dcp_link_rpc_header_encode(dcp->rpc, 0x41343130, 0, 4);
		*(__le32 *)(dcp->rpc + 12) = cpu_to_le32(0xa5);
		dcp->result = -ETIMEDOUT;
		dcp->awaiting_rpc = true;
		dma_wmb();
		ret = apple_rtkit_send_message(dcp->rtkit, APPLE_DCP_LINK_ENDPOINT,
					      (16ULL << 32) | APPLE_DCP_LINK_MSG_RPC, NULL, false);
		if (!ret) {
			if (!wait_for_completion_timeout(&dcp->ready, msecs_to_jiffies(3000)))
				ret = -ETIMEDOUT;
			else
				ret = dcp->result;
		}
	}
retained:
	dcp->result = ret;
	dev_info(dev, "M3 native DCP diagnostic finished ret=%d; resources retained until reboot\n", ret);
	return 0;
}

static const struct of_device_id matches[] = {
	{ .compatible = "apple,t6030-dcp" },
	{ }
};
/* Deliberately no module alias: this diagnostic is loaded explicitly. */
static struct platform_driver m3_dcp_driver = {
	.probe = probe,
	.driver = {
		.name = "m3-dcp-rtkit",
		.of_match_table = matches,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(m3_dcp_driver);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S native RTKit/DMA DCPLink startup diagnostic");

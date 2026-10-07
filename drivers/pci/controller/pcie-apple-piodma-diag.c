// SPDX-License-Identifier: GPL-2.0-only
/*
 * J700 PIODMA diagnostic scaffolding, not a production DMA driver.
 *
 * Public Linux DMA/IOMMU APIs own the SID17 mappings. The register and
 * address-view contract is attributed in the accompanying provenance ledger
 * to the reviewed Aurora reference and fresh ADT observations.
 *
 * This built-in driver deliberately retains its coherent arena and device
 * power reference until a coordinator-controlled full hardware reset. There
 * is no unbind interface, suspend, kexec or retry. Native ECAM is only
 * opened by the host after a successful bootstrap and typed read checks.
 */
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "pcie-apple-piodma-diag.h"

#define PIODMA_PACKET_MASK	DMA_BIT_MASK(36)
#define PIODMA_POINTER_MASK	DMA_BIT_MASK(42)
#define PIODMA_POINTER_PREFIX	BIT_ULL(40)
#define PIODMA_SID		17
#define PIODMA_REQUEST_COUNT	1
#define PIODMA_SLOT_SIZE	(3 * PAGE_SIZE)
#define PIODMA_ARENA_SIZE	(PIODMA_REQUEST_COUNT * PIODMA_SLOT_SIZE)
#define PIODMA_DATA_OFFSET	(2 * PAGE_SIZE)
#define PIODMA_COMPLETION_OFFSET	PAGE_SIZE
#define PIODMA_CONFIG_BASE	0x1cb0000000ULL
#define PIODMA_CONFIG_SIZE	SZ_256M
#define PIODMA_IRQ_COMPLETE	(BIT(0) | BIT(6))
#define PIODMA_IRQ_MASK		GENMASK(9, 0)
#define PIODMA_FIFO_MASK		GENMASK(5, 0)
#define PIODMA_IRQ_LIMIT		16

static bool enumerate;
module_param(enumerate, bool, 0400);
MODULE_PARM_DESC(enumerate, "Prime once, validate native ECAM, and enumerate the two Neo functions");

struct apple_piodma_request {
	u16 offset;
	u8 function;
	u32 mask;
	u32 expected;
};

static const struct apple_piodma_request requests[PIODMA_REQUEST_COUNT] = {
	{ PCI_VENDOR_ID, 0, U32_MAX, 0x793214c3 },
};

struct apple_piodma_slot {
	struct completion event;
	__le32 packet[4];
	__le32 completion[4];
	u32 result;
	u32 matched_status;
	bool submitted;
	bool observed;
};

struct apple_piodma_diag {
	struct device *dev;
	struct iommu_domain *domain;
	void __iomem *engine;
	void __iomem *fabric;
	struct apple_piodma_slot slots[PIODMA_REQUEST_COUNT];
	atomic_t irq_count;
	atomic_t attempted;
	u32 active_tag;
	u64 config_source;
	u32 last_irq_status;
	u32 last_irq_tag;
	int irq;
	bool irq_fault;
	void *arena;
	dma_addr_t dma;
	u64 pointer_prefix;
	u8 secondary_bus;
	bool retained;
};

static int apple_piodma_diag_root(struct apple_piodma_diag *diag, struct pci_dev *root)
{
	int ret;

	if (!pci_is_root_bus(root->bus) || pci_domain_nr(root->bus) ||
	    root->bus->number || root->devfn ||
	    root->vendor != PCI_VENDOR_ID_APPLE || root->device != 0x100c ||
	    root->class != PCI_CLASS_BRIDGE_PCI << 8)
		return -ENODEV;
	ret = pci_read_config_byte(root, PCI_SECONDARY_BUS, &diag->secondary_bus);
	if (ret)
		return pcibios_err_to_errno(ret);
	if (diag->secondary_bus != 1)
		return -EINVAL;
	diag->config_source += (u64)diag->secondary_bus << 20;
	return 0;
}

static int apple_piodma_diag_iommu(struct apple_piodma_diag *diag)
{
	struct of_phandle_args args;
	u64 range[2];
	int ret;

	ret = of_parse_phandle_with_args(diag->dev->of_node, "iommus",
					"#iommu-cells", 0, &args);
	if (ret)
		return ret;
	if (args.args_count != 1 || args.args[0] != PIODMA_SID ||
	    !of_device_is_compatible(args.np, "apple,t8140-dart")) {
		ret = -EINVAL;
		goto out;
	}
	/* Keep the prior pointer convention with low IAS36 page tables. */
	if (of_property_present(args.np, "apple,dma-offset")) {
		ret = -EINVAL;
		goto out;
	}
	diag->pointer_prefix = PIODMA_POINTER_PREFIX;
	ret = of_property_read_u64_array(args.np, "apple,dma-range", range, 2);
	if (ret)
		goto out;
	if (diag->pointer_prefix != PIODMA_POINTER_PREFIX || range[0] ||
	    range[1] != BIT_ULL(36)) {
		ret = -EINVAL;
		goto out;
	}
	diag->domain = iommu_get_domain_for_dev(diag->dev);
	if (!diag->domain ||
	    (diag->domain->type != IOMMU_DOMAIN_DMA &&
	     diag->domain->type != IOMMU_DOMAIN_DMA_FQ)) {
		ret = -ENODEV;
		goto out;
	}
	ret = dma_set_mask_and_coherent(diag->dev, PIODMA_PACKET_MASK);
out:
	of_node_put(args.np);
	return ret;
}

static int apple_piodma_diag_arena(struct apple_piodma_diag *diag)
{
	u64 last, pointer;
	int i;

	diag->arena = dma_alloc_coherent(diag->dev, PIODMA_ARENA_SIZE, &diag->dma, GFP_KERNEL);
	if (!diag->arena)
		return -ENOMEM;
	if (!IS_ALIGNED(diag->dma, SZ_16K) ||
	    check_add_overflow((u64)diag->dma, PIODMA_ARENA_SIZE - 1, &last) ||
	    last > PIODMA_PACKET_MASK ||
	    check_add_overflow(last, diag->pointer_prefix, &pointer) ||
	    pointer > PIODMA_POINTER_MASK ||
	    !diag->domain->geometry.force_aperture ||
	    diag->domain->geometry.aperture_start != 0 ||
	    diag->domain->geometry.aperture_end != PIODMA_PACKET_MASK)
		return -ERANGE;
	memset(diag->arena, 0, PIODMA_ARENA_SIZE);
	for (i = 0; i < PIODMA_REQUEST_COUNT; i++) {
		void *slot = diag->arena + i * PIODMA_SLOT_SIZE;
		u64 dma = diag->dma + i * PIODMA_SLOT_SIZE;

		memset(slot + PIODMA_DATA_OFFSET, 0xa5, PAGE_SIZE);
		*(__le32 *)(slot + PIODMA_DATA_OFFSET) = cpu_to_le32(U32_MAX);
		dev_info(diag->dev,
			 "owned SID17 slot=%d dma=%#llx descriptor=%#llx completion=%#llx data=%#llx\n",
			 i, dma, dma + diag->pointer_prefix,
			 dma + diag->pointer_prefix + PIODMA_COMPLETION_OFFSET,
			 dma + PIODMA_DATA_OFFSET);
	}
	dma_wmb();
	return 0;
}



static int apple_piodma_diag_resources(struct platform_device *pdev,
				      struct apple_piodma_diag *diag)
{
	static const struct {
		const char *name;
		resource_size_t base;
	} regions[] = {
		{ "engine", 0x390030000ULL },
		{ "irq-aggregator", 0x390034000ULL },
		{ "fabric", 0x390038000ULL },
	};
	struct device_node *host;
	struct resource ecam, *res;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, regions[i].name);
		if (!res || res->start != regions[i].base || resource_size(res) != SZ_16K)
			return -EINVAL;
	}
	host = of_parse_phandle(diag->dev->of_node, "apple,pcie-host", 0);
	if (!host)
		return -EINVAL;
	ret = of_address_to_resource(host, 0, &ecam);
	if (!ret && (!of_device_is_compatible(host, "apple,t8140-pcie") ||
		     ecam.start != PIODMA_CONFIG_BASE ||
		     resource_size(&ecam) != PIODMA_CONFIG_SIZE))
		ret = -EINVAL;
	of_node_put(host);
	if (ret)
		return ret;
	diag->config_source = ecam.start;
	if (diag->config_source + (1 << 12) + sizeof(u32) - 1 > ecam.end)
		return -ERANGE;
	diag->engine = devm_platform_ioremap_resource_byname(pdev, "engine");
	if (IS_ERR(diag->engine))
		return PTR_ERR(diag->engine);
	diag->fabric = devm_platform_ioremap_resource_byname(pdev, "fabric");
	if (IS_ERR(diag->fabric))
		return PTR_ERR(diag->fabric);
	diag->irq = platform_get_irq(pdev, 0);
	return diag->irq < 0 ? diag->irq : 0;
}

static int apple_piodma_diag_idle(struct apple_piodma_diag *diag)
{
	static const struct {
		u32 offset;
		u32 expected;
	} engine[] = {
		{ 0x00, 2 }, { 0x04, 0 }, { 0x08, 0 }, { 0x0c, 16 },
		{ 0x44, 0 }, { 0x48, 0 }, { 0x4c, 0 }, { 0x50, 0 },
		{ 0x54, 0 }, { 0xd8, 0 }, { 0xdc, 0 },
	};
	int i;
	u32 value;

	for (i = 0; i < ARRAY_SIZE(engine); i++) {
		value = readl(diag->engine + engine[i].offset);
		if (value != engine[i].expected) {
			dev_err(diag->dev, "idle mismatch engine[%#x]=%#x expected=%#x\n",
				engine[i].offset, value, engine[i].expected);
			return -EBUSY;
		}
	}
	if (readl(diag->fabric) || readl(diag->fabric + 4))
		return -EBUSY;
	return 0;
}

static irqreturn_t apple_piodma_diag_irq(int irq, void *data)
{
	struct apple_piodma_diag *diag = data;
	struct apple_piodma_slot *slot;
	u32 fabric, status, tag, active;
	int count;

	/* Acknowledge order follows the attributed prior controller contract. */
	fabric = readl(diag->fabric);
	writel(fabric, diag->fabric);
	status = readl(diag->engine + 4);
	writel(status, diag->engine + 4);
	tag = readl(diag->engine + 0xdc) & 0xff;
	active = READ_ONCE(diag->active_tag);
	count = atomic_inc_return(&diag->irq_count);
	WRITE_ONCE(diag->last_irq_status, status);
	WRITE_ONCE(diag->last_irq_tag, tag);
	dev_info(diag->dev, "irq=%d fabric=%#x status=%#x tag=%#x active=%#x\n",
		 count, fabric, status, tag, active);
	if (count >= PIODMA_IRQ_LIMIT || status & ~PIODMA_IRQ_COMPLETE ||
	    (status && (!tag || tag > active))) {
		WRITE_ONCE(diag->irq_fault, true);
		disable_irq_nosync(irq);
		if (active)
			complete(&diag->slots[active - 1].event);
	} else if (status & PIODMA_IRQ_COMPLETE && tag == active && active) {
		slot = &diag->slots[active - 1];
		WRITE_ONCE(slot->matched_status, READ_ONCE(slot->matched_status) | status);
		complete(&slot->event);
	}
	/* A late old-tag IRQ never wakes or qualifies the current request. */
	return IRQ_HANDLED;
}

static int apple_piodma_diag_initialize(struct apple_piodma_diag *diag)
{
	static const struct {
		u32 offset;
		u32 mask;
		u32 value;
	} tunables[] = {
		{ 0x00, 0x2, 0 },
		{ 0x10, 0x1fffffff, 0x1fbf2 },
		{ 0x14, 0x1fffffff, 0x1fbf2 },
		{ 0x18, 0x1fffffff, 0x1fbf3 },
		{ 0x1c, 0x7ffff, 0x1fbf0 },
		{ 0x20, 0x7ffff, 0x1fbf0 },
	};
	int i;
	u32 value;

	/* All later failures retain the device, IRQ, domain and arena. */
	diag->retained = true;
	writel(PIODMA_IRQ_MASK, diag->engine + 8);
	writel(0, diag->fabric + 4);
	for (i = 0; i < ARRAY_SIZE(tunables); i++) {
		value = readl(diag->engine + tunables[i].offset);
		value = (value & ~tunables[i].mask) | tunables[i].value;
		writel(value, diag->engine + tunables[i].offset);
	}
	writel(readl(diag->engine) & ~BIT(0), diag->engine);
	/* Exact prior bank values; their architectural meaning is unqualified. */
	for (i = 0; i < 8; i++) {
		writel(0, diag->engine + 0x24 + 4 * i);
		writel(i == 1 ? 1 : 0, diag->engine + 0xf0 + 4 * i);
	}
	/* Order the bank writes before their configuration readback. */
	wmb();
	if (readl(diag->engine) || readl(diag->engine + 8) != PIODMA_IRQ_MASK ||
	    readl(diag->engine + 0xf4) != 1 || READ_ONCE(diag->irq_fault))
		return -EIO;
	return 0;
}

static bool apple_piodma_diag_canary(const u8 *memory, size_t size, u8 value)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (READ_ONCE(memory[i]) != value)
			return false;
	return true;
}

static int apple_piodma_diag_check_slots(struct apple_piodma_diag *diag)
{
	int i, j;

	dma_rmb();
	for (i = 0; i < PIODMA_REQUEST_COUNT; i++) {
		struct apple_piodma_slot *slot = &diag->slots[i];
		void *memory = diag->arena + i * PIODMA_SLOT_SIZE;
		__le32 *packet = memory;
		__le32 *completion = memory + PIODMA_COMPLETION_OFFSET;
		__le32 *result = memory + PIODMA_DATA_OFFSET;

		for (j = 0; j < ARRAY_SIZE(slot->packet); j++) {
			if (READ_ONCE(packet[j]) != slot->packet[j] ||
			    (slot->observed && READ_ONCE(completion[j]) != slot->completion[j]) ||
			    (!slot->submitted && READ_ONCE(completion[j])))
				goto changed;
		}
		if ((slot->observed && le32_to_cpu(READ_ONCE(*result)) != slot->result) ||
		    (!slot->submitted && le32_to_cpu(READ_ONCE(*result)) != U32_MAX) ||
		    !apple_piodma_diag_canary(memory + sizeof(slot->packet),
					     PAGE_SIZE - sizeof(slot->packet), 0) ||
		    !apple_piodma_diag_canary(memory + PIODMA_COMPLETION_OFFSET +
					     sizeof(slot->completion),
					     PAGE_SIZE - sizeof(slot->completion), 0) ||
		    !apple_piodma_diag_canary(memory + PIODMA_DATA_OFFSET + sizeof(u32),
					     PAGE_SIZE - sizeof(u32), 0xa5))
			goto changed;
	}
	return 0;

changed:
	dev_err(diag->dev, "slot=%d changed outside its permitted response; stop\n", i);
	return -EIO;
}

static int apple_piodma_diag_submit(struct apple_piodma_diag *diag, unsigned int index)
{
	const struct apple_piodma_request *request = &requests[index];
	struct apple_piodma_slot *slot = &diag->slots[index];
	void *memory = diag->arena + index * PIODMA_SLOT_SIZE;
	__le32 *packet = memory;
	__le32 *result = memory + PIODMA_DATA_OFFSET;
	__le32 *completion = memory + PIODMA_COMPLETION_OFFSET;
	u64 dma = diag->dma + index * PIODMA_SLOT_SIZE;
	u64 source = diag->config_source + ((u64)request->function << 12) + request->offset;
	u64 dst = dma + PIODMA_DATA_OFFSET;
	u64 src = source & PIODMA_PACKET_MASK;
	u64 descriptor = dma + diag->pointer_prefix;
	u64 comp = descriptor + PIODMA_COMPLETION_OFFSET;
	unsigned long waited;
	u32 fifo, command, tag = index + 1;
	int i, ret;

	ret = apple_piodma_diag_check_slots(diag);
	if (ret || READ_ONCE(diag->irq_fault))
		return ret ?: -EIO;
	/* Only these fixed, validated ECAM addresses use aperture1/low36. */
	slot->packet[0] = cpu_to_le32(0xc007 | BIT(18) | ((src & 0xff) << 24));
	slot->packet[1] = cpu_to_le32((src >> 8) | ((dst & 0xf) << 28));
	slot->packet[2] = cpu_to_le32(dst >> 4);
	slot->packet[3] = cpu_to_le32(sizeof(u32));
	memcpy(packet, slot->packet, sizeof(slot->packet));
	dma_wmb();
	ret = readl_poll_timeout(diag->engine + 0x0c, fifo,
				(fifo & PIODMA_FIFO_MASK) == 16, 10, 50000);
	if (ret || READ_ONCE(diag->irq_fault))
		return ret ?: -EIO;
	slot->submitted = true;
	WRITE_ONCE(diag->active_tag, tag);
	dev_info(diag->dev,
		 "request=%u function=%u offset=%#x source=%#llx packet=%08x:%08x:%08x:%08x tag=%u\n",
		 index, request->function, request->offset, source,
		 le32_to_cpu(packet[0]), le32_to_cpu(packet[1]),
		 le32_to_cpu(packet[2]), le32_to_cpu(packet[3]), tag);
	writel(tag, diag->engine + 0xd8);
	writel(upper_32_bits(descriptor) & 0x3ff, diag->engine + 0x50);
	writel(lower_32_bits(descriptor), diag->engine + 0x4c);
	writel(upper_32_bits(comp) & 0x3ff, diag->engine + 0x48);
	writel(lower_32_bits(comp), diag->engine + 0x44);
	writel(0x411, diag->engine + 0x54);
	waited = wait_for_completion_timeout(&slot->event, msecs_to_jiffies(50));
	dma_rmb();
	slot->result = le32_to_cpu(READ_ONCE(*result));
	for (i = 0; i < ARRAY_SIZE(slot->completion); i++)
		slot->completion[i] = READ_ONCE(completion[i]);
	slot->observed = true;
	dev_info(diag->dev,
		 "response=%u waited=%lu matched_status=%#x word=%#x completion=%08x:%08x:%08x:%08x\n",
		 index, waited, READ_ONCE(slot->matched_status), slot->result,
		 le32_to_cpu(slot->completion[0]), le32_to_cpu(slot->completion[1]),
		 le32_to_cpu(slot->completion[2]), le32_to_cpu(slot->completion[3]));
	if (!waited)
		return -ETIMEDOUT;
	if (READ_ONCE(diag->irq_fault) || !READ_ONCE(slot->matched_status) ||
	    slot->result == U32_MAX ||
	    (slot->result & request->mask) != request->expected)
		return -EIO;
	/* Admission observations only: all published memory remains retained. */
	ret = readl_poll_timeout(diag->engine + 0x54, command, !(command & BIT(0)),
				10, 50000);
	if (ret)
		return ret;
	ret = readl_poll_timeout(diag->engine + 0x0c, fifo,
				(fifo & PIODMA_FIFO_MASK) == 16, 10, 50000);
	if (ret)
		return ret;
	for (i = 0; i < 10; i++) {
		usleep_range(1000, 2000);
		ret = apple_piodma_diag_check_slots(diag);
		if (ret || READ_ONCE(diag->irq_fault))
			return ret ?: -EIO;
	}
	dev_info(diag->dev,
		 "READ_VALIDATED request=%u word=%#x FIFO=%#x command=%#x; retained slots stable\n",
		 index, slot->result, fifo, command);
	return 0;
}

bool apple_piodma_bootstrap_enabled(void)
{
	return enumerate;
}

static void apple_piodma_put_supplier(void *data)
{
	put_device(data);
}

int apple_piodma_bootstrap_get(struct device *host, struct device **supplier)
{
	struct device_node *node, *host_node;
	struct platform_device *pdev;
	struct device_link *link;
	bool correct_host;
	int ret;

	if (!enumerate)
		return -ENODEV;
	node = of_parse_phandle(host->of_node, "apple,piodma", 0);
	if (!node)
		return -EINVAL;
	if (!of_device_is_compatible(node, "apple,t8140-piodma-diagnostic")) {
		of_node_put(node);
		return -EINVAL;
	}
	host_node = of_parse_phandle(node, "apple,pcie-host", 0);
	correct_host = host_node == host->of_node;
	of_node_put(host_node);
	pdev = correct_host ? of_find_device_by_node(node) : NULL;
	of_node_put(node);
	if (!correct_host)
		return -EINVAL;
	if (!pdev)
		return -EPROBE_DEFER;
	if (!device_is_bound(&pdev->dev) || !platform_get_drvdata(pdev)) {
		put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}
	link = device_link_add(host, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		put_device(&pdev->dev);
		return -EINVAL;
	}
	ret = devm_add_action_or_reset(host, apple_piodma_put_supplier, &pdev->dev);
	if (ret)
		return ret;
	*supplier = &pdev->dev;
	return 0;
}

int apple_piodma_bootstrap_prime(struct device *supplier, struct pci_dev *root)
{
	struct apple_piodma_diag *diag = dev_get_drvdata(supplier);
	int ret;

	/* One attempt for this provider's entire lifetime, including failures. */
	if (atomic_cmpxchg(&diag->attempted, 0, 1))
		return -EALREADY;
	ret = apple_piodma_diag_root(diag, root);
	if (ret)
		return ret;
	ret = apple_piodma_diag_idle(diag);
	if (ret)
		return ret;
	ret = apple_piodma_diag_initialize(diag);
	if (!ret)
		ret = apple_piodma_diag_submit(diag, 0);
	if (ret)
		dev_err(supplier, "bootstrap failure=%d; retained ownership; no retry\n", ret);
	else
		dev_info(supplier, "BOOTSTRAP_VALIDATED word=%#x; one retained slot\n",
			 diag->slots[0].result);
	return ret;
}

static int apple_piodma_diag_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_piodma_diag *diag;
	int i, ret;

	if (!enumerate || !of_machine_is_compatible("apple,j700") ||
	    !of_machine_is_compatible("apple,t8140") || PAGE_SIZE != SZ_16K)
		return -ENODEV;
	diag = kzalloc_obj(*diag);
	if (!diag)
		return -ENOMEM;
	diag->dev = dev;
	for (i = 0; i < PIODMA_REQUEST_COUNT; i++)
		init_completion(&diag->slots[i].event);
	atomic_set(&diag->irq_count, 0);
	atomic_set(&diag->attempted, 0);
	ret = apple_piodma_diag_iommu(diag);
	if (ret)
		goto err_free;
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto err_pm;
	ret = apple_piodma_diag_arena(diag);
	if (ret)
		goto err_arena;
	ret = apple_piodma_diag_resources(pdev, diag);
	if (ret)
		goto err_arena;
	ret = devm_request_irq(dev, diag->irq, apple_piodma_diag_irq, 0,
			       dev_name(dev), diag);
	if (ret)
		goto err_arena;
	platform_set_drvdata(pdev, diag);
	dev_info(dev, "bootstrap supplier ready; no command submitted during probe\n");
	return 0;

err_arena:
	/* No path after initialization may release ownership. */
	if (WARN_ON_ONCE(diag->retained))
		return 0;
	if (diag->arena)
		dma_free_coherent(dev, PIODMA_ARENA_SIZE, diag->arena, diag->dma);
	pm_runtime_put_sync(dev);
err_pm:
	pm_runtime_disable(dev);
err_free:
	kfree(diag);
	return dev_err_probe(dev, ret, "PIODMA diagnostic preflight failed\n");
}

static void apple_piodma_diag_shutdown(struct platform_device *pdev)
{
	dev_warn(&pdev->dev, "diagnostic arena retained; require full hardware reset\n");
}

static const struct of_device_id apple_piodma_diag_match[] = {
	{ .compatible = "apple,t8140-piodma-diagnostic" },
	{ }
};

static struct platform_driver apple_piodma_diag_driver = {
	.probe = apple_piodma_diag_probe,
	.shutdown = apple_piodma_diag_shutdown,
	.driver = {
		.name = "apple-piodma-diag",
		.of_match_table = apple_piodma_diag_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(apple_piodma_diag_driver);
MODULE_DESCRIPTION("T8140 retained one-read PCIe bootstrap experiment");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0
/*
 * MSI controller for one Apple PCIe port whose link the bootloader trained.
 *
 * pcie-apple owns the port when Linux brings the link up itself. On T8142
 * (J813) the bootloader trains apcie port 0 and Linux drives the root port as
 * a generic ECAM host, which has no MSI support. The port still turns writes
 * to the MSI doorbell into AIC interrupts through its MSI map; this driver
 * programs that map and provides the parent domain, as pcie-apple does.
 */

#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/irqdomain.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#define PORT_MSICFG		0x0124
#define   PORT_MSICFG_EN	BIT(0)
#define PORT_MSIADDR		0x016c
#define PORT_MSIMAP		0x3800
#define   PORT_MSIMAP_ENABLE	BIT(31)

#define DOORBELL_ADDR		CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR

struct apple_msi {
	struct mutex lock;
	struct irq_fwspec fwspec;
	unsigned long *bitmap;
	u32 nvecs;
};

static void apple_msi_compose_msg(struct irq_data *data, struct msi_msg *msg)
{
	msg->address_hi = upper_32_bits(DOORBELL_ADDR);
	msg->address_lo = lower_32_bits(DOORBELL_ADDR);
	msg->data = data->hwirq;
}

static struct irq_chip apple_msi_bottom_chip = {
	.name			= "MSI",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_set_type		= irq_chip_set_type_parent,
	.irq_compose_msi_msg	= apple_msi_compose_msg,
};

static int apple_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	struct apple_msi *msi = domain->host_data;
	struct irq_fwspec fwspec = msi->fwspec;
	unsigned int i;
	int ret, hwirq;

	mutex_lock(&msi->lock);
	hwirq = bitmap_find_free_region(msi->bitmap, msi->nvecs,
					order_base_2(nr_irqs));
	mutex_unlock(&msi->lock);
	if (hwirq < 0)
		return -ENOSPC;

	fwspec.param[fwspec.param_count - 2] += hwirq;

	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &fwspec);
	if (ret) {
		mutex_lock(&msi->lock);
		bitmap_release_region(msi->bitmap, hwirq, order_base_2(nr_irqs));
		mutex_unlock(&msi->lock);
		return ret;
	}

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &apple_msi_bottom_chip, msi);
	return 0;
}

static void apple_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct apple_msi *msi = domain->host_data;

	mutex_lock(&msi->lock);
	bitmap_release_region(msi->bitmap, d->hwirq, order_base_2(nr_irqs));
	mutex_unlock(&msi->lock);
}

static const struct irq_domain_ops apple_msi_domain_ops = {
	.alloc	= apple_msi_domain_alloc,
	.free	= apple_msi_domain_free,
};

static const struct msi_parent_ops apple_msi_parent_ops = {
	.supported_flags	= (MSI_GENERIC_FLAGS_MASK	|
				   MSI_FLAG_PCI_MSIX		|
				   MSI_FLAG_MULTI_PCI_MSI),
	.required_flags		= (MSI_FLAG_USE_DEF_DOM_OPS	|
				   MSI_FLAG_USE_DEF_CHIP_OPS	|
				   MSI_FLAG_PCI_MSI_MASK_PARENT),
	.chip_flags		= MSI_CHIP_FLAG_SET_EOI,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

static int apple_msi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct of_phandle_args args = {};
	struct irq_domain_info info = {
		.fwnode		= fwnode,
		.ops		= &apple_msi_domain_ops,
	};
	struct apple_msi *msi;
	void __iomem *port;
	int ret;
	u32 i;

	msi = devm_kzalloc(dev, sizeof(*msi), GFP_KERNEL);
	if (!msi)
		return -ENOMEM;
	mutex_init(&msi->lock);

	port = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(port))
		return PTR_ERR(port);

	ret = of_parse_phandle_with_args(dev->of_node, "msi-ranges",
					 "#interrupt-cells", 0, &args);
	if (ret)
		return ret;
	ret = of_property_read_u32_index(dev->of_node, "msi-ranges",
					 args.args_count + 1, &msi->nvecs);
	if (ret) {
		of_node_put(args.np);
		return ret;
	}
	of_phandle_args_to_fwspec(args.np, args.args, args.args_count,
				  &msi->fwspec);
	of_node_put(args.np);

	msi->bitmap = devm_bitmap_zalloc(dev, msi->nvecs, GFP_KERNEL);
	if (!msi->bitmap)
		return -ENOMEM;

	info.parent = irq_find_matching_fwspec(&msi->fwspec, DOMAIN_BUS_WIRED);
	if (!info.parent)
		return dev_err_probe(dev, -EPROBE_DEFER, "no parent domain\n");
	info.size = msi->nvecs;
	info.host_data = msi;

	/* MSI data N goes to the Nth interrupt of msi-ranges. */
	writel(lower_32_bits(DOORBELL_ADDR), port + PORT_MSIADDR);
	for (i = 0; i < msi->nvecs; i++)
		writel(PORT_MSIMAP_ENABLE | i, port + PORT_MSIMAP + 4 * i);
	writel(readl(port + PORT_MSICFG) | PORT_MSICFG_EN, port + PORT_MSICFG);

	if (!msi_create_parent_irq_domain(&info, &apple_msi_parent_ops))
		return -ENOMEM;
	dev_info(dev, "%u MSIs at %#x\n", msi->nvecs, DOORBELL_ADDR);
	return 0;
}

static const struct of_device_id apple_msi_of_match[] = {
	{ .compatible = "apple,t8142-pcie-msi" },
	{ }
};

static struct platform_driver apple_msi_driver = {
	.probe	= apple_msi_probe,
	.driver	= {
		.name			= "pcie-apple-msi",
		.of_match_table		= apple_msi_of_match,
		.suppress_bind_attrs	= true,
	},
};
builtin_platform_driver(apple_msi_driver);

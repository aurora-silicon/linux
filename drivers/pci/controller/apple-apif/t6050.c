// SPDX-License-Identifier: GPL-2.0-only
#include <linux/apple-dart-apif.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include "t6050.h"
#include "t6050_pcie_sequence.h"
#include "t6050_pcie_disable.h"
#include "t6050_pcie_gpio.h"

struct t6050_pcie {
	struct device *dev;
	struct mutex lock;
	void __iomem *port, *phy, *gpio, *intr2axi;
	struct pci_dev *bridge, *endpoint;
	struct t6050_pcie_io io;
	struct t6050_gpio_pad_io pad;
	u64 doorbell;
	unsigned int nvecs;
	u32 routes[3 * 64];
	unsigned int nroutes;
};

static void __iomem *t6050_reg(struct t6050_pcie *pcie, unsigned int reg,
			       unsigned int off)
{
	if (off & 3)
		return NULL;
	switch (reg) {
	case T6050_PCIE_PORT0:
		return off < 0x8000 ? pcie->port + off : NULL;
	case T6050_PCIE_PORT0_PHY:
		return off < 0x4000 ? pcie->phy + off : NULL;
	case T6050_PCIE_PORT0_INTR2AXI:
		return off < 0x4000 ? pcie->intr2axi + off : NULL;
	default:
		return NULL;
	}
}

static int t6050_read(void *ctx, unsigned int reg, unsigned int off, u32 *value)
{
	struct t6050_pcie *pcie = ctx;
	void __iomem *addr;
	if (reg == T6050_PCIE_ECAM)
		return pcie->bridge ?
			       pcibios_err_to_errno(pci_read_config_dword(
				       pcie->bridge, off, value)) :
			       -ENODEV;
	addr = t6050_reg(pcie, reg, off);
	if (!addr)
		return -EINVAL;
	*value = readl(addr);
	return 0;
}

static int t6050_write(void *ctx, unsigned int reg, unsigned int off, u32 value)
{
	struct t6050_pcie *pcie = ctx;
	void __iomem *addr;
	if (reg == T6050_PCIE_ECAM)
		return pcie->bridge ?
			       pcibios_err_to_errno(pci_write_config_dword(
				       pcie->bridge, off, value)) :
			       -ENODEV;
	addr = t6050_reg(pcie, reg, off);
	if (!addr)
		return -EINVAL;
	writel(value, addr);
	return 0;
}

static int t6050_read16(void *ctx, unsigned int reg, unsigned int off,
			u16 *value)
{
	struct t6050_pcie *pcie = ctx;
	if (reg != T6050_PCIE_ECAM || !pcie->bridge)
		return -EINVAL;
	return pcibios_err_to_errno(
		pci_read_config_word(pcie->bridge, off, value));
}

static int t6050_write16(void *ctx, unsigned int reg, unsigned int off,
			 u16 value)
{
	struct t6050_pcie *pcie = ctx;
	if (reg != T6050_PCIE_ECAM || !pcie->bridge)
		return -EINVAL;
	return pcibios_err_to_errno(
		pci_write_config_word(pcie->bridge, off, value));
}

static int t6050_poll(void *ctx, unsigned int reg, unsigned int off, u32 mask,
		      u32 expected, unsigned int timeout)
{
	struct t6050_pcie *pcie = ctx;
	u32 value = 0;
	int ret, err;
	ret = read_poll_timeout(t6050_read, err,
				err || (value & mask) == expected, 1, timeout,
				false, ctx, reg, off, &value);
	if (ret || err)
		dev_err(pcie->dev,
			"port wait reg%u+%#x value=%#x mask=%#x expected=%#x\n",
			reg, off, value, mask, expected);
	return err ?: ret;
}

static void t6050_delay(void *ctx, unsigned int us)
{
	if (us <= 10)
		udelay(us);
	else
		usleep_range(us, us + max(1U, us / 10));
}

static int t6050_tune(void *ctx, const char *name, unsigned int reg)
{
	struct t6050_pcie *pcie = ctx;
	char property[64];
	u32 cells[64], old;
	int count, ret;
	snprintf(property, sizeof(property), "apple,%s", name);
	if (!of_find_property(pcie->dev->of_node, property, NULL))
		return 0;
	count = of_property_count_u32_elems(pcie->dev->of_node, property);
	if (count <= 0 || count > ARRAY_SIZE(cells) || count % 4)
		return -EINVAL;
	ret = of_property_read_u32_array(pcie->dev->of_node, property, cells,
					 count);
	if (ret)
		return ret;
	/* Validate every entry before the first mutation. */
	for (int i = 0; i < count; i += 4)
		if (cells[i + 1] != 4 || (cells[i] & 3) ||
		    (reg == T6050_PCIE_ECAM ?
			     cells[i] >= PCI_CFG_SPACE_EXP_SIZE :
			     !t6050_reg(pcie, reg, cells[i])))
			return -EINVAL;
	for (int i = 0; i < count; i += 4) {
		ret = t6050_read(ctx, reg, cells[i], &old);
		if (ret)
			return ret;
		ret = t6050_write(ctx, reg, cells[i],
				  (old & ~cells[i + 2]) | cells[i + 3]);
		if (ret)
			return ret;
	}
	return 0;
}

static int t6050_gpio_read(void *ctx, u32 *value)
{
	*value = readl(((struct t6050_pcie *)ctx)->gpio);
	return 0;
}
static int t6050_gpio_write(void *ctx, u32 value)
{
	writel(value, ((struct t6050_pcie *)ctx)->gpio);
	return 0;
}
static int t6050_reset(void *ctx, bool asserted)
{
	return t6050_pcie_gpio_perst(&((struct t6050_pcie *)ctx)->pad,
				     asserted);
}
static int t6050_force_available(void *ctx, bool on)
{
	return apple_dart_apif_set_power(
		&((struct t6050_pcie *)ctx)->endpoint->dev, on);
}

struct t6050_pcie *t6050_pcie_init(struct platform_device *pdev, u64 doorbell,
				   unsigned int nvecs)
{
	struct t6050_pcie *pcie;
	struct resource *r;
	u32 speed, lanes;
	int ret;

	if (!of_device_is_compatible(pdev->dev.of_node,
				     "apple,t6050-pcie-apif"))
		return NULL;
	if (of_property_read_u32(pdev->dev.of_node, "max-link-speed", &speed) ||
	    of_property_read_u32(pdev->dev.of_node, "num-lanes", &lanes) ||
	    speed != 3 || lanes != 1)
		return ERR_PTR(-EINVAL);
	pcie = devm_kzalloc(&pdev->dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return ERR_PTR(-ENOMEM);
	pcie->dev = &pdev->dev;
	pcie->doorbell = doorbell;
	pcie->nvecs = nvecs;
	ret = of_property_count_u32_elems(pdev->dev.of_node,
					  "apple,rid-to-sid");
	if (ret <= 0 || ret > ARRAY_SIZE(pcie->routes) || ret % 3)
		return ERR_PTR(-EINVAL);
	pcie->nroutes = ret / 3;
	ret = of_property_read_u32_array(pdev->dev.of_node, "apple,rid-to-sid",
					 pcie->routes, ret);
	if (ret)
		return ERR_PTR(ret);
	for (unsigned int i = 0; i < pcie->nroutes; i++) {
		u32 *r = &pcie->routes[3 * i];
		if (!r[0] || r[0] >= 16 || r[1] > 0xffff || r[0] != r[2])
			return ERR_PTR(-EINVAL);
		struct device_node *target = NULL;
		u32 sid;

		ret = of_map_id(pdev->dev.of_node, r[1], "iommu-map",
				"iommu-map-mask", &target, &sid);
		if (ret || !target ||
		    !of_device_is_compatible(target, "apple,dart-apif") ||
		    sid != r[2]) {
			of_node_put(target);
			return ERR_PTR(-EINVAL);
		}
		of_node_put(target);
		for (unsigned int j = 0; j < i; j++)
			if (r[0] == pcie->routes[3 * j] ||
			    r[1] == pcie->routes[3 * j + 1])
				return ERR_PTR(-EINVAL);
	}

	mutex_init(&pcie->lock);
	pcie->port = devm_platform_ioremap_resource_byname(pdev, "port");
	pcie->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	pcie->gpio = devm_platform_ioremap_resource_byname(pdev, "perst");
	pcie->intr2axi =
		devm_platform_ioremap_resource_byname(pdev, "intr2axi");
	if (IS_ERR(pcie->port))
		return ERR_CAST(pcie->port);
	if (IS_ERR(pcie->phy))
		return ERR_CAST(pcie->phy);
	if (IS_ERR(pcie->gpio))
		return ERR_CAST(pcie->gpio);
	if (IS_ERR(pcie->intr2axi))
		return ERR_CAST(pcie->intr2axi);
	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "port");
	if (resource_size(r) < 0x8000)
		return ERR_PTR(-EINVAL);
	for (unsigned int i = 0; i < 3; i++) {
		static const char *const names[] = { "phy", "perst",
						     "intr2axi" };
		r = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						 names[i]);
		if (!r || resource_size(r) < (i == 1 ? 4 : 0x4000))
			return ERR_PTR(-EINVAL);
	}
	pcie->pad = (struct t6050_gpio_pad_io){ .context = pcie,
						.read = t6050_gpio_read,
						.write = t6050_gpio_write };
	pcie->io = (struct t6050_pcie_io){ .context = pcie,
					   .read = t6050_read,
					   .write = t6050_write,
					   .poll = t6050_poll,
					   .apply_tunables = t6050_tune,
					   .apply_port_tunables = t6050_tune,
					   .read16 = t6050_read16,
					   .write16 = t6050_write16,
					   .delay = t6050_delay,
					   .endpoint_reset = t6050_reset };

	ret = t6050_pcie_configure_msi_port0(&pcie->io, pcie->doorbell,
					     pcie->nvecs);
	return ret ? ERR_PTR(ret) : pcie;
}

int t6050_pcie_cycle(struct t6050_pcie *pcie, struct pci_dev *endpoint)
{
	struct pci_dev *bridge = endpoint->bus->self, *sibling;
	u32 value, identity;
	u16 command, pmctrl;
	int ret, pm;

	if (!bridge || endpoint->vendor != PCI_VENDOR_ID_APPLE ||
	    (endpoint->device != 0x1900 && endpoint->device != 0x1901))
		return -ENODEV;

	if (!pci_trylock_rescan_remove())
		return -EAGAIN;
	mutex_lock(&pcie->lock);
	list_for_each_entry(sibling, &endpoint->bus->devices, bus_list) {
		if (sibling != endpoint) {
			ret = -EBUSY;
			goto out;
		}
	}
	pcie->bridge = bridge;
	pcie->endpoint = endpoint;
	ret = pci_save_state(bridge);
	if (ret)
		goto out;
	ret = pci_save_state(endpoint);
	if (ret)
		goto out;
	ret = pcibios_err_to_errno(
		pci_read_config_word(endpoint, PCI_COMMAND, &command));
	if (ret)
		goto out;
	pm = pci_find_capability(endpoint, PCI_CAP_ID_PM);
	if (!pm) {
		ret = -ENODEV;
		goto out;
	}
	/* This API is only for the isolated ROM/preboot function. */
	pci_clear_master(endpoint);
	ret = pcibios_err_to_errno(
		pci_read_config_word(endpoint, PCI_COMMAND, &pmctrl));
	if (ret || (pmctrl & PCI_COMMAND_MASTER)) {
		ret = ret ?: -EIO;
		goto stopped;
	}
	if (!pci_wait_for_pending_transaction(endpoint)) {
		ret = -EBUSY;
		goto stopped;
	}
	ret = pcibios_err_to_errno(
		pci_read_config_word(endpoint, pm + PCI_PM_CTRL, &pmctrl));
	if (ret)
		goto stopped;
	ret = pcibios_err_to_errno(pci_write_config_word(
		endpoint, pm + PCI_PM_CTRL,
		(pmctrl & ~PCI_PM_CTRL_STATE_MASK) | PCI_D3hot |
			PCI_PM_CTRL_PME_ENABLE));
	if (ret)
		goto stopped;
	usleep_range(10000, 11000);

	ret = pcibios_err_to_errno(
		pci_write_config_word(bridge, PCI_COMMAND, 0));
	if (ret)
		goto stopped;
	ret = pcibios_err_to_errno(
		pci_read_config_dword(bridge, PCI_PRIMARY_BUS, &value));
	if (ret)
		goto stopped;
	ret = pcibios_err_to_errno(pci_write_config_dword(
		bridge, PCI_PRIMARY_BUS, value & 0xff0000ff));
	if (ret)
		goto stopped;
	value = readl(pcie->port + 0x800);
	writel(value | 0x100, pcie->port + 0x800);
	writel(0x11, pcie->port + 0x88);

	ret = t6050_poll(pcie, T6050_PCIE_PORT0, 0x88, 1, 0, 500000);
	if (ret)
		goto stopped;
	ret = t6050_poll(pcie, T6050_PCIE_PORT0, 0x208, BIT(6), BIT(6), 500000);
	if (ret)
		goto stopped;
	ret = t6050_reset(pcie, true);
	if (ret)
		goto stopped;
	writel(readl(pcie->phy) & ~BIT(31), pcie->phy);
	ret = t6050_pcie_disable_port0(&pcie->io, t6050_force_available,
				       250000);
	if (ret)
		goto stopped;
	ret = t6050_pcie_enable_port0(&pcie->io, 250000);
	if (ret)
		goto stopped;
	ret = t6050_pcie_configure_port0(&pcie->io, 3);
	if (ret)
		goto stopped;

	for (unsigned int i = 0; i < pcie->nroutes; i++) {
		u32 *r = &pcie->routes[3 * i];
		writel(BIT(31) | (r[2] << 16) | r[1],
		       pcie->port + 0x3000 + 4 * r[0]);
	}
	ret = t6050_pcie_configure_msi_port0(&pcie->io, pcie->doorbell,
					     pcie->nvecs);
	if (ret)
		goto stopped;
	ret = t6050_force_available(pcie, true);
	if (ret)
		goto stopped;
	ret = t6050_pcie_train_port0(&pcie->io, 500000);
	if (ret)
		goto stopped;
	pci_restore_state(bridge);
	ret = pcibios_err_to_errno(
		pci_read_config_dword(endpoint, PCI_VENDOR_ID, &identity));
	if (ret)
		goto stopped;
	if (identity != 0x1900106b && identity != 0x1901106b) {
		ret = -ENODEV;
		goto stopped;
	}
	pci_restore_state(endpoint);
	ret = pcibios_err_to_errno(pci_write_config_word(
		endpoint, pm + PCI_PM_CTRL, pmctrl & ~PCI_PM_CTRL_STATE_MASK));
	if (ret)
		goto stopped;
	usleep_range(10000, 11000);
	ret = pcibios_err_to_errno(
		pci_write_config_word(endpoint, PCI_COMMAND, command));
	if (ret)
		goto stopped;
	goto out;
stopped:
	/* A partial transition never restores endpoint DMA or bridge decoding. */
	pci_clear_master(endpoint);
	pci_write_config_word(bridge, PCI_COMMAND, 0);
	dev_err(&endpoint->dev, "T6050 port cycle stopped: %d\n", ret);
out:
	pcie->endpoint = pcie->bridge = NULL;
	mutex_unlock(&pcie->lock);
	pci_unlock_rescan_remove();
	return ret;
}

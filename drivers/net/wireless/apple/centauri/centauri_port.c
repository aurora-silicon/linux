// SPDX-License-Identifier: GPL-2.0-only
/* J813 preboot -> boot port cycle, transcribed from the measured proxy
 * alpha_wifi_scan.py pcie_cycle/apple_teardown/apple_bringup sequence.
 * These apertures were verified from the live ADT before this Linux boot.
 * This temporary board helper belongs in the host-controller driver once
 * T8142 platform/clock/GPIO bindings are available.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/pci.h>
#include "centauri.h"

static void update(void __iomem *base, u32 offset, u32 clear, u32 set)
{
	writel((readl(base + offset) & ~clear) | set, base + offset);
}

static bool wait_bits(void __iomem *address, u32 mask, u32 wanted, u32 us)
{
	u32 value;

	return !readl_poll_timeout(address, value, (value & mask) == wanted, 100, us);
}

int centauri_j813_port_cycle(struct pci_dev *pdev)
{
	static const u32 reset[][2] = {
		{0x88, 0x110}, {0x100, ~0U}, {0x148, ~0U}, {0x210, ~0U},
		{0x80, 0}, {0x84, 0}, {0x104, 0xfffffff0}, {0x124, 0x100},
		{0x16c, 0}, {0x13c, 0}, {0x800, 0x00100100},
		{0x808, 0x001001ff}, {0x82c, 0x00010000},
	};
	static const u32 tune[][2] = {
		{0x130, 0x03020000}, {0x140, 0x10}, {0x144, 0x00253770},
		{0x21c, 0}, {0x834, 0}, {0x83c, 0},
	};
	struct pci_dev *bridge = pdev->bus->self;
	void __iomem *port, *phy, *pad;
	u32 value, gpio, identity;
	u16 link;
	unsigned int lanes, i;
	int ret = -ETIMEDOUT, pm;
	bool ack, l2;

	if (!of_machine_is_compatible("apple,j813") || !bridge ||
	    bridge->vendor != PCI_VENDOR_ID_APPLE || bridge->device != 0x100c)
		return -ENODEV;
	port = ioremap(0x488028000ULL, 0x4000);
	phy = ioremap(0x48f010000ULL, 0x1000);
	pad = ioremap(0x39a000300ULL, sizeof(u32));
	if (!port || !phy || !pad) {
		ret = -ENOMEM;
		goto out;
	}
	gpio = readl(pad);
	if (gpio & BIT(21)) {
		ret = -EPERM;
		goto out;
	}
	ret = pci_save_state(bridge);
	if (ret)
		goto out;
	ret = pci_save_state(pdev);
	if (ret)
		goto out;
	pcie_capability_read_word(bridge, PCI_EXP_LNKSTA, &link);
	lanes = (link & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT;
	lanes = lanes >= 1 && lanes <= 16 ? BIT(lanes) - 1 : 1;
	pm = pci_find_capability(pdev, PCI_CAP_ID_PM);
	if (pm) {
		pci_write_config_word(pdev, pm + PCI_PM_CTRL, 0x103);
		msleep(10);
	}
	update(port, 0x800, 0, 0x100);
	writel(0x11, port + 0x88);
	ack = wait_bits(port + 0x88, 1, 0, 500000);
	l2 = wait_bits(port + 0x208, BIT(6), BIT(6), 500000);
	dev_info(&pdev->dev, "port preboot transition: PME ack=%d L2=%d\n", ack, l2);
	/* Endpoint reads cease here until the root LTSSM is back in L0. */
	pci_write_config_word(bridge, PCI_COMMAND, 0);
	pci_read_config_dword(bridge, PCI_PRIMARY_BUS, &value);
	pci_write_config_dword(bridge, PCI_PRIMARY_BUS, value & 0xff0000ff);
	update(phy, 0, BIT(31), 0);
	writel((gpio & ~0xfU) | 2, pad);
	if (readl(pad) != ((gpio & ~0xfU) | 2)) {
		ret = -EIO;
		goto out;
	}
	wait_bits(port + 0x804, BIT(2), 0, 50000);
	wait_bits(port + 0x820, 0x1ff, 0, 100000);
	update(port, 0x13c, 0, 0x100);
	for (i = 0; i < 200; i++) {
		pci_read_config_dword(bridge, 0x728, &value);
		if ((value & 0x3f) == 0x14)
			break;
		usleep_range(1000, 1200);
	}
	wait_bits(port + 0x208, BIT(12), 0, 200000);
	readl(phy);
	wait_bits(port + 0xa8, 3, 0, 10000);
	update(port, 0x82c, 0, BIT(24));
	wait_bits(port + 0xa8, lanes << 16, lanes << 16, 20000);
	update(port, 0x82c, 0, BIT(16));
	udelay(10);
	update(port, 0x82c, 1, 0);
	udelay(10);
	update(port, 0x82c, BIT(24), 0);
	update(port, 0x13c, 0x100, 0);
	update(phy, 0, BIT(31), 0);
	update(phy, 0, 0x400, 0);
	update(phy, 0, 0x200, 0);
	update(phy, 0, 0, 0x10);
	update(phy, 0, 1, 0);
	update(phy, 0, 2, 0);
	writel(~0U, port + 0x100);
	update(port, 0x800, 1, 0);
	wait_bits(port + 0x804, 1, 0, 200000);
	update(phy, 0, BIT(31), 0);
	for (i = 0; i < ARRAY_SIZE(reset); i++)
		writel(reset[i][1], port + reset[i][0]);
	for (i = 0; i < 16; i++)
		writel(0, port + 0x3000 + 4 * i);
	for (i = 0; i < 256; i++)
		writel(0, port + 0x3800 + 4 * i);
	for (i = 0; i < ARRAY_SIZE(tune); i++)
		writel(tune[i][1], port + tune[i][0]);
	update(port, 0x800, 0, 1);
	udelay(10);
	update(phy, 0, 0, 1);
	if (!wait_bits(phy, 4, 4, 50000))
		goto timed_out;
	update(phy, 0, 0, 2);
	if (!wait_bits(phy, 8, 8, 50000))
		goto timed_out;
	update(phy, 0, 0x10, 0);
	udelay(100);
	update(phy, 0, 0, 0x200);
	update(phy, 0, 0, 0x400);
	udelay(100);
	update(port, 0x82c, 0, 1);
	writel((gpio & ~0xfU) | 3, pad);
	udelay(100);
	readl(phy);
	wait_bits(port + 0xa8, 3, 0, 20000);
	update(port, 0x82c, BIT(16), 0);
	if (!wait_bits(port + 0x804, 1, 1, 200000))
		goto timed_out;
	update(port, 0x800, 0x100, 0);
	pci_read_config_dword(bridge, 0x8bc, &value);
	pci_write_config_dword(bridge, 0x8bc, value ^ 1);
	pci_read_config_dword(bridge, 0x80, &value);
	pci_write_config_dword(bridge, 0x80, value | 0x400);
	update(phy, 0, 0, BIT(31));
	update(port, 0x80, 0, 1);
	for (i = 0; i < 2000; i++) {
		pci_read_config_dword(bridge, 0x728, &value);
		if ((value & 0x3f) == 0x11)
			break;
		usleep_range(1000, 1200);
	}
	if (i == 2000)
		goto timed_out;
	msleep(100);
	pci_restore_state(bridge);
	/* Bus numbers/windows now restored; endpoint config may be read again. */
	pci_read_config_dword(pdev, PCI_VENDOR_ID, &identity);
	if (identity != 0x1901106b && identity != 0x1900106b) {
		ret = -ENODEV;
		goto out;
	}
	pci_restore_state(pdev);
	if (pm) {
		pci_write_config_word(pdev, pm + PCI_PM_CTRL, 0);
		msleep(10);
	}
	dev_info(&pdev->dev, "port transition complete: endpoint=%#x LTSSM=%#x\n",
		 identity, value & 0x3f);
	ret = 0;
	goto out;
timed_out:
	ret = -ETIMEDOUT;
out:
	if (pad)
		iounmap(pad);
	if (phy)
		iounmap(phy);
	if (port)
		iounmap(port);
	return ret;
}

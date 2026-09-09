// SPDX-License-Identifier: GPL-2.0-only
/* Initial real-hardware N1 PCI/ACIPC backend for J813.
 * Register offsets are from AppleCentauriCommon/PROTOCOL.md and its measured
 * J813 prototype. Firmware execution and radio operations are separate gates.
 */
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "centauri.h"

static struct dentry *centauri_debug;

static int centauri_status_show(struct seq_file *s, void *unused)
{
	struct centauri *c = s->private;

	seq_printf(s, "pci=%s vendor=%04x device=%04x\n",
		   pci_name(c->pdev), c->pdev->vendor, c->pdev->device);
	if (c->link_ready)
		seq_printf(s, "bar0_size=%pa exec_stage=%#x\n", &c->size,
			   readl(c->bar + c->stage_offset));
	else
		seq_puts(s, "link unavailable; endpoint reads disabled\n");
	seq_printf(s, "transport=hardware-pci firmware_boot=%s image_response=%#x wifi=%s\n",
		   c->boot_attempted ? "attempted" : "not-started", c->image_response,
		   READ_ONCE(c->wifi) ? "registered" : "not-owned-by-this-function");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(centauri_status);

static void centauri_free_boot(struct centauri *c)
{
	struct device *dev = &c->pdev->dev;

	if (!c->link_ready && c->boot_attempted) {
		dev_warn(dev, "retaining DMA buffers after failed link transition; reboot required\n");
		return;
	}
	/* Stop device writes before any DMA storage is released. */
	centauri_free_ipc(c);
	if (c->msi_cap)
		pci_write_config_word(c->pdev, c->msi_cap + PCI_MSI_FLAGS,
				      c->msi_original & ~PCI_MSI_FLAGS_ENABLE);
	pci_clear_master(c->pdev);
	if (c->firmware)
		dma_free_coherent(dev, c->firmware_size, c->firmware, c->firmware_dma);
	if (c->descriptor)
		dma_free_coherent(dev, SZ_16K, c->descriptor, c->descriptor_dma);
	if (c->msi_page)
		dma_free_coherent(dev, SZ_16K, c->msi_page, c->msi_dma);
	c->firmware = c->descriptor = c->msi_page = NULL;
}

static int centauri_rom_boot(struct centauri *c)
{
	const struct firmware *fw;
	struct device *dev = &c->pdev->dev;
	u32 stage;
	int ret;

	if ((c->pdev->device != 0x1900 && c->pdev->device != 0x1901) ||
	    !of_machine_is_compatible("apple,j813"))
		return -ENODEV;
	if (!c->link_ready || c->boot_attempted)
		return -EBUSY;
	stage = readl(c->bar + 0x8000);
	if (stage != 0 && stage != 4 && stage != 1)
		return -EBUSY;
	ret = request_firmware(&fw, "apple/centauri/j813-ftab.bin", dev);
	if (ret)
		return ret;
	if (fw->size < 0x30 || fw->size > SZ_64M ||
	    memcmp(fw->data + 0x20, "rkosftab", 8)) {
		ret = -EINVAL;
		goto release;
	}
	c->firmware_size = PAGE_ALIGN(fw->size);
	c->firmware = dma_alloc_coherent(dev, c->firmware_size, &c->firmware_dma, GFP_KERNEL);
	c->descriptor = dma_alloc_coherent(dev, SZ_16K, &c->descriptor_dma, GFP_KERNEL);
	c->msi_page = dma_alloc_coherent(dev, SZ_16K, &c->msi_dma, GFP_KERNEL);
	if (!c->firmware || !c->descriptor || !c->msi_page) {
		ret = -ENOMEM;
		goto free_buffers;
	}
	memset(c->firmware, 0, c->firmware_size);
	memcpy(c->firmware, fw->data, fw->size);
	memset(c->descriptor, 0, SZ_16K);
	memset(c->msi_page, 0, SZ_16K);
	/* Measured ROM scatter entry: u64 DMA, u32 length, u32 flags, u64 x2 zero. */
	put_unaligned_le64(c->firmware_dma, c->descriptor);
	put_unaligned_le32(ALIGN(fw->size, SZ_4K), c->descriptor + 8);
	c->msi_cap = pci_find_capability(c->pdev, PCI_CAP_ID_MSI);
	if (!c->msi_cap) {
		ret = -ENODEV;
		goto free_buffers;
	}
	pci_read_config_word(c->pdev, c->msi_cap + PCI_MSI_FLAGS, &c->msi_original);
	if (!(c->msi_original & PCI_MSI_FLAGS_64BIT)) {
		ret = -EOPNOTSUPP;
		goto free_buffers;
	}
	/* Poll the measured DMA-backed MSI mailbox until native IRQ support exists. */
	pci_write_config_dword(c->pdev, c->msi_cap + PCI_MSI_ADDRESS_LO, lower_32_bits(c->msi_dma));
	pci_write_config_dword(c->pdev, c->msi_cap + PCI_MSI_ADDRESS_HI, upper_32_bits(c->msi_dma));
	pci_write_config_word(c->pdev, c->msi_cap + PCI_MSI_DATA_64, 0xa5a0);
	pci_write_config_word(c->pdev, c->msi_cap + PCI_MSI_FLAGS,
		(c->msi_original & ~PCI_MSI_FLAGS_QSIZE) | PCI_MSI_FLAGS_ENABLE);
	pci_set_master(c->pdev);
	writel(0x00248142, c->bar + 0x807c); /* J813 board 0x24, T8142 chip. */
	writel(0, c->bar + 0x803c);
	writel(lower_32_bits(c->descriptor_dma), c->bar + 0x8044);
	writel(upper_32_bits(c->descriptor_dma), c->bar + 0x8048);
	writel(0x20, c->bar + 0x804c);
	dma_wmb();
	c->boot_attempted = true;
	if (stage == 4 || stage == 1) {
		c->image_response = readl(c->bar + 0x8040);
		dev_info(dev, "adopted firmware stage %u; native DMA buffers ready\n", stage);
		ret = 0;
		goto release;
	}
	writel(1, c->bar + 0x9000);
	ret = readl_poll_timeout(c->bar + 0x8040, c->image_response,
				c->image_response != 0, 20000, 20000000);
	if (!ret)
		ret = readl_poll_timeout(c->bar + 0x8000, stage, stage != 0, 20000, 20000000);
	else
		stage = readl(c->bar + 0x8000);
	dma_rmb();
	dev_info(dev, "ROM firmware result: response=%#x stage=%#x mailbox=%#x poll=%d\n",
		 c->image_response, stage, READ_ONCE(*(u32 *)c->msi_page), ret);
	/* Keep DMA buffers owned until device removal, even on timeout. */
	if (!ret && stage != 4)
		ret = -EIO;
	goto release;
free_buffers:
	centauri_free_boot(c);
release:
	release_firmware(fw);
	return ret;
}

static ssize_t centauri_boot_write(struct file *file, const char __user *buffer,
				  size_t count, loff_t *pos)
{
	struct centauri *c = file->private_data;
	u32 value;
	int ret = kstrtou32_from_user(buffer, count, 0, &value);

	if (ret || value < 1 || value > 3)
		return ret ?: -EINVAL;
	mutex_lock(&c->boot_lock);
	if (value == 1) {
		ret = centauri_rom_boot(c);
	} else if (value == 3) {
		ret = centauri_start_ipc(c);
	} else if (!c->firmware || !c->link_ready || readl(c->bar + 0x8000) != 4) {
		ret = -EBUSY;
	} else {
		c->link_ready = false;
		ret = centauri_j813_port_cycle(c->pdev);
		if (!ret) {
			c->link_ready = true;
			dev_info(&c->pdev->dev, "post-reset execution stage=%#x\n",
				 readl(c->bar + 0x8000));
		}
	}
	mutex_unlock(&c->boot_lock);
	return ret ?: count;
}

static const struct file_operations centauri_boot_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = centauri_boot_write,
};

static int centauri_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct centauri *c;
	dma_addr_t dma;
	void *page;
	int ret;

	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->pdev = pdev;
	c->link_ready = true;
	mutex_init(&c->boot_lock);
	c->stage_offset = id->driver_data;
	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	c->size = pci_resource_len(pdev, 0);
	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    c->size < c->stage_offset + sizeof(u32))
		return dev_err_probe(&pdev->dev, -ENODEV, "invalid ACIPC BAR0\n");
	ret = pcim_iomap_regions(pdev, BIT(0), "apple_centauri");
	if (ret)
		return ret;
	c->bar = pcim_iomap_table(pdev)[0];
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "40-bit DMA unavailable\n");
	/* Test Linux's DART allocation without publishing it to the device. */
	page = dma_alloc_coherent(&pdev->dev, SZ_16K, &dma, GFP_KERNEL);
	if (!page)
		return dev_err_probe(&pdev->dev, -ENOMEM, "DART DMA allocation failed\n");
	memset(page, 0, SZ_16K);
	dev_info(&pdev->dev, "ACIPC endpoint reachable: stage=%#x, 40-bit DMA mapping=%pad\n",
		 readl(c->bar + c->stage_offset), &dma);
	dma_free_coherent(&pdev->dev, SZ_16K, page, dma);
	/* Bus mastering remains disabled until the firmware transport owns buffers. */
	pci_clear_master(pdev);
	pci_set_drvdata(pdev, c);
	c->debug = debugfs_create_dir(pci_name(pdev), centauri_debug);
	debugfs_create_file("status", 0400, c->debug, c, &centauri_status_fops);
	debugfs_create_file("rom_boot", 0200, c->debug, c, &centauri_boot_fops);
	return 0;
}

static void centauri_remove(struct pci_dev *pdev)
{
	struct centauri *c = pci_get_drvdata(pdev);

	centauri_wifi_unregister(c);
	debugfs_remove_recursive(c->debug);
	centauri_free_boot(c);
}

static const struct pci_device_id centauri_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1900), .driver_data = 0x8000 },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1901), .driver_data = 0x8000 },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1902), .driver_data = 0x0008 },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1903), .driver_data = 0x0018 },
	{ }
};
MODULE_DEVICE_TABLE(pci, centauri_ids);

static struct pci_driver centauri_driver = {
	.name = "apple_centauri",
	.id_table = centauri_ids,
	.probe = centauri_probe,
	.remove = centauri_remove,
};

static int __init centauri_init(void)
{
	int ret;

	centauri_debug = debugfs_create_dir("apple_centauri", NULL);
	ret = pci_register_driver(&centauri_driver);
	if (ret)
		debugfs_remove_recursive(centauri_debug);
	return ret;
}

static void __exit centauri_exit(void)
{
	pci_unregister_driver(&centauri_driver);
	debugfs_remove_recursive(centauri_debug);
}
module_init(centauri_init);
module_exit(centauri_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple N1 Centauri native PCI and ACIPC bring-up backend");
MODULE_FIRMWARE("apple/centauri/j813-ftab.bin");

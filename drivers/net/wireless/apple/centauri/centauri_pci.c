// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/pci.h>
#include <linux/pci-apple-apif.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "centauri.h"

/* No DMA backing is released without verified bus-master shutdown. */
bool centauri_stop_dma(struct pci_dev *pdev)
{
	u16 command;
	int ret;

	pci_clear_master(pdev);
	ret = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (ret || (command & PCI_COMMAND_MASTER))
		return false;
	return pci_wait_for_pending_transaction(pdev);
}

void centauri_free_irqs(struct centauri_irqs *irqs)
{
	if (!irqs->pdev)
		return;
	while (irqs->count)
		free_irq(pci_irq_vector(irqs->pdev, --irqs->count), irqs->data);
	pci_free_irq_vectors(irqs->pdev);
	irqs->pdev = NULL;
}

int centauri_request_irqs(struct centauri_irqs *irqs, struct pci_dev *pdev,
			 unsigned int count, irq_handler_t handler, void *data)
{
	int ret;

	ret = pci_alloc_irq_vectors(pdev, count, count, PCI_IRQ_MSI);
	if (ret < 0)
		return ret;
	irqs->pdev = pdev;
	irqs->data = data;
	for (irqs->count = 0; irqs->count < count; irqs->count++) {
		ret = request_irq(pci_irq_vector(pdev, irqs->count), handler, 0,
				  "centauri", data);
		if (ret) {
			centauri_free_irqs(irqs);
			return ret;
		}
	}
	return 0;
}

static irqreturn_t centauri_control_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}

static void centauri_free_boot(struct centauri *c)
{
	struct device *dev = &c->pdev->dev;

	if (!c->link_ready && c->boot_attempted) {
		while (c->alpha_irqs.count)
			free_irq(pci_irq_vector(c->alpha_irqs.pdev, --c->alpha_irqs.count), c);
		while (c->irqs.count)
			free_irq(pci_irq_vector(c->pdev, --c->irqs.count), c);
		dev_warn(dev, "retaining DMA buffers after failed link transition; reboot required\n");
		return;
	}

	centauri_free_ipc(c);
	centauri_free_irqs(&c->irqs);
	if (c->ipc || !centauri_stop_dma(c->pdev)) {
		dev_warn(dev, "retaining boot DMA until outstanding transactions stop\n");
		return;
	}
	if (c->firmware)
		dma_free_coherent(dev, c->firmware_size, c->firmware, c->firmware_dma);
	if (c->descriptor)
		dma_free_coherent(dev, SZ_16K, c->descriptor, c->descriptor_dma);
	c->firmware = NULL;
	c->descriptor = NULL;
}

static int centauri_rom_boot(struct centauri *c)
{
	const struct firmware *fw;
	struct device *dev = &c->pdev->dev;
	u32 stage;
	int ret;

	if (c->pdev->device != 0x1900 && c->pdev->device != 0x1901)
		return -ENODEV;
	if (!c->link_ready || c->boot_attempted)
		return -EBUSY;
	stage = readl(c->bar + 0x8000);
	if (stage != 0)
		return -EBUSY;

	ret = request_firmware_direct(&fw, c->firmware_name, dev);
	if (ret) {
		dev_err(dev, "required firmware %s unavailable: %d\n", c->firmware_name, ret);
		return ret;
	}
	if (READ_ONCE(c->removing)) {
		ret = -ECANCELED;
		goto release;
	}
	if (fw->size < 0x30 || fw->size > SZ_64M ||
	    memcmp(fw->data + 0x20, "rkosftab", 8)) {
		ret = -EINVAL;
		goto release;
	}
	c->firmware_size = PAGE_ALIGN(fw->size);
	c->firmware = dma_alloc_coherent(dev, c->firmware_size, &c->firmware_dma, GFP_KERNEL);
	c->descriptor = dma_alloc_coherent(dev, SZ_16K, &c->descriptor_dma, GFP_KERNEL);
	if (!c->firmware || !c->descriptor) {
		ret = -ENOMEM;
		goto free_buffers;
	}
	memset(c->firmware, 0, c->firmware_size);
	memcpy(c->firmware, fw->data, fw->size);
	memset(c->descriptor, 0, SZ_16K);

	put_unaligned_le64(c->firmware_dma, c->descriptor);
	put_unaligned_le32(ALIGN(fw->size, SZ_4K), c->descriptor + 8);
	ret = centauri_request_irqs(&c->irqs, c->pdev, 4,
				    centauri_control_irq, c);
	if (ret)
		goto free_buffers;
	pci_set_master(c->pdev);
	writel(c->platform_id, c->bar + 0x807c);
	writel(0, c->bar + 0x803c);
	writel(lower_32_bits(c->descriptor_dma), c->bar + 0x8044);
	writel(upper_32_bits(c->descriptor_dma), c->bar + 0x8048);
	writel(0x20, c->bar + 0x804c);
	dma_wmb();
	c->boot_attempted = true;
	writel(1, c->bar + 0x9000);
	ret = readl_poll_timeout(c->bar + 0x8040, c->image_response,
				READ_ONCE(c->removing) || c->image_response != 0, 20000, 20000000);
	if (READ_ONCE(c->removing)) {
		ret = -ECANCELED;
		goto release;
	}
	if (!ret && c->image_response != 1)
		ret = -EIO;
	if (!ret)
		ret = readl_poll_timeout(c->bar + 0x8000, stage,
				READ_ONCE(c->removing) || stage != 0, 20000, 20000000);
	else
		stage = readl(c->bar + 0x8000);
	if (READ_ONCE(c->removing))
		ret = -ECANCELED;
	dma_rmb();

	if (!ret && stage != 4)
		ret = -EIO;
	goto release;
free_buffers:
	centauri_free_boot(c);
release:
	release_firmware(fw);
	return ret;
}

static int centauri_cycle(struct centauri *c)
{
	int ret;

	if (!c->firmware || !c->link_ready || readl(c->bar + 0x8000) != 4)
		return -EBUSY;
	c->link_ready = false;
	ret = apple_apif_pcie_cycle(c->pdev);
	if (ret == -EAGAIN) {
		c->link_ready = true;
	} else if (!ret) {
		pci_restore_msi_state(c->pdev);
		c->link_ready = true;
	}
	return ret;
}

static void centauri_boot_worker(struct work_struct *work)
{
	struct centauri *c = container_of(to_delayed_work(work), struct centauri, boot_work);
	bool retry = false;
	int ret;

	mutex_lock(&c->boot_lock);
	while (!READ_ONCE(c->removing)) {
		switch (c->boot_phase) {
		case CEN_BOOT_ROM:
			ret = centauri_rom_boot(c);
			if (!ret)
				c->boot_phase = readl(c->bar + 0x8000) == 1 ?
					CEN_BOOT_IPC : CEN_BOOT_CYCLE;
			break;
		case CEN_BOOT_CYCLE:
			ret = centauri_cycle(c);
			if (!ret)
				c->boot_phase = CEN_BOOT_IPC;
			break;
		case CEN_BOOT_IPC:
			ret = centauri_start_ipc(c);
			if (!ret)
				c->boot_phase = CEN_BOOT_READY;
			break;
		default:
			goto out;
		}
		if (ret == -EAGAIN && (c->boot_phase == CEN_BOOT_CYCLE ||
		    (c->boot_phase == CEN_BOOT_IPC && !c->ipc))) {
			retry = true;
			break;
		}
		if (ret) {
			dev_err(&c->pdev->dev, "N1 automatic startup failed in phase%u: %d\n",
				c->boot_phase, ret);
			c->boot_phase = CEN_BOOT_FAILED;
			break;
		}
	}
out:
	if (READ_ONCE(c->removing))
		c->boot_phase = CEN_BOOT_STOPPED;
	mutex_unlock(&c->boot_lock);
	if (retry && !READ_ONCE(c->removing))
		queue_delayed_work(system_long_wq, &c->boot_work, msecs_to_jiffies(20));
}

static int centauri_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct centauri *c;
	int ret;
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);

	if (!bridge || !bridge->dev.parent ||
	    !of_device_is_compatible(bridge->dev.parent->of_node, "apple,t6050-pcie-apif"))
		return -ENODEV;
	if (PCI_FUNC(pdev->devfn) != 0)
		return -ENODEV;
	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->pdev = pdev;
	c->link_ready = true;
	mutex_init(&c->boot_lock);
	INIT_DELAYED_WORK(&c->boot_work, centauri_boot_worker);
	c->boot_phase = CEN_BOOT_ROM;
	ret = device_property_read_string(&pdev->dev, "firmware-name", &c->firmware_name);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "missing personalized firmware name\n");
	ret = device_property_read_u32(&pdev->dev, "apple,centauri-platform-id", &c->platform_id);
	if (ret)
		return ret;
	ret = device_property_read_u32(&pdev->dev, "apple,centauri-protocol", &c->protocol);
	if (ret || c->protocol != 27)
		return -EINVAL;
	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	c->size = pci_resource_len(pdev, 0);
	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    c->size < 0x9080)
		return dev_err_probe(&pdev->dev, -ENODEV, "invalid ACIPC BAR0\n");
	ret = pcim_iomap_regions(pdev, BIT(0), "apple_centauri");
	if (ret)
		return ret;
	c->bar = pcim_iomap_table(pdev)[0];
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "40-bit DMA unavailable\n");

	pci_clear_master(pdev);
	pci_set_drvdata(pdev, c);
	queue_delayed_work(system_long_wq, &c->boot_work, 0);
	return 0;
}

static void centauri_remove(struct pci_dev *pdev)
{
	struct centauri *c = pci_get_drvdata(pdev);

	WRITE_ONCE(c->removing, true);
	cancel_delayed_work_sync(&c->boot_work);
	mutex_lock(&c->boot_lock);
	centauri_wifi_unregister(c);
	centauri_free_boot(c);
	c->boot_phase = CEN_BOOT_STOPPED;
	mutex_unlock(&c->boot_lock);
}

static int centauri_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct centauri *c = pci_get_drvdata(pdev);

	guard(mutex)(&c->boot_lock);
	if (c->boot_phase != CEN_BOOT_READY)
		return -EBUSY;
	return pci_save_state(pdev);
}

static int centauri_resume(struct device *dev)
{
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(centauri_pm_ops, centauri_suspend, centauri_resume);

static const struct pci_device_id centauri_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1900) },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1901) },
	{ }
};
MODULE_DEVICE_TABLE(pci, centauri_ids);

static struct pci_driver centauri_driver = {
	.name = "apple_centauri",
	.id_table = centauri_ids,
	.probe = centauri_probe,
	.remove = centauri_remove,
	.shutdown = centauri_remove,
	.driver.pm = pm_sleep_ptr(&centauri_pm_ops),
};

module_pci_driver(centauri_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple N1 Centauri PCI firmware and ACIPC transport");

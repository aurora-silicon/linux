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
#include <linux/workqueue.h>

#include "centauri.h"

static struct dentry *centauri_debug;

static unsigned int board_word;
module_param(board_word, uint, 0644);
MODULE_PARM_DESC(board_word, "Diagnostic: word written to BAR0+0x807c before the ROM boot, 0 = per-machine table value");

static int centauri_status_show(struct seq_file *s, void *unused)
{
	struct centauri *c = s->private;

	/* A restart cycles the link: no BAR read while it runs */
	mutex_lock(&c->boot_lock);
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
	mutex_unlock(&c->boot_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(centauri_status);

/* Returns false, with the buffers kept, if the N1's DMA could not be stopped. */
static bool centauri_free_boot(struct centauri *c)
{
	struct device *dev = &c->pdev->dev;
	bool stopped;

	if (!c->link_ready && c->boot_attempted) {
		dev_warn(dev, "retaining DMA buffers after failed link transition; reboot required\n");
		return false;
	}
	/* Stop all device writes before any DMA storage is released. */
	stopped = centauri_stop_ipc(c);
	if (c->msi_cap)
		pci_write_config_word(c->pdev, c->msi_cap + PCI_MSI_FLAGS,
				      c->msi_original & ~PCI_MSI_FLAGS_ENABLE);
	if (!centauri_stop_dma(c->pdev) || !stopped) {
		dev_err(dev, "retaining DMA buffers: the N1's DMA did not stop\n");
		return false;
	}
	centauri_free_ipc(c);
	if (c->firmware)
		dma_free_coherent(dev, c->firmware_size, c->firmware, c->firmware_dma);
	if (c->descriptor)
		dma_free_coherent(dev, SZ_16K, c->descriptor, c->descriptor_dma);
	if (c->msi_page)
		dma_free_coherent(dev, SZ_16K, c->msi_page, c->msi_dma);
	c->firmware = c->descriptor = c->msi_page = NULL;
	return true;
}

static int centauri_rom_boot(struct centauri *c)
{
	const struct firmware *fw;
	const struct centauri_board *board = centauri_board();
	struct device *dev = &c->pdev->dev;
	u32 stage;
	int ret;

	if ((c->pdev->device != 0x1900 && c->pdev->device != 0x1901) || !board)
		return -ENODEV;
	if (!c->link_ready || c->boot_attempted)
		return -EBUSY;
	stage = readl(c->bar + 0x8000);
	if (stage != 0 && stage != 4 && stage != 1)
		return -EBUSY;
	ret = request_firmware(&fw, board->firmware, dev);
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
	dev_info(dev, "ROM boot: %s, %zu bytes, board word %#x\n", board->firmware, fw->size,
		 board_word ?: board->board_word);
	writel(board_word ?: board->board_word, c->bar + 0x807c);
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

/*
 * Restart the N1 from a running or stopped firmware, without the bootloader: release
 * everything the firmware could write to, then a port cycle with the SMC warm reset
 * (PERST# alone leaves the firmware running), which leaves the chip in its ROM (stage 0),
 * then the boot steps 1-3 as at probe. btcentauri is released first: it drives function 2
 * over the IPC that is freed here.
 *
 * Not safe against function 0 being removed under the PCI rescan lock (sysfs remove, host
 * unbind, hot unplug) while a restart runs: remove waits for the restart, which waits for the
 * rescan lock. The debugfs boot steps have the same limit.
 */
static int centauri_recover(struct centauri *c)
{
	struct device *dev = &c->pdev->dev;
	u32 stage = readl(c->bar + 0x8000);
	struct pci_dev *beta;
	int ret, reset_ret, i;

	if (!c->warm_reset)
		return -EOPNOTSUPP;
	if (!c->link_ready || PCI_FUNC(c->pdev->devfn))
		return -EBUSY;
	dev_info(dev, "recovery: stage %#x, releasing the firmware's buffers\n", stage);
	beta = pci_get_slot(c->pdev->bus, PCI_DEVFN(PCI_SLOT(c->pdev->devfn), 2));
	if (beta)
		device_release_driver(&beta->dev);
	pci_dev_put(beta);
	/* Nothing is reset while the firmware's buffers are still its own. */
	if (!centauri_free_boot(c))
		return -EBUSY;
	/*
	 * The warm reset puts the chip back in its single-function ROM personality and wipes
	 * functions 1 and 2. Remove them now; the IPC start's rescan adds them again, as at
	 * probe. A stale function's BAR read after the reset hangs the fabric.
	 */
	pci_lock_rescan_remove();
	for (i = 2; i >= 1; i--) {
		struct pci_dev *fn = pci_get_slot(c->pdev->bus,
						  PCI_DEVFN(PCI_SLOT(c->pdev->devfn), i));

		if (fn)
			pci_stop_and_remove_bus_device(fn);
		pci_dev_put(fn);
	}
	pci_unlock_rescan_remove();
	c->boot_attempted = false;
	c->image_response = 0;
	c->link_ready = false;
	ret = centauri_port_cycle(c->pdev, &reset_ret);
	if (ret) {
		dev_err(dev, "recovery: reset port cycle failed: %d\n", ret);
		return ret;
	}
	c->link_ready = true;
	if (reset_ret) {
		dev_err(dev, "recovery: warm reset failed: %d\n", reset_ret);
		return reset_ret;
	}
	stage = readl(c->bar + 0x8000);
	dev_info(dev, "recovery: stage %#x after the warm reset\n", stage);
	if (stage != 0)
		return -EIO;
	/* Returns once the ROM reports the image loaded (stage 4), as the boot step does. */
	ret = centauri_rom_boot(c);
	if (ret) {
		dev_err(dev, "recovery: ROM boot failed: %d\n", ret);
		return ret;
	}
	c->link_ready = false;
	ret = centauri_port_cycle(c->pdev, NULL);
	if (ret) {
		dev_err(dev, "recovery: boot port cycle failed: %d\n", ret);
		return ret;
	}
	c->link_ready = true;
	/* The IPC start needs the image's first stage (1) after the port cycle. */
	ret = readl_poll_timeout(c->bar + 0x8000, stage, stage == 1, 20 * USEC_PER_MSEC,
				 5 * USEC_PER_SEC);
	dev_info(dev, "recovery: post-reset execution stage=%#x\n", stage);
	if (ret)
		return ret;
	ret = centauri_start_ipc(c);
	dev_info(dev, "recovery: IPC start %d\n", ret);
	return ret;
}

/* Ordered and freezable: a restart never runs during a system sleep or beside another one. */
static struct workqueue_struct *centauri_wq;

/*
 * The Wi-Fi code saw a sign of a stopped firmware: an Alpha execution stage other than 2,
 * a command ring the firmware no longer drains, or a command timeout. The worker looks
 * again 2 s later and restarts the chip only if the firmware is still stopped.
 */
void centauri_recovery_request(struct centauri *c, const char *why)
{
	if (!c->warm_reset || READ_ONCE(c->removing) || READ_ONCE(c->recover_gave_up))
		return;
	if (queue_delayed_work(centauri_wq, &c->recover_work, 2 * HZ))
		dev_warn(&c->pdev->dev, "firmware fault (%s); checking again in 2 s\n", why);
}

static bool centauri_beta_up(struct centauri *c)
{
	bool up;

	mutex_lock(&c->boot_lock);
	up = READ_ONCE(c->removing) || centauri_beta_running(c);
	mutex_unlock(&c->boot_lock);
	return up;
}

/*
 * The IPC start's rescan adds the Beta (Bluetooth) function before Beta's firmware runs,
 * so btcentauri cannot take it then. Wait for that firmware and attach the function.
 * Called without the boot lock and outside the PCI rescan lock. Returns 1 when
 * function 2 has a driver, 0 when none took it, or a negative error.
 */
static int centauri_attach_beta(struct centauri *c)
{
	struct pci_dev *beta;
	bool up;
	int ret;

	beta = pci_get_slot(c->pdev->bus, PCI_DEVFN(PCI_SLOT(c->pdev->devfn), 2));
	if (!beta)
		return -ENODEV;
	ret = 1;
	if (!READ_ONCE(beta->dev.driver)) {
		ret = read_poll_timeout(centauri_beta_up, up, up, 20 * USEC_PER_MSEC,
					10 * USEC_PER_SEC, false, c);
		if (!ret)
			ret = READ_ONCE(c->removing) ? -ENODEV : device_attach(&beta->dev);
	}
	pci_dev_put(beta);
	return ret;
}

static void centauri_recover_worker(struct work_struct *work)
{
	struct centauri *c = container_of(to_delayed_work(work), struct centauri, recover_work);
	struct device *dev = &c->pdev->dev;
	unsigned long now = jiffies;
	int ret;

	mutex_lock(&c->boot_lock);
	if (READ_ONCE(c->removing) || (!c->ipc && !c->restart_failed)) {
		mutex_unlock(&c->boot_lock);
		return;
	}
	if (!xchg(&c->inject_stall, false) && !c->restart_failed && !centauri_wifi_stalled(c)) {
		mutex_unlock(&c->boot_lock);
		dev_info(dev, "firmware fault cleared; no restart\n");
		return;
	}
	/* Three restarts in 30 minutes: the chip is left down rather than reset in a loop. */
	if (c->recover_at[0] && time_before(now, c->recover_at[0] + 30 * 60 * HZ)) {
		WRITE_ONCE(c->recover_gave_up, true);
		mutex_unlock(&c->boot_lock);
		dev_err(dev, "firmware stopped again after 3 restarts in 30 min; leaving Wi-Fi down until the driver is reloaded\n");
		return;
	}
	memmove(c->recover_at, c->recover_at + 1, sizeof(c->recover_at) - sizeof(c->recover_at[0]));
	c->recover_at[ARRAY_SIZE(c->recover_at) - 1] = now;
	dev_warn(dev, "firmware stopped; restarting the N1\n");
	ret = centauri_recover(c);
	/* With the link up the chip can be reset again; without it nothing more can be done. */
	c->restart_failed = ret && c->link_ready;
	mutex_unlock(&c->boot_lock);
	if (ret && !c->restart_failed) {
		dev_err(dev, "restart failed: %d, and the link is down; Wi-Fi stays down until a reboot\n",
			ret);
		return;
	}
	if (ret) {
		/* Counts against the same limit: at most three attempts in 30 minutes. */
		dev_err(dev, "restart failed: %d; trying again in 10 s\n", ret);
		if (!READ_ONCE(c->removing))
			queue_delayed_work(centauri_wq, &c->recover_work, 10 * HZ);
		return;
	}
	dev_info(dev, "restarted; Bluetooth function attach: %d\n", centauri_attach_beta(c));
}

static ssize_t centauri_boot_write(struct file *file, const char __user *buffer,
				  size_t count, loff_t *pos)
{
	struct centauri *c = file->private_data;
	u32 value;
	int ret = kstrtou32_from_user(buffer, count, 0, &value);

	if (ret || value < 1 || value > 6)
		return ret ?: -EINVAL;
	mutex_lock(&c->boot_lock);
	if (value == 1) {
		ret = centauri_rom_boot(c);
	} else if (value == 3) {
		ret = centauri_start_ipc(c);
	} else if (value == 4) {
		ret = centauri_recover(c);
	} else if (value == 5 || value == 6) {
		/* Test the fault path: 5 as a stopped firmware, 6 as a fault that clears. */
		WRITE_ONCE(c->inject_stall, value == 5);
		centauri_recovery_request(c, value == 5 ? "debugfs: stopped" : "debugfs: spurious");
		ret = 0;
	} else if (!c->firmware || !c->link_ready || readl(c->bar + 0x8000) != 4) {
		ret = -EBUSY;
	} else {
		c->link_ready = false;
		ret = centauri_port_cycle(c->pdev, NULL);
		if (!ret) {
			c->link_ready = true;
			dev_info(&c->pdev->dev, "post-reset execution stage=%#x\n",
				 readl(c->bar + 0x8000));
		}
	}
	mutex_unlock(&c->boot_lock);
	/* A restart (4) ends with the IPC started (3): Bluetooth comes back the same way */
	if ((value == 3 || value == 4) && !ret) {
		int attached = centauri_attach_beta(c);

		if (attached == 1)
			dev_info(&c->pdev->dev, "Bluetooth function attached\n");
		else
			dev_warn(&c->pdev->dev, "Bluetooth function not attached: %d\n", attached);
	}
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
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);

	/* The T6050 transport owns every function behind an adopted host. */
	if (bridge && bridge->dev.parent &&
	    of_device_is_compatible(bridge->dev.parent->of_node, "apple,t6050-pcie"))
		return -ENODEV;

	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->pdev = pdev;
	c->link_ready = true;
	mutex_init(&c->boot_lock);
	INIT_DELAYED_WORK(&c->recover_work, centauri_recover_worker);
	c->warm_reset = centauri_warm_reset_described(pdev);
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

	/* Disabled, not only cancelled: a request racing this remove can no longer queue it */
	WRITE_ONCE(c->removing, true);
	disable_delayed_work_sync(&c->recover_work);
	centauri_wifi_unregister(c);
	debugfs_remove_recursive(c->debug);
	centauri_free_boot(c);
}

/*
 * The N1 firmware keeps running across s2idle: it cannot be restarted without
 * the bootloader's link training. Without PM callbacks the PCI core disables a
 * function at suspend, which turns bus mastering off under the running
 * firmware and leaves every ring dead (each scan then fails with -EIO). Save
 * the state ourselves so the core keeps the function enabled and in D0, and
 * keep the PCIe host's power domains up through the noirq phase.
 */
static int centauri_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct centauri *c = pci_get_drvdata(pdev);
	int ret;

	ret = centauri_wifi_suspend(c);
	if (ret)
		return ret;
	device_set_awake_path(dev);
	ret = pci_save_state(pdev);
	if (ret)
		centauri_wifi_resume(c);
	return ret;
}

static int centauri_resume(struct device *dev)
{
	return centauri_wifi_resume(pci_get_drvdata(to_pci_dev(dev)));
}

static DEFINE_SIMPLE_DEV_PM_OPS(centauri_pm_ops, centauri_suspend, centauri_resume);

/* 0x1903 is the Beta (Bluetooth) function: btcentauri claims it. This driver binds functions
 * 0 and 1 (1 is enabled here, its BAR is used by the Wi-Fi code) and enables function 2's
 * memory space during IPC bootstrap so centauri_beta_running() can poll Beta's BAR0 safely
 * even when btcentauri has not probed yet.
 */
static const struct pci_device_id centauri_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1900), .driver_data = 0x8000 },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1901), .driver_data = 0x8000 },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1902), .driver_data = 0x0008 },
	{ }
};
MODULE_DEVICE_TABLE(pci, centauri_ids);

static struct pci_driver centauri_driver = {
	.name = "apple_centauri",
	.id_table = centauri_ids,
	.probe = centauri_probe,
	.remove = centauri_remove,
	.driver.pm = pm_sleep_ptr(&centauri_pm_ops),
};

static int __init centauri_init(void)
{
	int ret;

	centauri_wq = alloc_ordered_workqueue("apple_centauri", WQ_FREEZABLE);
	if (!centauri_wq)
		return -ENOMEM;
	centauri_debug = debugfs_create_dir("apple_centauri", NULL);
	ret = pci_register_driver(&centauri_driver);
	if (ret) {
		debugfs_remove_recursive(centauri_debug);
		destroy_workqueue(centauri_wq);
	}
	return ret;
}

static void __exit centauri_exit(void)
{
	pci_unregister_driver(&centauri_driver);
	debugfs_remove_recursive(centauri_debug);
	destroy_workqueue(centauri_wq);
}
module_init(centauri_init);
module_exit(centauri_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple N1 Centauri native PCI and ACIPC bring-up backend");
MODULE_FIRMWARE("apple/centauri/j813-ftab.bin");
MODULE_FIRMWARE("apple/centauri/j815-ftab.bin");
MODULE_FIRMWARE("apple/centauri/j873-ftab.bin");

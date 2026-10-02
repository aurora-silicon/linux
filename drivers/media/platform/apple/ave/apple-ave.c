// SPDX-License-Identifier: GPL-2.0-only
/* T8140 AVE bring-up: resources, firmware negotiation and IPC transport.
 * Experimental root-only command interface. No registered encoder yet.
 */
#include <linux/debugfs.h>
#include <linux/crc32.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/ktime.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#define AVE_IPC_BYTES (20 * SZ_1M)
#define AVE_QUEUE_BYTES SZ_64K
#define AVE_FW_IOVA 0x10000000000ULL
#define AVE_LOG_BYTES 0x24000
#define AVE_LOG_USED 0x20200

static bool enable;
module_param(enable, bool, 0400);
MODULE_PARM_DESC(enable, "Opt in to the experimental AVE transport");

/* Read the known Uranus registers once after PM resume. The separate hello
 * command pins all resources because reset/quiescence is not yet qualified.
 */
static bool snapshot;
module_param(snapshot, bool, 0400);
MODULE_PARM_DESC(snapshot, "Capture powered AVE registers once without writing them");

struct ave_firmware {
	phys_addr_t physical;
	u64 iova;
	size_t size;
	u32 crc;
	unsigned int verified_pages;
};

struct ave_buffer {
	void *cpu;
	dma_addr_t dma;
	size_t size;
	unsigned int verified_pages;
};

struct ave_channel {
	char name[64];
	u32 mode, interrupt, slots;
	u32 offset;
	int consumer, producer;
};

struct apple_ave {
	struct device *dev;
	struct device *power[5];
	unsigned int num_power;
	struct iommu_domain *domain;
	struct ave_buffer ipc;
	struct ave_buffer queue;
	struct ave_buffer log;
	struct ave_buffer heap;
	struct ave_firmware fw[2];
	bool firmware_verified;
	bool start_requested;
	int start_result;
	struct mutex lock;
	void __iomem *coproc, *mailbox;
	u32 hello[4];
	u64 ipc_fw_base;
	u32 ipc_reply[4], channel_bytes, extra_bytes;
	const char *phase;
	bool ipc_ready, command_failed;
	struct ave_channel channel[2];
	u32 commands, replies, transport_acks;
	u8 response[SZ_64K];
	size_t response_bytes;
	struct debugfs_blob_wrapper ipc_blob;
	struct debugfs_blob_wrapper log_blob;
	struct debugfs_blob_wrapper config_blob;
	u64 vcbar;
	u32 core_control, core_status, mailbox_status, scratch[8];
	struct dentry *debugfs;
};

static int ave_alloc_buffer(struct apple_ave *ave, struct ave_buffer *buf, size_t size);
#include "apple-ave-ipc.h"

/* Cold-boot hello only. A successful response still needs the remaining IPC
 * negotiation. Keep the module, PM reference and DMA buffers pinned until a
 * target reboot, on both success and timeout; no unproven stop/free sequence.
 */
static ssize_t ave_start_write(struct file *file, const char __user *data,
			       size_t len, loff_t *ppos)
{
	struct apple_ave *ave = file->private_data;
	__le32 *cfg = ave->queue.cpu, *log = ave->log.cpu;
	bool requested;
	u32 status;
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	ret = kstrtobool_from_user(data, len, &requested);
	if (ret || !requested)
		return ret ?: -EINVAL;
	mutex_lock(&ave->lock);
	if (ave->start_requested) {
		ret = -EALREADY;
		goto out;
	}
	if (!ave->firmware_verified || readq(ave->coproc + 0x50000) != (AVE_FW_IOVA | 1) ||
	    readl(ave->coproc + 0x600044) || readl(ave->mailbox + 0x10)) {
		ret = -EBUSY;
		goto out;
	}
	for (unsigned int i = 0; i < 8; i++) {
		if (readl(ave->mailbox + 0x18 + i * 4)) {
			ret = -EBUSY;
			goto out;
		}
	}
	/* Startup values observed with the qualified 25G76 firmware: device
	 * enum 29, one active unit/group and sub-ID mask 1. Firmware maps the
	 * full page-aligned log allocation; the ring header advertises usage.
	 */
	memset(cfg, 0, 0x40);
	cfg[1] = cpu_to_le32(29);
	cfg[2] = cpu_to_le32(1);
	cfg[3] = cpu_to_le32(1);
	cfg[4] = cpu_to_le32(1);
	cfg[8] = cpu_to_le32(lower_32_bits(ave->log.dma));
	cfg[9] = cpu_to_le32(upper_32_bits(ave->log.dma));
	cfg[10] = cpu_to_le32(AVE_LOG_BYTES);
	cfg[12] = cpu_to_le32(0x77);
	/* Log header: 128 KiB ring after its 512-byte header. */
	log[0] = cpu_to_le32(AVE_LOG_USED);
	log[1] = cpu_to_le32(0x200);
	log[2] = cpu_to_le32(0x20000);
	dma_wmb();
	__module_get(THIS_MODULE);
	ave->start_requested = true;
	ave->phase = "hello";
	ave->start_result = -EINPROGRESS;
	dev_info(ave->dev, "AVE_START_ONCE cfg=%pad log=%pad resources_pinned_until_reboot=1\n",
		 &ave->queue.dma, &ave->log.dma);
	writel(0x08042006, ave->mailbox + 0x18);
	writel(lower_32_bits(ave->queue.dma), ave->mailbox + 0x1c);
	writel(upper_32_bits(ave->queue.dma), ave->mailbox + 0x20);
	/* Preserve the protected firmware mappings inherited from iBoot. */
	writel(1, ave->coproc + 0x600808);
	writel(0, ave->coproc + 0x600044);
	writel(0x10000, ave->coproc + 0x600400);
	writel(0x10, ave->coproc + 0x600044);
	ret = readl_poll_timeout(ave->mailbox + 0x10, status, status & 1, 50, 5000000);
	ave->start_result = ret;
	ave->core_control = readl(ave->coproc + 0x600044);
	ave->core_status = readl(ave->coproc + 0x600048);
	ave->mailbox_status = status;
	for (unsigned int i = 0; i < 8; i++)
		ave->scratch[i] = readl(ave->mailbox + 0x18 + i * 4);
	for (unsigned int i = 0; i < 4; i++)
		ave->hello[i] = ave->scratch[i];
	dma_rmb();
	dev_info(ave->dev, "AVE_HELLO_RESULT result=%d status=%#x words=%#x,%#x,%#x,%#x core=%#x\n",
		 ret, status, ave->hello[0], ave->hello[1], ave->hello[2], ave->hello[3], ave->core_status);
	if (!ret)
		ret = ave_negotiate_ipc(ave);
	ave->start_result = ret;
	dev_info(ave->dev, "AVE_IPC_RESULT result=%d phase=%s ready=%u\n",
		 ret, ave->phase, ave->ipc_ready);
out:
	mutex_unlock(&ave->lock);
	return ret ?: len;
}

static const struct file_operations ave_start_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ave_start_write,
};

static int ave_verify_firmware(struct apple_ave *ave)
{
	static const char * const crc_names[] = {
		"aurora,ave-text-crc32", "aurora,ave-data-crc32",
	};
	static const size_t sizes[] = { 0x14c000, 0x130000 };
	struct device_node *chosen, *node;
	unsigned int i;
	int ret = 0;

	if (of_count_phandle_with_args(ave->dev->of_node, "memory-region", NULL) != 2)
		return dev_err_probe(ave->dev, -EINVAL, "Expected two live AVE firmware reservations\n");
	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return -ENOENT;
	for (i = 0; i < ARRAY_SIZE(ave->fw); i++) {
		struct ave_firmware *fw = &ave->fw[i];
		struct resource res;
		const char *crc_string;
		u32 maps[5], expected_crc;
		void *cpu;
		size_t offset;

		node = of_parse_phandle(ave->dev->of_node, "memory-region", i);
		ret = of_address_to_resource(node, 0, &res);
		if (!ret)
			ret = of_property_read_u32_array(node, "iommu-addresses", maps, ARRAY_SIZE(maps));
		if (!ret && (!of_property_read_bool(node, "no-map") ||
			     !of_device_is_compatible(node, "apple,asc-mem")))
			ret = -EINVAL;
		of_node_put(node);
		if (ret)
			break;
		fw->physical = res.start;
		fw->size = resource_size(&res);
		fw->iova = (u64)maps[1] << 32 | maps[2];
		if (fw->size != sizes[i] || !IS_ALIGNED(fw->physical, PAGE_SIZE) ||
		    maps[0] != ave->dev->of_node->phandle || maps[3] || maps[4] != sizes[i] ||
		    fw->iova != AVE_FW_IOVA + (i ? sizes[0] : 0)) {
			ret = -EINVAL;
			break;
		}
		/* The IOMMU core owns these translated reservations across module
		 * unloads. DMA allocation must never reuse their IOVAs.
		 */
		for (offset = 0; offset < fw->size; offset += PAGE_SIZE) {
			if (iommu_iova_to_phys(ave->domain, fw->iova + offset) != fw->physical + offset) {
				dev_err(ave->dev, "Firmware %u mapping mismatch at %#zx\n", i, offset);
				ret = -EIO;
				break;
			}
			fw->verified_pages++;
		}
		if (ret)
			break;
		ret = of_property_read_string(chosen, crc_names[i], &crc_string);
		if (!ret)
			ret = kstrtou32(crc_string, 16, &expected_crc);
		if (ret)
			break;
		cpu = memremap(fw->physical, fw->size, MEMREMAP_WB);
		if (!cpu) {
			ret = -ENOMEM;
			break;
		}
		fw->crc = crc32_le(~0U, cpu, fw->size) ^ ~0U;
		memunmap(cpu);
		if (fw->crc != expected_crc) {
			dev_err(ave->dev, "Firmware %u changed since the boot preflight\n", i);
			ret = -EILSEQ;
			break;
		}
		/* Only the two inspected 9003.78.0 text images are supported.
		 * CRCs detect build drift; they are not firmware authentication.
		 */
		if (!i && fw->crc != 0x2ce7359e && fw->crc != 0x69c54454) {
			dev_err(ave->dev, "Unsupported AVE firmware text CRC %#x\n", fw->crc);
			ret = -ENODEV;
			break;
		}
	}
	of_node_put(chosen);
	if (!ret)
		ave->firmware_verified = true;
	return ret;
}

/* Check the real IOMMU page table against the CPU mapping of each allocation.
 * This proves mappings, not that firmware has performed a DMA transaction.
 */
static int ave_verify_mapping(struct apple_ave *ave, struct ave_buffer *buf)
{
	size_t offset;

	if (!IS_ALIGNED(buf->dma, PAGE_SIZE) || !IS_ALIGNED(buf->size, PAGE_SIZE))
		return -EINVAL;
	for (offset = 0; offset < buf->size; offset += PAGE_SIZE) {
		void *cpu = buf->cpu + offset;
		struct page *page = is_vmalloc_addr(cpu) ? vmalloc_to_page(cpu) : virt_to_page(cpu);
		phys_addr_t physical = iommu_iova_to_phys(ave->domain, buf->dma + offset);

		if (!page || physical != page_to_phys(page)) {
			dev_err(ave->dev, "DMA mapping mismatch at offset %#zx\n", offset);
			return -EIO;
		}
		buf->verified_pages++;
	}
	return 0;
}

static int ave_alloc_buffer(struct apple_ave *ave, struct ave_buffer *buf, size_t size)
{
	buf->size = size;
	buf->cpu = dma_alloc_coherent(ave->dev, size, &buf->dma, GFP_KERNEL);
	if (!buf->cpu)
		return -ENOMEM;
	memset(buf->cpu, 0, size);
	return ave_verify_mapping(ave, buf);
}

static void ave_free_buffer(struct apple_ave *ave, struct ave_buffer *buf)
{
	if (buf->cpu)
		dma_free_coherent(ave->dev, buf->size, buf->cpu, buf->dma);
	buf->cpu = NULL;
}

static void ave_power_off(struct apple_ave *ave)
{
	while (ave->num_power) {
		struct device *pd = ave->power[--ave->num_power];

		pm_runtime_put_sync(pd);
		dev_pm_domain_detach(pd, true);
	}
}

static int ave_power_on(struct apple_ave *ave)
{
	static const char * const names[] = { "dma", "pipe4", "pipe5", "me0", "me1" };
	int ret;

	for (unsigned int i = 0; i < ARRAY_SIZE(names); i++) {
		struct device *pd = dev_pm_domain_attach_by_name(ave->dev, names[i]);

		if (IS_ERR(pd))
			return PTR_ERR(pd);
		if (!pd)
			return -ENODEV;
		ret = pm_runtime_resume_and_get(pd);
		if (ret < 0) {
			dev_pm_domain_detach(pd, true);
			return ret;
		}
		ave->power[ave->num_power++] = pd;
	}
	return 0;
}

static int ave_status_show(struct seq_file *s, void *unused)
{
	struct apple_ave *ave = s->private;

	mutex_lock(&ave->lock);
	seq_printf(s, "state=%s\nfirmware_start_requested=%u\nstart_result=%d\n",
		   ave->start_requested ? (ave->start_result ? "startup-failed" : "ipc-ready") :
		   "firmware-mapped", ave->start_requested, ave->start_result);
	seq_printf(s, "hardware_dma_tested=%u\nencoder_registered=0\nphase=%s\n",
		   ave->ipc_ready, ave->phase ?: "not-started");
	seq_printf(s, "ipc_ready=%u\nipc_fw_base=%#llx\nchannel_bytes=%#x\nextra_bytes=%#x\n",
		   ave->ipc_ready, ave->ipc_fw_base, ave->channel_bytes, ave->extra_bytes);
	seq_printf(s, "commands=%u\nreplies=%u\ntransport_acks=%u\ncommand_failed=%u\nresponse_bytes=%zu\n",
		   ave->commands, ave->replies, ave->transport_acks, ave->command_failed, ave->response_bytes);
	seq_printf(s, "arena_bytes=%zu\narena_ipc_offset=%#x\narena_dma=%#llx\narena_fw=%#llx\n",
		   ave->ipc.size - AVE_ARENA_OFF, AVE_ARENA_OFF,
		   (u64)ave->ipc.dma + AVE_ARENA_OFF, ave->ipc_fw_base + AVE_ARENA_OFF);
	for (unsigned int i = 0; i < 2; i++) {
		struct ave_channel *c = &ave->channel[i];
		seq_printf(s, "channel%u_name=%s\nchannel%u_mode=%u\nchannel%u_interrupt=%u\n"
			   "channel%u_slots=%u\nchannel%u_offset=%#x\nchannel%u_consumer=%d\nchannel%u_producer=%d\n",
			   i, c->name, i, c->mode, i, c->interrupt, i, c->slots,
			   i, c->offset, i, c->consumer, i, c->producer);
	}
	seq_printf(s, "page_size=%lu\n", PAGE_SIZE);
	seq_printf(s, "powered_domains=%u\n", ave->num_power);
	seq_printf(s, "ipc_bytes=%zu\nipc_dma=%pad\nipc_verified_pages=%u\n",
		   ave->ipc.size, &ave->ipc.dma, ave->ipc.verified_pages);
	seq_printf(s, "queue_bytes=%zu\nqueue_dma=%pad\nqueue_verified_pages=%u\n",
		   ave->queue.size, &ave->queue.dma, ave->queue.verified_pages);
	seq_printf(s, "log_bytes=%zu\nlog_dma=%pad\nlog_verified_pages=%u\n",
		   ave->log.size, &ave->log.dma, ave->log.verified_pages);
	seq_printf(s, "heap_bytes=%zu\nheap_dma=%pad\nheap_verified_pages=%u\n",
		   ave->heap.size, &ave->heap.dma, ave->heap.verified_pages);
	for (unsigned int i = 0; i < ARRAY_SIZE(ave->fw); i++)
		seq_printf(s, "fw%u_physical=%pa\nfw%u_iova=%#llx\nfw%u_bytes=%zu\n"
			   "fw%u_verified_pages=%u\nfw%u_crc32=%08x\n",
			   i, &ave->fw[i].physical, i, ave->fw[i].iova, i, ave->fw[i].size,
			   i, ave->fw[i].verified_pages, i, ave->fw[i].crc);
	seq_printf(s, "register_snapshot=%u\n", snapshot);
	if (snapshot) {
		seq_printf(s, "vcbar=%#llx\ncore_control=%#x\ncore_status=%#x\nmailbox_status=%#x\n",
			   ave->vcbar, ave->core_control, ave->core_status, ave->mailbox_status);
		for (unsigned int i = 0; i < ARRAY_SIZE(ave->scratch); i++)
			seq_printf(s, "scratch%u=%#x\n", i, ave->scratch[i]);
	}
	seq_printf(s, "resources_pinned_until_reboot=%u\n", ave->start_requested);
	mutex_unlock(&ave->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ave_status);

static int apple_ave_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_ave *ave;
	void __iomem *regs, *mailbox, *coproc;
	int ret;

	if (!enable)
		return -ENODEV;
	if (!of_machine_is_compatible("apple,j700") || PAGE_SIZE != SZ_16K)
		return -ENODEV;
	ave = devm_kzalloc(dev, sizeof(*ave), GFP_KERNEL);
	if (!ave)
		return -ENOMEM;
	ave->dev = dev;
	mutex_init(&ave->lock);
	/* Claim only dedicated AVE ranges, never the shared PMGR/SRAM windows. */
	regs = devm_platform_ioremap_resource_byname(pdev, "codec");
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	mailbox = devm_platform_ioremap_resource_byname(pdev, "mailbox");
	if (IS_ERR(mailbox))
		return PTR_ERR(mailbox);
	coproc = devm_platform_ioremap_resource_byname(pdev, "coproc");
	if (IS_ERR(coproc))
		return PTR_ERR(coproc);
	ave->coproc = coproc;
	ave->mailbox = mailbox;
	ave->domain = iommu_get_domain_for_dev(dev);
	if (!ave->domain || !(ave->domain->type & __IOMMU_DOMAIN_PAGING))
		return dev_err_probe(dev, -ENODEV, "A translated IOMMU domain is required\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	ret = ave_power_on(ave);
	if (ret)
		goto release_power;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		goto release_power;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto release_power;
	ret = ave_alloc_buffer(ave, &ave->ipc, AVE_IPC_BYTES);
	if (ret)
		goto free_buffers;
	ret = ave_alloc_buffer(ave, &ave->queue, AVE_QUEUE_BYTES);
	if (ret)
		goto free_buffers;
	ret = ave_alloc_buffer(ave, &ave->log, AVE_LOG_BYTES);
	if (ret)
		goto free_buffers;
	ret = ave_verify_firmware(ave);
	if (ret)
		goto free_buffers;
	if (snapshot) {
		ave->vcbar = readq(coproc + 0x50000);
		ave->core_control = readl(coproc + 0x600044);
		ave->core_status = readl(coproc + 0x600048);
		ave->mailbox_status = readl(mailbox + 0x10);
		for (unsigned int i = 0; i < ARRAY_SIZE(ave->scratch); i++)
			ave->scratch[i] = readl(mailbox + 0x18 + i * 4);
	}
	platform_set_drvdata(pdev, ave);
	ave->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("ave-status", 0400, ave->debugfs, ave, &ave_status_fops);
	debugfs_create_file("ave-start-once", 0200, ave->debugfs, ave, &ave_start_fops);
	debugfs_create_file("ave-command", 0600, ave->debugfs, ave, &ave_command_fops);
	debugfs_create_file("ave-poll", 0200, ave->debugfs, ave, &ave_poll_fops);
	debugfs_create_file("ave-arena", 0600, ave->debugfs, ave, &ave_arena_fops);
	ave->ipc_blob.data = ave->ipc.cpu;
	ave->ipc_blob.size = ave->ipc.size;
	debugfs_create_blob("ave-ipc-memory", 0400, ave->debugfs, &ave->ipc_blob);
	ave->log_blob.data = ave->log.cpu;
	ave->log_blob.size = ave->log.size;
	debugfs_create_blob("ave-firmware-log", 0400, ave->debugfs, &ave->log_blob);
	ave->config_blob.data = ave->queue.cpu;
	ave->config_blob.size = 0x40;
	debugfs_create_blob("ave-boot-config", 0400, ave->debugfs, &ave->config_blob);
	dev_info(dev, "AVE_DMA_READY ipc=%zu queue=%zu verified_pages=%u firmware_start=0\n",
		 ave->ipc.size, ave->queue.size,
		 ave->ipc.verified_pages + ave->queue.verified_pages);
	dev_info(dev, "AVE_FIRMWARE_MAPPED pages=%u contents_match_boot=1 register_snapshot=%u\n",
		 ave->fw[0].verified_pages + ave->fw[1].verified_pages, snapshot);
	return 0;

free_buffers:
	ave_free_buffer(ave, &ave->heap);
	ave_free_buffer(ave, &ave->log);
	ave_free_buffer(ave, &ave->queue);
	ave_free_buffer(ave, &ave->ipc);
	pm_runtime_put_sync(dev);
release_power:
	ave_power_off(ave);
	return dev_err_probe(dev, ret, "AVE DMA allocation/verification failed\n");
}

static void apple_ave_remove(struct platform_device *pdev)
{
	struct apple_ave *ave = platform_get_drvdata(pdev);

	debugfs_remove_recursive(ave->debugfs);
	ave_free_buffer(ave, &ave->heap);
	ave_free_buffer(ave, &ave->log);
	ave_free_buffer(ave, &ave->queue);
	ave_free_buffer(ave, &ave->ipc);
	pm_runtime_put_sync(ave->dev);
	ave_power_off(ave);
	dev_info(ave->dev, "AVE_DMA_RELEASED\n");
}

static const struct of_device_id apple_ave_match[] = {
	{ .compatible = "apple,t8140-ave" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_ave_match);

static struct platform_driver apple_ave_driver = {
	.probe = apple_ave_probe,
	.remove = apple_ave_remove,
	.driver = {
		.name = "apple-ave",
		.of_match_table = apple_ave_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(apple_ave_driver);
MODULE_DESCRIPTION("Apple T8140 experimental video encoder transport");
MODULE_LICENSE("GPL");

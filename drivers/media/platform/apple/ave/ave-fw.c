// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: firmware lifecycle
 *
 * The bootloader leaves the coprocessor firmware in two reserved, DART-mapped
 * segments: TEXT and DATA. Running the firmware changes DATA, so the driver
 * keeps a copy of DATA taken before the first boot and puts it back before
 * every later boot. That lets the coprocessor lose power (runtime PM, system
 * sleep) and start again from the same state as the first time.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>

#include "ave.h"
#include "ave-regs.h"

/*
 * Firmware builds whose command layouts this driver implements, by the CRC32
 * of their TEXT segment. The command formats change between builds.
 */
static const u32 ave_known_text_crc[] = {
	0x2ce7359e,	/* AppleAVE2FW 9003.78.0 */
	0x69c54454,	/* AppleAVE2FW 9003.78.0 (macOS 26.0.x) */
};

static int ave_fw_chosen_crc(struct ave_dev *ave, const char *name, u32 *crc)
{
	struct device_node *chosen = of_find_node_by_path("/chosen");
	const char *str;
	int ret;

	if (!chosen)
		return -ENOENT;
	ret = of_property_read_string(chosen, name, &str);
	of_node_put(chosen);
	if (ret)
		return ret;
	return kstrtou32(str, 16, crc);
}

static int ave_fw_segment(struct ave_dev *ave, unsigned int index,
			  struct ave_fw_segment *seg)
{
	struct device_node *np;
	struct resource res;
	const __be32 *prop;
	int len, ret;

	np = of_parse_phandle(ave->dev->of_node, "memory-region", index);
	if (!np)
		return -ENOENT;
	ret = of_address_to_resource(np, 0, &res);
	if (ret)
		goto out;

	/* iommu-addresses = <&ave iova-hi iova-lo size-hi size-lo> */
	prop = of_get_property(np, "iommu-addresses", &len);
	if (!prop || len != 5 * sizeof(u32) ||
	    be32_to_cpup(prop) != ave->dev->of_node->phandle) {
		ret = -EINVAL;
		goto out;
	}
	seg->phys = res.start;
	seg->size = resource_size(&res);
	seg->iova = of_read_number(prop + 1, 2);
	if (of_read_number(prop + 3, 2) != seg->size ||
	    !IS_ALIGNED(seg->phys, SZ_16K) || !IS_ALIGNED(seg->iova, SZ_16K))
		ret = -EINVAL;
out:
	of_node_put(np);
	return ret;
}

static int ave_fw_check_mapping(struct ave_dev *ave, const struct ave_fw_segment *seg)
{
	for (size_t off = 0; off < seg->size; off += PAGE_SIZE)
		if (iommu_iova_to_phys(ave->domain, seg->iova + off) != seg->phys + off)
			return -EIO;
	return 0;
}

static u32 ave_fw_crc(const struct ave_fw_segment *seg, void **copy)
{
	void *va = memremap(seg->phys, seg->size, MEMREMAP_WB);
	u32 crc;

	if (!va)
		return 0;
	crc = crc32_le(~0U, va, seg->size) ^ ~0U;
	if (copy)
		memcpy(*copy, va, seg->size);
	memunmap(va);
	return crc;
}

/*
 * Find the firmware, check that it is a build we speak to and that DATA is
 * still as the bootloader left it, and keep a copy of DATA for restarts.
 */
int ave_fw_probe(struct ave_dev *ave)
{
	struct device *dev = ave->dev;
	u32 text_crc, data_crc, want;
	bool known = false;
	int ret;

	if (of_count_phandle_with_args(dev->of_node, "memory-region", NULL) != 2)
		return dev_err_probe(dev, -ENODEV, "no preloaded firmware\n");

	ret = ave_fw_segment(ave, 0, &ave->text);
	if (!ret)
		ret = ave_fw_segment(ave, 1, &ave->data);
	if (ret)
		return dev_err_probe(dev, ret, "bad firmware reservation\n");

	if (ave_fw_check_mapping(ave, &ave->text) || ave_fw_check_mapping(ave, &ave->data))
		return dev_err_probe(dev, -EIO, "firmware is not mapped where it was loaded\n");

	text_crc = ave_fw_crc(&ave->text, NULL);
	for (unsigned int i = 0; i < ARRAY_SIZE(ave_known_text_crc); i++)
		known |= text_crc == ave_known_text_crc[i];
	if (!known)
		return dev_err_probe(dev, -ENODEV, "unsupported firmware (text CRC %08x)\n",
				     text_crc);

	ave->data_pristine = vmalloc(ave->data.size);
	if (!ave->data_pristine)
		return -ENOMEM;
	data_crc = ave_fw_crc(&ave->data, &ave->data_pristine);

	/*
	 * The loader records the DATA checksum before anything runs. If it
	 * differs, the firmware already ran and there is no clean copy left.
	 */
	ret = ave_fw_chosen_crc(ave, "aurora,ave-data-crc32", &want);
	if (ret || data_crc != want) {
		vfree(ave->data_pristine);
		ave->data_pristine = NULL;
		return dev_err_probe(dev, -ENODEV,
				     "firmware data is not in its boot state\n");
	}

	ave->fw_state = AVE_FW_OFF;
	return 0;
}

static void ave_fw_restore_data(struct ave_dev *ave)
{
	void *va;

	if (!ave->data_dirty)
		return;
	va = memremap(ave->data.phys, ave->data.size, MEMREMAP_WC);
	if (WARN_ON(!va))
		return;
	memcpy(va, ave->data_pristine, ave->data.size);
	wmb();
	memunmap(va);
	ave->data_dirty = false;
}

static void ave_free(struct ave_dev *ave, struct ave_buf *b)
{
	if (b->cpu)
		dma_free_coherent(ave->dev, b->size, b->cpu, b->dma);
	b->cpu = NULL;
}

static int ave_alloc(struct ave_dev *ave, struct ave_buf *b, size_t size)
{
	if (b->cpu)
		return 0;
	b->size = size;
	b->cpu = dma_alloc_coherent(ave->dev, size, &b->dma, GFP_KERNEL);
	if (!b->cpu)
		return -ENOMEM;
	if (!IS_ALIGNED(b->dma, SZ_16K)) {
		ave_free(ave, b);
		return -EINVAL;
	}
	return 0;
}

void ave_fw_remove(struct ave_dev *ave)
{
	ave_free(ave, &ave->heap);
	ave_free(ave, &ave->log);
	ave_free(ave, &ave->boot_cfg);
	ave_free(ave, &ave->ipc);
	vfree(ave->data_pristine);
	ave->data_pristine = NULL;
}

static bool ave_fw_idle(struct ave_dev *ave)
{
	if (readq(ave->coproc + AVE_COPROC_VCBAR) != (ave->text.iova | AVE_COPROC_VCBAR_VALID))
		return false;
	if (readl(ave->coproc + AVE_COPROC_CTRL) || readl(ave->mbox + AVE_MBOX_STATUS))
		return false;
	for (unsigned int i = 0; i < AVE_MBOX_SCRATCH_COUNT; i++)
		if (readl(ave->mbox + AVE_MBOX_SCRATCH(i)))
			return false;
	return true;
}

/* Boot the coprocessor and bring up its command rings. Caller holds hw_lock. */
static int ave_fw_boot(struct ave_dev *ave)
{
	__le32 *cfg, *log;
	u32 status, hello[4];
	int ret;

	ret = ave_alloc(ave, &ave->ipc, AVE_IPC_BYTES);
	if (!ret)
		ret = ave_alloc(ave, &ave->boot_cfg, SZ_16K);
	if (!ret)
		ret = ave_alloc(ave, &ave->log, AVE_LOG_BYTES);
	if (ret)
		return ret;

	/*
	 * The bootloader leaves the coprocessor stopped with its code base
	 * set. Do not start it from any other state.
	 */
	if (!ave_fw_idle(ave)) {
		dev_err(ave->dev, "coprocessor is not in its reset state (vcbar %#llx ctrl %#x)\n",
			readq(ave->coproc + AVE_COPROC_VCBAR),
			readl(ave->coproc + AVE_COPROC_CTRL));
		return -EIO;
	}

	ave_fw_restore_data(ave);

	cfg = ave->boot_cfg.cpu;
	log = ave->log.cpu;
	memset(cfg, 0, AVE_BOOT_CFG_BYTES);
	memset(log, 0, ave->log.size);
	cfg[AVE_BOOT_CFG_DEVICE] = cpu_to_le32(AVE_BOOT_CFG_DEVICE_T8140);
	cfg[AVE_BOOT_CFG_UNITS] = cpu_to_le32(1);
	cfg[AVE_BOOT_CFG_GROUPS] = cpu_to_le32(1);
	cfg[AVE_BOOT_CFG_SUBID_MASK] = cpu_to_le32(1);
	cfg[AVE_BOOT_CFG_LOG_LO] = cpu_to_le32(lower_32_bits(ave->log.dma));
	cfg[AVE_BOOT_CFG_LOG_HI] = cpu_to_le32(upper_32_bits(ave->log.dma));
	cfg[AVE_BOOT_CFG_LOG_BYTES] = cpu_to_le32(AVE_LOG_BYTES);
	cfg[AVE_BOOT_CFG_OPTIONS] = cpu_to_le32(AVE_BOOT_CFG_OPTIONS_VALUE);
	log[0] = cpu_to_le32(AVE_LOG_HDR_USED);
	log[1] = cpu_to_le32(AVE_LOG_HDR_BYTES);
	log[2] = cpu_to_le32(AVE_LOG_RING_BYTES);
	dma_wmb();

	writel(AVE_BOOT_MAGIC, ave->mbox + AVE_MBOX_SCRATCH(0));
	writel(lower_32_bits(ave->boot_cfg.dma), ave->mbox + AVE_MBOX_SCRATCH(1));
	writel(upper_32_bits(ave->boot_cfg.dma), ave->mbox + AVE_MBOX_SCRATCH(2));

	ave->data_dirty = true;
	writel(1, ave->coproc + AVE_COPROC_KEEP_MAPPINGS);
	writel(0, ave->coproc + AVE_COPROC_CTRL);
	writel(AVE_COPROC_BOOT_ARG_VALUE, ave->coproc + AVE_COPROC_BOOT_ARG);
	writel(AVE_COPROC_CTRL_RUN, ave->coproc + AVE_COPROC_CTRL);

	ret = readl_poll_timeout(ave->mbox + AVE_MBOX_STATUS, status,
				 status & AVE_MBOX_STATUS_PENDING, 50, AVE_MBOX_TIMEOUT_US);
	if (ret) {
		dev_err(ave->dev, "firmware did not answer (status %#x)\n",
			readl(ave->coproc + AVE_COPROC_STATUS));
		return ret;
	}
	for (unsigned int i = 0; i < 4; i++)
		hello[i] = readl(ave->mbox + AVE_MBOX_SCRATCH(i));
	dma_rmb();

	ret = ave_ipc_negotiate(ave, hello);
	if (ret) {
		dev_err(ave->dev, "firmware start-up failed: %d\n", ret);
		return ret;
	}

	ave->fw_state = AVE_FW_RUNNING;
	ret = ave_cmd_config(ave);
	if (ret)
		return ret;

	dev_dbg(ave->dev, "firmware running, %u/%u ring slots\n",
		ave->ring[0].slots, ave->ring[1].slots);
	return 0;
}

/* Caller holds hw_lock. */
int ave_fw_ensure_running(struct ave_dev *ave)
{
	int ret;

	lockdep_assert_held(&ave->hw_lock);
	switch (ave->fw_state) {
	case AVE_FW_RUNNING:
		return 0;
	case AVE_FW_BROKEN:
		/* Only a power cycle gets the coprocessor back; see runtime PM. */
		return -EIO;
	case AVE_FW_OFF:
		break;
	}

	ret = ave_fw_boot(ave);
	if (ret)
		ave->fw_state = AVE_FW_BROKEN;
	return ret;
}

/*
 * Stop the firmware before its power goes away. Caller holds hw_lock.
 *
 * The firmware is told to halt once every session is closed. Stopping the
 * coprocessor core itself and checking that the fabric is idle are not
 * described yet, so until then a running firmware keeps its power (runtime
 * suspend fails with -EBUSY). Across system sleep the domains go down anyway;
 * ave_fw_resume() notices and the next session boots the firmware again from
 * the saved DATA copy.
 */
int ave_fw_shutdown(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state == AVE_FW_OFF)
		return 0;
	return -EBUSY;
}

/* The coprocessor lost power behind the firmware's back. Caller holds hw_lock. */
void ave_fw_lost(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	ave->fw_state = AVE_FW_OFF;
	memset(ave->ring, 0, sizeof(ave->ring));
	ave->ipc_fw_base = 0;
}

/* After system resume: did the coprocessor keep running? Caller holds hw_lock. */
void ave_fw_resume(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state == AVE_FW_OFF)
		return;
	/* A running firmware keeps the device runtime active, so this is powered. */
	if (readl(ave->coproc + AVE_COPROC_CTRL) & AVE_COPROC_CTRL_RUN)
		return;
	dev_dbg(ave->dev, "coprocessor lost power during sleep\n");
	ave_fw_lost(ave);
}

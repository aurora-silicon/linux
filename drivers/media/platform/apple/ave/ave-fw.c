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
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "ave.h"
#include "ave-regs.h"

/*
 * Legacy TEXT CRCs accepted at T8140 probe. Encoding separately requires
 * a layout from ave_layout_for_crc(); command formats change between builds.
 */
static const u32 ave_known_text_crc[] = {
	0x2ce7359e, /* AppleAVE2FW 9003.78.0 */
	0x69c54454, /* AppleAVE2FW 9003.78.0 (macOS 26.0.x) */
	AVE_FW_TEXT_CRC_T6040_25G83, /* AppleAVE2FW 9003.78.0 / T6040 */
	0x460c7a7c, /* AppleAVE2FW 6070.11.1 / T6000 */
	0x25bfd039, /* AppleAVE2FW 6070.11.1 / T8112 */
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
	if (!of_node_name_eq(np->parent, "reserved-memory") ||
	    !of_device_is_compatible(np, "apple,asc-mem") ||
	    !of_property_read_bool(np, "no-map")) {
		ret = -EINVAL;
		goto out;
	}
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
	if (!seg->size || seg->phys > PHYS_ADDR_MAX - seg->size ||
	    seg->iova > U64_MAX - seg->size ||
	    of_read_number(prop + 3, 2) != seg->size ||
	    !IS_ALIGNED(seg->phys, SZ_16K) || !IS_ALIGNED(seg->iova, SZ_16K))
		ret = -EINVAL;
out:
	of_node_put(np);
	return ret;
}

static int ave_fw_check_mapping(struct ave_dev *ave,
				const struct ave_fw_segment *seg)
{
	for (size_t off = 0; off < seg->size; off += SZ_16K)
		if (iommu_iova_to_phys(ave->domain, seg->iova + off) !=
		    seg->phys + off)
			return -EIO;
	return 0;
}

static int ave_fw_crc(const struct ave_fw_segment *seg, u32 *crc, void *copy)
{
	void *va = memremap(seg->phys, seg->size, MEMREMAP_WB);

	if (!va)
		return -ENOMEM;
	*crc = crc32_le(~0U, va, seg->size) ^ ~0U;
	if (copy)
		memcpy(copy, va, seg->size);
	memunmap(va);
	return 0;
}

/*
 * Find the firmware, check that it is a build we speak to and that DATA is
 * still as the bootloader left it, and keep a copy of DATA for restarts.
 */
int ave_fw_probe(struct ave_dev *ave)
{
	struct device *dev = ave->dev;
	u32 text_crc, data_crc, want;
	bool known = !ave->variant->boot_qualified;
	int ret;

	if (of_count_phandle_with_args(dev->of_node, "memory-region", NULL) !=
	    2)
		return dev_err_probe(dev, -ENODEV, "no preloaded firmware\n");

	ret = ave_fw_segment(ave, 0, &ave->text);
	if (!ret)
		ret = ave_fw_segment(ave, 1, &ave->data);
	if (ret)
		return dev_err_probe(dev, ret, "bad firmware reservation\n");

	if (ave->text.size != ave->variant->fw_text_size ||
	    ave->data.size != ave->variant->fw_data_size)
		return dev_err_probe(dev, -EINVAL,
				     "unexpected firmware segment sizes\n");
	if ((ave->text.phys < ave->data.phys + ave->data.size &&
	     ave->data.phys < ave->text.phys + ave->text.size) ||
	    (ave->text.iova < ave->data.iova + ave->data.size &&
	     ave->data.iova < ave->text.iova + ave->text.size))
		return dev_err_probe(dev, -EINVAL,
				     "overlapping firmware segments\n");

	if (ave_fw_check_mapping(ave, &ave->text) ||
	    ave_fw_check_mapping(ave, &ave->data))
		return dev_err_probe(
			dev, -EIO,
			"firmware is not mapped where it was loaded\n");

	ret = ave_fw_crc(&ave->text, &text_crc, NULL);
	if (ret)
		return ret;
	ret = ave_fw_chosen_crc(ave, "aurora,ave-text-crc32", &want);
	if (ret || text_crc != want)
		return dev_err_probe(dev, -ENODEV,
				     "firmware text differs from loader CRC\n");
	for (unsigned int i = 0; i < ARRAY_SIZE(ave_known_text_crc); i++)
		known |= text_crc == ave_known_text_crc[i];
	if (!known ||
	    (ave->variant->text_crc && text_crc != ave->variant->text_crc))
		return dev_err_probe(dev, -ENODEV,
				     "unsupported firmware (text CRC %08x)\n",
				     text_crc);
	ave->text_crc = text_crc;
	ave->layout = ave_layout_for_crc(text_crc);

	ave->data_pristine = vmalloc(ave->data.size);
	if (!ave->data_pristine)
		return -ENOMEM;
	ret = ave_fw_crc(&ave->data, &data_crc, ave->data_pristine);
	if (ret)
		goto free_data;

	/*
	 * The loader records the DATA checksum before anything runs. If it
	 * differs, the firmware already ran and there is no clean copy left.
	 */
	ret = ave_fw_chosen_crc(ave, "aurora,ave-data-crc32", &want);
	if (ret || data_crc != want) {
		ret = dev_err_probe(dev, -ENODEV,
				    "firmware data is not in its boot state\n");
		goto free_data;
	}

	ave->data_crc = data_crc;
	ave->fw_state = AVE_FW_OFF;
	if (!ave->variant->boot_qualified || !ave->layout)
		dev_info(
			dev,
			"probe only: firmware TEXT %08x DATA %08x needs qualification\n",
			text_crc, data_crc);
	else if (!ave->layout->encode_ready)
		dev_info(
			dev,
			"%s: TEXT %08x pristine DATA %08x; encoding disabled\n",
			ave->layout->name, text_crc, data_crc);
	return 0;

free_data:
	vfree(ave->data_pristine);
	ave->data_pristine = NULL;
	return ret;
}

static int ave_fw_restore_data(struct ave_dev *ave)
{
	void *va;

	if (!ave->data_dirty)
		return 0;
	va = memremap(ave->data.phys, ave->data.size, MEMREMAP_WC);
	if (!va)
		return -ENOMEM;
	memcpy(va, ave->data_pristine, ave->data.size);
	/* The restored image must be in memory before the coprocessor starts. */
	wmb();
	memunmap(va);
	if (ave->layout && ave->layout->abi->transport->scratch_halt) {
		u32 crc;
		int ret = ave_fw_crc(&ave->data, &crc, NULL);

		if (ret || crc != ave->data_crc)
			return ret ?: -EIO;
		dev_dbg(ave->dev, "restored DATA CRC %08x\n", ave->data_crc);
	}
	ave->data_dirty = false;
	return 0;
}

void ave_free(struct ave_dev *ave, struct ave_buf *b)
{
	if (b->cpu)
		dma_free_attrs(ave->dev, b->alloc, b->cpu, b->dma, b->attrs);
	b->cpu = NULL;
}

/*
 * Firmware-visible memory, zeroed and 16 KiB aligned in the encoder's DART.
 * @size is what the firmware is told; @alloc (at least @size) is mapped.
 */
int ave_alloc_padded(struct ave_dev *ave, struct ave_buf *b, size_t size,
		     size_t alloc, unsigned long attrs)
{
	if (b->cpu)
		return 0;
	b->size = size;
	b->alloc = PAGE_ALIGN(max(size, alloc));
	b->attrs = attrs;
	b->cpu =
		dma_alloc_attrs(ave->dev, b->alloc, &b->dma, GFP_KERNEL, attrs);
	if (!b->cpu)
		return -ENOMEM;
	if (!IS_ALIGNED(b->dma, SZ_16K)) {
		ave_free(ave, b);
		return -EINVAL;
	}
	return 0;
}

int ave_alloc(struct ave_dev *ave, struct ave_buf *b, size_t size,
	      unsigned long attrs)
{
	return ave_alloc_padded(ave, b, size, size, attrs);
}

/*
 * Lifecycle support and streaming qualification are independent: a known
 * transport is not enough to send unqualified Start/Process payloads.
 */
int ave_fw_check_boot(struct ave_dev *ave)
{
	if (ave->variant->boot_qualified && ave->variant->regs && ave->layout &&
	    ave->layout == ave_layout_for_crc(ave->text_crc))
		return 0;
	return -EOPNOTSUPP;
}

int ave_fw_check_build(struct ave_dev *ave)
{
	if (!ave_fw_check_boot(ave) && ave->layout->encode_ready)
		return 0;
	dev_info(
		ave->dev,
		"encoding refused: firmware TEXT %08x DATA %08x has no qualified command payload\n",
		ave->text_crc, ave->data_crc);
	return -EOPNOTSUPP;
}

/* Only after confirmed Halt, or before the firmware was ever started. */
void ave_fw_release_boot(struct ave_dev *ave)
{
	if (ave->extra0_mapped) {
		dma_unmap_resource(ave->dev, ave->extra0_dma,
				   ave->extra0_map_size, DMA_BIDIRECTIONAL, 0);
		ave->extra0_mapped = false;
	}
	ave_free(ave, &ave->heap);
	ave_free(ave, &ave->log);
	ave_free(ave, &ave->boot_cfg);
	ave_free(ave, &ave->ipc);
	if (ave->module_pinned) {
		ave->module_pinned = false;
		module_put(THIS_MODULE);
	}
}

static int ave_fw_map_extra0(struct ave_dev *ave)
{
	struct platform_device *pdev = to_platform_device(ave->dev);
	struct resource *res;

	if (!ave->variant->extra0_size || ave->extra0_mapped)
		return 0;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "extra0");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	if (!res || resource_size(res) < ave->variant->extra0_size ||
	    !IS_ALIGNED(res->start, SZ_16K))
		return dev_err_probe(
			ave->dev, -EINVAL,
			"missing or invalid extra0 register resource\n");
	/* The platform resource supplies the translated physical address. */
	ave->extra0_map_size = ALIGN(ave->variant->extra0_size, SZ_16K);
	ave->extra0_dma = dma_map_resource(ave->dev, res->start,
					   ave->extra0_map_size,
					   DMA_BIDIRECTIONAL, 0);
	if (dma_mapping_error(ave->dev, ave->extra0_dma))
		return -EIO;
	ave->extra0_mapped = true;
	return 0;
}

void ave_fw_remove(struct ave_dev *ave)
{
	ave_fw_release_boot(ave);
	vfree(ave->data_pristine);
	ave->data_pristine = NULL;
}

u64 ave_fw_vcbar(const struct ave_variant *variant, u64 text_iova)
{
	return (text_iova & variant->vcbar_mask) | variant->vcbar_flags;
}

static bool ave_fw_idle(struct ave_dev *ave)
{
	if (readq(ave->coproc + ave->variant->regs->vcbar) !=
	    ave_fw_vcbar(ave->variant, ave->text.iova))
		return false;
	if (readl(ave->coproc + ave->variant->regs->ctrl) ||
	    readl(ave->mbox + AVE_MBOX_STATUS))
		return false;
	for (unsigned int i = 0; i < AVE_MBOX_SCRATCH_COUNT; i++)
		if (readl(ave->mbox + AVE_MBOX_SCRATCH(i)))
			return false;
	return true;
}

/*
 * After a Halt the firmware is restarted from a power cycle, as macOS does
 * for every session: the encoder must have been runtime suspended in
 * between, which puts the coprocessor back in reset. Hardware runs showed
 * that a restart while it kept power gets no answer, so that is refused
 * (-EAGAIN) and the state stays halted until the power really went. The
 * 9003 code base register must still be set: that ABI uses the loader value.
 */
static int ave_fw_prepare_restart(struct ave_dev *ave)
{
	u32 status;

	if (!ave->layout->abi->transport->boot_config) {
		if (ave->fw_state == AVE_FW_HALTED)
			return -EAGAIN; /* A runtime PM suspend/resume must intervene. */
		if (readl(ave->coproc + ave->variant->regs->ctrl))
			return -EBUSY;
		status = readl(ave->mbox + AVE_MBOX_STATUS);
		if (status)
			writel(status, ave->mbox + AVE_MBOX_STATUS);
		for (unsigned int i = 0; i < AVE_MBOX_SCRATCH_COUNT; i++)
			writel(0, ave->mbox + AVE_MBOX_SCRATCH(i));
		return 0;
	}
	if (readq(ave->coproc + ave->variant->regs->vcbar) !=
	    ave_fw_vcbar(ave->variant, ave->text.iova)) {
		dev_err(ave->dev,
			"coprocessor lost its code base (vcbar %#llx)\n",
			readq(ave->coproc + ave->variant->regs->vcbar));
		return -EIO;
	}
	if (ave->fw_state == AVE_FW_RECOVERING) {
		/*
		 * Recovery cleared CTRL itself and re-armed BOOT_ARG as a
		 * reset witness with control off: require it to have lost
		 * power too, before reusing DMA memory or restoring DATA. DARTs
		 * sharing VENC power may suspend later than the consumer.
		 */
		if (readl(ave->coproc + ave->variant->regs->ctrl) ||
		    readl(ave->coproc + ave->variant->regs->boot_arg))
			return -EAGAIN;
		ave_sess_reap(ave);
		ave->fw_state = AVE_FW_HALTED;
		ave->inject_wedge = false;
		dev_info(ave->dev, "firmware recovery power cycle confirmed\n");
	}
	if (ave->fw_state != AVE_FW_HALTED)
		return 0;

	/*
	 * Boot left CTRL running and BOOT_ARG set; only a power loss clears
	 * both. Until then stopped sessions' memory stays, and so does the
	 * coprocessor: retry once it suspends.
	 */
	dev_dbg(ave->dev, "restart: CTRL %#x BOOT_ARG %#x\n",
		readl(ave->coproc + ave->variant->regs->ctrl),
		readl(ave->coproc + ave->variant->regs->boot_arg));
	if (readl(ave->coproc + ave->variant->regs->ctrl) ||
	    (!ave->witness_unusable &&
	     readl(ave->coproc + ave->variant->regs->boot_arg))) {
		dev_dbg(ave->dev,
			"coprocessor kept power after its halt; retry once it suspends\n");
		return -EAGAIN;
	}
	/* The coprocessor and its engines were reset: stopped sessions' memory can go. */
	ave_sess_reap(ave);
	status = readl(ave->mbox + AVE_MBOX_STATUS);
	if (status)
		writel(status, ave->mbox + AVE_MBOX_STATUS);
	for (unsigned int i = 0; i < AVE_MBOX_SCRATCH_COUNT; i++)
		writel(0, ave->mbox + AVE_MBOX_SCRATCH(i));
	return 0;
}

/* Boot the coprocessor and bring up its command rings. Caller holds hw_lock. */
static int ave_fw_boot(struct ave_dev *ave)
{
	const struct ave_fw_transport *t = ave->layout->abi->transport;
	struct iommu_domain *domain;
	__le32 *cfg, *log;
	u32 status, hello[4];
	int ret;

	ret = ave_alloc(ave, &ave->ipc, t->ipc_bytes, 0);
	if (!ret && t->boot_config)
		ret = ave_alloc(ave, &ave->boot_cfg, SZ_16K, 0);
	if (!ret && t->boot_config)
		ret = ave_alloc(ave, &ave->log, AVE_LOG_BYTES, 0);
	if (!ret)
		ret = ave_fw_map_extra0(ave);
	if (ret)
		return ret;

	if (ave_fw_check_mapping(ave, &ave->text) ||
	    ave_fw_check_mapping(ave, &ave->data)) {
		dev_err(ave->dev, "firmware segments are no longer mapped\n");
		return -EIO;
	}

	ret = ave_fw_prepare_restart(ave);
	if (ret)
		return ret;

	/*
	 * The bootloader leaves the coprocessor stopped with its code base
	 * set. Do not start it from any other state.
	 */
	if (t->boot_config && !ave_fw_idle(ave)) {
		dev_err(ave->dev,
			"coprocessor is not in its reset state (vcbar %#llx ctrl %#x)\n",
			readq(ave->coproc + ave->variant->regs->vcbar),
			readl(ave->coproc + ave->variant->regs->ctrl));
		return -EIO;
	}

	memset(ave->ring, 0, sizeof(ave->ring));
	ave->unacked = 0;
	memset(ave->want, 0, sizeof(ave->want));
	ave->reply_err = 0;
	ave->scalar_next = 0;

	ret = ave_fw_restore_data(ave);
	if (ret)
		return ret;
	/* Nothing from an earlier firmware lifetime may be read as ring state. */
	memset(ave->ipc.cpu, 0, ave->ipc.alloc);
	if (ave->heap.cpu)
		memset(ave->heap.cpu, 0, ave->heap.alloc);

	if (t->boot_config) {
		cfg = ave->boot_cfg.cpu;
		log = ave->log.cpu;
		memset(cfg, 0, AVE_BOOT_CFG_BYTES);
		memset(log, 0, ave->log.size);
		cfg[AVE_BOOT_CFG_DEVICE] =
			cpu_to_le32(ave->variant->boot_device_id);
		cfg[AVE_BOOT_CFG_UNITS] = cpu_to_le32(1);
		cfg[AVE_BOOT_CFG_GROUPS] = cpu_to_le32(1);
		cfg[AVE_BOOT_CFG_SUBID_MASK] = cpu_to_le32(1);
		cfg[AVE_BOOT_CFG_LOG_LO] =
			cpu_to_le32(lower_32_bits(ave->log.dma));
		cfg[AVE_BOOT_CFG_LOG_HI] =
			cpu_to_le32(upper_32_bits(ave->log.dma));
		cfg[AVE_BOOT_CFG_LOG_BYTES] = cpu_to_le32(AVE_LOG_BYTES);
		cfg[AVE_BOOT_CFG_OPTIONS] =
			cpu_to_le32(AVE_BOOT_CFG_OPTIONS_VALUE);
		log[0] = cpu_to_le32(AVE_LOG_HDR_USED);
		log[1] = cpu_to_le32(AVE_LOG_HDR_BYTES);
		log[2] = cpu_to_le32(AVE_LOG_RING_BYTES);
		dma_wmb();

		writel(AVE_BOOT_MAGIC, ave->mbox + AVE_MBOX_SCRATCH(0));
		writel(lower_32_bits(ave->boot_cfg.dma),
		       ave->mbox + AVE_MBOX_SCRATCH(1));
		writel(upper_32_bits(ave->boot_cfg.dma),
		       ave->mbox + AVE_MBOX_SCRATCH(2));

	} else {
		writel(AVE_BOOT_MAGIC, ave->mbox + AVE_MBOX_SCRATCH(0));
		writel(0, ave->mbox + AVE_MBOX_SCRATCH(1));
		writel(ave->variant->boot_device_id,
		       ave->mbox + AVE_MBOX_SCRATCH(2));
	}
	if (!t->boot_config) {
		writeq(ave_fw_vcbar(ave->variant, ave->text.iova),
		       ave->coproc + ave->variant->regs->vcbar);
		if (readq(ave->coproc + ave->variant->regs->vcbar) !=
		    ave_fw_vcbar(ave->variant, ave->text.iova))
			return -EIO;
		dev_dbg(ave->dev, "VCBAR %#llx boot argument %#x\n",
			ave_fw_vcbar(ave->variant, ave->text.iova),
			ave->variant->boot_arg_value);
	}

	if (!t->boot_config && !ave->module_pinned) {
		if (!try_module_get(THIS_MODULE))
			return -ENODEV;
		ave->module_pinned = true;
	}
	/* The coprocessor is in reset: drop every cached translation before it runs. */
	domain = iommu_get_domain_for_dev(ave->dev);
	if (domain)
		iommu_flush_iotlb_all(domain);
	ave->data_dirty = true;
	ave->fw_ever_ran = true;
	writel(1, ave->coproc + ave->variant->regs->keep_mappings);
	writel(0, ave->coproc + ave->variant->regs->ctrl);
	writel(ave->variant->boot_arg_value,
	       ave->coproc + ave->variant->regs->boot_arg);
	writel(AVE_COPROC_CTRL_RUN, ave->coproc + ave->variant->regs->ctrl);

	ret = readl_poll_timeout(ave->mbox + AVE_MBOX_STATUS, status,
				 status & AVE_MBOX_STATUS_PENDING, 50,
				 AVE_MBOX_TIMEOUT_US);
	if (ret) {
		dev_err(ave->dev, "firmware did not answer (status %#x)\n",
			readl(ave->coproc + ave->variant->regs->status));
		return ret;
	}
	for (unsigned int i = 0; i < 4; i++)
		hello[i] = readl(ave->mbox + AVE_MBOX_SCRATCH(i));
	dma_rmb();

	/* The reset witness is only as good as BOOT_ARG holding what boot wrote. */
	status = readl(ave->coproc + ave->variant->regs->boot_arg);
	dev_dbg(ave->dev, "BOOT_ARG after start %#x (wrote %#x)\n", status,
		ave->variant->boot_arg_value);
	if (t->boot_config && status != ave->variant->boot_arg_value &&
	    !ave->witness_unusable) {
		ave->witness_unusable = true;
		dev_warn(ave->dev,
			 "BOOT_ARG reads %#x after start, not %#x: no reset proof, stopped sessions keep their memory\n",
			 status, ave->variant->boot_arg_value);
	}

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
	enum ave_fw_state prev;
	int ret;

	lockdep_assert_held(&ave->hw_lock);
	ret = ave_fw_check_boot(ave);
	if (ret)
		return ret;
	switch (ave->fw_state) {
	case AVE_FW_RUNNING:
		return 0;
	case AVE_FW_BROKEN:
		dev_err_ratelimited(ave->dev, "firmware recovery is waiting for idle runtime suspend\n");
		return -EIO;
	case AVE_FW_OFF:
	case AVE_FW_HALTED:
	case AVE_FW_RECOVERING:
		break;
	}

	/* A boot that reached RUN must use the same recovery as a session fault. */
	prev = ave->fw_state;
	ret = ave_fw_boot(ave);
	if (ret && ret != -EAGAIN) {
		if (ave->layout->abi->transport->scratch_halt &&
		    (ave->module_pinned ||
		     readl(ave->coproc + ave->variant->regs->ctrl))) {
			if (!ave->module_pinned && try_module_get(THIS_MODULE))
				ave->module_pinned = true;
			ave_fw_mark_broken(ave, ret, "boot");
			return ret;
		}
		if (readl(ave->coproc + ave->variant->regs->ctrl))
			ave_fw_mark_broken(ave, ret, "boot");
		else
			ave->fw_state = prev;
	}
	return ret;
}

/*
 * Tell the firmware to stop, once every session is closed. Halt gets no
 * ring reply. 6070 confirms quiescence through scratch0 before the host
 * stops the IOP; 9003 retains its existing settle interval.
 * Caller holds hw_lock.
 */
void ave_fw_halt(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_RUNNING)
		return;
	if (ave->layout->abi->transport->scratch_halt) {
		for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
			if (ave->sessions[i] &&
			    ave->sessions[i]->sess->fw_open) {
				ave->fw_state = AVE_FW_BROKEN;
				return;
			}
		writel(0, ave->mbox + AVE_MBOX_SCRATCH(0));
	}
	if (ave_cmd_halt(ave)) {
		dev_err(ave->dev, "cannot send Halt; firmware treated as stuck\n");
		ave_fw_mark_broken(ave, -EIO, "Halt");
		return;
	}
	if (ave->layout->abi->transport->scratch_halt) {
		u32 status;
		int ret;

		ret = readl_poll_timeout(ave->mbox + AVE_MBOX_SCRATCH(0),
					 status, status == AVE_BOOT_MAGIC, 100,
					 AVE_MBOX_TIMEOUT_US);
		if (ret) {
			dev_err(ave->dev,
				"Halt not confirmed; retaining firmware mappings and power\n");
			ave->fw_state = AVE_FW_BROKEN;
			return;
		}
		dev_dbg(ave->dev, "Halt confirmed: scratch0 %#x\n", status);
		/* Scratch confirmation establishes quiescence; stop and read back the IOP.
		 */
		writel(0, ave->coproc + ave->variant->regs->ctrl);
		ret = readl_poll_timeout(ave->coproc + ave->variant->regs->ctrl,
					 status, !status, 100,
					 AVE_MBOX_TIMEOUT_US);
		if (ret) {
			ave->fw_state = AVE_FW_BROKEN;
			return;
		}
	} else {
		msleep(AVE_HALT_SETTLE_MS);
	}
	ave->fw_state = AVE_FW_HALTED;
	memset(ave->ring, 0, sizeof(ave->ring));
	ave->ipc_fw_base = 0;
}

/*
 * Stop the firmware before its power goes away. Caller holds hw_lock.
 *
 * Sessions keep the device runtime active, so the firmware has normally
 * been halted already. A firmware that stopped answering keeps its power:
 * system sleep must not silently force-stop a potentially live DMA engine.
 * Runtime recovery is separate and only runs after all owners have gone.
 */
int ave_fw_shutdown(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	switch (ave->fw_state) {
	case AVE_FW_RUNNING:
		ave_fw_halt(ave);
		return ave->fw_state == AVE_FW_HALTED ? 0 : -EBUSY;
	case AVE_FW_BROKEN:
	case AVE_FW_RECOVERING:
		return -EBUSY;
	default:
		return 0;
	}
}

/* Caller holds hw_lock. Wake even idle sessions so none pins a broken boot. */
void ave_fw_mark_broken(struct ave_dev *ave, int err, const char *what)
{
	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_BROKEN)
		dev_err(ave->dev, "firmware failed (%s: %d); scheduling recovery\n", what, err);
	ave->fw_state = AVE_FW_BROKEN;
	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i])
			mod_delayed_work(ave->wq, &ave->sessions[i]->poll_work, 0);
}

/*
 * Only runtime PM may force-stop a broken firmware, with no session owners
 * and no runtime references. genpd and the IOMMU's PM device links then drop
 * the VENC domains and DARTs; AVE must not reset or reprogram the DART itself.
 * System sleep continues to use the strict ave_fw_shutdown() above.
 */
int ave_fw_runtime_suspend(struct ave_dev *ave)
{
	int ret;

	lockdep_assert_held(&ave->hw_lock);
	/* 6070 requires scratch-confirmed Halt; never force its power gate. */
	if (ave->layout && ave->layout->abi->transport->scratch_halt)
		return ave_fw_shutdown(ave);
	if (ave->fw_state != AVE_FW_BROKEN && ave->fw_state != AVE_FW_RECOVERING)
		return ave_fw_shutdown(ave);
	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i])
			return -EBUSY;
	if (ave->fw_state == AVE_FW_RECOVERING)
		return 0;
	if (!ave_recovery_allow(&ave->recovery, jiffies)) {
		dev_err_ratelimited(ave->dev, "firmware recovery limit: 3 attempts per 10 minutes\n");
		return -EBUSY;
	}
	ave->recovery_count++;
	dev_warn(ave->dev, "firmware recovery %u: stopping coprocessor and cycling VENC power\n",
		 ave->recovery_count);

	/* A ring may not exist after a failed boot. Never wait for Halt's reply. */
	ret = ave->inject_wedge ? -ETIMEDOUT : -EIO;
	if (ave->ipc_fw_base && ave->ring[0].slots && !ave->inject_wedge) {
		ave->fw_state = AVE_FW_RUNNING;
		ret = ave_cmd_halt(ave);
	}
	if (ret)
		dev_warn(ave->dev, "recovery Halt failed: %d; forcing control off\n", ret);
	else
		msleep(AVE_HALT_SETTLE_MS);
	writel(0, ave->coproc + ave->variant->regs->ctrl);
	if (readl(ave->coproc + ave->variant->regs->ctrl)) {
		ave->fw_state = AVE_FW_BROKEN;
		return -EIO;
	}
	/* CTRL=0 must not masquerade as power loss on the next attach. */
	writel(AVE_COPROC_BOOT_ARG_VALUE, ave->coproc + ave->variant->regs->boot_arg);
	if (readl(ave->coproc + ave->variant->regs->boot_arg) != AVE_COPROC_BOOT_ARG_VALUE) {
		ave->fw_state = AVE_FW_BROKEN;
		return -EIO;
	}
	ave->fw_state = AVE_FW_RECOVERING;
	return 0;
}

/* The coprocessor lost power behind the firmware's back. Caller holds hw_lock. */
void ave_fw_lost(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	ave->fw_state = AVE_FW_OFF;
	memset(ave->ring, 0, sizeof(ave->ring));
	ave->ipc_fw_base = 0;
}

/* Explicit lifecycle-only validation, without allocating encoder surfaces. */
int ave_fw_lifecycle_test(struct ave_dev *ave, enum ave_codec codec)
{
	struct ave_ctx *ctx;
	struct ave_session *sess;
	int ret;

	if (!ave->layout || ave->layout->abi != &ave_abi_6070)
		return -EOPNOTSUPP;
	ctx = kzalloc_obj(*ctx);
	sess = kzalloc_obj(*sess);
	if (!ctx || !sess) {
		ret = -ENOMEM;
		goto free;
	}
	ctx->ave = ave;
	ctx->sess = sess;
	sess->p.codec = codec;
	ret = pm_runtime_resume_and_get(ave->dev);
	if (ret < 0)
		goto free;
	mutex_lock(&ave->hw_lock);
	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i]) {
			ret = -EBUSY;
			goto unlock;
		}
	ret = ave_fw_ensure_running(ave);
	if (!ret)
		ret = ave_cmd_open(ctx);
	if (!ret)
		ret = ave_cmd_close(ctx);
	if (!ret) {
		ave_fw_halt(ave);
		if (ave->fw_state != AVE_FW_HALTED)
			ret = -EIO;
	} else if (ave->fw_state == AVE_FW_RUNNING) {
		/* Unconfirmed client lifetime: do not Halt or free anything it may own. */
		ave->fw_state = AVE_FW_BROKEN;
	}
unlock:
	mutex_unlock(&ave->hw_lock);
	/* Runtime suspend refuses broken firmware, retaining its DMA and power. */
	if (ave->fw_state != AVE_FW_BROKEN) {
		int err = pm_runtime_put_sync_suspend(ave->dev);

		if (!ret && err < 0)
			ret = err;
	} else {
		dev_err(ave->dev,
			"lifecycle failed (%d); retaining runtime power reference\n",
			ret);
	}
	dev_info(ave->dev, "6070 lifecycle codec %u: %d\n", codec, ret);
free:
	kfree(sess);
	kfree(ctx);
	return ret;
}

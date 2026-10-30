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
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
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
 * The session images are per machine: "j700" from "apple,j700", the first
 * root compatible.
 */
static void ave_fw_board_name(struct ave_dev *ave)
{
	const char *compat = NULL, *comma;

	of_property_read_string_index(of_root, "compatible", 0, &compat);
	if (!compat)
		compat = "unknown";
	comma = strchr(compat, ',');
	strscpy(ave->board, comma ? comma + 1 : compat, sizeof(ave->board));
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
	ave->text_crc = text_crc;
	mutex_init(&ave->tmpl_lock);
	ave_fw_board_name(ave);

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
	/* The restored image must be in memory before the coprocessor starts. */
	wmb();
	memunmap(va);
	ave->data_dirty = false;
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
int ave_alloc_padded(struct ave_dev *ave, struct ave_buf *b, size_t size, size_t alloc,
		     unsigned long attrs)
{
	if (b->cpu)
		return 0;
	b->size = size;
	b->alloc = PAGE_ALIGN(max(size, alloc));
	b->attrs = attrs;
	b->cpu = dma_alloc_attrs(ave->dev, b->alloc, &b->dma, GFP_KERNEL, attrs);
	if (!b->cpu)
		return -ENOMEM;
	if (!IS_ALIGNED(b->dma, SZ_16K)) {
		ave_free(ave, b);
		return -EINVAL;
	}
	return 0;
}

int ave_alloc(struct ave_dev *ave, struct ave_buf *b, size_t size, unsigned long attrs)
{
	return ave_alloc_padded(ave, b, size, size, attrs);
}

/*
 * Start and Process are mostly constant for a machine and its firmware.
 * Those constant words come from image files named after the machine,
 * which the user extracts from their own macOS installation; the driver
 * fills in every session and frame field itself.
 */
static const char * const ave_codec_name[] = { "h264", "hevc" };
static const char * const ave_tmpl_name[AVE_TMPL_COUNT] = { "start", "process" };
static const size_t ave_tmpl_size[2][AVE_TMPL_COUNT] = {
	{ 0x3180, 0x63d8 },	/* H.264 */
	{ 0x13f28, 0xb1c0 },	/* HEVC */
};

#define AVE_TMPL_CODEC		0xc0	/* Start: codec word of the session image */

int ave_fw_templates(struct ave_dev *ave, enum ave_codec codec)
{
	unsigned int c = codec - 1;
	char name[64];
	int ret = 0;

	mutex_lock(&ave->tmpl_lock);
	for (unsigned int t = 0; t < AVE_TMPL_COUNT && !ret; t++) {
		const struct firmware *fw;

		if (ave->tmpl[c][t])
			continue;
		snprintf(name, sizeof(name), "apple/ave-%s-%s-%s.bin", ave->board,
			 ave_codec_name[c], ave_tmpl_name[t]);
		ret = request_firmware(&fw, name, ave->dev);
		if (ret) {
			dev_err(ave->dev, "cannot load %s (%d); extract it from macOS first\n",
				name, ret);
			break;
		}
		if (fw->size != ave_tmpl_size[c][t]) {
			dev_err(ave->dev, "%s is %zu bytes, expected %zu\n", name, fw->size,
				ave_tmpl_size[c][t]);
			release_firmware(fw);
			ret = -EINVAL;
			break;
		}
		if (t == AVE_TMPL_START && get_unaligned_le32(fw->data + AVE_TMPL_CODEC) != codec) {
			dev_err(ave->dev, "%s is not a %s session image\n", name,
				ave_codec_name[c]);
			release_firmware(fw);
			ret = -EINVAL;
			break;
		}
		ave->tmpl[c][t] = fw;
	}
	mutex_unlock(&ave->tmpl_lock);
	return ret;
}

void ave_fw_remove(struct ave_dev *ave)
{
	for (unsigned int c = 0; c < 2; c++)
		for (unsigned int t = 0; t < AVE_TMPL_COUNT; t++) {
			release_firmware(ave->tmpl[c][t]);
			ave->tmpl[c][t] = NULL;
		}
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

/*
 * After a Halt the firmware is restarted from a power cycle, as macOS does
 * for every session: the encoder must have been runtime suspended in
 * between, which puts the coprocessor back in reset. Hardware runs showed
 * that a restart while it kept power gets no answer, so that is refused
 * (-EAGAIN) and the state stays halted until the power really went. The
 * code base register must still be set: the driver never writes it.
 */
static int ave_fw_prepare_restart(struct ave_dev *ave)
{
	u32 status;

	if (readq(ave->coproc + AVE_COPROC_VCBAR) != (ave->text.iova | AVE_COPROC_VCBAR_VALID)) {
		dev_err(ave->dev, "coprocessor lost its code base (vcbar %#llx)\n",
			readq(ave->coproc + AVE_COPROC_VCBAR));
		return -EIO;
	}
	if (ave->fw_state != AVE_FW_HALTED)
		return 0;

	if (readl(ave->coproc + AVE_COPROC_CTRL)) {
		dev_warn(ave->dev, "coprocessor kept power after its halt; retry once it suspends\n");
		return -EAGAIN;
	}
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
	__le32 *cfg, *log;
	u32 status, hello[4];
	int ret;

	ret = ave_alloc(ave, &ave->ipc, AVE_IPC_BYTES, 0);
	if (!ret)
		ret = ave_alloc(ave, &ave->boot_cfg, SZ_16K, 0);
	if (!ret)
		ret = ave_alloc(ave, &ave->log, AVE_LOG_BYTES, 0);
	if (ret)
		return ret;

	if (ave_fw_check_mapping(ave, &ave->text) || ave_fw_check_mapping(ave, &ave->data)) {
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
	if (!ave_fw_idle(ave)) {
		dev_err(ave->dev, "coprocessor is not in its reset state (vcbar %#llx ctrl %#x)\n",
			readq(ave->coproc + AVE_COPROC_VCBAR),
			readl(ave->coproc + AVE_COPROC_CTRL));
		return -EIO;
	}

	memset(ave->ring, 0, sizeof(ave->ring));
	ave->unacked = 0;
	memset(ave->want, 0, sizeof(ave->want));
	ave->reply_err = 0;
	ave->scalar_next = 0;

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
	enum ave_fw_state prev;
	int ret;

	lockdep_assert_held(&ave->hw_lock);
	switch (ave->fw_state) {
	case AVE_FW_RUNNING:
		return 0;
	case AVE_FW_BROKEN:
		dev_err(ave->dev, "firmware stopped answering earlier; reboot to use the encoder\n");
		return -EIO;
	case AVE_FW_OFF:
	case AVE_FW_HALTED:
		break;
	}

	/*
	 * A failed boot has no session buffers in the firmware's hands. If the
	 * coprocessor was set going, treat it as halted: the next attempt
	 * then waits for a power cycle. Otherwise nothing changed.
	 */
	prev = ave->fw_state;
	ret = ave_fw_boot(ave);
	if (ret) {
		ave->fw_state = readl(ave->coproc + AVE_COPROC_CTRL) ? AVE_FW_HALTED : prev;
		memset(ave->ring, 0, sizeof(ave->ring));
		ave->ipc_fw_base = 0;
	}
	return ret;
}

/*
 * Tell the firmware to stop, once every session is closed. Halt gets no
 * ack and no answer; after a short wait the coprocessor is treated as
 * stopped and the next session restarts it through the boot path.
 * Caller holds hw_lock.
 */
void ave_fw_halt(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_RUNNING)
		return;
	if (ave_cmd_halt(ave)) {
		dev_err(ave->dev, "cannot send Halt; firmware treated as stuck\n");
		ave->fw_state = AVE_FW_BROKEN;
		return;
	}
	msleep(AVE_HALT_SETTLE_MS);
	ave->fw_state = AVE_FW_HALTED;
	memset(ave->ring, 0, sizeof(ave->ring));
	ave->ipc_fw_base = 0;
}

/*
 * Stop the firmware before its power goes away. Caller holds hw_lock.
 *
 * Sessions keep the device runtime active, so the firmware has normally
 * been halted already. A firmware that stopped answering keeps its power:
 * whether it still has transfers in flight cannot be checked, and only a
 * reboot brings it back.
 */
int ave_fw_shutdown(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	switch (ave->fw_state) {
	case AVE_FW_RUNNING:
		ave_fw_halt(ave);
		return ave->fw_state == AVE_FW_HALTED ? 0 : -EBUSY;
	case AVE_FW_BROKEN:
		return -EBUSY;
	default:
		return 0;
	}
}

/* The coprocessor lost power behind the firmware's back. Caller holds hw_lock. */
void ave_fw_lost(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	ave->fw_state = AVE_FW_OFF;
	memset(ave->ring, 0, sizeof(ave->ring));
	ave->ipc_fw_base = 0;
}

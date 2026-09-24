// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2023 Eileen Yoon <eyn@gmx.com> */

#include "isp-fw.h"

#include <asm/io.h>
#include <linux/delay.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>

#include "isp-cmd.h"
#include "isp-fw.h"
#include "isp-iommu.h"
#include "isp-ipc.h"
#include "isp-regs.h"
#include "isp-v4l2.h"

#define ISP_FIRMWARE_MDELAY    1
#define ISP_FIRMWARE_MAX_TRIES 1000

#define ISP_FIRMWARE_IPC_SIZE  0x1c000
#define ISP_FIRMWARE_DATA_SIZE 0x28000

#define ISP_COPROC_IN_WFI      0x3

static inline u32 isp_coproc_read32(struct apple_isp *isp, u32 reg)
{
	return readl(isp->coproc + reg);
}

static inline void isp_coproc_write32(struct apple_isp *isp, u32 reg, u32 val)
{
	writel(val, isp->coproc + reg);
}

static inline u32 isp_gpio_read32(struct apple_isp *isp, u32 reg)
{
	return readl(isp->gpio + reg);
}

static inline void isp_gpio_write32(struct apple_isp *isp, u32 reg, u32 val)
{
	writel(val, isp->gpio + reg);
}

static inline u32 isp_asc_control_reg(struct apple_isp *isp)
{
	return isp->hw->asc_control ?: ISP_COPROC_CONTROL;
}

static int apple_isp_power_up_domains(struct apple_isp *isp)
{
	int ret;

	if (isp->pds_active)
		return 0;

	for (int i = 1; i < isp->pd_count; i++) {
		ret = pm_runtime_resume_and_get(isp->pd_dev[i]);
		if (ret < 0) {
			dev_err(isp->dev,
				"Failed to power up power domain %d: %d\n", i, ret);
			while (--i >= 1)
				pm_runtime_put_sync(isp->pd_dev[i]);
			return ret;
		}
	}

	isp->pds_active = true;

	return 0;
}

static void apple_isp_power_down_domains(struct apple_isp *isp)
{
	int ret;

	if (!isp->pds_active)
		return;

	for (int i = isp->pd_count - 1; i >= 1; i--) {
		ret = pm_runtime_put_sync(isp->pd_dev[i]);
		if (ret < 0)
			dev_err(isp->dev,
				"Failed to power up power domain %d: %d\n", i, ret);
	}

	isp->pds_active = false;
}

void *apple_isp_translate(struct apple_isp *isp, struct isp_surf *surf,
			  dma_addr_t iova, size_t size)
{
	dma_addr_t end = iova + size;
	if (!surf) {
		dev_err(isp->dev,
			"Failed to translate IPC iova 0x%llx (0x%zx): No surface\n",
			(long long)iova, size);
		return NULL;
	}

	if (end < iova || iova < surf->iova ||
	    iova - surf->iova > surf->size ||
	    size > surf->size - (iova - surf->iova)) {
		dev_err(isp->dev,
			"Failed to translate IPC iova 0x%llx (0x%zx): Out of bounds\n",
			(long long)iova, size);
		return NULL;
	}

	if (!surf->virt) {
		dev_err(isp->dev,
			"Failed to translate IPC iova 0x%llx (0x%zx): No VMap\n",
			(long long)iova, size);
		return NULL;
	}

	return surf->virt + (iova - surf->iova);
}

struct isp_firmware_bootargs {
	u32 pad_0[2];
	u64 ipc_iova;
	u64 shared_base;
	u64 shared_size;
	u64 extra_iova;
	u64 extra_size;
	u32 platform_id;
	u32 pad_40;
	u64 logbuf_addr;
	u64 logbuf_size;
	u64 logbuf_entsize;
	u32 ipc_size;
	u32 pad_60[5];
	u32 unk5;
	u32 pad_7c[13];
	u32 pad_b0;
	u32 unk7;
	u32 pad_b8[5];
	u32 unk_iova1;
	u32 pad_c0[47];
	u32 unk9;
} __packed;
static_assert(sizeof(struct isp_firmware_bootargs) == 0x180);

/*
 * ISP17a / H16-style boot descriptor (t8140).  Measured on the J700 with
 * the macOS 26 (25G83) firmware: the queue-size word at +0x50 is 64-bit and
 * carries args_offset + 1, +0x68 is 0x40 and the 0x200-byte connection
 * descriptor at +0x70 only needs "no optical-dev-card-id" (+0xb0 = 1).
 */
#define ISP_H16_BOOTARGS_SIZE    0x290
#define ISP_H16_DESCRIPTOR_SIZE  0x200
#define ISP_H16_SHARED_SIZE_T8140 0xee000000ULL

struct isp_firmware_bootargs_h16 {
	u32 pad_0[2];
	u64 ipc_iova;
	u64 shared_base;
	u64 shared_size;
	u64 extra_iova;
	u64 extra_size;
	u32 platform_id;
	u32 pad_34;
	u64 logbuf_addr;
	u64 logbuf_size;
	u64 logbuf_entsize;
	u64 ipc_queue_size;
	u32 pad_58[4];
	u32 unk_68;
	u32 pad_6c;
	u8 descriptor[ISP_H16_DESCRIPTOR_SIZE];
	u32 pad_270[8];
} __packed;
static_assert(sizeof(struct isp_firmware_bootargs_h16) == ISP_H16_BOOTARGS_SIZE);

#define ISP_H16_DESC_NO_OPTICAL_CARD_ID 0xb0

struct isp_chan_desc {
	char name[64];
	u32 type;
	u32 src;
	u32 num;
	u32 pad;
	u64 iova;
	u32 padding[0x2a];
} __packed;
static_assert(sizeof(struct isp_chan_desc) == 0x100);

static const struct isp_chan_ops tm_ops = {
	.handle = ipc_tm_handle,
};

static const struct isp_chan_ops sm_ops = {
	.handle = ipc_sm_handle,
};

static const struct isp_chan_ops bt_ops = {
	.handle = ipc_bt_handle,
};

static irqreturn_t apple_isp_isr(int irq, void *dev)
{
	struct apple_isp *isp = dev;

	isp_mbox2_write32(isp, ISP_MBOX2_IRQ_ACK,
			 isp_mbox_read32(isp, ISP_MBOX_IRQ_INTERRUPT));

	return IRQ_WAKE_THREAD;
}

static irqreturn_t apple_isp_isr_thread(int irq, void *dev)
{
	struct apple_isp *isp = dev;

	wake_up_all(&isp->wait);

	ipc_chan_handle(isp, isp->chan_sm);
	wake_up_all(&isp->wait); /* Some commands depend on sm */

	ipc_chan_handle(isp, isp->chan_tm);

	ipc_chan_handle(isp, isp->chan_bt);
	wake_up_all(&isp->wait);

	return IRQ_HANDLED;
}

static void isp_disable_irq(struct apple_isp *isp)
{
	isp_mbox_write32(isp, isp->hw->mbox_irq_enable, 0x0);
	if (isp->hw->gen == ISP_GEN_T8140) {
		isp_mbox_write32(isp, ISP_MBOX_IRQ_ENABLE1_T8140, 0x0);
		isp_mbox_write32(isp, ISP_MBOX_IRQ_ENABLE2_T8140, 0x0);
	}
	free_irq(isp->irq, isp);
	isp_gpio_write32(isp, ISP_GPIO_1, 0xfeedbabe); /* real funny */
}

static int isp_enable_irq(struct apple_isp *isp)
{
	int err;

	err = request_threaded_irq(isp->irq, apple_isp_isr,
				   apple_isp_isr_thread, 0, "apple-isp", isp);
	if (err < 0) {
		isp_err(isp, "failed to request IRQ#%u (%d)\n", isp->irq, err);
		return err;
	}
	isp_dbg(isp, "about to enable interrupts...\n");

	isp_mbox_write32(isp, isp->hw->mbox_irq_enable, 0xf);
	if (isp->hw->gen == ISP_GEN_T8140) {
		/* ISP17a: two more enable words and an IRQ steering write */
		isp_mbox_write32(isp, ISP_MBOX_IRQ_ENABLE1_T8140,
				 ISP_MBOX_IRQ_ENABLE1_T8140_VAL);
		isp_mbox_write32(isp, ISP_MBOX_IRQ_ENABLE2_T8140,
				 ISP_MBOX_IRQ_ENABLE2_T8140_VAL);
		isp_mbox_write32(isp, ISP_MBOX_IRQ_STEER_T8140,
				 ISP_MBOX_IRQ_STEER_T8140_VAL);
	}

	return 0;
}

/*
 * T8140 compatibility sequence retained from the reviewed host implementation.
 * Only the reset request bit is used as a readiness condition. The meaning
 * of the following fabric/IRQ writes, repeated readbacks and delays remains
 * unresolved; preserve them until hardware evidence justifies a change.
 * The ASC status is not polled for WFI on this generation.
 */
static int isp_reset_coproc_t8140(struct apple_isp *isp)
{
	int retries;

	isp_coproc_write32(isp, ISP_COPROC_EDPRCR, 0x2);
	for (retries = 0; retries < 100; retries++) {
		if (!(isp_coproc_read32(isp, ISP_COPROC_EDPRCR) & 0x2))
			break;
		mdelay(1);
	}
	if (retries == 100) {
		isp_err(isp, "coprocessor reset request did not clear\n");
		return -ETIMEDOUT;
	}
	msleep(50);

	isp_coproc_write32(isp, ISP_COPROC_FABRIC_0_T8140, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_FABRIC_1_T8140, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_FABRIC_2_T8140, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_FABRIC_3_T8140, 0xffffffff);

	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_0_T8140, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_1_T8140, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_2_T8140, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_3_T8140, 0xffffffff);

	for (retries = 0; retries < 8; retries++) {
		isp_coproc_read32(isp, ISP_COPROC_RESET_ACK_0_T8140);
		isp_coproc_read32(isp, ISP_COPROC_RESET_ACK_1_T8140);
	}
	msleep(50);

	return 0;
}

static int isp_reset_coproc(struct apple_isp *isp)
{
	int retries;
	u32 status;
	u32 val;

	if (isp->hw->gen == ISP_GEN_T8140)
		return isp_reset_coproc_t8140(isp);

	isp_coproc_write32(isp, ISP_COPROC_EDPRCR, 0x2);

	isp_coproc_write32(isp, ISP_COPROC_FABRIC_0, 0xff00ff);
	isp_coproc_write32(isp, ISP_COPROC_FABRIC_1, 0xff00ff);
	isp_coproc_write32(isp, ISP_COPROC_FABRIC_2, 0xff00ff);
	isp_coproc_write32(isp, ISP_COPROC_FABRIC_3, 0xff00ff);

	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_0, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_1, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_2, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_3, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_4, 0xffffffff);
	isp_coproc_write32(isp, ISP_COPROC_IRQ_MASK_5, 0xffffffff);

	for (retries = 0; retries < 128; retries++) {
		val = isp_coproc_read32(isp, 0x818);
		if (val == 0)
			break;
	}

	for (retries = 0; retries < 128; retries++) {
		val = isp_coproc_read32(isp, 0x81c);
		if (val == 0)
			break;
	}

	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		status = isp_coproc_read32(isp, ISP_COPROC_STATUS);
		if (status & ISP_COPROC_IN_WFI) {
			isp_dbg(isp, "%d: coproc in WFI (status: 0x%x)\n",
				retries, status);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp, "coproc NOT in WFI (status: 0x%x)\n", status);
		return -ENODEV;
	}

	return 0;
}

static void isp_firmware_shutdown_stage1(struct apple_isp *isp)
{
	isp_coproc_write32(isp, isp_asc_control_reg(isp), 0x0);

	apple_isp_power_down_domains(isp);
}

static int isp_firmware_boot_stage1(struct apple_isp *isp)
{
	int err, retries;
	// u32 val;

	err = apple_isp_power_up_domains(isp);
	if (err < 0)
		return err;


	/* ISP17a has no clock-enable GPIO word; the native host never wrote it */
	if (isp->hw->gen != ISP_GEN_T8140)
		isp_gpio_write32(isp, ISP_GPIO_CLOCK_EN, 0x1);

#if 0
	/* This doesn't work well with system sleep */
	val = isp_gpio_read32(isp, ISP_GPIO_1);
	if (val == 0xfeedbabe) {
		err = isp_reset_coproc(isp);
		if (err < 0)
			return err;
	}
#endif

	err = isp_reset_coproc(isp);
	if (err < 0)
		goto shutdown;

	isp_gpio_write32(isp, ISP_GPIO_0, 0x0);
	isp_gpio_write32(isp, ISP_GPIO_1, 0x0);
	isp_gpio_write32(isp, ISP_GPIO_2, 0x0);
	isp_gpio_write32(isp, ISP_GPIO_3, 0x0);
	isp_gpio_write32(isp, ISP_GPIO_4, 0x0);
	isp_gpio_write32(isp, ISP_GPIO_5, 0x0);
	/* ISP17a: GPIO6 is the boot-mode field, ISP_StartFirmware mode 0 = 1 */
	isp_gpio_write32(isp, ISP_GPIO_6,
			 isp->hw->gen == ISP_GEN_T8140 ? 0x1 : 0x0);
	isp_gpio_write32(isp, ISP_GPIO_7, 0x0);

	isp_mbox_write32(isp, isp->hw->mbox_irq_enable, 0x0);

	isp_coproc_write32(isp, isp_asc_control_reg(isp), 0x0);
	isp_coproc_write32(isp, isp_asc_control_reg(isp), 0x10);

	/* Wait for ISP_GPIO_7 to 0x0 -> 0x8042006 */
	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		u32 val = isp_gpio_read32(isp, ISP_GPIO_7);
		if (val == 0x8042006) {
			isp_dbg(isp,
				"got first magic number (0x%x) from firmware\n",
				val);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp,
			"never received first magic number from firmware\n");
		err = -ENODEV;
		goto shutdown;
	}

	return 0;

shutdown:
	isp_firmware_shutdown_stage1(isp);
	return err;
}

int apple_isp_alloc_firmware_surface(struct apple_isp *isp)
{
	/* These are static, so let's do it once and for all */
	isp->ipc_surf = isp_alloc_surface_vmap(isp, ISP_FIRMWARE_IPC_SIZE);
	if (!isp->ipc_surf) {
		isp_err(isp, "failed to alloc shared surface for ipc\n");
		return -ENOMEM;
	}
	dev_dbg(isp->dev, "IPC surface iova: 0x%llx\n",
		 (long long)isp->ipc_surf->iova);

	isp->data_surf = isp_alloc_surface_vmap(isp, ISP_FIRMWARE_DATA_SIZE);
	if (!isp->data_surf) {
		isp_err(isp, "failed to alloc shared surface for data files\n");
		isp_free_surface(isp, isp->ipc_surf);
		return -ENOMEM;
	}
	dev_dbg(isp->dev, "Data surface iova: 0x%llx\n",
		 (long long)isp->data_surf->iova);

	return 0;
}

void apple_isp_free_firmware_surface(struct apple_isp *isp)
{
	isp_free_surface(isp, isp->data_surf);
	isp_free_surface(isp, isp->ipc_surf);
}

static void isp_firmware_shutdown_stage2(struct apple_isp *isp)
{
	isp_free_surface(isp, isp->extra_surf);
}

static int isp_firmware_boot_stage2(struct apple_isp *isp)
{
	struct isp_firmware_bootargs args;
	struct isp_firmware_bootargs_h16 args_h16;
	size_t args_size;
	size_t command_size;
	dma_addr_t args_iova, command_iova;
	void *args_virt, *command_virt;
	int err, retries;

	u32 num_ipc_chans = isp_gpio_read32(isp, ISP_GPIO_0);
	u64 args_offset = isp_gpio_read32(isp, ISP_GPIO_1);
	u32 desc_flags = isp_gpio_read32(isp, ISP_GPIO_2);
	u32 extra_size = isp_gpio_read32(isp, ISP_GPIO_3);
	if (!num_ipc_chans || num_ipc_chans > INT_MAX) {
		dev_err(isp->dev, "Invalid IPC channel count: %u\n", num_ipc_chans);
		return -ENODEV;
	}
	isp->num_ipc_chans = num_ipc_chans;

	if (isp->num_ipc_chans != 7)
		dev_warn(isp->dev, "unexpected channel count (%d)\n",
			 num_ipc_chans);

	args_size = isp->hw->gen == ISP_GEN_T8140 ? sizeof(args_h16) : sizeof(args);
	/*
	 * The firmware supplies an offset, not a validated host pointer. Check
	 * both the boot descriptor and the command area before allocating or
	 * publishing either address. The latter must fit the largest buffer
	 * batch: a 16-byte header and up to 40 64-byte descriptors.
	 */
	command_size = 2 * sizeof(u64) + 0x40 *
		(ARRAY_SIZE(isp->meta_surfs) + ARRAY_SIZE(isp->capmeta_surfs) +
		 ISP_MAX_BUFFERS);
	if (!isp->ipc_surf ||
	    check_add_overflow(isp->ipc_surf->iova,
			       (dma_addr_t)(args_offset + 0x40), &args_iova) ||
	    check_add_overflow(args_iova, (dma_addr_t)(args_size + 0x40),
			       &command_iova))
		return -EIO;
	args_virt = apple_isp_ipc_translate(isp, args_iova, args_size);
	command_virt = apple_isp_ipc_translate(isp, command_iova, command_size);
	if (!args_virt || !command_virt)
		return -EIO;

	isp->extra_surf = isp_alloc_surface_vmap(isp, extra_size);
	if (!isp->extra_surf) {
		isp_err(isp, "failed to alloc surface for extra heap\n");
		return -ENOMEM;
	}

	isp->cmd_iova = command_iova;
	isp->cmd_virt = command_virt;

	if (isp->hw->gen == ISP_GEN_T8140) {
		if (!(desc_flags & 0x2))
			dev_warn(isp->dev,
				 "firmware did not request the H16 descriptor (flags 0x%x)\n",
				 desc_flags);
		memset(&args_h16, 0, sizeof(args_h16));
		args_h16.ipc_iova = isp->ipc_surf->iova;
		args_h16.shared_base = isp->fw.heap_top & 0xffffffff;
		args_h16.shared_size = ISP_H16_SHARED_SIZE_T8140;
		args_h16.extra_iova = isp->extra_surf->iova;
		args_h16.extra_size = isp->extra_surf->size;
		args_h16.platform_id = isp->platform_id;
		args_h16.ipc_queue_size = args_offset + 1;
		args_h16.unk_68 = 0x40;
		args_h16.descriptor[ISP_H16_DESC_NO_OPTICAL_CARD_ID] = 1;
		memcpy(args_virt, &args_h16, sizeof(args_h16));
	} else {
		memset(&args, 0, sizeof(args));
		args.ipc_iova = isp->ipc_surf->iova;
		args.ipc_size = isp->ipc_surf->size;
		args.shared_base = isp->fw.heap_top & 0xffffffff;
		args.shared_size = 0x10000000UL - args.shared_base;
		args.extra_iova = isp->extra_surf->iova;
		args.extra_size = isp->extra_surf->size;
		args.platform_id = isp->platform_id;
		args.unk5 = 0x40;
		args.unk7 = 0x1; // 0?
		args.unk_iova1 = args_iova + sizeof(args) - 0xc;
		args.unk9 = 0x3;
		memcpy(args_virt, &args, sizeof(args));
	}

	isp_gpio_write32(isp, ISP_GPIO_0, args_iova);
	/* TODO: handle this via Kconfig depends? hardware is only present on
	 *       64-bit SoCs.
	 */
	if (IS_ENABLED(CONFIG_ARCH_DMA_ADDR_T_64BIT))
		isp_gpio_write32(isp, ISP_GPIO_1, args_iova >> 32);
	dma_wmb();

	/* Wait for ISP_GPIO_7 to 0xf7fbdff9 -> 0x8042006 */
	isp_gpio_write32(isp, ISP_GPIO_7, 0xf7fbdff9);

	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		u32 val = isp_gpio_read32(isp, ISP_GPIO_7);
		if (val == 0x8042006) {
			isp_dbg(isp,
				"got second magic number (0x%x) from firmware\n",
				val);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp,
			"never received second magic number from firmware\n");
		err = -ENODEV;
		goto free_extra;
	}

	return 0;

free_extra:
	isp_free_surface(isp, isp->extra_surf);
	return err;
}

static inline struct isp_channel *isp_get_chan_index(struct apple_isp *isp,
						     const char *name)
{
	for (int i = 0; i < isp->num_ipc_chans; i++) {
		if (!strcasecmp(isp->ipc_chans[i]->name, name))
			return isp->ipc_chans[i];
	}
	return NULL;
}

static void isp_free_channel_info(struct apple_isp *isp)
{
	for (int i = 0; i < isp->num_ipc_chans; i++) {
		struct isp_channel *chan = isp->ipc_chans[i];
		if (!chan)
			continue;
		kfree(chan->name);
		kfree(chan);
		isp->ipc_chans[i] = NULL;
	}
	kfree(isp->ipc_chans);
	isp->ipc_chans = NULL;
}

static int isp_fill_channel_info(struct apple_isp *isp)
{
	u64 table_iova = isp_gpio_read32(isp, ISP_GPIO_0) |
			 ((u64)isp_gpio_read32(isp, ISP_GPIO_1)) << 32;
	void *table_virt = apple_isp_ipc_translate(
		isp, isp_fw_iova(isp, table_iova),
		sizeof(struct isp_chan_desc) * isp->num_ipc_chans);

	if (!table_virt) {
		dev_err(isp->dev, "Failed to find channel table\n");
		return -EIO;
	}

	isp->ipc_chans = kcalloc(isp->num_ipc_chans,
				 sizeof(struct isp_channel *), GFP_KERNEL);
	if (!isp->ipc_chans)
		goto out;

	for (int i = 0; i < isp->num_ipc_chans; i++) {
		struct isp_chan_desc desc;
		void *desc_virt = table_virt + (i * sizeof(desc));
		struct isp_channel *chan =
			kzalloc(sizeof(struct isp_channel), GFP_KERNEL);
		if (!chan)
			goto out;
		isp->ipc_chans[i] = chan;

		memcpy(&desc, desc_virt, sizeof(desc));
		chan->name = kstrdup(desc.name, GFP_KERNEL);
		chan->type = desc.type;
		chan->src = desc.src;
		chan->doorbell = 1 << chan->src;
		chan->num = desc.num;
		chan->size = desc.num * ISP_IPC_MESSAGE_SIZE;
		chan->iova = isp_fw_iova(isp, desc.iova);
		chan->virt =
			apple_isp_ipc_translate(isp, chan->iova, chan->size);
		chan->cursor = 0;
		mutex_init(&chan->lock);

		if (!chan->virt) {
			dev_err(isp->dev, "Failed to find channel buffer\n");
			goto out;
		}

		if ((chan->type != ISP_IPC_CHAN_TYPE_COMMAND) &&
		    (chan->type != ISP_IPC_CHAN_TYPE_REPLY) &&
		    (chan->type != ISP_IPC_CHAN_TYPE_REPORT)) {
			isp_err(isp, "invalid ipc chan type (%d)\n",
				chan->type);
			goto out;
		}

		isp_dbg(isp, "chan: %s type: %d src: %d num: %d iova: %pad\n",
			chan->name, chan->type, chan->src, chan->num,
			&chan->iova);
	}

	isp->chan_tm = isp_get_chan_index(isp, "TERMINAL");
	isp->chan_io = isp_get_chan_index(isp, "IO");
	isp->chan_dg = isp_get_chan_index(isp, "DEBUG");
	isp->chan_bh = isp_get_chan_index(isp, "BUF_H2T");
	isp->chan_bt = isp_get_chan_index(isp, "BUF_T2H");
	isp->chan_sm = isp_get_chan_index(isp, "SHAREDMALLOC");
	isp->chan_it = isp_get_chan_index(isp, "IO_T2H");

	if (!isp->chan_tm || !isp->chan_io || !isp->chan_dg || !isp->chan_bh ||
	    !isp->chan_bt || !isp->chan_sm || !isp->chan_it) {
		isp_err(isp, "did not find all of the required ipc chans\n");
		goto out;
	}

	isp->chan_tm->ops = &tm_ops;
	isp->chan_sm->ops = &sm_ops;
	isp->chan_bt->ops = &bt_ops;

	return 0;
out:
	isp_free_channel_info(isp);
	return -ENOMEM;
}

static void isp_firmware_shutdown_stage3(struct apple_isp *isp)
{
	isp_free_channel_info(isp);
}

static int isp_firmware_boot_stage3(struct apple_isp *isp)
{
	int err, retries;

	err = isp_fill_channel_info(isp);
	if (err < 0)
		return err;

	/* Mask the command channels to prepare for submission */
	for (int i = 0; i < isp->num_ipc_chans; i++) {
		struct isp_channel *chan = isp->ipc_chans[i];
		if (chan->type != ISP_IPC_CHAN_TYPE_COMMAND)
			continue;
		for (int j = 0; j < chan->num; j++) {
			struct isp_message msg;
			void *msg_virt = chan->virt + (j * sizeof(msg));

			memset(&msg, 0, sizeof(msg));
			msg.arg0 = ISP_IPC_FLAG_ACK;
			memcpy(msg_virt, &msg, sizeof(msg));
		}
	}
	dma_wmb();

	/* Wait for ISP_GPIO_3 to 0x8042006 -> 0x0 */
	isp_gpio_write32(isp, ISP_GPIO_3, 0x8042006);

	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		u32 val = isp_gpio_read32(isp, ISP_GPIO_3);
		if (val == 0x0) {
			isp_dbg(isp,
				"got third magic number (0x%x) from firmware\n",
				val);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp,
			"never received third magic number from firmware\n");
		isp_free_channel_info(isp);
		return -ENODEV;
	}

	isp_dbg(isp, "firmware booted!\n");

	return 0;
}

static int isp_stop_command_processor(struct apple_isp *isp)
{
	int retries;

#if 0
	int res = isp_cmd_stop(isp, 0);
	if (res) {
		isp_err(isp, "isp_cmd_stop() failed\n");
		return res;
	}

	/* Wait for ISP_GPIO_0 to 0xf7fbdff9 -> 0x8042006 */
	isp_gpio_write32(isp, ISP_GPIO_0, 0xf7fbdff9);

	isp_cmd_power_down(isp);
#else
	isp_gpio_write32(isp, ISP_GPIO_0, 0xf7fbdff9);

	int res = isp_cmd_suspend(isp);
	if (res) {
		isp_err(isp, "isp_cmd_suspend() failed\n");
		return res;
	}
#endif

	for (retries = 0; retries < ISP_FIRMWARE_MAX_TRIES; retries++) {
		u32 val = isp_gpio_read32(isp, ISP_GPIO_0);
		if (val == 0x8042006) {
			isp_dbg(isp, "got magic number (0x%x) from firmware\n",
				val);
			break;
		}
		mdelay(ISP_FIRMWARE_MDELAY);
	}
	if (retries >= ISP_FIRMWARE_MAX_TRIES) {
		isp_err(isp, "never received magic number from firmware\n");
		return -ENODEV;
	}

	return 0;
}

static int isp_start_command_processor(struct apple_isp *isp)
{
	int err;

	err = isp_cmd_print_enable(isp, 1);
	if (err)
		return err;

	if (isp->hw->gen == ISP_GEN_T8140) {
		/*
		 * H17 init (matched from the macOS 25G83 J700 log): no PMU base,
		 * one DSID broadcast-clear window in the multi-BC form, then the
		 * PMP control set.
		 */
		err = isp_cmd_set_dsid_clr_multi_bc_reg_base(
			isp, isp->hw->dsid_clr_base0, isp->hw->dsid_clr_range0);
		if (err)
			return err;
		goto pmp;
	}

	err = isp_cmd_set_isp_pmu_base(isp, isp->hw->pmu_base);
	if (err)
		return err;

	if (isp->hw->dsid_count == 1) {
		err = isp_cmd_set_dsid_clr_req_base(
			isp, isp->hw->dsid_clr_base0, isp->hw->dsid_clr_range0);
		if (err)
			return err;
	} else {
		err = isp_cmd_set_dsid_clr_req_base2(
			isp, isp->hw->dsid_clr_base0, isp->hw->dsid_clr_base1,
			isp->hw->dsid_clr_base2, isp->hw->dsid_clr_base3,
			isp->hw->dsid_clr_range0, isp->hw->dsid_clr_range1,
			isp->hw->dsid_clr_range2, isp->hw->dsid_clr_range3);
		if (err)
			return err;
	}

pmp:
	if (isp->hw->clock_scratch) {
		err = isp_cmd_pmp_ctrl_set(
			isp, isp->hw->clock_scratch, isp->hw->clock_base,
			isp->hw->clock_bit, isp->hw->clock_size,
			isp->hw->bandwidth_scratch, isp->hw->bandwidth_base,
			isp->hw->bandwidth_bit, isp->hw->bandwidth_size);
		if (err)
			return err;
	}

	err = isp_cmd_start(isp, 0);
	if (err)
		return err;

	/* Now we can access CISP_CMD_CH_* commands */

	return 0;
}

static void isp_collect_gc_surface(struct apple_isp *isp)
{
	struct isp_surf *tmp, *surf;

	isp->log_surf = NULL;
	isp->bt_surf = NULL;

	list_for_each_entry_safe_reverse(surf, tmp, &isp->gc, head) {
		isp_dbg(isp, "freeing iova: %pad size: 0x%llx virt: %pS\n",
			&surf->iova, surf->size, (void *)surf->virt);
		isp_free_surface(isp, surf);
	}
}

static int isp_firmware_boot(struct apple_isp *isp)
{
	int err;

	err = isp_firmware_boot_stage1(isp);
	if (err < 0) {
		isp_err(isp, "failed firmware boot stage 1: %d\n", err);
		goto garbage_collect;
	}

	err = isp_firmware_boot_stage2(isp);
	if (err < 0) {
		isp_err(isp, "failed firmware boot stage 2: %d\n", err);
		goto shutdown_stage1;
	}

	err = isp_firmware_boot_stage3(isp);
	if (err < 0) {
		isp_err(isp, "failed firmware boot stage 3: %d\n", err);
		goto shutdown_stage2;
	}

	err = isp_enable_irq(isp);
	if (err < 0) {
		isp_err(isp, "failed to enable interrupts: %d\n", err);
		goto shutdown_stage3;
	}

	err = isp_start_command_processor(isp);
	if (err < 0) {
		isp_err(isp, "failed to start command processor: %d\n", err);
		goto disable_irqs;
	}

	flush_workqueue(isp->wq);

	return 0;

disable_irqs:
	isp_disable_irq(isp);
shutdown_stage3:
	isp_firmware_shutdown_stage3(isp);
shutdown_stage2:
	isp_firmware_shutdown_stage2(isp);
shutdown_stage1:
	isp_firmware_shutdown_stage1(isp);
garbage_collect:
	isp_collect_gc_surface(isp);
	return err;
}

static void isp_firmware_shutdown(struct apple_isp *isp)
{
	flush_workqueue(isp->wq);
	isp_stop_command_processor(isp);
	isp_disable_irq(isp);
	isp_firmware_shutdown_stage3(isp);
	isp_firmware_shutdown_stage2(isp);
	isp_firmware_shutdown_stage1(isp);
	isp_collect_gc_surface(isp);
}

/*
 * t8140: the EIC exclave's ISP watchdog window.  On macOS the
 * ExclaveIndicatorController kicks it at 30 Hz while the camera indicator is
 * healthy; without the kicks the H17 firmware keeps running but every frame
 * is the diagnostic fill (LINUX-ISP-HOT-FRAMES-REQUIREMENTS-2026-09-16).
 */
static void apple_isp_wdt_kick(struct apple_isp *isp)
{
	/* exact EIC sequence: clear, reload value, kick, dsb sy */
	writel(0, isp->wdt + ISP_WDT_CLEAR);
	writel(ISP_WDT_RELOAD_VAL, isp->wdt + ISP_WDT_RELOAD);
	writel(1, isp->wdt + ISP_WDT_KICK);
	dsb(sy);
}

static enum hrtimer_restart apple_isp_wdt_pet(struct hrtimer *timer)
{
	struct apple_isp *isp = container_of(timer, struct apple_isp, wdt_timer);

	apple_isp_wdt_kick(isp);
	hrtimer_forward_now(timer, ns_to_ktime(ISP_WDT_PERIOD_NS));
	return HRTIMER_RESTART;
}

void apple_isp_wdt_init(struct apple_isp *isp)
{
	hrtimer_setup(&isp->wdt_timer, apple_isp_wdt_pet, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_HARD);
}

void apple_isp_wdt_start(struct apple_isp *isp)
{
	if (!isp->wdt || isp->wdt_running)
		return;

	isp->wdt_running = true;
	/* first kick right away, then every 33 ms for the life of the stream */
	apple_isp_wdt_kick(isp);
	hrtimer_start(&isp->wdt_timer, ns_to_ktime(ISP_WDT_PERIOD_NS),
		      HRTIMER_MODE_REL_HARD);
}

void apple_isp_wdt_stop(struct apple_isp *isp)
{
	if (!isp->wdt_running)
		return;

	hrtimer_cancel(&isp->wdt_timer);
	isp->wdt_running = false;
}

int apple_isp_firmware_boot(struct apple_isp *isp)
{
	int err;

	if (isp->fw_persistent && isp->fw_booted)
		return 0;

	/* Needs to be power cycled for IOMMU to behave correctly */
	err = pm_runtime_resume_and_get(isp->dev);
	if (err < 0) {
		dev_err(isp->dev, "failed to enable power: %d\n", err);
		return err;
	}

	err = isp_firmware_boot(isp);
	if (err) {
		dev_err(isp->dev, "failed to boot firmware: %d\n", err);
		pm_runtime_put_sync(isp->dev);
		return err;
	}

	isp->fw_booted = true;

	return 0;
}

/* The end of a stream: resident firmware (t8140) keeps running. */
void apple_isp_firmware_shutdown(struct apple_isp *isp)
{
	if (isp->fw_persistent)
		return;

	apple_isp_firmware_halt(isp);
}

/*
 * The driver going away (or failing to probe): the firmware is stopped and
 * its power domains released whether or not it is kept resident between
 * streams. Current T8140 evidence requires a cold boot after full power
 * gating; warm re-probe and system-resume recovery remain unvalidated.
 */
void apple_isp_firmware_halt(struct apple_isp *isp)
{
	apple_isp_wdt_stop(isp);

	if (!isp->fw_booted)
		return;

	isp_firmware_shutdown(isp);
	isp->fw_booted = false;
	pm_runtime_put_sync(isp->dev);
}

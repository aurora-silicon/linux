// SPDX-License-Identifier: GPL-2.0
/*
 * Apple SoC SPMI device driver
 *
 * Copyright The Asahi Linux Contributors
 *
 * Inspired by:
 *		OpenBSD support Copyright (c) 2021 Mark Kettenis <kettenis@openbsd.org>
 *		Correllium support Copyright (C) 2021 Corellium LLC
 *		hisi-spmi-controller.c
 *		spmi-pmic-arb.c Copyright (c) 2021, The Linux Foundation.
 */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/spinlock.h>
#include <linux/spmi.h>
#include <linux/suspend.h>

/* SPMI Controller Registers */
#define SPMI_STATUS_REG 0
#define SPMI_CMD_REG 0x4
#define SPMI_RSP_REG 0x8
#define SPMI_ACT_REG 0xa4

#define SPMI_IRQ_MASK_BASE 0x20
#define SPMI_IRQ_ACK_BASE 0x60
/*
 * Peripheral interrupts come first. The RX FIFO notification sits right
 * after them, followed by the controller's own fault interrupts, so the
 * notification index is the number of peripheral interrupts.
 */
#define SPMI_MAX_PERIPHERAL_IRQS 256

/* SPMI_RSP_REG reply word */
#define SPMI_REPLY_FRAME_PARITY_STATUS GENMASK(31, 16)
#define SPMI_REPLY_ACK BIT(15)
#define SPMI_REPLY_SLAVE_ID GENMASK(14, 8)
#define SPMI_REPLY_CMD GENMASK(7, 0)

#define SPMI_ACT_FIFO_FLUSH BIT(0)
#define SPMI_RX_FIFO_EMPTY BIT(24)

/*
 * Generation 4 queues fault records in its second register window, a FIFO laid out as the
 * reply FIFO. macOS acks the controller's fault interrupt (source 263, after the 256
 * peripheral sources; ack words from 0x600) and empties the queue when the controller starts.
 */
#define SPMI_GEN4_IRQ_ACK_BASE 0x600
#define SPMI_GEN4_FAULT_IRQ (SPMI_MAX_PERIPHERAL_IRQS + 7)
#define SPMI_FAULT_QUEUE_MAX 64

#define REG_POLL_INTERVAL_US 10000
#define REG_POLL_TIMEOUT_US (REG_POLL_INTERVAL_US * 5)

struct apple_spmi_hw {
	u32 status;
	u32 command;
	u32 reply;
	u32 rx_empty;
	bool legacy_irqs;
	u32 num_irqs;
};

static const struct apple_spmi_hw apple_spmi_gen1 = {
	.status = SPMI_STATUS_REG,
	.command = SPMI_CMD_REG,
	.reply = SPMI_RSP_REG,
	.rx_empty = SPMI_RX_FIFO_EMPTY,
	.legacy_irqs = true,
	.num_irqs = 256,
};

/* M1 Pro, Max and Ultra: 192 peripheral interrupts, notification at 192 */
static const struct apple_spmi_hw apple_spmi_t6000 = {
	.status = SPMI_STATUS_REG,
	.command = SPMI_CMD_REG,
	.reply = SPMI_RSP_REG,
	.rx_empty = SPMI_RX_FIFO_EMPTY,
	.legacy_irqs = true,
	.num_irqs = 192,
};

static const struct apple_spmi_hw apple_spmi_gen4 = {
	.status = 0x200,
	.command = 0x210,
	.reply = 0x220,
	.rx_empty = BIT(30),
};

struct apple_spmi {
	void __iomem *regs;
	void __iomem *faults;
	unsigned int nfaults;
	const struct apple_spmi_hw *hw;
	struct mutex fifo_lock;
	struct completion fifo_rx;
	struct irq_domain *irqd;
	raw_spinlock_t irq_mask_lock;
	DECLARE_BITMAP(irq_mask_cache, SPMI_MAX_PERIPHERAL_IRQS);
	/* The sources a driver uses, enabled or not. */
	DECLARE_BITMAP(irq_used, SPMI_MAX_PERIPHERAL_IRQS);
	int irq;
	bool notify_irq;
	bool notify_parked;
	bool prev_fail;
	/* The noirq restore rewrote the masks after a resume from disk. */
	bool hib_resynced;
};

static void apple_spmi_irq_ack_raw(struct apple_spmi *spmi, u32 irq)
{
	u32 __iomem *reg = spmi->regs + SPMI_IRQ_ACK_BASE + (irq / 32) * 4;

	writel(BIT(irq % 32), reg);
}

static void apple_spmi_irq_mask_raw(struct apple_spmi *spmi, u32 irq)
{
	u32 __iomem *reg = spmi->regs + SPMI_IRQ_MASK_BASE + (irq / 32) * 4;

	writel(readl(reg) & ~BIT(irq % 32), reg);
}

static void apple_spmi_irq_unmask_raw(struct apple_spmi *spmi, u32 irq)
{
	u32 __iomem *reg = spmi->regs + SPMI_IRQ_MASK_BASE + (irq / 32) * 4;

	writel(readl(reg) | BIT(irq % 32), reg);
}

static void apple_spmi_irq_ack(struct irq_data *d)
{
	struct apple_spmi *spmi = irq_data_get_irq_chip_data(d);

	apple_spmi_irq_ack_raw(spmi, d->hwirq);
}

static void apple_spmi_irq_mask(struct irq_data *d)
{
	struct apple_spmi *spmi = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&spmi->irq_mask_lock, flags);
	apple_spmi_irq_mask_raw(spmi, d->hwirq);
	clear_bit(d->hwirq, spmi->irq_mask_cache);
	raw_spin_unlock_irqrestore(&spmi->irq_mask_lock, flags);
}

static void apple_spmi_irq_unmask(struct irq_data *d)
{
	struct apple_spmi *spmi = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&spmi->irq_mask_lock, flags);
	set_bit(d->hwirq, spmi->irq_mask_cache);
	apple_spmi_irq_unmask_raw(spmi, d->hwirq);
	raw_spin_unlock_irqrestore(&spmi->irq_mask_lock, flags);
}

static inline u32 apple_spmi_pack_cmd(u8 opc, u8 sid, u16 param)
{
	return opc | sid << 8 | (u32)param << 16 | (1 << 15);
}

/* Ack the fault interrupt, then pop and log every queued fault record */
static void apple_spmi_drain_faults(struct device *dev, struct apple_spmi *spmi)
{
	void __iomem *ack = spmi->regs + SPMI_GEN4_IRQ_ACK_BASE + (SPMI_GEN4_FAULT_IRQ / 32) * 4;
	u32 bit = BIT(SPMI_GEN4_FAULT_IRQ % 32), fault;
	int i;

	if (!spmi->faults)
		return;
	if (readl(ack) & bit)
		writel(bit, ack);
	for (i = 0; i < SPMI_FAULT_QUEUE_MAX; i++) {
		if (readl(spmi->faults + spmi->hw->status) & spmi->hw->rx_empty)
			return;
		fault = readl(spmi->faults + spmi->hw->reply);
		spmi->nfaults++;
		dev_warn_ratelimited(dev, "fault %#010x (%u so far)\n", fault, spmi->nfaults);
	}
}

/* Wait for Rx FIFO to have something */
static int apple_spmi_wait_rx_not_empty(struct spmi_controller *ctrl)
{
	struct apple_spmi *spmi = spmi_controller_get_drvdata(ctrl);
	int ret;
	u32 status;

	if (spmi->notify_irq) {
		ret = wait_for_completion_timeout(&spmi->fifo_rx,
			usecs_to_jiffies(REG_POLL_TIMEOUT_US));
		if (!ret)
			ret = -ETIMEDOUT;
		else if (readl(spmi->regs + spmi->hw->status) & spmi->hw->rx_empty)
			ret = -EIO;
		else
			ret = 0;
	} else {
		ret = readl_poll_timeout(spmi->regs + spmi->hw->status, status,
					 !(status & spmi->hw->rx_empty), 10,
					 REG_POLL_TIMEOUT_US);
	}

	if (ret) {
		spmi->prev_fail = true;
		dev_err(&ctrl->dev,
			"failed to wait for RX FIFO not empty\n");
		return ret;
	}

	return 0;
}

static int __spmi_raw_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid, u16 param,
			  const u8 *buf_wr, size_t len_wr, u8 *buf_rd, size_t len_rd)
{
	struct apple_spmi *spmi = spmi_controller_get_drvdata(ctrl);
	u32 spmi_cmd = apple_spmi_pack_cmd(opc, sid, param);
	u32 reply, rsp;
	size_t i = 0, j;
	int ret;

	if (spmi->prev_fail) {
		/* Do not consume a late reply as the reply to a new command. */
		if (!spmi->hw->legacy_irqs)
			return -EIO;
		writel(SPMI_ACT_FIFO_FLUSH, spmi->regs + SPMI_ACT_REG);
		apple_spmi_irq_ack_raw(spmi, spmi->hw->num_irqs);
		spmi->prev_fail = false;
	}
	reinit_completion(&spmi->fifo_rx);

	writel(spmi_cmd, spmi->regs + spmi->hw->command);

	while (i < len_wr) {
		j = min_t(size_t, sizeof(spmi_cmd), len_wr - i);
		spmi_cmd = 0;
		memcpy(&spmi_cmd, buf_wr + i, j);
		writel(spmi_cmd, spmi->regs + spmi->hw->command);
		i += j;
	}

	ret = apple_spmi_wait_rx_not_empty(ctrl);
	if (ret)
		return ret;

	reply = readl(spmi->regs + spmi->hw->reply);
	if (FIELD_GET(SPMI_REPLY_CMD, reply) != opc ||
	    FIELD_GET(SPMI_REPLY_SLAVE_ID, reply) != sid) {
		spmi->prev_fail = true;
		dev_err(&ctrl->dev, "unexpected SPMI response 0x%x\n", reply);
		return -EIO;
	}

	/* Read SPMI data reply */
	i = 0;
	while (i < len_rd) {
		if (readl(spmi->regs + spmi->hw->status) & spmi->hw->rx_empty) {
			spmi->prev_fail = true;
			dev_err_ratelimited(&ctrl->dev,
					    "FIFO lacks reply data, controller stuck?\n");
			return -EIO;
		}
		rsp = readl(spmi->regs + spmi->hw->reply);
		j = min_t(size_t, sizeof(spmi_cmd), len_rd - i);
		memcpy(buf_rd + i, &rsp, j);
		i += j;
	}

	if (!(readl(spmi->regs + spmi->hw->status) & spmi->hw->rx_empty)) {
		dev_warn(&ctrl->dev, "FIFO has extra data\n");
		spmi->prev_fail = true;
	}

	if (!len_rd && !FIELD_GET(SPMI_REPLY_ACK, reply)) {
		dev_err(&ctrl->dev, "command not acknowledged\n");
		return -EIO;
	}
	if (~FIELD_GET(SPMI_REPLY_FRAME_PARITY_STATUS, reply) & ((1 << len_rd) - 1)) {
		dev_err(&ctrl->dev, "some frames failed parity check\n");
		return -EIO;
	}
	return 0;
}

/* Logs the fault queue's records after a failed command */
static int spmi_raw_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid, u16 param,
			const u8 *buf_wr, size_t len_wr, u8 *buf_rd, size_t len_rd)
{
	struct apple_spmi *spmi = spmi_controller_get_drvdata(ctrl);
	int ret;

	guard(mutex)(&spmi->fifo_lock);
	ret = __spmi_raw_cmd(ctrl, opc, sid, param, buf_wr, len_wr, buf_rd, len_rd);
	if (ret)
		apple_spmi_drain_faults(&ctrl->dev, spmi);
	return ret;
}

/* Send a raw command with 1..16 input data frames */
static int spmi_raw_cmd_input(struct spmi_controller *ctrl, u8 opc, u8 sid,
			 u16 param, u8 *buf, size_t len)
{
	return spmi_raw_cmd(ctrl, opc, sid, param, NULL, 0, buf, len);
}

/* Send a raw command with (optional) body and an input ACK */
static int spmi_raw_cmd_ack(struct spmi_controller *ctrl, u8 opc, u8 sid,
			  u16 param, const u8 *buf, size_t len)
{
	return spmi_raw_cmd(ctrl, opc, sid, param, buf, len, NULL, 0);
}

static int spmi_read_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			 u16 saddr, u8 *buf, size_t len)
{
	switch (opc) {
	case SPMI_CMD_EXT_READ:
	case SPMI_CMD_EXT_READL:
		return spmi_raw_cmd_input(ctrl, opc | (len - 1), sid, saddr, buf, len);
	case SPMI_CMD_READ:
		return spmi_raw_cmd_input(ctrl, opc | saddr, sid, saddr, buf, len);
	}
	return -EINVAL;
}

static int spmi_write_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			  u16 saddr, const u8 *buf, size_t len)
{
	switch (opc) {
	case SPMI_CMD_WRITE:
		return spmi_raw_cmd_ack(ctrl, opc | saddr, sid, buf[0] << 8 | saddr, NULL, 0);
	case SPMI_CMD_ZERO_WRITE:
		return spmi_raw_cmd_ack(ctrl, opc | buf[0], sid, buf[0] << 8 | saddr, NULL, 0);
	case SPMI_CMD_EXT_WRITE:
	case SPMI_CMD_EXT_WRITEL:
		return spmi_raw_cmd_ack(ctrl, opc | (len - 1), sid, saddr, buf, len);
	}
	return -EINVAL;
}

static int spmi_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid)
{
	switch (opc) {
	case SPMI_CMD_RESET:
	case SPMI_CMD_SLEEP:
	case SPMI_CMD_SHUTDOWN:
	case SPMI_CMD_WAKEUP:
		return spmi_raw_cmd_ack(ctrl, opc, sid, 0, NULL, 0);
	}
	return -EINVAL;
}

static int apple_spmi_irq_set_type(struct irq_data *d, unsigned int type)
{
	/* all interrupts have MSI semantics */
	return type == IRQ_TYPE_EDGE_RISING ? 0 : -EINVAL;
}

static struct irq_chip apple_spmi_irq_chip = {
	.name = "apple_spmi",
	.irq_mask = apple_spmi_irq_mask,
	.irq_unmask = apple_spmi_irq_unmask,
	.irq_ack = apple_spmi_irq_ack,
	.irq_set_type = apple_spmi_irq_set_type,
	/*
	 * The controller sits in an always-on block and its parent interrupt is
	 * chained, so it stays live through suspend: a wake-armed peripheral
	 * interrupt needs nothing more from the chip to wake the system.
	 */
	.flags = IRQCHIP_ONESHOT_SAFE | IRQCHIP_SKIP_SET_WAKE,
};

static int apple_spmi_irq_domain_map(struct irq_domain *irqd,
					unsigned int irq, irq_hw_number_t hw)
{
	struct apple_spmi *spmi = irqd->host_data;

	irq_domain_set_info(irqd, irq, hw, &apple_spmi_irq_chip, irqd->host_data,
				handle_edge_irq, NULL, NULL);
	set_bit(hw, spmi->irq_used);
	return 0;
}

static int apple_spmi_irq_domain_translate(struct irq_domain *irqd,
					struct irq_fwspec *fwspec,
					unsigned long *hwirq,
					unsigned int *type)
{
	struct apple_spmi *spmi = irqd->host_data;
	u32 *args = fwspec->param;

	if (fwspec->param_count != 2)
		return -EINVAL;

	if (args[0] >= spmi->hw->num_irqs)
		return -EINVAL;
	*hwirq = args[0];
	*type = args[1] & IRQ_TYPE_SENSE_MASK;
	return 0;
}

static int apple_spmi_irq_domain_alloc(struct irq_domain *irqd, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_fwspec *fwspec = arg;
	struct apple_spmi *spmi = irqd->host_data;
	irq_hw_number_t hwirq;
	int i, ret;

	ret = apple_spmi_irq_domain_translate(irqd, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	if (hwirq + nr_irqs > spmi->hw->num_irqs)
		return -EINVAL;

	for (i = 0; i < nr_irqs; i++) {
		ret = apple_spmi_irq_domain_map(irqd, virq + i, hwirq + i);
		if (ret)
			return ret;
	}

	return 0;
}

static void apple_spmi_irq_domain_free(struct irq_domain *irqd, unsigned int virq,
				unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		struct irq_data *d = irq_domain_get_irq_data(irqd, virq + i);
		struct apple_spmi *spmi = irqd->host_data;

		clear_bit(d->hwirq, spmi->irq_used);
		irq_set_handler(virq + i, NULL);
		irq_domain_reset_irq_data(d);
	}
}

static const struct irq_domain_ops apple_spmi_irq_domain_ops = {
	.translate	= apple_spmi_irq_domain_translate,
	.alloc		= apple_spmi_irq_domain_alloc,
	.free		= apple_spmi_irq_domain_free,
};

static void apple_spmi_irq_handler(struct irq_desc *desc)
{
	struct apple_spmi *spmi = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 notify = spmi->hw->num_irqs;
	bool handled = false, held = false, raw_pending = false;
	unsigned long val, offset, bit;

	chained_irq_enter(chip, desc);
	val = readl(spmi->regs + SPMI_IRQ_ACK_BASE + (notify / 32) * 4);
	raw_pending |= !!val;
	if (val & BIT(notify % 32)) {
		apple_spmi_irq_ack_raw(spmi, notify);
		complete(&spmi->fifo_rx);
		handled = true;
	}
	/* Other queues' responses, such as the sleep's own commands at the debug link's cut. */
	if (val & ~BIT(notify % 32)) {
		writel(val & ~BIT(notify % 32), spmi->regs + SPMI_IRQ_ACK_BASE + (notify / 32) * 4);
		handled = true;
	}

	for (offset = 0; offset < notify / 8; offset += sizeof(val)) {
		unsigned long stray;

		val = readq(spmi->regs + SPMI_IRQ_ACK_BASE + offset);
		raw_pending |= !!val;
		/*
		 * After a real sleep, sources no driver uses come up pending (on the Neo the
		 * USB-PD controllers' even lines, latched by the debug link's cut), and the
		 * interrupt fires for them with nothing of ours to run. Clear them, as probe does.
		 * A used source that is masked, as all are until the resume enables them, stays
		 * pending for its unmask: it isn't a bad interrupt either.
		 */
		stray = val & ~spmi->irq_used[offset / sizeof(val)];
		if (stray) {
			writel(lower_32_bits(stray), spmi->regs + SPMI_IRQ_ACK_BASE + offset);
			writel(upper_32_bits(stray), spmi->regs + SPMI_IRQ_ACK_BASE + offset + 4);
			handled = true;
		}
		if (val & spmi->irq_used[offset / sizeof(val)] &
		    ~spmi->irq_mask_cache[offset / sizeof(val)])
			held = true;
		/**
		 * because of other masters in the bus, we're going to get a multitude of
		 * interrupts we're not interested in. irq_resolve_mapping isn't very
		 * optimized for the nonexistent path, so instead we mask with (a locally
		 * cached version of) the IRQ mask
		 */
		val &= spmi->irq_mask_cache[offset / sizeof(val)];
		for_each_set_bit(bit, &val, 64) {
			generic_handle_domain_irq(spmi->irqd, offset * 8 + bit);
			handled = true;
		}
	}
	/*
	 * Native dispatch completes normally when every sampled bank is empty.
	 * Keep the diagnostic for unclassified nonempty samples, including a
	 * mask transition; an empty parent needs no invented controller ACK.
	 */
	if (!handled && !held && raw_pending)
		handle_bad_irq(desc);
	/*
	 * The acks are posted writes: let them reach the controller before the parent is
	 * unmasked, or it fires again for what was just cleared.
	 */
	if (handled)
		readl(spmi->regs + SPMI_IRQ_ACK_BASE);
	chained_irq_exit(chip, desc);
}

static void apple_spmi_teardown_irq(void *data)
{
	struct apple_spmi *spmi = data;

	for (size_t offset = 0; offset < (spmi->hw->num_irqs + 32) / 8; offset += 4)
		writel(0, spmi->regs + SPMI_IRQ_MASK_BASE + offset);

	synchronize_irq(spmi->irq);
	irq_set_chained_handler_and_data(spmi->irq, NULL, NULL);
}

static int apple_spmi_init_irq(struct platform_device *pdev,
			       struct apple_spmi *spmi, int irq)
{
	int ret;
	struct irq_domain_info info = {
		.fwnode		= pdev->dev.fwnode,
		.hwirq_max	= ~0U,
		.ops		= &apple_spmi_irq_domain_ops,
		.host_data	= spmi,
	};

	raw_spin_lock_init(&spmi->irq_mask_lock);

	for (size_t offset = 0; offset < (spmi->hw->num_irqs + 32) / 8; offset += 4) {
		writel(0, spmi->regs + SPMI_IRQ_MASK_BASE + offset);
		writel(U32_MAX, spmi->regs + SPMI_IRQ_ACK_BASE + offset);
	}

	spmi->irqd = devm_irq_domain_instantiate(&pdev->dev, &info);
	if (IS_ERR(spmi->irqd))
		return PTR_ERR(spmi->irqd);

	ret = devm_add_action(&pdev->dev, apple_spmi_teardown_irq, spmi);
	if (ret)
		return ret;

	irq_set_chained_handler_and_data(spmi->irq, apple_spmi_irq_handler, spmi);
	apple_spmi_irq_unmask_raw(spmi, spmi->hw->num_irqs);
	spmi->notify_irq = true;

	return 0;
}

/*
 * Every command asks for an interrupt on its response, on the vector this
 * driver waits on. The system's wake logic watches this queue's interrupt line
 * for the PMU's RTC alarm, so that vector has to be quiet and clear before a
 * firmware-driven suspend, or the commit may read our own traffic as a wake.
 * macOS clears the same enable bit at about the same point (measured under aura).
 * Any command sent from here on polls for its response.
 */
static int apple_spmi_suspend_noirq(struct device *dev)
{
	struct apple_spmi *spmi = dev_get_drvdata(dev);
	unsigned long flags;

	if (!spmi->notify_irq || !pm_suspend_via_firmware())
		return 0;

	guard(mutex)(&spmi->fifo_lock);
	spmi->notify_irq = false;
	spmi->notify_parked = true;
	raw_spin_lock_irqsave(&spmi->irq_mask_lock, flags);
	apple_spmi_irq_mask_raw(spmi, spmi->hw->num_irqs);
	apple_spmi_irq_ack_raw(spmi, spmi->hw->num_irqs);
	raw_spin_unlock_irqrestore(&spmi->irq_mask_lock, flags);

	return 0;
}

static int apple_spmi_resume_noirq(struct device *dev)
{
	struct apple_spmi *spmi = dev_get_drvdata(dev);
	u32 words[SPMI_MAX_PERIPHERAL_IRQS / 32];
	unsigned long flags;
	size_t i;

	if (!spmi->notify_parked)
		return 0;

	guard(mutex)(&spmi->fifo_lock);
	/*
	 * Firmware and the platform's sleep code use this queue while Linux is
	 * away. Every command of ours was answered before the suspend, so any
	 * reply waiting now is someone else's, and would be read as the reply
	 * to our next command.
	 */
	if (pm_resume_via_firmware()) {
		unsigned int stale = 0;

		while (!(readl(spmi->regs + spmi->hw->status) & spmi->hw->rx_empty) &&
		       stale < 64) {
			readl(spmi->regs + spmi->hw->reply);
			stale++;
		}
		if (stale)
			dev_info(dev, "dropped %u stale reply words after the wake\n", stale);
	}
	raw_spin_lock_irqsave(&spmi->irq_mask_lock, flags);
	/* Whether the enables survive a real sleep has not been measured. */
	if (pm_resume_via_firmware()) {
		bitmap_to_arr32(words, spmi->irq_mask_cache, spmi->hw->num_irqs);
		for (i = 0; i < spmi->hw->num_irqs / 32; i++)
			writel(words[i], spmi->regs + SPMI_IRQ_MASK_BASE + i * 4);
	}
	apple_spmi_irq_ack_raw(spmi, spmi->hw->num_irqs);
	apple_spmi_irq_unmask_raw(spmi, spmi->hw->num_irqs);
	raw_spin_unlock_irqrestore(&spmi->irq_mask_lock, flags);
	spmi->notify_parked = false;
	spmi->notify_irq = true;

	return 0;
}

static int apple_spmi_probe(struct platform_device *pdev)
{
	struct apple_spmi *spmi;
	struct spmi_controller *ctrl;
	int ret;

	ctrl = devm_spmi_controller_alloc(&pdev->dev, sizeof(*spmi));
	if (IS_ERR(ctrl))
		return -ENOMEM;

	spmi = spmi_controller_get_drvdata(ctrl);
	spmi->hw = device_get_match_data(&pdev->dev);
	if (!spmi->hw)
		return -EINVAL;
	mutex_init(&spmi->fifo_lock);
	init_completion(&spmi->fifo_rx);
	platform_set_drvdata(pdev, spmi);

	spmi->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spmi->regs))
		return PTR_ERR(spmi->regs);

	if (!spmi->hw->legacy_irqs && platform_get_resource(pdev, IORESOURCE_MEM, 1)) {
		spmi->faults = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(spmi->faults))
			return PTR_ERR(spmi->faults);
		apple_spmi_drain_faults(&pdev->dev, spmi);
	}

	ctrl->dev.of_node = pdev->dev.of_node;

	ctrl->read_cmd = spmi_read_cmd;
	ctrl->write_cmd = spmi_write_cmd;
	ctrl->cmd = spmi_cmd;

	spmi->irq = platform_get_irq_optional(pdev, 0);
	if (spmi->irq < 0 && spmi->irq != -ENXIO)
		return spmi->irq;
	if (spmi->irq >= 0) {
		if (!spmi->hw->legacy_irqs)
			return dev_err_probe(&pdev->dev, -EOPNOTSUPP,
					     "Generation 4 interrupt layout is not supported\n");
		ret = apple_spmi_init_irq(pdev, spmi, spmi->irq);
		if (ret)
			return ret;
	}

	ret = devm_spmi_controller_add(&pdev->dev, ctrl);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "spmi_controller_add failed\n");

	return 0;
}

/*
 * After a resume from disk the controller holds the boot kernel's masks and
 * pending interrupts, not this kernel's: drop the pending ones and rewrite
 * the masks from the cache the mask/unmask paths keep, notification IRQ
 * included.
 */
static void apple_spmi_hib_resync_irqs(struct apple_spmi *spmi)
{
	u32 nirq = spmi->hw->num_irqs;
	unsigned long flags;

	raw_spin_lock_irqsave(&spmi->irq_mask_lock, flags);
	/*
	 * As at probe, drop what is latched: interrupts the boot kernel left
	 * pending complete a register selection of a device restored now
	 * before its own answer arrives. The devices read their state afresh.
	 */
	for (size_t offset = 0; offset < (nirq + 32) / 8; offset += 4) {
		writel(0, spmi->regs + SPMI_IRQ_MASK_BASE + offset);
		writel(U32_MAX, spmi->regs + SPMI_IRQ_ACK_BASE + offset);
	}
	for (size_t offset = 0; offset < (nirq + 32) / 8; offset += 4) {
		u32 val = 0;

		if (offset < nirq / 8)
			val = bitmap_read(spmi->irq_mask_cache, offset * 8, 32);
		else if (spmi->notify_irq && offset == (nirq / 32) * 4)
			val = BIT(nirq % 32);
		writel(val, spmi->regs + SPMI_IRQ_MASK_BASE + offset);
	}
	raw_spin_unlock_irqrestore(&spmi->irq_mask_lock, flags);
}

static int apple_spmi_restore_noirq(struct device *dev)
{
	struct apple_spmi *spmi = dev_get_drvdata(dev);

	/* Hibernation aborted before the image (PMSG_RESTORE then too): nothing restarted. */
	if (!pm_hibernate_is_restored())
		return apple_spmi_resume_noirq(dev);
	if (spmi->irq < 0)
		return 0;

	apple_spmi_hib_resync_irqs(spmi);
	spmi->hib_resynced = true;

	return 0;
}

/*
 * A controller in a PM domain does not get the callback above: genpd's
 * restore_noirq calls the driver's resume_noirq instead (outside a wakeup
 * path). The masks then stay as the boot kernel left them, everything masked
 * but the interrupts of the drivers it loaded, and the IRQ core does not write
 * them again (it disables lazily and unmasks only an IRQ it recorded as
 * masked). A device whose driver the boot kernel did not load, such as a
 * Type-C port controller on nub_spmi_a0/a1, would raise no event after the
 * restore. genpd does not wrap the early restore, so the masks are rewritten
 * there, before the devices on the bus are restored.
 */
static int apple_spmi_restore_early(struct device *dev)
{
	struct apple_spmi *spmi = dev_get_drvdata(dev);

	if (spmi->hib_resynced) {
		spmi->hib_resynced = false;
		return 0;
	}
	if (!pm_hibernate_is_restored() || spmi->irq < 0)
		return 0;

	/* Commands can run by now: none may have its reply acknowledged away. */
	scoped_guard(mutex, &spmi->fifo_lock)
		apple_spmi_hib_resync_irqs(spmi);
	dev_dbg(dev, "resume from disk: interrupt masks rewritten in the early restore\n");

	return 0;
}

/*
 * Suspend-to-RAM parks the notification queue; a resume from disk rewrites the
 * masks, in the early restore where a PM domain kept the noirq one from running.
 */
static const struct dev_pm_ops apple_spmi_pm_ops = {
	.suspend_noirq = pm_sleep_ptr(apple_spmi_suspend_noirq),
	.resume_noirq = pm_sleep_ptr(apple_spmi_resume_noirq),
	.freeze_noirq = pm_sleep_ptr(apple_spmi_suspend_noirq),
	.thaw_noirq = pm_sleep_ptr(apple_spmi_resume_noirq),
	.poweroff_noirq = pm_sleep_ptr(apple_spmi_suspend_noirq),
	.restore_noirq = pm_sleep_ptr(apple_spmi_restore_noirq),
	.restore_early = pm_sleep_ptr(apple_spmi_restore_early),
};

static const struct of_device_id apple_spmi_match_table[] = {
	{ .compatible = "apple,t8142-spmi", .data = &apple_spmi_gen4 },
	{ .compatible = "apple,t8103-spmi", .data = &apple_spmi_gen1 },
	{ .compatible = "apple,t6000-spmi", .data = &apple_spmi_t6000 },
	{ .compatible = "apple,spmi", .data = &apple_spmi_gen1 },
	{}
};
MODULE_DEVICE_TABLE(of, apple_spmi_match_table);

static struct platform_driver apple_spmi_driver = {
	.probe		= apple_spmi_probe,
	.driver		= {
		.name	= "apple-spmi",
		.of_match_table = apple_spmi_match_table,
		.pm	= pm_sleep_ptr(&apple_spmi_pm_ops),
	},
};
module_platform_driver(apple_spmi_driver);

MODULE_AUTHOR("Jean-Francois Bortolotti <jeff@borto.fr>");
MODULE_DESCRIPTION("Apple SoC SPMI driver");
MODULE_LICENSE("GPL");
